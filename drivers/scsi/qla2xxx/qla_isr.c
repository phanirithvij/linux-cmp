/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2005 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"

static void qla2x00_mbx_completion(scsi_qla_host_t *, uint16_t);
static void qla2x00_async_event(scsi_qla_host_t *, uint16_t *);
static void qla2x00_process_completed_request(struct scsi_qla_host *, uint32_t);
static void qla2x00_status_entry(scsi_qla_host_t *, void *);
static void qla2x00_status_cont_entry(scsi_qla_host_t *, sts_cont_entry_t *);
static void qla2x00_error_entry(scsi_qla_host_t *, sts_entry_t *);
static void qla2x00_ms_entry(scsi_qla_host_t *, ms_iocb_entry_t *);

static void qla24xx_ms_entry(scsi_qla_host_t *, struct ct_entry_24xx *);

/**
 * qla2100_intr_handler() - Process interrupts for the ISP2100 and ISP2200.
 * @irq:
 * @dev_id: SCSI driver HA context
 * @regs:
 *
 * Called by system whenever the host adapter generates an interrupt.
 *
 * Returns handled flag.
 */
irqreturn_t
qla2100_intr_handler(int irq, void *dev_id, struct pt_regs *regs)
{
	scsi_qla_host_t	*ha;
	struct device_reg_2xxx __iomem *reg;
	int		status;
	unsigned long	flags;
	unsigned long	iter;
	uint16_t	mb[4];

	ha = (scsi_qla_host_t *) dev_id;
	if (!ha) {
		printk(KERN_INFO
		    "%s(): NULL host pointer\n", __func__);
		return (IRQ_NONE);
	}

	reg = &ha->iobase->isp;
	status = 0;

	spin_lock_irqsave(&ha->hardware_lock, flags);
	for (iter = 50; iter--; ) {
		if ((RD_REG_WORD(&reg->istatus) & ISR_RISC_INT) == 0)
			break;

		if (RD_REG_WORD(&reg->semaphore) & BIT_0) {
			WRT_REG_WORD(&reg->hccr, HCCR_CLR_RISC_INT);
			RD_REG_WORD(&reg->hccr);

			/* Get mailbox data. */
			mb[0] = RD_MAILBOX_REG(ha, reg, 0);
			if (mb[0] > 0x3fff && mb[0] < 0x8000) {
				qla2x00_mbx_completion(ha, mb[0]);
				status |= MBX_INTERRUPT;
			} else if (mb[0] > 0x7fff && mb[0] < 0xc000) {
				mb[1] = RD_MAILBOX_REG(ha, reg, 1);
				mb[2] = RD_MAILBOX_REG(ha, reg, 2);
				mb[3] = RD_MAILBOX_REG(ha, reg, 3);
				qla2x00_async_event(ha, mb);
			} else {
				/*EMPTY*/
				DEBUG2(printk("scsi(%ld): Unrecognized "
				    "interrupt type (%d).\n",
				    ha->host_no, mb[0]));
			}
			/* Release mailbox registers. */
			WRT_REG_WORD(&reg->semaphore, 0);
			RD_REG_WORD(&reg->semaphore);
		} else {
			qla2x00_process_response_queue(ha);

			WRT_REG_WORD(&reg->hccr, HCCR_CLR_RISC_INT);
			RD_REG_WORD(&reg->hccr);
		}
	}
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	if (test_bit(MBX_INTR_WAIT, &ha->mbx_cmd_flags) &&
	    (status & MBX_INTERRUPT) && ha->flags.mbox_int) {
		spin_lock_irqsave(&ha->mbx_reg_lock, flags);

		set_bit(MBX_INTERRUPT, &ha->mbx_cmd_flags);
		up(&ha->mbx_intr_sem);

		spin_unlock_irqrestore(&ha->mbx_reg_lock, flags);
	}

	return (IRQ_HANDLED);
}

/**
 * qla2300_intr_handler() - Process interrupts for the ISP23xx and ISP63xx.
 * @irq:
 * @dev_id: SCSI driver HA context
 * @regs:
 *
 * Called by system whenever the host adapter generates an interrupt.
 *
 * Returns handled flag.
 */
irqreturn_t
qla2300_intr_handler(int irq, void *dev_id, struct pt_regs *regs)
{
	scsi_qla_host_t	*ha;
	struct device_reg_2xxx __iomem *reg;
	int		status;
	unsigned long	flags;
	unsigned long	iter;
	uint32_t	stat;
	uint16_t	hccr;
	uint16_t	mb[4];

	ha = (scsi_qla_host_t *) dev_id;
	if (!ha) {
		printk(KERN_INFO
		    "%s(): NULL host pointer\n", __func__);
		return (IRQ_NONE);
	}

	reg = &ha->iobase->isp;
	status = 0;

	spin_lock_irqsave(&ha->hardware_lock, flags);
	for (iter = 50; iter--; ) {
		stat = RD_REG_DWORD(&reg->u.isp2300.host_status);
		if (stat & HSR_RISC_PAUSED) {
			hccr = RD_REG_WORD(&reg->hccr);
			if (hccr & (BIT_15 | BIT_13 | BIT_11 | BIT_8))
				qla_printk(KERN_INFO, ha,
				    "Parity error -- HCCR=%x.\n", hccr);
			else
				qla_printk(KERN_INFO, ha,
				    "RISC paused -- HCCR=%x.\n", hccr);

			/*
			 * Issue a "HARD" reset in order for the RISC
			 * interrupt bit to be cleared.  Schedule a big
			 * hammmer to get out of the RISC PAUSED state.
			 */
			WRT_REG_WORD(&reg->hccr, HCCR_RESET_RISC);
			RD_REG_WORD(&reg->hccr);
			set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
			break;
		} else if ((stat & HSR_RISC_INT) == 0)
			break;

		switch (stat & 0xff) {
		case 0x1:
		case 0x2:
		case 0x10:
		case 0x11:
			qla2x00_mbx_completion(ha, MSW(stat));
			status |= MBX_INTERRUPT;

			/* Release mailbox registers. */
			WRT_REG_WORD(&reg->semaphore, 0);
			break;
		case 0x12:
			mb[0] = MSW(stat);
			mb[1] = RD_MAILBOX_REG(ha, reg, 1);
			mb[2] = RD_MAILBOX_REG(ha, reg, 2);
			mb[3] = RD_MAILBOX_REG(ha, reg, 3);
			qla2x00_async_event(ha, mb);
			break;
		case 0x13:
			qla2x00_process_response_queue(ha);
			break;
		case 0x15:
			mb[0] = MBA_CMPLT_1_16BIT;
			mb[1] = MSW(stat);
			qla2x00_async_event(ha, mb);
			break;
		case 0x16:
			mb[0] = MBA_SCSI_COMPLETION;
			mb[1] = MSW(stat);
			mb[2] = RD_MAILBOX_REG(ha, reg, 2);
			qla2x00_async_event(ha, mb);
			break;
		default:
			DEBUG2(printk("scsi(%ld): Unrecognized interrupt type "
			    "(%d).\n",
			    ha->host_no, stat & 0xff));
			break;
		}
		WRT_REG_WORD(&reg->hccr, HCCR_CLR_RISC_INT);
		RD_REG_WORD_RELAXED(&reg->hccr);
	}
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	if (test_bit(MBX_INTR_WAIT, &ha->mbx_cmd_flags) &&
	    (status & MBX_INTERRUPT) && ha->flags.mbox_int) {
		spin_lock_irqsave(&ha->mbx_reg_lock, flags);

		set_bit(MBX_INTERRUPT, &ha->mbx_cmd_flags);
		up(&ha->mbx_intr_sem);

		spin_unlock_irqrestore(&ha->mbx_reg_lock, flags);
	}

	return (IRQ_HANDLED);
}

/**
 * qla2x00_mbx_completion() - Process mailbox command completions.
 * @ha: SCSI driver HA context
 * @mb0: Mailbox0 register
 */
static void
qla2x00_mbx_completion(scsi_qla_host_t *ha, uint16_t mb0)
{
	uint16_t	cnt;
	uint16_t __iomem *wptr;
	struct device_reg_2xxx __iomem *reg = &ha->iobase->isp;

	/* Load return mailbox registers. */
	ha->flags.mbox_int = 1;
	ha->mailbox_out[0] = mb0;
	wptr = (uint16_t __iomem *)MAILBOX_REG(ha, reg, 1);

	for (cnt = 1; cnt < ha->mbx_count; cnt++) {
		if (IS_QLA2200(ha) && cnt == 8)
			wptr = (uint16_t __iomem *)MAILBOX_REG(ha, reg, 8);
		if (cnt == 4 || cnt == 5)
			ha->mailbox_out[cnt] = qla2x00_debounce_register(wptr);
		else
			ha->mailbox_out[cnt] = RD_REG_WORD(wptr);

		wptr++;
	}

	if (ha->mcp) {
		DEBUG3(printk("%s(%ld): Got mailbox completion. cmd=%x.\n",
		    __func__, ha->host_no, ha->mcp->mb[0]));
	} else {
		DEBUG2_3(printk("%s(%ld): MBX pointer ERROR!\n",
		    __func__, ha->host_no));
	}
}

/**
 * qla2x00_async_event() - Process aynchronous events.
 * @ha: SCSI driver HA context
 * @mb: Mailbox registers (0 - 3)
 */
static void
qla2x00_async_event(scsi_qla_host_t *ha, uint16_t *mb)
{
#define LS_UNKNOWN	2
	static char	*link_speeds[5] = { "1", "2", "?", "4", "10" };
	char		*link_speed;
	uint16_t	handle_cnt;
	uint16_t	cnt;
	uint32_t	handles[5];
	struct device_reg_2xxx __iomem *reg = &ha->iobase->isp;
	uint32_t	rscn_entry, host_pid;
	uint8_t		rscn_queue_index;

	/* Setup to process RIO completion. */
	handle_cnt = 0;
	switch (mb[0]) {
	case MBA_SCSI_COMPLETION:
		handles[0] = le32_to_cpu((uint32_t)((mb[2] << 16) | mb[1]));
		handle_cnt = 1;
		break;
	case MBA_CMPLT_1_16BIT:
		handles[0] = mb[1];
		handle_cnt = 1;
		mb[0] = MBA_SCSI_COMPLETION;
		break;
	case MBA_CMPLT_2_16BIT:
		handles[0] = mb[1];
		handles[1] = mb[2];
		handle_cnt = 2;
		mb[0] = MBA_SCSI_COMPLETION;
		break;
	case MBA_CMPLT_3_16BIT:
		handles[0] = mb[1];
		handles[1] = mb[2];
		handles[2] = mb[3];
		handle_cnt = 3;
		mb[0] = MBA_SCSI_COMPLETION;
		break;
	case MBA_CMPLT_4_16BIT:
		handles[0] = mb[1];
		handles[1] = mb[2];
		handles[2] = mb[3];
		handles[3] = (uint32_t)RD_MAILBOX_REG(ha, reg, 6);
		handle_cnt = 4;
		mb[0] = MBA_SCSI_COMPLETION;
		break;
	case MBA_CMPLT_5_16BIT:
		handles[0] = mb[1];
		handles[1] = mb[2];
		handles[2] = mb[3];
		handles[3] = (uint32_t)RD_MAILBOX_REG(ha, reg, 6);
		handles[4] = (uint32_t)RD_MAILBOX_REG(ha, reg, 7);
		handle_cnt = 5;
		mb[0] = MBA_SCSI_COMPLETION;
		break;
	case MBA_CMPLT_2_32BIT:
		handles[0] = le32_to_cpu((uint32_t)((mb[2] << 16) | mb[1]));
		handles[1] = le32_to_cpu(
		    ((uint32_t)(RD_MAILBOX_REG(ha, reg, 7) << 16)) |
		    RD_MAILBOX_REG(ha, reg, 6));
		handle_cnt = 2;
		mb[0] = MBA_SCSI_COMPLETION;
		break;
	default:
		break;
	}

	switch (mb[0]) {
	case MBA_SCSI_COMPLETION:	/* Fast Post */
		if (!ha->flags.online)
			break;

		for (cnt = 0; cnt < handle_cnt; cnt++)
			qla2x00_process_completed_request(ha, handles[cnt]);
		break;

	case MBA_RESET:			/* Reset */
		DEBUG2(printk("scsi(%ld): Asynchronous RESET.\n", ha->host_no));

		set_bit(RESET_MARKER_NEEDED, &ha->dpc_flags);
		break;

	case MBA_SYSTEM_ERR:		/* System Error */
		mb[1] = RD_MAILBOX_REG(ha, reg, 1);
		mb[2] = RD_MAILBOX_REG(ha, reg, 2);
		mb[3] = RD_MAILBOX_REG(ha, reg, 3);

		qla_printk(KERN_INFO, ha,
		    "ISP System Error - mbx1=%xh mbx2=%xh mbx3=%xh.\n",
		    mb[1], mb[2], mb[3]);

		ha->isp_ops.fw_dump(ha, 1);

		if (IS_QLA24XX(ha) || IS_QLA25XX(ha)) {
			if (mb[1] == 0 && mb[2] == 0) {
				qla_printk(KERN_ERR, ha,
				    "Unrecoverable Hardware Error: adapter "
				    "marked OFFLINE!\n");
				ha->flags.online = 0;
			} else
				set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		} else if (mb[1] == 0) {
			qla_printk(KERN_INFO, ha,
			    "Unrecoverable Hardware Error: adapter marked "
			    "OFFLINE!\n");
			ha->flags.online = 0;
		} else
			set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		break;

	case MBA_REQ_TRANSFER_ERR:	/* Request Transfer Error */
		DEBUG2(printk("scsi(%ld): ISP Request Transfer Error.\n",
		    ha->host_no));
		qla_printk(KERN_WARNING, ha, "ISP Request Transfer Error.\n");

		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		break;

	case MBA_RSP_TRANSFER_ERR:	/* Response Transfer Error */
		DEBUG2(printk("scsi(%ld): ISP Response Transfer Error.\n",
		    ha->host_no));
		qla_printk(KERN_WARNING, ha, "ISP Response Transfer Error.\n");

		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		break;

	case MBA_WAKEUP_THRES:		/* Request Queue Wake-up */
		DEBUG2(printk("scsi(%ld): Asynchronous WAKEUP_THRES.\n",
		    ha->host_no));
		break;

	case MBA_LIP_OCCURRED:		/* Loop Initialization Procedure */
		DEBUG2(printk("scsi(%ld): LIP occured (%x).\n", ha->host_no,
		    mb[1]));
		qla_printk(KERN_INFO, ha, "LIP occured (%x).\n", mb[1]);

		if (atomic_read(&ha->loop_state) != LOOP_DOWN) {
			atomic_set(&ha->loop_state, LOOP_DOWN);
			atomic_set(&ha->loop_down_timer, LOOP_DOWN_TIME);
			qla2x00_mark_all_devices_lost(ha, 1);
		}

		set_bit(REGISTER_FC4_NEEDED, &ha->dpc_flags);

		ha->flags.management_server_logged_in = 0;

		/* Update AEN queue. */
		qla2x00_enqueue_aen(ha, MBA_LIP_OCCURRED, NULL);

		break;

	case MBA_LOOP_UP:		/* Loop Up Event */
		ha->link_data_rate = 0;
		if (IS_QLA2100(ha) || IS_QLA2200(ha)) {
			link_speed = link_speeds[0];
		} else {
			link_speed = link_speeds[LS_UNKNOWN];
			if (mb[1] < 5)
				link_speed = link_speeds[mb[1]];
			ha->link_data_rate = mb[1];
		}

		DEBUG2(printk("scsi(%ld): Asynchronous LOOP UP (%s Gbps).\n",
		    ha->host_no, link_speed));
		qla_printk(KERN_INFO, ha, "LOOP UP detected (%s Gbps).\n",
		    link_speed);

		ha->flags.management_server_logged_in = 0;

		/* Update AEN queue. */
		qla2x00_enqueue_aen(ha, MBA_LOOP_UP, NULL);
		break;

	case MBA_LOOP_DOWN:		/* Loop Down Event */
		DEBUG2(printk("scsi(%ld): Asynchronous LOOP DOWN (%x).\n",
		    ha->host_no, mb[1]));
		qla_printk(KERN_INFO, ha, "LOOP DOWN detected (%x).\n", mb[1]);

		if (atomic_read(&ha->loop_state) != LOOP_DOWN) {
			atomic_set(&ha->loop_state, LOOP_DOWN);
			atomic_set(&ha->loop_down_timer, LOOP_DOWN_TIME);
			ha->device_flags |= DFLG_NO_CABLE;
			qla2x00_mark_all_devices_lost(ha, 1);
		}

		ha->flags.management_server_logged_in = 0;
		ha->link_data_rate = 0;
		if (ql2xfdmienable)
			set_bit(REGISTER_FDMI_NEEDED, &ha->dpc_flags);

		/* Update AEN queue. */
		qla2x00_enqueue_aen(ha, MBA_LOOP_DOWN, NULL);
		break;

	case MBA_LIP_RESET:		/* LIP reset occurred */
		DEBUG2(printk("scsi(%ld): Asynchronous LIP RESET (%x).\n",
		    ha->host_no, mb[1]));
		qla_printk(KERN_INFO, ha,
		    "LIP reset occured (%x).\n", mb[1]);

		if (atomic_read(&ha->loop_state) != LOOP_DOWN) {
			atomic_set(&ha->loop_state, LOOP_DOWN);
			atomic_set(&ha->loop_down_timer, LOOP_DOWN_TIME);
			qla2x00_mark_all_devices_lost(ha, 1);
		}

		set_bit(RESET_MARKER_NEEDED, &ha->dpc_flags);

		ha->operating_mode = LOOP;
		ha->flags.management_server_logged_in = 0;

		/* Update AEN queue. */
		qla2x00_enqueue_aen(ha, MBA_LIP_RESET, NULL);

		break;

	case MBA_POINT_TO_POINT:	/* Point-to-Point */
		if (IS_QLA2100(ha))
			break;

		DEBUG2(printk("scsi(%ld): Asynchronous P2P MODE received.\n",
		    ha->host_no));

		/*
		 * Until there's a transition from loop down to loop up, treat
		 * this as loop down only.
		 */
		if (atomic_read(&ha->loop_state) != LOOP_DOWN) {
			atomic_set(&ha->loop_state, LOOP_DOWN);
			if (!atomic_read(&ha->loop_down_timer))
				atomic_set(&ha->loop_down_timer,
				    LOOP_DOWN_TIME);
			qla2x00_mark_all_devices_lost(ha, 1);
		}

		if (!(test_bit(ABORT_ISP_ACTIVE, &ha->dpc_flags))) {
			set_bit(RESET_MARKER_NEEDED, &ha->dpc_flags);
		}
		set_bit(REGISTER_FC4_NEEDED, &ha->dpc_flags);
		break;

	case MBA_CHG_IN_CONNECTION:	/* Change in connection mode */
		if (IS_QLA2100(ha))
			break;

		DEBUG2(printk("scsi(%ld): Asynchronous Change In Connection "
		    "received.\n",
		    ha->host_no));
		qla_printk(KERN_INFO, ha,
		    "Configuration change detected: value=%x.\n", mb[1]);

		if (atomic_read(&ha->loop_state) != LOOP_DOWN) {
			atomic_set(&ha->loop_state, LOOP_DOWN);
			if (!atomic_read(&ha->loop_down_timer))
				atomic_set(&ha->loop_down_timer,
				    LOOP_DOWN_TIME);
			qla2x00_mark_all_devices_lost(ha, 1);
		}

		set_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags);
		set_bit(LOCAL_LOOP_UPDATE, &ha->dpc_flags);
		break;

	case MBA_PORT_UPDATE:		/* Port database update */
		/*
		 * If a single remote port just logged into (or logged out of)
		 * us, create a new entry in our rscn fcports list and handle
		 * the event like an RSCN.
		 */
		if (ql2xprocessrscn &&
		    !IS_QLA2100(ha) && !IS_QLA2200(ha) && !IS_QLA6312(ha) &&
		    !IS_QLA6322(ha) && !IS_QLA24XX(ha) && !IS_QLA25XX(ha) &&
		    ha->flags.init_done && mb[1] != 0xffff &&
		    ((ha->operating_mode == P2P && mb[1] != 0) ||
		    (ha->operating_mode != P2P && mb[1] !=
			SNS_FIRST_LOOP_ID)) && (mb[2] == 6 || mb[2] == 7)) {
			int rval;
			fc_port_t *rscn_fcport;

			/* Create new fcport for login. */
			rscn_fcport = qla2x00_alloc_rscn_fcport(ha, GFP_ATOMIC);
			if (rscn_fcport) {
				DEBUG14(printk("scsi(%ld): Port Update -- "
				    "creating RSCN fcport %p for %x/%x/%x.\n",
				    ha->host_no, rscn_fcport, mb[1], mb[2],
				    mb[3]));

				rscn_fcport->loop_id = mb[1];
				rscn_fcport->d_id.b24 = INVALID_PORT_ID;
				atomic_set(&rscn_fcport->state,
				    FCS_DEVICE_LOST);
				list_add_tail(&rscn_fcport->list,
				    &ha->rscn_fcports);

				rval = qla2x00_handle_port_rscn(ha, 0,
				    rscn_fcport, 1);
				if (rval == QLA_SUCCESS)
					break;
			} else {
				DEBUG14(printk("scsi(%ld): Port Update -- "
				    "-- unable to allocate RSCN fcport "
				    "login.\n", ha->host_no));
			}
		}

		/*
		 * If PORT UPDATE is global (recieved LIP_OCCURED/LIP_RESET
		 * event etc. earlier indicating loop is down) then process
		 * it.  Otherwise ignore it and Wait for RSCN to come in.
		 */
		atomic_set(&ha->loop_down_timer, 0);
		if (atomic_read(&ha->loop_state) != LOOP_DOWN &&
		    atomic_read(&ha->loop_state) != LOOP_DEAD) {
			DEBUG2(printk("scsi(%ld): Asynchronous PORT UPDATE "
			    "ignored %04x/%04x/%04x.\n", ha->host_no, mb[1],
			    mb[2], mb[3]));
			break;
		}

		DEBUG2(printk("scsi(%ld): Asynchronous PORT UPDATE.\n",
		    ha->host_no));
		DEBUG(printk(KERN_INFO
		    "scsi(%ld): Port database changed %04x %04x %04x.\n",
		    ha->host_no, mb[1], mb[2], mb[3]));

		/*
		 * Mark all devices as missing so we will login again.
		 */
		atomic_set(&ha->loop_state, LOOP_UP);

		qla2x00_mark_all_devices_lost(ha, 1);

		ha->flags.rscn_queue_overflow = 1;

		set_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags);
		set_bit(LOCAL_LOOP_UPDATE, &ha->dpc_flags);

		/* Update AEN queue. */
		qla2x00_enqueue_aen(ha, MBA_PORT_UPDATE, NULL);
		break;

	case MBA_RSCN_UPDATE:		/* State Change Registration */
		DEBUG2(printk("scsi(%ld): Asynchronous RSCR UPDATE.\n",
		    ha->host_no));
		DEBUG(printk(KERN_INFO
		    "scsi(%ld): RSCN database changed -- %04x %04x.\n",
		    ha->host_no, mb[1], mb[2]));

		rscn_entry = (mb[1] << 16) | mb[2];
		host_pid = (ha->d_id.b.domain << 16) | (ha->d_id.b.area << 8) |
		    ha->d_id.b.al_pa;
		if (rscn_entry == host_pid) {
			DEBUG(printk(KERN_INFO
			    "scsi(%ld): Ignoring RSCN update to local host "
			    "port ID (%06x)\n",
			    ha->host_no, host_pid));
			break;
		}

		rscn_queue_index = ha->rscn_in_ptr + 1;
		if (rscn_queue_index == MAX_RSCN_COUNT)
			rscn_queue_index = 0;
		if (rscn_queue_index != ha->rscn_out_ptr) {
			ha->rscn_queue[ha->rscn_in_ptr] = rscn_entry;
			ha->rscn_in_ptr = rscn_queue_index;
		} else {
			ha->flags.rscn_queue_overflow = 1;
		}

		atomic_set(&ha->loop_state, LOOP_UPDATE);
		atomic_set(&ha->loop_down_timer, 0);
		ha->flags.management_server_logged_in = 0;

		set_bit(LOOP_RESYNC_NEEDED, &ha->dpc_flags);
		set_bit(RSCN_UPDATE, &ha->dpc_flags);

		/* Update AEN queue. */
		qla2x00_enqueue_aen(ha, MBA_RSCN_UPDATE, &mb[0]);
		break;

	/* case MBA_RIO_RESPONSE: */
	case MBA_ZIO_RESPONSE:
		DEBUG2(printk("scsi(%ld): [R|Z]IO update completion.\n",
		    ha->host_no));
		DEBUG(printk(KERN_INFO
		    "scsi(%ld): [R|Z]IO update completion.\n",
		    ha->host_no));

		if (IS_QLA24XX(ha) || IS_QLA25XX(ha))
			qla24xx_process_response_queue(ha);
		else
			qla2x00_process_response_queue(ha);
		break;

	case MBA_DISCARD_RND_FRAME:
		DEBUG2(printk("scsi(%ld): Discard RND Frame -- %04x %04x "
		    "%04x.\n", ha->host_no, mb[1], mb[2], mb[3]));
		break;
	}
}

/**
 * qla2x00_process_completed_request() - Process a Fast Post response.
 * @ha: SCSI driver HA context
 * @index: SRB index
 */
static void
qla2x00_process_completed_request(struct scsi_qla_host *ha, uint32_t index)
{
	srb_t *sp;

	/* Validate handle. */
	if (index >= MAX_OUTSTANDING_COMMANDS) {
		DEBUG2(printk("scsi(%ld): Invalid SCSI completion handle %d.\n",
		    ha->host_no, index));
		qla_printk(KERN_WARNING, ha,
		    "Invalid SCSI completion handle %d.\n", index);

		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		return;
	}

	sp = ha->outstanding_cmds[index];
	if (sp) {
		/* Free outstanding command slot. */
		ha->outstanding_cmds[index] = NULL;

		CMD_COMPL_STATUS(sp->cmd) = 0L;
		CMD_SCSI_STATUS(sp->cmd) = 0L;

		/* Save ISP completion status */
		sp->cmd->result = DID_OK << 16;
		qla2x00_sp_compl(ha, sp);
	} else {
		DEBUG2(printk("scsi(%ld): Invalid ISP SCSI completion handle\n",
		    ha->host_no));
		qla_printk(KERN_WARNING, ha,
		    "Invalid ISP SCSI completion handle\n");

		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
	}
}

/**
 * qla2x00_process_response_queue() - Process response queue entries.
 * @ha: SCSI driver HA context
 */
void
qla2x00_process_response_queue(struct scsi_qla_host *ha)
{
	struct device_reg_2xxx __iomem *reg = &ha->iobase->isp;
	sts_entry_t	*pkt;
	uint16_t        handle_cnt;
	uint16_t        cnt;

	if (!ha->flags.online)
		return;

	while (ha->response_ring_ptr->signature != RESPONSE_PROCESSED) {
		pkt = (sts_entry_t *)ha->response_ring_ptr;

		ha->rsp_ring_index++;
		if (ha->rsp_ring_index == ha->response_q_length) {
			ha->rsp_ring_index = 0;
			ha->response_ring_ptr = ha->response_ring;
		} else {
			ha->response_ring_ptr++;
		}

		if (pkt->entry_status != 0) {
			DEBUG3(printk(KERN_INFO
			    "scsi(%ld): Process error entry.\n", ha->host_no));

			qla2x00_error_entry(ha, pkt);
			((response_t *)pkt)->signature = RESPONSE_PROCESSED;
			wmb();
			continue;
		}

		switch (pkt->entry_type) {
		case STATUS_TYPE:
			qla2x00_status_entry(ha, pkt);
			break;
		case STATUS_TYPE_21:
			handle_cnt = ((sts21_entry_t *)pkt)->handle_count;
			for (cnt = 0; cnt < handle_cnt; cnt++) {
				qla2x00_process_completed_request(ha,
				    ((sts21_entry_t *)pkt)->handle[cnt]);
			}
			break;
		case STATUS_TYPE_22:
			handle_cnt = ((sts22_entry_t *)pkt)->handle_count;
			for (cnt = 0; cnt < handle_cnt; cnt++) {
				qla2x00_process_completed_request(ha,
				    ((sts22_entry_t *)pkt)->handle[cnt]);
			}
			break;
		case STATUS_CONT_TYPE:
			qla2x00_status_cont_entry(ha, (sts_cont_entry_t *)pkt);
			break;
		case MS_IOCB_TYPE:
			qla2x00_ms_entry(ha, (ms_iocb_entry_t *)pkt);
			break;
		case MBX_IOCB_TYPE:
			if (!IS_QLA2100(ha) && !IS_QLA2200(ha) &&
			    !IS_QLA6312(ha) && !IS_QLA6322(ha)) {
				if (pkt->sys_define == SOURCE_ASYNC_IOCB) {
					qla2x00_process_iodesc(ha,
					    (struct mbx_entry *)pkt);
				} else {
					/* MBX IOCB Type Not Supported. */
					DEBUG4(printk(KERN_WARNING
					    "scsi(%ld): Received unknown MBX "
					    "IOCB response pkt type=%x "
					    "source=%x entry status=%x.\n",
					    ha->host_no, pkt->entry_type,
					    pkt->sys_define,
					    pkt->entry_status));
				}
				break;
			}
			/* Fallthrough. */
		default:
			/* Type Not Supported. */
			DEBUG4(printk(KERN_WARNING
			    "scsi(%ld): Received unknown response pkt type %x "
			    "entry status=%x.\n",
			    ha->host_no, pkt->entry_type, pkt->entry_status));
			break;
		}
		((response_t *)pkt)->signature = RESPONSE_PROCESSED;
		wmb();
	}

	/* Adjust ring index */
	WRT_REG_WORD(ISP_RSP_Q_OUT(ha, reg), ha->rsp_ring_index);
}

/**
 * qla2x00_status_entry() - Process a Status IOCB entry.
 * @ha: SCSI driver HA context
 * @pkt: Entry pointer
 */
static void
qla2x00_status_entry(scsi_qla_host_t *ha, void *pkt)
{
	srb_t		*sp;
	fc_port_t	*fcport;
	struct scsi_cmnd *cp;
	sts_entry_t *sts;
	struct sts_entry_24xx *sts24;
	uint16_t	comp_status;
	uint16_t	scsi_status;
	uint8_t		lscsi_status;
	int32_t		resid;
	uint32_t	sense_len, rsp_info_len, resid_len;
	uint8_t		*rsp_info, *sense_data;

	sts = (sts_entry_t *) pkt;
	sts24 = (struct sts_entry_24xx *) pkt;
	if (IS_QLA24XX(ha) || IS_QLA25XX(ha)) {
		comp_status = le16_to_cpu(sts24->comp_status);
		scsi_status = le16_to_cpu(sts24->scsi_status) & SS_MASK;
	} else {
		comp_status = le16_to_cpu(sts->comp_status);
		scsi_status = le16_to_cpu(sts->scsi_status) & SS_MASK;
	}

	/* Fast path completion. */
	if (comp_status == CS_COMPLETE && scsi_status == 0) {
		qla2x00_process_completed_request(ha, sts->handle);

		return;
	}

	/* Validate handle. */
	if (sts->handle < MAX_OUTSTANDING_COMMANDS) {
		sp = ha->outstanding_cmds[sts->handle];
		ha->outstanding_cmds[sts->handle] = NULL;
	} else
		sp = NULL;

	if (sp == NULL) {
		DEBUG2(printk("scsi(%ld): Status Entry invalid handle.\n",
		    ha->host_no));
		qla_printk(KERN_WARNING, ha, "Status Entry invalid handle.\n");

		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		if (ha->dpc_wait && !ha->dpc_active)
			up(ha->dpc_wait);

		return;
	}
	cp = sp->cmd;
	if (cp == NULL) {
		DEBUG2(printk("scsi(%ld): Command already returned back to OS "
		    "pkt->handle=%d sp=%p sp->state:%d\n",
		    ha->host_no, sts->handle, sp, sp->state));
		qla_printk(KERN_WARNING, ha,
		    "Command is NULL: already returned to OS (sp=%p)\n", sp);

		return;
	}

  	lscsi_status = scsi_status & STATUS_MASK;
	CMD_ENTRY_STATUS(cp) = sts->entry_status;
	CMD_COMPL_STATUS(cp) = comp_status;
	CMD_SCSI_STATUS(cp) = scsi_status;

	fcport = sp->fcport;

	sense_len = rsp_info_len = resid_len = 0;
	if (IS_QLA24XX(ha) || IS_QLA25XX(ha)) {
		sense_len = le32_to_cpu(sts24->sense_len);
		rsp_info_len = le32_to_cpu(sts24->rsp_data_len);
		resid_len = le32_to_cpu(sts24->rsp_residual_count);
		rsp_info = sts24->data;
		sense_data = sts24->data;
		host_to_fcp_swap(sts24->data, sizeof(sts24->data));
	} else {
		sense_len = le16_to_cpu(sts->req_sense_length);
		rsp_info_len = le16_to_cpu(sts->rsp_info_len);
		resid_len = le32_to_cpu(sts->residual_length);
		rsp_info = sts->rsp_info;
		sense_data = sts->req_sense_data;
	}

	/* Check for any FCP transport errors. */
	if (scsi_status & SS_RESPONSE_INFO_LEN_VALID) {
		/* Sense data lies beyond any FCP RESPONSE data. */
		if (IS_QLA24XX(ha) || IS_QLA25XX(ha))
			sense_data += rsp_info_len;
		if (rsp_info_len > 3 && rsp_info[3]) {
			DEBUG2(printk("scsi(%ld:%d:%d:%d) FCP I/O protocol "
			    "failure (%x/%02x%02x%02x%02x%02x%02x%02x%02x)..."
			    "retrying command\n", ha->host_no,
			    cp->device->channel, cp->device->id,
			    cp->device->lun, rsp_info_len, rsp_info[0],
			    rsp_info[1], rsp_info[2], rsp_info[3], rsp_info[4],
			    rsp_info[5], rsp_info[6], rsp_info[7]));

			cp->result = DID_BUS_BUSY << 16;
			qla2x00_sp_compl(ha, sp);
			return;
		}
	}

	/*
	 * Based on Host and scsi status generate status code for Linux
	 */
	switch (comp_status) {
	case CS_COMPLETE:
		if (scsi_status == 0) {
			cp->result = DID_OK << 16;
			break;
		}
		if (scsi_status & (SS_RESIDUAL_UNDER | SS_RESIDUAL_OVER)) {
			resid = resid_len;
			cp->resid = resid;
			CMD_RESID_LEN(cp) = resid;

			if (!lscsi_status &&
			    ((unsigned)(cp->request_bufflen - resid) <
			     cp->underflow)) {
				qla_printk(KERN_INFO, ha,
				    "scsi(%ld:%d:%d:%d): Mid-layer underflow "
				    "detected (%x of %x bytes)...returning "
				    "error status.\n", ha->host_no,
				    cp->device->channel, cp->device->id,
				    cp->device->lun, resid,
				    cp->request_bufflen);

				cp->result = DID_ERROR << 16;
				break;
			}
		}
		cp->result = DID_OK << 16 | lscsi_status;

		if (lscsi_status != SS_CHECK_CONDITION)
			break;

		/* Copy Sense Data into sense buffer. */
		memset(cp->sense_buffer, 0, sizeof(cp->sense_buffer));

		if (!(scsi_status & SS_SENSE_LEN_VALID))
			break;

		if (sense_len >= sizeof(cp->sense_buffer))
			sense_len = sizeof(cp->sense_buffer);

		CMD_ACTUAL_SNSLEN(cp) = sense_len;
		sp->request_sense_length = sense_len;
		sp->request_sense_ptr = cp->sense_buffer;

		if (sp->request_sense_length > 32)
			sense_len = 32;

		memcpy(cp->sense_buffer, sense_data, sense_len);

		sp->request_sense_ptr += sense_len;
		sp->request_sense_length -= sense_len;
		if (sp->request_sense_length != 0)
			ha->status_srb = sp;

		DEBUG5(printk("%s(): Check condition Sense data, "
		    "scsi(%ld:%d:%d:%d) cmd=%p pid=%ld\n", __func__,
		    ha->host_no, cp->device->channel, cp->device->id,
		    cp->device->lun, cp, cp->serial_number));
		if (sense_len)
			DEBUG5(qla2x00_dump_buffer(cp->sense_buffer,
			    CMD_ACTUAL_SNSLEN(cp)));
		break;

	case CS_DATA_UNDERRUN:
		resid = resid_len;
		if (scsi_status & SS_RESIDUAL_UNDER) {
			cp->resid = resid;
			CMD_RESID_LEN(cp) = resid;
		} else {
			DEBUG2(printk(KERN_INFO
			    "scsi(%ld:%d:%d) UNDERRUN status detected "
			    "0x%x-0x%x.\n", ha->host_no, cp->device->id,
			    cp->device->lun, comp_status, scsi_status));

		}

		/*
		 * Check to see if SCSI Status is non zero. If so report SCSI
		 * Status.
		 */
		if (lscsi_status != 0) {
			cp->result = DID_OK << 16 | lscsi_status;

			if (lscsi_status != SS_CHECK_CONDITION)
				break;

			/* Copy Sense Data into sense buffer */
			memset(cp->sense_buffer, 0, sizeof(cp->sense_buffer));

			if (!(scsi_status & SS_SENSE_LEN_VALID))
				break;

			if (sense_len >= sizeof(cp->sense_buffer))
				sense_len = sizeof(cp->sense_buffer);

			CMD_ACTUAL_SNSLEN(cp) = sense_len;
			sp->request_sense_length = sense_len;
			sp->request_sense_ptr = cp->sense_buffer;

			if (sp->request_sense_length > 32)
				sense_len = 32;

			memcpy(cp->sense_buffer, sense_data, sense_len);

			sp->request_sense_ptr += sense_len;
			sp->request_sense_length -= sense_len;
			if (sp->request_sense_length != 0)
				ha->status_srb = sp;

			DEBUG5(printk("%s(): Check condition Sense data, "
			    "scsi(%ld:%d:%d:%d) cmd=%p pid=%ld\n",
			    __func__, ha->host_no, cp->device->channel,
			    cp->device->id, cp->device->lun, cp,
			    cp->serial_number));

			if (sense_len)
				DEBUG5(qla2x00_dump_buffer(cp->sense_buffer,
				    CMD_ACTUAL_SNSLEN(cp)));
		} else {
			/*
			 * If RISC reports underrun and target does not report
			 * it then we must have a lost frame, so tell upper
			 * layer to retry it by reporting a bus busy.
			 */
			if (!(scsi_status & SS_RESIDUAL_UNDER)) {
				DEBUG2(printk("scsi(%ld:%d:%d:%d) Dropped "
				    "frame(s) detected (%x of %x bytes)..."
				    "retrying command.\n", ha->host_no,
				    cp->device->channel, cp->device->id,
				    cp->device->lun, resid,
				    cp->request_bufflen));

				cp->result = DID_BUS_BUSY << 16;
				break;
			}

			/* Handle mid-layer underflow */
			if ((unsigned)(cp->request_bufflen - resid) <
			    cp->underflow) {
				qla_printk(KERN_INFO, ha,
				    "scsi(%ld:%d:%d:%d): Mid-layer underflow "
				    "detected (%x of %x bytes)...returning "
				    "error status.\n", ha->host_no,
				    cp->device->channel, cp->device->id,
				    cp->device->lun, resid,
				    cp->request_bufflen);

				cp->result = DID_ERROR << 16;
				break;
			}

			/* Everybody online, looking good... */
			cp->result = DID_OK << 16;
		}
		break;

	case CS_DATA_OVERRUN:
		DEBUG2(printk(KERN_INFO
		    "scsi(%ld:%d:%d): OVERRUN status detected 0x%x-0x%x\n",
		    ha->host_no, cp->device->id, cp->device->lun, comp_status,
		    scsi_status));
		DEBUG2(printk(KERN_INFO
		    "CDB: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
		    cp->cmnd[0], cp->cmnd[1], cp->cmnd[2], cp->cmnd[3],
		    cp->cmnd[4], cp->cmnd[5]));
		DEBUG2(printk(KERN_INFO
		    "PID=0x%lx req=0x%x xtra=0x%x -- returning DID_ERROR "
		    "status!\n",
		    cp->serial_number, cp->request_bufflen, resid_len));

		cp->result = DID_ERROR << 16;
		break;

	case CS_PORT_LOGGED_OUT:
	case CS_PORT_CONFIG_CHG:
	case CS_PORT_BUSY:
	case CS_INCOMPLETE:
	case CS_PORT_UNAVAILABLE:
		/*
		 * If the port is in Target Down state, return all IOs for this
		 * Target with DID_NO_CONNECT ELSE Queue the IOs in the
		 * retry_queue.
		 */
		DEBUG2(printk("scsi(%ld:%d:%d): status_entry: Port Down "
		    "pid=%ld, compl status=0x%x, port state=0x%x\n",
		    ha->host_no, cp->device->id, cp->device->lun,
		    cp->serial_number, comp_status,
		    atomic_read(&fcport->state)));

		cp->result = DID_BUS_BUSY << 16;
		if (atomic_read(&fcport->state) == FCS_ONLINE) {
			qla2x00_mark_device_lost(ha, fcport, 1, 1);
		}
		break;

	case CS_RESET:
		DEBUG2(printk(KERN_INFO
		    "scsi(%ld): RESET status detected 0x%x-0x%x.\n",
		    ha->host_no, comp_status, scsi_status));

		cp->result = DID_RESET << 16;
		break;

	case CS_ABORTED:
		/*
		 * hv2.19.12 - DID_ABORT does not retry the request if we
		 * aborted this request then abort otherwise it must be a
		 * reset.
		 */
		DEBUG2(printk(KERN_INFO
		    "scsi(%ld): ABORT status detected 0x%x-0x%x.\n",
		    ha->host_no, comp_status, scsi_status));

		cp->result = DID_RESET << 16;
		break;

	case CS_TIMEOUT:
		cp->result = DID_BUS_BUSY << 16;

		if (IS_QLA24XX(ha) || IS_QLA25XX(ha)) {
			DEBUG2(printk(KERN_INFO
			    "scsi(%ld:%d:%d:%d): TIMEOUT status detected "
			    "0x%x-0x%x\n", ha->host_no, cp->device->channel,
			    cp->device->id, cp->device->lun, comp_status,
			    scsi_status));
			break;
		}
		DEBUG2(printk(KERN_INFO
		    "scsi(%ld:%d:%d:%d): TIMEOUT status detected 0x%x-0x%x "
		    "sflags=%x.\n", ha->host_no, cp->device->channel,
		    cp->device->id, cp->device->lun, comp_status, scsi_status,
		    le16_to_cpu(sts->status_flags)));

		/* Check to see if logout occurred. */
		if ((le16_to_cpu(sts->status_flags) & SF_LOGOUT_SENT))
			qla2x00_mark_device_lost(ha, fcport, 1, 1);
		break;

	case CS_QUEUE_FULL:
		DEBUG2(printk(KERN_INFO
		    "scsi(%ld): QUEUE FULL status detected 0x%x-0x%x.\n",
		    ha->host_no, comp_status, scsi_status));

		/* SCSI Mid-Layer handles device queue full */

		cp->result = DID_OK << 16 | lscsi_status;

		break;

	default:
		DEBUG3(printk("scsi(%ld): Error detected (unknown status) "
		    "0x%x-0x%x.\n", ha->host_no, comp_status, scsi_status));
		qla_printk(KERN_INFO, ha,
		    "Unknown status detected 0x%x-0x%x.\n",
		    comp_status, scsi_status);

		cp->result = DID_ERROR << 16;
		break;
	}

	/* Place command on done queue. */
	if (ha->status_srb == NULL)
		qla2x00_sp_compl(ha, sp);
}

/**
 * qla2x00_status_cont_entry() - Process a Status Continuations entry.
 * @ha: SCSI driver HA context
 * @pkt: Entry pointer
 *
 * Extended sense data.
 */
static void
qla2x00_status_cont_entry(scsi_qla_host_t *ha, sts_cont_entry_t *pkt)
{
	uint8_t		sense_sz = 0;
	srb_t		*sp = ha->status_srb;
	struct scsi_cmnd *cp;

	if (sp != NULL && sp->request_sense_length != 0) {
		cp = sp->cmd;
		if (cp == NULL) {
			DEBUG2(printk("%s(): Cmd already returned back to OS "
			    "sp=%p sp->state:%d\n", __func__, sp, sp->state));
			qla_printk(KERN_INFO, ha,
			    "cmd is NULL: already returned to OS (sp=%p)\n",
			    sp);

			ha->status_srb = NULL;
			return;
		}

		if (sp->request_sense_length > sizeof(pkt->data)) {
			sense_sz = sizeof(pkt->data);
		} else {
			sense_sz = sp->request_sense_length;
		}

		/* Move sense data. */
		if (IS_QLA24XX(ha) || IS_QLA25XX(ha))
			host_to_fcp_swap(pkt->data, sizeof(pkt->data));
		memcpy(sp->request_sense_ptr, pkt->data, sense_sz);
		DEBUG5(qla2x00_dump_buffer(sp->request_sense_ptr, sense_sz));

		sp->request_sense_ptr += sense_sz;
		sp->request_sense_length -= sense_sz;

		/* Place command on done queue. */
		if (sp->request_sense_length == 0) {
			ha->status_srb = NULL;
			qla2x00_sp_compl(ha, sp);
		}
	}
}

/**
 * qla2x00_error_entry() - Process an error entry.
 * @ha: SCSI driver HA context
 * @pkt: Entry pointer
 */
static void
qla2x00_error_entry(scsi_qla_host_t *ha, sts_entry_t *pkt)
{
	srb_t *sp;

#if defined(QL_DEBUG_LEVEL_2)
	if (pkt->entry_status & RF_INV_E_ORDER)
		qla_printk(KERN_ERR, ha, "%s: Invalid Entry Order\n", __func__);
	else if (pkt->entry_status & RF_INV_E_COUNT)
		qla_printk(KERN_ERR, ha, "%s: Invalid Entry Count\n", __func__);
	else if (pkt->entry_status & RF_INV_E_PARAM)
		qla_printk(KERN_ERR, ha,
		    "%s: Invalid Entry Parameter\n", __func__);
	else if (pkt->entry_status & RF_INV_E_TYPE)
		qla_printk(KERN_ERR, ha, "%s: Invalid Entry Type\n", __func__);
	else if (pkt->entry_status & RF_BUSY)
		qla_printk(KERN_ERR, ha, "%s: Busy\n", __func__);
	else
		qla_printk(KERN_ERR, ha, "%s: UNKNOWN flag error\n", __func__);
#endif

	/* Validate handle. */
	if (pkt->handle < MAX_OUTSTANDING_COMMANDS)
		sp = ha->outstanding_cmds[pkt->handle];
	else
		sp = NULL;

	if (sp) {
		/* Free outstanding command slot. */
		ha->outstanding_cmds[pkt->handle] = NULL;

		/* Bad payload or header */
		if (pkt->entry_status &
		    (RF_INV_E_ORDER | RF_INV_E_COUNT |
		     RF_INV_E_PARAM | RF_INV_E_TYPE)) {
			sp->cmd->result = DID_ERROR << 16;
		} else if (pkt->entry_status & RF_BUSY) {
			sp->cmd->result = DID_BUS_BUSY << 16;
		} else {
			sp->cmd->result = DID_ERROR << 16;
		}
		qla2x00_sp_compl(ha, sp);

	} else if (pkt->entry_type == COMMAND_A64_TYPE || pkt->entry_type ==
	    COMMAND_TYPE || pkt->entry_type == COMMAND_TYPE_7) {
		DEBUG2(printk("scsi(%ld): Error entry - invalid handle\n",
		    ha->host_no));
		qla_printk(KERN_WARNING, ha,
		    "Error entry - invalid handle\n");

		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		if (ha->dpc_wait && !ha->dpc_active)
			up(ha->dpc_wait);
	}
}

/**
 * qla2x00_ms_entry() - Process a Management Server entry.
 * @ha: SCSI driver HA context
 * @index: Response queue out pointer
 */
static void
qla2x00_ms_entry(scsi_qla_host_t *ha, ms_iocb_entry_t *pkt)
{
	srb_t          *sp;

	DEBUG3(printk("%s(%ld): pkt=%p pkthandle=%d.\n",
	    __func__, ha->host_no, pkt, pkt->handle1));

	/* Validate handle. */
 	if (pkt->handle1 < MAX_OUTSTANDING_COMMANDS)
 		sp = ha->outstanding_cmds[pkt->handle1];
	else
		sp = NULL;

	if (sp == NULL) {
		DEBUG2(printk("scsi(%ld): MS entry - invalid handle\n",
		    ha->host_no));
		qla_printk(KERN_WARNING, ha, "MS entry - invalid handle\n");

		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		return;
	}

	CMD_COMPL_STATUS(sp->cmd) = le16_to_cpu(pkt->status);
	CMD_ENTRY_STATUS(sp->cmd) = pkt->entry_status;

	/* Free outstanding command slot. */
	ha->outstanding_cmds[pkt->handle1] = NULL;

	qla2x00_sp_compl(ha, sp);
}


/**
 * qla24xx_mbx_completion() - Process mailbox command completions.
 * @ha: SCSI driver HA context
 * @mb0: Mailbox0 register
 */
static void
qla24xx_mbx_completion(scsi_qla_host_t *ha, uint16_t mb0)
{
	uint16_t	cnt;
	uint16_t __iomem *wptr;
	struct device_reg_24xx __iomem *reg = &ha->iobase->isp24;

	/* Load return mailbox registers. */
	ha->flags.mbox_int = 1;
	ha->mailbox_out[0] = mb0;
	wptr = (uint16_t __iomem *)&reg->mailbox1;

	for (cnt = 1; cnt < ha->mbx_count; cnt++) {
		ha->mailbox_out[cnt] = RD_REG_WORD(wptr);
		wptr++;
	}

	if (ha->mcp) {
		DEBUG3(printk("%s(%ld): Got mailbox completion. cmd=%x.\n",
		    __func__, ha->host_no, ha->mcp->mb[0]));
	} else {
		DEBUG2_3(printk("%s(%ld): MBX pointer ERROR!\n",
		    __func__, ha->host_no));
	}
}

/**
 * qla24xx_process_response_queue() - Process response queue entries.
 * @ha: SCSI driver HA context
 */
void
qla24xx_process_response_queue(struct scsi_qla_host *ha)
{
	struct device_reg_24xx __iomem *reg = &ha->iobase->isp24;
	struct sts_entry_24xx *pkt;

	if (!ha->flags.online)
		return;

	while (ha->response_ring_ptr->signature != RESPONSE_PROCESSED) {
		pkt = (struct sts_entry_24xx *)ha->response_ring_ptr;

		ha->rsp_ring_index++;
		if (ha->rsp_ring_index == ha->response_q_length) {
			ha->rsp_ring_index = 0;
			ha->response_ring_ptr = ha->response_ring;
		} else {
			ha->response_ring_ptr++;
		}

		if (pkt->entry_status != 0) {
			DEBUG3(printk(KERN_INFO
			    "scsi(%ld): Process error entry.\n", ha->host_no));

			qla2x00_error_entry(ha, (sts_entry_t *) pkt);
			((response_t *)pkt)->signature = RESPONSE_PROCESSED;
			wmb();
			continue;
		}

		switch (pkt->entry_type) {
		case STATUS_TYPE:
			qla2x00_status_entry(ha, pkt);
			break;
		case STATUS_CONT_TYPE:
			qla2x00_status_cont_entry(ha, (sts_cont_entry_t *)pkt);
			break;
		case MS_IOCB_TYPE:
			qla24xx_ms_entry(ha, (struct ct_entry_24xx *)pkt);
			break;
		default:
			/* Type Not Supported. */
			DEBUG4(printk(KERN_WARNING
			    "scsi(%ld): Received unknown response pkt type %x "
			    "entry status=%x.\n",
			    ha->host_no, pkt->entry_type, pkt->entry_status));
			break;
		}
		((response_t *)pkt)->signature = RESPONSE_PROCESSED;
		wmb();
	}

	/* Adjust ring index */
	WRT_REG_DWORD(&reg->rsp_q_out, ha->rsp_ring_index);
}

/**
 * qla24xx_intr_handler() - Process interrupts for the ISP23xx and ISP63xx.
 * @irq:
 * @dev_id: SCSI driver HA context
 * @regs:
 *
 * Called by system whenever the host adapter generates an interrupt.
 *
 * Returns handled flag.
 */
irqreturn_t
qla24xx_intr_handler(int irq, void *dev_id, struct pt_regs *regs)
{
	scsi_qla_host_t	*ha;
	struct device_reg_24xx __iomem *reg;
	int		status;
	unsigned long	flags;
	unsigned long	iter;
	uint32_t	stat;
	uint32_t	hccr;
	uint16_t	mb[4];

	ha = (scsi_qla_host_t *) dev_id;
	if (!ha) {
		printk(KERN_INFO
		    "%s(): NULL host pointer\n", __func__);
		return IRQ_NONE;
	}

	reg = &ha->iobase->isp24;
	status = 0;

	spin_lock_irqsave(&ha->hardware_lock, flags);
	for (iter = 50; iter--; ) {
		stat = RD_REG_DWORD(&reg->host_status);
		if (stat & HSRX_RISC_PAUSED) {
			hccr = RD_REG_DWORD(&reg->hccr);

			qla_printk(KERN_INFO, ha, "RISC paused -- HCCR=%x, "
			    "Dumping firmware!\n", hccr);
			qla24xx_fw_dump(ha, 1);

			set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
			break;
		} else if ((stat & HSRX_RISC_INT) == 0)
			break;

		switch (stat & 0xff) {
		case 0x1:
		case 0x2:
		case 0x10:
		case 0x11:
			qla24xx_mbx_completion(ha, MSW(stat));
			status |= MBX_INTERRUPT;

			break;
		case 0x12:
			mb[0] = MSW(stat);
			mb[1] = RD_REG_WORD(&reg->mailbox1);
			mb[2] = RD_REG_WORD(&reg->mailbox2);
			mb[3] = RD_REG_WORD(&reg->mailbox3);
			qla2x00_async_event(ha, mb);
			break;
		case 0x13:
			qla24xx_process_response_queue(ha);
			break;
		default:
			DEBUG2(printk("scsi(%ld): Unrecognized interrupt type "
			    "(%d).\n",
			    ha->host_no, stat & 0xff));
			break;
		}
		WRT_REG_DWORD(&reg->hccr, HCCRX_CLR_RISC_INT);
		RD_REG_DWORD_RELAXED(&reg->hccr);
	}
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	if (test_bit(MBX_INTR_WAIT, &ha->mbx_cmd_flags) &&
	    (status & MBX_INTERRUPT) && ha->flags.mbox_int) {
		spin_lock_irqsave(&ha->mbx_reg_lock, flags);

		set_bit(MBX_INTERRUPT, &ha->mbx_cmd_flags);
		up(&ha->mbx_intr_sem);

		spin_unlock_irqrestore(&ha->mbx_reg_lock, flags);
	}

	return IRQ_HANDLED;
}

/**
 * qla24xx_ms_entry() - Process a Management Server entry.
 * @ha: SCSI driver HA context
 * @index: Response queue out pointer
 */
static void
qla24xx_ms_entry(scsi_qla_host_t *ha, struct ct_entry_24xx *pkt)
{
	srb_t          *sp;

	DEBUG3(printk("%s(%ld): pkt=%p pkthandle=%d.\n",
	    __func__, ha->host_no, pkt, pkt->handle));

	DEBUG9(printk("%s: ct pkt dump:\n", __func__);)
	DEBUG9(qla2x00_dump_buffer((void *)pkt, sizeof(struct ct_entry_24xx));)

	/* Validate handle. */
 	if (pkt->handle < MAX_OUTSTANDING_COMMANDS)
 		sp = ha->outstanding_cmds[pkt->handle];
	else
		sp = NULL;

	if (sp == NULL) {
		DEBUG2(printk("scsi(%ld): MS entry - invalid handle\n",
		    ha->host_no));
		DEBUG10(printk("scsi(%ld): MS entry - invalid handle\n",
		    ha->host_no));
		qla_printk(KERN_WARNING, ha, "MS entry - invalid handle %d\n",
		    pkt->handle);

		set_bit(ISP_ABORT_NEEDED, &ha->dpc_flags);
		return;
	}

	CMD_COMPL_STATUS(sp->cmd) = le16_to_cpu(pkt->comp_status);
	CMD_ENTRY_STATUS(sp->cmd) = pkt->entry_status;

	/* Free outstanding command slot. */
	ha->outstanding_cmds[pkt->handle] = NULL;

	qla2x00_sp_compl(ha, sp);
}

