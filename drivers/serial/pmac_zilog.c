/*
 * linux/drivers/serial/pmac_zilog.c
 * 
 * Driver for PowerMac Z85c30 based ESCC cell found in the
 * "macio" ASICs of various PowerMac models
 * 
 * Copyright (C) 2003 Ben. Herrenschmidt (benh@kernel.crashing.org)
 *
 * Derived from drivers/macintosh/macserial.c by Paul Mackerras
 * and drivers/serial/sunzilog.c by David S. Miller
 *
 * Hrm... actually, I ripped most of sunzilog (Thanks David !) and
 * adapted special tweaks needed for us. I don't think it's worth
 * merging back those though. The DMA code still has to get in
 * and once done, I expect that driver to remain fairly stable in
 * the long term, unless we change the driver model again...
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * 2004-08-06 Harald Welte <laforge@gnumonks.org>
 *	- Enable BREAK interrupt
 *	- Add support for sysreq
 *
 * TODO:   - Add DMA support
 *         - Defer port shutdown to a few seconds after close
 *         - maybe put something right into uap->clk_divisor
 */

#undef DEBUG
#undef DEBUG_HARD
#undef USE_CTRL_O_SYSRQ

#include <linux/config.h>
#include <linux/module.h>
#include <linux/tty.h>

#include <linux/tty_flip.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/slab.h>
#include <linux/adb.h>
#include <linux/pmu.h>
#include <linux/bitops.h>
#include <linux/sysrq.h>
#include <linux/mutex.h>
#include <asm/sections.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/prom.h>
#include <asm/machdep.h>
#include <asm/pmac_feature.h>
#include <asm/dbdma.h>
#include <asm/macio.h>

#if defined (CONFIG_SERIAL_PMACZILOG_CONSOLE) && defined(CONFIG_MAGIC_SYSRQ)
#define SUPPORT_SYSRQ
#endif

#include <linux/serial.h>
#include <linux/serial_core.h>

#include "pmac_zilog.h"

/* Not yet implemented */
#undef HAS_DBDMA

static char version[] __initdata = "pmac_zilog: 0.6 (Benjamin Herrenschmidt <benh@kernel.crashing.org>)";
MODULE_AUTHOR("Benjamin Herrenschmidt <benh@kernel.crashing.org>");
MODULE_DESCRIPTION("Driver for the PowerMac serial ports.");
MODULE_LICENSE("GPL");

#define PWRDBG(fmt, arg...)	printk(KERN_DEBUG fmt , ## arg)


/*
 * For the sake of early serial console, we can do a pre-probe
 * (optional) of the ports at rather early boot time.
 */
static struct uart_pmac_port	pmz_ports[MAX_ZS_PORTS];
static int			pmz_ports_count;
static DEFINE_MUTEX(pmz_irq_mutex);

static struct uart_driver pmz_uart_reg = {
	.owner		=	THIS_MODULE,
	.driver_name	=	"ttyS",
	.devfs_name	=	"tts/",
	.dev_name	=	"ttyS",
	.major		=	TTY_MAJOR,
};


/* 
 * Load all registers to reprogram the port
 * This function must only be called when the TX is not busy.  The UART
 * port lock must be held and local interrupts disabled.
 */
static void pmz_load_zsregs(struct uart_pmac_port *uap, u8 *regs)
{
	int i;

	if (ZS_IS_ASLEEP(uap))
		return;

	/* Let pending transmits finish.  */
	for (i = 0; i < 1000; i++) {
		unsigned char stat = read_zsreg(uap, R1);
		if (stat & ALL_SNT)
			break;
		udelay(100);
	}

	ZS_CLEARERR(uap);
	zssync(uap);
	ZS_CLEARFIFO(uap);
	zssync(uap);
	ZS_CLEARERR(uap);

	/* Disable all interrupts.  */
	write_zsreg(uap, R1,
		    regs[R1] & ~(RxINT_MASK | TxINT_ENAB | EXT_INT_ENAB));

	/* Set parity, sync config, stop bits, and clock divisor.  */
	write_zsreg(uap, R4, regs[R4]);

	/* Set misc. TX/RX control bits.  */
	write_zsreg(uap, R10, regs[R10]);

	/* Set TX/RX controls sans the enable bits.  */
       	write_zsreg(uap, R3, regs[R3] & ~RxENABLE);
       	write_zsreg(uap, R5, regs[R5] & ~TxENABLE);

	/* now set R7 "prime" on ESCC */
	write_zsreg(uap, R15, regs[R15] | EN85C30);
	write_zsreg(uap, R7, regs[R7P]);

	/* make sure we use R7 "non-prime" on ESCC */
	write_zsreg(uap, R15, regs[R15] & ~EN85C30);

	/* Synchronous mode config.  */
	write_zsreg(uap, R6, regs[R6]);
	write_zsreg(uap, R7, regs[R7]);

	/* Disable baud generator.  */
	write_zsreg(uap, R14, regs[R14] & ~BRENAB);

	/* Clock mode control.  */
	write_zsreg(uap, R11, regs[R11]);

	/* Lower and upper byte of baud rate generator divisor.  */
	write_zsreg(uap, R12, regs[R12]);
	write_zsreg(uap, R13, regs[R13]);
	
	/* Now rewrite R14, with BRENAB (if set).  */
	write_zsreg(uap, R14, regs[R14]);

	/* Reset external status interrupts.  */
	write_zsreg(uap, R0, RES_EXT_INT);
	write_zsreg(uap, R0, RES_EXT_INT);

	/* Rewrite R3/R5, this time without enables masked.  */
	write_zsreg(uap, R3, regs[R3]);
	write_zsreg(uap, R5, regs[R5]);

	/* Rewrite R1, this time without IRQ enabled masked.  */
	write_zsreg(uap, R1, regs[R1]);

	/* Enable interrupts */
	write_zsreg(uap, R9, regs[R9]);
}

/* 
 * We do like sunzilog to avoid disrupting pending Tx
 * Reprogram the Zilog channel HW registers with the copies found in the
 * software state struct.  If the transmitter is busy, we defer this update
 * until the next TX complete interrupt.  Else, we do it right now.
 *
 * The UART port lock must be held and local interrupts disabled.
 */
static void pmz_maybe_update_regs(struct uart_pmac_port *uap)
{
       	if (!ZS_REGS_HELD(uap)) {
		if (ZS_TX_ACTIVE(uap)) {
			uap->flags |= PMACZILOG_FLAG_REGS_HELD;
		} else {
			pmz_debug("pmz: maybe_update_regs: updating\n");
			pmz_load_zsregs(uap, uap->curregs);
		}
	}
}

static struct tty_struct *pmz_receive_chars(struct uart_pmac_port *uap,
					    struct pt_regs *regs)
{
	struct tty_struct *tty = NULL;
	unsigned char ch, r1, drop, error, flag;
	int loops = 0;

	/* The interrupt can be enabled when the port isn't open, typically
	 * that happens when using one port is open and the other closed (stale
	 * interrupt) or when one port is used as a console.
	 */
	if (!ZS_IS_OPEN(uap)) {
		pmz_debug("pmz: draining input\n");
		/* Port is closed, drain input data */
		for (;;) {
			if ((++loops) > 1000)
				goto flood;
			(void)read_zsreg(uap, R1);
			write_zsreg(uap, R0, ERR_RES);
			(void)read_zsdata(uap);
			ch = read_zsreg(uap, R0);
			if (!(ch & Rx_CH_AV))
				break;
		}
		return NULL;
	}

	/* Sanity check, make sure the old bug is no longer happening */
	if (uap->port.info == NULL || uap->port.info->tty == NULL) {
		WARN_ON(1);
		(void)read_zsdata(uap);
		return NULL;
	}
	tty = uap->port.info->tty;

	while (1) {
		error = 0;
		drop = 0;

		r1 = read_zsreg(uap, R1);
		ch = read_zsdata(uap);

		if (r1 & (PAR_ERR | Rx_OVR | CRC_ERR)) {
			write_zsreg(uap, R0, ERR_RES);
			zssync(uap);
		}

		ch &= uap->parity_mask;
		if (ch == 0 && uap->flags & PMACZILOG_FLAG_BREAK) {
			uap->flags &= ~PMACZILOG_FLAG_BREAK;
		}

#if defined(CONFIG_MAGIC_SYSRQ) && defined(CONFIG_SERIAL_CORE_CONSOLE)
#ifdef USE_CTRL_O_SYSRQ
		/* Handle the SysRq ^O Hack */
		if (ch == '\x0f') {
			uap->port.sysrq = jiffies + HZ*5;
			goto next_char;
		}
#endif /* USE_CTRL_O_SYSRQ */
		if (uap->port.sysrq) {
			int swallow;
			spin_unlock(&uap->port.lock);
			swallow = uart_handle_sysrq_char(&uap->port, ch, regs);
			spin_lock(&uap->port.lock);
			if (swallow)
				goto next_char;
 		}
#endif /* CONFIG_MAGIC_SYSRQ && CONFIG_SERIAL_CORE_CONSOLE */

		/* A real serial line, record the character and status.  */
		if (drop)
			goto next_char;

		flag = TTY_NORMAL;
		uap->port.icount.rx++;

		if (r1 & (PAR_ERR | Rx_OVR | CRC_ERR | BRK_ABRT)) {
			error = 1;
			if (r1 & BRK_ABRT) {
				pmz_debug("pmz: got break !\n");
				r1 &= ~(PAR_ERR | CRC_ERR);
				uap->port.icount.brk++;
				if (uart_handle_break(&uap->port))
					goto next_char;
			}
			else if (r1 & PAR_ERR)
				uap->port.icount.parity++;
			else if (r1 & CRC_ERR)
				uap->port.icount.frame++;
			if (r1 & Rx_OVR)
				uap->port.icount.overrun++;
			r1 &= uap->port.read_status_mask;
			if (r1 & BRK_ABRT)
				flag = TTY_BREAK;
			else if (r1 & PAR_ERR)
				flag = TTY_PARITY;
			else if (r1 & CRC_ERR)
				flag = TTY_FRAME;
		}

		if (uap->port.ignore_status_mask == 0xff ||
		    (r1 & uap->port.ignore_status_mask) == 0) {
		    	tty_insert_flip_char(tty, ch, flag);
		}
		if (r1 & Rx_OVR)
			tty_insert_flip_char(tty, 0, TTY_OVERRUN);
	next_char:
		/* We can get stuck in an infinite loop getting char 0 when the
		 * line is in a wrong HW state, we break that here.
		 * When that happens, I disable the receive side of the driver.
		 * Note that what I've been experiencing is a real irq loop where
		 * I'm getting flooded regardless of the actual port speed.
		 * Something stange is going on with the HW
		 */
		if ((++loops) > 1000)
			goto flood;
		ch = read_zsreg(uap, R0);
		if (!(ch & Rx_CH_AV))
			break;
	}

	return tty;
 flood:
	uap->curregs[R1] &= ~(EXT_INT_ENAB | TxINT_ENAB | RxINT_MASK);
	write_zsreg(uap, R1, uap->curregs[R1]);
	zssync(uap);
	dev_err(&uap->dev->ofdev.dev, "pmz: rx irq flood !\n");
	return tty;
}

static void pmz_status_handle(struct uart_pmac_port *uap, struct pt_regs *regs)
{
	unsigned char status;

	status = read_zsreg(uap, R0);
	write_zsreg(uap, R0, RES_EXT_INT);
	zssync(uap);

	if (ZS_IS_OPEN(uap) && ZS_WANTS_MODEM_STATUS(uap)) {
		if (status & SYNC_HUNT)
			uap->port.icount.dsr++;

		/* The Zilog just gives us an interrupt when DCD/CTS/etc. change.
		 * But it does not tell us which bit has changed, we have to keep
		 * track of this ourselves.
		 * The CTS input is inverted for some reason.  -- paulus
		 */
		if ((status ^ uap->prev_status) & DCD)
			uart_handle_dcd_change(&uap->port,
					       (status & DCD));
		if ((status ^ uap->prev_status) & CTS)
			uart_handle_cts_change(&uap->port,
					       !(status & CTS));

		wake_up_interruptible(&uap->port.info->delta_msr_wait);
	}

	if (status & BRK_ABRT)
		uap->flags |= PMACZILOG_FLAG_BREAK;

	uap->prev_status = status;
}

static void pmz_transmit_chars(struct uart_pmac_port *uap)
{
	struct circ_buf *xmit;

	if (ZS_IS_ASLEEP(uap))
		return;
	if (ZS_IS_CONS(uap)) {
		unsigned char status = read_zsreg(uap, R0);

		/* TX still busy?  Just wait for the next TX done interrupt.
		 *
		 * It can occur because of how we do serial console writes.  It would
		 * be nice to transmit console writes just like we normally would for
		 * a TTY line. (ie. buffered and TX interrupt driven).  That is not
		 * easy because console writes cannot sleep.  One solution might be
		 * to poll on enough port->xmit space becomming free.  -DaveM
		 */
		if (!(status & Tx_BUF_EMP))
			return;
	}

	uap->flags &= ~PMACZILOG_FLAG_TX_ACTIVE;

	if (ZS_REGS_HELD(uap)) {
		pmz_load_zsregs(uap, uap->curregs);
		uap->flags &= ~PMACZILOG_FLAG_REGS_HELD;
	}

	if (ZS_TX_STOPPED(uap)) {
		uap->flags &= ~PMACZILOG_FLAG_TX_STOPPED;
		goto ack_tx_int;
	}

	if (uap->port.x_char) {
		uap->flags |= PMACZILOG_FLAG_TX_ACTIVE;
		write_zsdata(uap, uap->port.x_char);
		zssync(uap);
		uap->port.icount.tx++;
		uap->port.x_char = 0;
		return;
	}

	if (uap->port.info == NULL)
		goto ack_tx_int;
	xmit = &uap->port.info->xmit;
	if (uart_circ_empty(xmit)) {
		uart_write_wakeup(&uap->port);
		goto ack_tx_int;
	}
	if (uart_tx_stopped(&uap->port))
		goto ack_tx_int;

	uap->flags |= PMACZILOG_FLAG_TX_ACTIVE;
	write_zsdata(uap, xmit->buf[xmit->tail]);
	zssync(uap);

	xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
	uap->port.icount.tx++;

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(&uap->port);

	return;

ack_tx_int:
	write_zsreg(uap, R0, RES_Tx_P);
	zssync(uap);
}

/* Hrm... we register that twice, fixme later.... */
static irqreturn_t pmz_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct uart_pmac_port *uap = dev_id;
	struct uart_pmac_port *uap_a;
	struct uart_pmac_port *uap_b;
	int rc = IRQ_NONE;
	struct tty_struct *tty;
	u8 r3;

	uap_a = pmz_get_port_A(uap);
	uap_b = uap_a->mate;
       
       	spin_lock(&uap_a->port.lock);
	r3 = read_zsreg(uap_a, R3);

#ifdef DEBUG_HARD
	pmz_debug("irq, r3: %x\n", r3);
#endif
       	/* Channel A */
	tty = NULL;
       	if (r3 & (CHAEXT | CHATxIP | CHARxIP)) {
		write_zsreg(uap_a, R0, RES_H_IUS);
		zssync(uap_a);		
       		if (r3 & CHAEXT)
       			pmz_status_handle(uap_a, regs);
		if (r3 & CHARxIP)
			tty = pmz_receive_chars(uap_a, regs);
       		if (r3 & CHATxIP)
       			pmz_transmit_chars(uap_a);
	        rc = IRQ_HANDLED;
       	}
       	spin_unlock(&uap_a->port.lock);
	if (tty != NULL)
		tty_flip_buffer_push(tty);

	if (uap_b->node == NULL)
		goto out;

       	spin_lock(&uap_b->port.lock);
	tty = NULL;
	if (r3 & (CHBEXT | CHBTxIP | CHBRxIP)) {
		write_zsreg(uap_b, R0, RES_H_IUS);
		zssync(uap_b);
       		if (r3 & CHBEXT)
       			pmz_status_handle(uap_b, regs);
       	       	if (r3 & CHBRxIP)
       			tty = pmz_receive_chars(uap_b, regs);
       		if (r3 & CHBTxIP)
       			pmz_transmit_chars(uap_b);
	       	rc = IRQ_HANDLED;
       	}
       	spin_unlock(&uap_b->port.lock);
	if (tty != NULL)
		tty_flip_buffer_push(tty);

 out:
#ifdef DEBUG_HARD
	pmz_debug("irq done.\n");
#endif
	return rc;
}

/*
 * Peek the status register, lock not held by caller
 */
static inline u8 pmz_peek_status(struct uart_pmac_port *uap)
{
	unsigned long flags;
	u8 status;
	
	spin_lock_irqsave(&uap->port.lock, flags);
	status = read_zsreg(uap, R0);
	spin_unlock_irqrestore(&uap->port.lock, flags);

	return status;
}

/* 
 * Check if transmitter is empty
 * The port lock is not held.
 */
static unsigned int pmz_tx_empty(struct uart_port *port)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned char status;

	if (ZS_IS_ASLEEP(uap) || uap->node == NULL)
		return TIOCSER_TEMT;

	status = pmz_peek_status(to_pmz(port));
	if (status & Tx_BUF_EMP)
		return TIOCSER_TEMT;
	return 0;
}

/* 
 * Set Modem Control (RTS & DTR) bits
 * The port lock is held and interrupts are disabled.
 * Note: Shall we really filter out RTS on external ports or
 * should that be dealt at higher level only ?
 */
static void pmz_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned char set_bits, clear_bits;

        /* Do nothing for irda for now... */
	if (ZS_IS_IRDA(uap))
		return;
	/* We get called during boot with a port not up yet */
	if (ZS_IS_ASLEEP(uap) ||
	    !(ZS_IS_OPEN(uap) || ZS_IS_CONS(uap)))
		return;

	set_bits = clear_bits = 0;

	if (ZS_IS_INTMODEM(uap)) {
		if (mctrl & TIOCM_RTS)
			set_bits |= RTS;
		else
			clear_bits |= RTS;
	}
	if (mctrl & TIOCM_DTR)
		set_bits |= DTR;
	else
		clear_bits |= DTR;

	/* NOTE: Not subject to 'transmitter active' rule.  */ 
	uap->curregs[R5] |= set_bits;
	uap->curregs[R5] &= ~clear_bits;
	if (ZS_IS_ASLEEP(uap))
		return;
	write_zsreg(uap, R5, uap->curregs[R5]);
	pmz_debug("pmz_set_mctrl: set bits: %x, clear bits: %x -> %x\n",
		  set_bits, clear_bits, uap->curregs[R5]);
	zssync(uap);
}

/* 
 * Get Modem Control bits (only the input ones, the core will
 * or that with a cached value of the control ones)
 * The port lock is held and interrupts are disabled.
 */
static unsigned int pmz_get_mctrl(struct uart_port *port)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned char status;
	unsigned int ret;

	if (ZS_IS_ASLEEP(uap) || uap->node == NULL)
		return 0;

	status = read_zsreg(uap, R0);

	ret = 0;
	if (status & DCD)
		ret |= TIOCM_CAR;
	if (status & SYNC_HUNT)
		ret |= TIOCM_DSR;
	if (!(status & CTS))
		ret |= TIOCM_CTS;

	return ret;
}

/* 
 * Stop TX side. Dealt like sunzilog at next Tx interrupt,
 * though for DMA, we will have to do a bit more.
 * The port lock is held and interrupts are disabled.
 */
static void pmz_stop_tx(struct uart_port *port)
{
	to_pmz(port)->flags |= PMACZILOG_FLAG_TX_STOPPED;
}

/* 
 * Kick the Tx side.
 * The port lock is held and interrupts are disabled.
 */
static void pmz_start_tx(struct uart_port *port)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned char status;

	pmz_debug("pmz: start_tx()\n");

	uap->flags |= PMACZILOG_FLAG_TX_ACTIVE;
	uap->flags &= ~PMACZILOG_FLAG_TX_STOPPED;

	if (ZS_IS_ASLEEP(uap) || uap->node == NULL)
		return;

	status = read_zsreg(uap, R0);

	/* TX busy?  Just wait for the TX done interrupt.  */
	if (!(status & Tx_BUF_EMP))
		return;

	/* Send the first character to jump-start the TX done
	 * IRQ sending engine.
	 */
	if (port->x_char) {
		write_zsdata(uap, port->x_char);
		zssync(uap);
		port->icount.tx++;
		port->x_char = 0;
	} else {
		struct circ_buf *xmit = &port->info->xmit;

		write_zsdata(uap, xmit->buf[xmit->tail]);
		zssync(uap);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		port->icount.tx++;

		if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
			uart_write_wakeup(&uap->port);
	}
	pmz_debug("pmz: start_tx() done.\n");
}

/* 
 * Stop Rx side, basically disable emitting of
 * Rx interrupts on the port. We don't disable the rx
 * side of the chip proper though
 * The port lock is held.
 */
static void pmz_stop_rx(struct uart_port *port)
{
	struct uart_pmac_port *uap = to_pmz(port);

	if (ZS_IS_ASLEEP(uap) || uap->node == NULL)
		return;

	pmz_debug("pmz: stop_rx()()\n");

	/* Disable all RX interrupts.  */
	uap->curregs[R1] &= ~RxINT_MASK;
	pmz_maybe_update_regs(uap);

	pmz_debug("pmz: stop_rx() done.\n");
}

/* 
 * Enable modem status change interrupts
 * The port lock is held.
 */
static void pmz_enable_ms(struct uart_port *port)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned char new_reg;

	if (ZS_IS_IRDA(uap) || uap->node == NULL)
		return;
	new_reg = uap->curregs[R15] | (DCDIE | SYNCIE | CTSIE);
	if (new_reg != uap->curregs[R15]) {
		uap->curregs[R15] = new_reg;

		if (ZS_IS_ASLEEP(uap))
			return;
		/* NOTE: Not subject to 'transmitter active' rule.  */ 
		write_zsreg(uap, R15, uap->curregs[R15]);
	}
}

/* 
 * Control break state emission
 * The port lock is not held.
 */
static void pmz_break_ctl(struct uart_port *port, int break_state)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned char set_bits, clear_bits, new_reg;
	unsigned long flags;

	if (uap->node == NULL)
		return;
	set_bits = clear_bits = 0;

	if (break_state)
		set_bits |= SND_BRK;
	else
		clear_bits |= SND_BRK;

	spin_lock_irqsave(&port->lock, flags);

	new_reg = (uap->curregs[R5] | set_bits) & ~clear_bits;
	if (new_reg != uap->curregs[R5]) {
		uap->curregs[R5] = new_reg;

		/* NOTE: Not subject to 'transmitter active' rule.  */ 
		if (ZS_IS_ASLEEP(uap))
			return;
		write_zsreg(uap, R5, uap->curregs[R5]);
	}

	spin_unlock_irqrestore(&port->lock, flags);
}

/*
 * Turn power on or off to the SCC and associated stuff
 * (port drivers, modem, IR port, etc.)
 * Returns the number of milliseconds we should wait before
 * trying to use the port.
 */
static int pmz_set_scc_power(struct uart_pmac_port *uap, int state)
{
	int delay = 0;
	int rc;

	if (state) {
		rc = pmac_call_feature(
			PMAC_FTR_SCC_ENABLE, uap->node, uap->port_type, 1);
		pmz_debug("port power on result: %d\n", rc);
		if (ZS_IS_INTMODEM(uap)) {
			rc = pmac_call_feature(
				PMAC_FTR_MODEM_ENABLE, uap->node, 0, 1);
			delay = 2500;	/* wait for 2.5s before using */
			pmz_debug("modem power result: %d\n", rc);
		}
	} else {
		/* TODO: Make that depend on a timer, don't power down
		 * immediately
		 */
		if (ZS_IS_INTMODEM(uap)) {
			rc = pmac_call_feature(
				PMAC_FTR_MODEM_ENABLE, uap->node, 0, 0);
			pmz_debug("port power off result: %d\n", rc);
		}
		pmac_call_feature(PMAC_FTR_SCC_ENABLE, uap->node, uap->port_type, 0);
	}
	return delay;
}

/*
 * FixZeroBug....Works around a bug in the SCC receving channel.
 * Inspired from Darwin code, 15 Sept. 2000  -DanM
 *
 * The following sequence prevents a problem that is seen with O'Hare ASICs
 * (most versions -- also with some Heathrow and Hydra ASICs) where a zero
 * at the input to the receiver becomes 'stuck' and locks up the receiver.
 * This problem can occur as a result of a zero bit at the receiver input
 * coincident with any of the following events:
 *
 *	The SCC is initialized (hardware or software).
 *	A framing error is detected.
 *	The clocking option changes from synchronous or X1 asynchronous
 *		clocking to X16, X32, or X64 asynchronous clocking.
 *	The decoding mode is changed among NRZ, NRZI, FM0, or FM1.
 *
 * This workaround attempts to recover from the lockup condition by placing
 * the SCC in synchronous loopback mode with a fast clock before programming
 * any of the asynchronous modes.
 */
static void pmz_fix_zero_bug_scc(struct uart_pmac_port *uap)
{
	write_zsreg(uap, 9, ZS_IS_CHANNEL_A(uap) ? CHRA : CHRB);
	zssync(uap);
	udelay(10);
	write_zsreg(uap, 9, (ZS_IS_CHANNEL_A(uap) ? CHRA : CHRB) | NV);
	zssync(uap);

	write_zsreg(uap, 4, X1CLK | MONSYNC);
	write_zsreg(uap, 3, Rx8);
	write_zsreg(uap, 5, Tx8 | RTS);
	write_zsreg(uap, 9, NV);	/* Didn't we already do this? */
	write_zsreg(uap, 11, RCBR | TCBR);
	write_zsreg(uap, 12, 0);
	write_zsreg(uap, 13, 0);
	write_zsreg(uap, 14, (LOOPBAK | BRSRC));
	write_zsreg(uap, 14, (LOOPBAK | BRSRC | BRENAB));
	write_zsreg(uap, 3, Rx8 | RxENABLE);
	write_zsreg(uap, 0, RES_EXT_INT);
	write_zsreg(uap, 0, RES_EXT_INT);
	write_zsreg(uap, 0, RES_EXT_INT);	/* to kill some time */

	/* The channel should be OK now, but it is probably receiving
	 * loopback garbage.
	 * Switch to asynchronous mode, disable the receiver,
	 * and discard everything in the receive buffer.
	 */
	write_zsreg(uap, 9, NV);
	write_zsreg(uap, 4, X16CLK | SB_MASK);
	write_zsreg(uap, 3, Rx8);

	while (read_zsreg(uap, 0) & Rx_CH_AV) {
		(void)read_zsreg(uap, 8);
		write_zsreg(uap, 0, RES_EXT_INT);
		write_zsreg(uap, 0, ERR_RES);
	}
}

/*
 * Real startup routine, powers up the hardware and sets up
 * the SCC. Returns a delay in ms where you need to wait before
 * actually using the port, this is typically the internal modem
 * powerup delay. This routine expect the lock to be taken.
 */
static int __pmz_startup(struct uart_pmac_port *uap)
{
	int pwr_delay = 0;

	memset(&uap->curregs, 0, sizeof(uap->curregs));

	/* Power up the SCC & underlying hardware (modem/irda) */
	pwr_delay = pmz_set_scc_power(uap, 1);

	/* Nice buggy HW ... */
	pmz_fix_zero_bug_scc(uap);

	/* Reset the channel */
	uap->curregs[R9] = 0;
	write_zsreg(uap, 9, ZS_IS_CHANNEL_A(uap) ? CHRA : CHRB);
	zssync(uap);
	udelay(10);
	write_zsreg(uap, 9, 0);
	zssync(uap);

	/* Clear the interrupt registers */
	write_zsreg(uap, R1, 0);
	write_zsreg(uap, R0, ERR_RES);
	write_zsreg(uap, R0, ERR_RES);
	write_zsreg(uap, R0, RES_H_IUS);
	write_zsreg(uap, R0, RES_H_IUS);

	/* Setup some valid baud rate */
	uap->curregs[R4] = X16CLK | SB1;
	uap->curregs[R3] = Rx8;
	uap->curregs[R5] = Tx8 | RTS;
	if (!ZS_IS_IRDA(uap))
		uap->curregs[R5] |= DTR;
	uap->curregs[R12] = 0;
	uap->curregs[R13] = 0;
	uap->curregs[R14] = BRENAB;

	/* Clear handshaking, enable BREAK interrupts */
	uap->curregs[R15] = BRKIE;

	/* Master interrupt enable */
	uap->curregs[R9] |= NV | MIE;

	pmz_load_zsregs(uap, uap->curregs);

	/* Enable receiver and transmitter.  */
	write_zsreg(uap, R3, uap->curregs[R3] |= RxENABLE);
	write_zsreg(uap, R5, uap->curregs[R5] |= TxENABLE);

	/* Remember status for DCD/CTS changes */
	uap->prev_status = read_zsreg(uap, R0);


	return pwr_delay;
}

static void pmz_irda_reset(struct uart_pmac_port *uap)
{
	uap->curregs[R5] |= DTR;
	write_zsreg(uap, R5, uap->curregs[R5]);
	zssync(uap);
	mdelay(110);
	uap->curregs[R5] &= ~DTR;
	write_zsreg(uap, R5, uap->curregs[R5]);
	zssync(uap);
	mdelay(10);
}

/*
 * This is the "normal" startup routine, using the above one
 * wrapped with the lock and doing a schedule delay
 */
static int pmz_startup(struct uart_port *port)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned long flags;
	int pwr_delay = 0;

	pmz_debug("pmz: startup()\n");

	if (ZS_IS_ASLEEP(uap))
		return -EAGAIN;
	if (uap->node == NULL)
		return -ENODEV;

	mutex_lock(&pmz_irq_mutex);

	uap->flags |= PMACZILOG_FLAG_IS_OPEN;

	/* A console is never powered down. Else, power up and
	 * initialize the chip
	 */
	if (!ZS_IS_CONS(uap)) {
		spin_lock_irqsave(&port->lock, flags);
		pwr_delay = __pmz_startup(uap);
		spin_unlock_irqrestore(&port->lock, flags);
	}	

	pmz_get_port_A(uap)->flags |= PMACZILOG_FLAG_IS_IRQ_ON;
	if (request_irq(uap->port.irq, pmz_interrupt, SA_SHIRQ, "PowerMac Zilog", uap)) {
		dev_err(&uap->dev->ofdev.dev,
			"Unable to register zs interrupt handler.\n");
		pmz_set_scc_power(uap, 0);
		mutex_unlock(&pmz_irq_mutex);
		return -ENXIO;
	}

	mutex_unlock(&pmz_irq_mutex);

	/* Right now, we deal with delay by blocking here, I'll be
	 * smarter later on
	 */
	if (pwr_delay != 0) {
		pmz_debug("pmz: delaying %d ms\n", pwr_delay);
		msleep(pwr_delay);
	}

	/* IrDA reset is done now */
	if (ZS_IS_IRDA(uap))
		pmz_irda_reset(uap);

	/* Enable interrupts emission from the chip */
	spin_lock_irqsave(&port->lock, flags);
	uap->curregs[R1] |= INT_ALL_Rx | TxINT_ENAB;
	if (!ZS_IS_EXTCLK(uap))
		uap->curregs[R1] |= EXT_INT_ENAB;
	write_zsreg(uap, R1, uap->curregs[R1]);
       	spin_unlock_irqrestore(&port->lock, flags);

	pmz_debug("pmz: startup() done.\n");

	return 0;
}

static void pmz_shutdown(struct uart_port *port)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned long flags;

	pmz_debug("pmz: shutdown()\n");

	if (uap->node == NULL)
		return;

	mutex_lock(&pmz_irq_mutex);

	/* Release interrupt handler */
       	free_irq(uap->port.irq, uap);

	spin_lock_irqsave(&port->lock, flags);

	uap->flags &= ~PMACZILOG_FLAG_IS_OPEN;

	if (!ZS_IS_OPEN(uap->mate))
		pmz_get_port_A(uap)->flags &= ~PMACZILOG_FLAG_IS_IRQ_ON;

	/* Disable interrupts */
	if (!ZS_IS_ASLEEP(uap)) {
		uap->curregs[R1] &= ~(EXT_INT_ENAB | TxINT_ENAB | RxINT_MASK);
		write_zsreg(uap, R1, uap->curregs[R1]);
		zssync(uap);
	}

	if (ZS_IS_CONS(uap) || ZS_IS_ASLEEP(uap)) {
		spin_unlock_irqrestore(&port->lock, flags);
		mutex_unlock(&pmz_irq_mutex);
		return;
	}

	/* Disable receiver and transmitter.  */
	uap->curregs[R3] &= ~RxENABLE;
	uap->curregs[R5] &= ~TxENABLE;

	/* Disable all interrupts and BRK assertion.  */
	uap->curregs[R5] &= ~SND_BRK;
	pmz_maybe_update_regs(uap);

	/* Shut the chip down */
	pmz_set_scc_power(uap, 0);

	spin_unlock_irqrestore(&port->lock, flags);

	mutex_unlock(&pmz_irq_mutex);

	pmz_debug("pmz: shutdown() done.\n");
}

/* Shared by TTY driver and serial console setup.  The port lock is held
 * and local interrupts are disabled.
 */
static void pmz_convert_to_zs(struct uart_pmac_port *uap, unsigned int cflag,
			      unsigned int iflag, unsigned long baud)
{
	int brg;


	/* Switch to external clocking for IrDA high clock rates. That
	 * code could be re-used for Midi interfaces with different
	 * multipliers
	 */
	if (baud >= 115200 && ZS_IS_IRDA(uap)) {
		uap->curregs[R4] = X1CLK;
		uap->curregs[R11] = RCTRxCP | TCTRxCP;
		uap->curregs[R14] = 0; /* BRG off */
		uap->curregs[R12] = 0;
		uap->curregs[R13] = 0;
		uap->flags |= PMACZILOG_FLAG_IS_EXTCLK;
	} else {
		switch (baud) {
		case ZS_CLOCK/16:	/* 230400 */
			uap->curregs[R4] = X16CLK;
			uap->curregs[R11] = 0;
			uap->curregs[R14] = 0;
			break;
		case ZS_CLOCK/32:	/* 115200 */
			uap->curregs[R4] = X32CLK;
			uap->curregs[R11] = 0;
			uap->curregs[R14] = 0;
			break;
		default:
			uap->curregs[R4] = X16CLK;
			uap->curregs[R11] = TCBR | RCBR;
			brg = BPS_TO_BRG(baud, ZS_CLOCK / 16);
			uap->curregs[R12] = (brg & 255);
			uap->curregs[R13] = ((brg >> 8) & 255);
			uap->curregs[R14] = BRENAB;
		}
		uap->flags &= ~PMACZILOG_FLAG_IS_EXTCLK;
	}

	/* Character size, stop bits, and parity. */
	uap->curregs[3] &= ~RxN_MASK;
	uap->curregs[5] &= ~TxN_MASK;

	switch (cflag & CSIZE) {
	case CS5:
		uap->curregs[3] |= Rx5;
		uap->curregs[5] |= Tx5;
		uap->parity_mask = 0x1f;
		break;
	case CS6:
		uap->curregs[3] |= Rx6;
		uap->curregs[5] |= Tx6;
		uap->parity_mask = 0x3f;
		break;
	case CS7:
		uap->curregs[3] |= Rx7;
		uap->curregs[5] |= Tx7;
		uap->parity_mask = 0x7f;
		break;
	case CS8:
	default:
		uap->curregs[3] |= Rx8;
		uap->curregs[5] |= Tx8;
		uap->parity_mask = 0xff;
		break;
	};
	uap->curregs[4] &= ~(SB_MASK);
	if (cflag & CSTOPB)
		uap->curregs[4] |= SB2;
	else
		uap->curregs[4] |= SB1;
	if (cflag & PARENB)
		uap->curregs[4] |= PAR_ENAB;
	else
		uap->curregs[4] &= ~PAR_ENAB;
	if (!(cflag & PARODD))
		uap->curregs[4] |= PAR_EVEN;
	else
		uap->curregs[4] &= ~PAR_EVEN;

	uap->port.read_status_mask = Rx_OVR;
	if (iflag & INPCK)
		uap->port.read_status_mask |= CRC_ERR | PAR_ERR;
	if (iflag & (BRKINT | PARMRK))
		uap->port.read_status_mask |= BRK_ABRT;

	uap->port.ignore_status_mask = 0;
	if (iflag & IGNPAR)
		uap->port.ignore_status_mask |= CRC_ERR | PAR_ERR;
	if (iflag & IGNBRK) {
		uap->port.ignore_status_mask |= BRK_ABRT;
		if (iflag & IGNPAR)
			uap->port.ignore_status_mask |= Rx_OVR;
	}

	if ((cflag & CREAD) == 0)
		uap->port.ignore_status_mask = 0xff;
}


/*
 * Set the irda codec on the imac to the specified baud rate.
 */
static void pmz_irda_setup(struct uart_pmac_port *uap, unsigned long *baud)
{
	u8 cmdbyte;
	int t, version;

	switch (*baud) {
	/* SIR modes */
	case 2400:
		cmdbyte = 0x53;
		break;
	case 4800:
		cmdbyte = 0x52;
		break;
	case 9600:
		cmdbyte = 0x51;
		break;
	case 19200:
		cmdbyte = 0x50;
		break;
	case 38400:
		cmdbyte = 0x4f;
		break;
	case 57600:
		cmdbyte = 0x4e;
		break;
	case 115200:
		cmdbyte = 0x4d;
		break;
	/* The FIR modes aren't really supported at this point, how
	 * do we select the speed ? via the FCR on KeyLargo ?
	 */
	case 1152000:
		cmdbyte = 0;
		break;
	case 4000000:
		cmdbyte = 0;
		break;
	default: /* 9600 */
		cmdbyte = 0x51;
		*baud = 9600;
		break;
	}

	/* Wait for transmitter to drain */
	t = 10000;
	while ((read_zsreg(uap, R0) & Tx_BUF_EMP) == 0
	       || (read_zsreg(uap, R1) & ALL_SNT) == 0) {
		if (--t <= 0) {
			dev_err(&uap->dev->ofdev.dev, "transmitter didn't drain\n");
			return;
		}
		udelay(10);
	}

	/* Drain the receiver too */
	t = 100;
	(void)read_zsdata(uap);
	(void)read_zsdata(uap);
	(void)read_zsdata(uap);
	mdelay(10);
	while (read_zsreg(uap, R0) & Rx_CH_AV) {
		read_zsdata(uap);
		mdelay(10);
		if (--t <= 0) {
			dev_err(&uap->dev->ofdev.dev, "receiver didn't drain\n");
			return;
		}
	}

	/* Switch to command mode */
	uap->curregs[R5] |= DTR;
	write_zsreg(uap, R5, uap->curregs[R5]);
	zssync(uap);
       	mdelay(1);

	/* Switch SCC to 19200 */
	pmz_convert_to_zs(uap, CS8, 0, 19200);		
	pmz_load_zsregs(uap, uap->curregs);
       	mdelay(1);

	/* Write get_version command byte */
	write_zsdata(uap, 1);
	t = 5000;
	while ((read_zsreg(uap, R0) & Rx_CH_AV) == 0) {
		if (--t <= 0) {
			dev_err(&uap->dev->ofdev.dev,
				"irda_setup timed out on get_version byte\n");
			goto out;
		}
		udelay(10);
	}
	version = read_zsdata(uap);

	if (version < 4) {
		dev_info(&uap->dev->ofdev.dev, "IrDA: dongle version %d not supported\n",
			 version);
		goto out;
	}

	/* Send speed mode */
	write_zsdata(uap, cmdbyte);
	t = 5000;
	while ((read_zsreg(uap, R0) & Rx_CH_AV) == 0) {
		if (--t <= 0) {
			dev_err(&uap->dev->ofdev.dev,
				"irda_setup timed out on speed mode byte\n");
			goto out;
		}
		udelay(10);
	}
	t = read_zsdata(uap);
	if (t != cmdbyte)
		dev_err(&uap->dev->ofdev.dev,
			"irda_setup speed mode byte = %x (%x)\n", t, cmdbyte);

	dev_info(&uap->dev->ofdev.dev, "IrDA setup for %ld bps, dongle version: %d\n",
		 *baud, version);

	(void)read_zsdata(uap);
	(void)read_zsdata(uap);
	(void)read_zsdata(uap);

 out:
	/* Switch back to data mode */
	uap->curregs[R5] &= ~DTR;
	write_zsreg(uap, R5, uap->curregs[R5]);
	zssync(uap);

	(void)read_zsdata(uap);
	(void)read_zsdata(uap);
	(void)read_zsdata(uap);
}


static void __pmz_set_termios(struct uart_port *port, struct termios *termios,
			      struct termios *old)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned long baud;

	pmz_debug("pmz: set_termios()\n");

	if (ZS_IS_ASLEEP(uap))
		return;

	memcpy(&uap->termios_cache, termios, sizeof(struct termios));

	/* XXX Check which revs of machines actually allow 1 and 4Mb speeds
	 * on the IR dongle. Note that the IRTTY driver currently doesn't know
	 * about the FIR mode and high speed modes. So these are unused. For
	 * implementing proper support for these, we should probably add some
	 * DMA as well, at least on the Rx side, which isn't a simple thing
	 * at this point.
	 */
	if (ZS_IS_IRDA(uap)) {
		/* Calc baud rate */
		baud = uart_get_baud_rate(port, termios, old, 1200, 4000000);
		pmz_debug("pmz: switch IRDA to %ld bauds\n", baud);
		/* Cet the irda codec to the right rate */
		pmz_irda_setup(uap, &baud);
		/* Set final baud rate */
		pmz_convert_to_zs(uap, termios->c_cflag, termios->c_iflag, baud);
		pmz_load_zsregs(uap, uap->curregs);
		zssync(uap);
	} else {
		baud = uart_get_baud_rate(port, termios, old, 1200, 230400);
		pmz_convert_to_zs(uap, termios->c_cflag, termios->c_iflag, baud);
		/* Make sure modem status interrupts are correctly configured */
		if (UART_ENABLE_MS(&uap->port, termios->c_cflag)) {
			uap->curregs[R15] |= DCDIE | SYNCIE | CTSIE;
			uap->flags |= PMACZILOG_FLAG_MODEM_STATUS;
		} else {
			uap->curregs[R15] &= ~(DCDIE | SYNCIE | CTSIE);
			uap->flags &= ~PMACZILOG_FLAG_MODEM_STATUS;
		}

		/* Load registers to the chip */
		pmz_maybe_update_regs(uap);
	}
	uart_update_timeout(port, termios->c_cflag, baud);

	pmz_debug("pmz: set_termios() done.\n");
}

/* The port lock is not held.  */
static void pmz_set_termios(struct uart_port *port, struct termios *termios,
			    struct termios *old)
{
	struct uart_pmac_port *uap = to_pmz(port);
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);	

	/* Disable IRQs on the port */
	uap->curregs[R1] &= ~(EXT_INT_ENAB | TxINT_ENAB | RxINT_MASK);
	write_zsreg(uap, R1, uap->curregs[R1]);

	/* Setup new port configuration */
	__pmz_set_termios(port, termios, old);

	/* Re-enable IRQs on the port */
	if (ZS_IS_OPEN(uap)) {
		uap->curregs[R1] |= INT_ALL_Rx | TxINT_ENAB;
		if (!ZS_IS_EXTCLK(uap))
			uap->curregs[R1] |= EXT_INT_ENAB;
		write_zsreg(uap, R1, uap->curregs[R1]);
	}
	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *pmz_type(struct uart_port *port)
{
	struct uart_pmac_port *uap = to_pmz(port);

	if (ZS_IS_IRDA(uap))
		return "Z85c30 ESCC - Infrared port";
	else if (ZS_IS_INTMODEM(uap))
		return "Z85c30 ESCC - Internal modem";
	return "Z85c30 ESCC - Serial port";
}

/* We do not request/release mappings of the registers here, this
 * happens at early serial probe time.
 */
static void pmz_release_port(struct uart_port *port)
{
}

static int pmz_request_port(struct uart_port *port)
{
	return 0;
}

/* These do not need to do anything interesting either.  */
static void pmz_config_port(struct uart_port *port, int flags)
{
}

/* We do not support letting the user mess with the divisor, IRQ, etc. */
static int pmz_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	return -EINVAL;
}

static struct uart_ops pmz_pops = {
	.tx_empty	=	pmz_tx_empty,
	.set_mctrl	=	pmz_set_mctrl,
	.get_mctrl	=	pmz_get_mctrl,
	.stop_tx	=	pmz_stop_tx,
	.start_tx	=	pmz_start_tx,
	.stop_rx	=	pmz_stop_rx,
	.enable_ms	=	pmz_enable_ms,
	.break_ctl	=	pmz_break_ctl,
	.startup	=	pmz_startup,
	.shutdown	=	pmz_shutdown,
	.set_termios	=	pmz_set_termios,
	.type		=	pmz_type,
	.release_port	=	pmz_release_port,
	.request_port	=	pmz_request_port,
	.config_port	=	pmz_config_port,
	.verify_port	=	pmz_verify_port,
};

/*
 * Setup one port structure after probing, HW is down at this point,
 * Unlike sunzilog, we don't need to pre-init the spinlock as we don't
 * register our console before uart_add_one_port() is called
 */
static int __init pmz_init_port(struct uart_pmac_port *uap)
{
	struct device_node *np = uap->node;
	char *conn;
	struct slot_names_prop {
		int	count;
		char	name[1];
	} *slots;
	int len;
	struct resource r_ports, r_rxdma, r_txdma;

	/*
	 * Request & map chip registers
	 */
	if (of_address_to_resource(np, 0, &r_ports))
		return -ENODEV;
	uap->port.mapbase = r_ports.start;
	uap->port.membase = ioremap(uap->port.mapbase, 0x1000);
      
	uap->control_reg = uap->port.membase;
	uap->data_reg = uap->control_reg + 0x10;
	
	/*
	 * Request & map DBDMA registers
	 */
#ifdef HAS_DBDMA
	if (of_address_to_resource(np, 1, &r_txdma) == 0 &&
	    of_address_to_resource(np, 2, &r_rxdma) == 0)
		uap->flags |= PMACZILOG_FLAG_HAS_DMA;
#else
	memset(&r_txdma, 0, sizeof(struct resource));
	memset(&r_rxdma, 0, sizeof(struct resource));
#endif	
	if (ZS_HAS_DMA(uap)) {
		uap->tx_dma_regs = ioremap(r_txdma.start, 0x100);
		if (uap->tx_dma_regs == NULL) {	
			uap->flags &= ~PMACZILOG_FLAG_HAS_DMA;
			goto no_dma;
		}
		uap->rx_dma_regs = ioremap(r_rxdma.start, 0x100);
		if (uap->rx_dma_regs == NULL) {	
			iounmap(uap->tx_dma_regs);
			uap->tx_dma_regs = NULL;
			uap->flags &= ~PMACZILOG_FLAG_HAS_DMA;
			goto no_dma;
		}
		uap->tx_dma_irq = np->intrs[1].line;
		uap->rx_dma_irq = np->intrs[2].line;
	}
no_dma:

	/*
	 * Detect port type
	 */
	if (device_is_compatible(np, "cobalt"))
		uap->flags |= PMACZILOG_FLAG_IS_INTMODEM;
	conn = get_property(np, "AAPL,connector", &len);
	if (conn && (strcmp(conn, "infrared") == 0))
		uap->flags |= PMACZILOG_FLAG_IS_IRDA;
	uap->port_type = PMAC_SCC_ASYNC;
	/* 1999 Powerbook G3 has slot-names property instead */
	slots = (struct slot_names_prop *)get_property(np, "slot-names", &len);
	if (slots && slots->count > 0) {
		if (strcmp(slots->name, "IrDA") == 0)
			uap->flags |= PMACZILOG_FLAG_IS_IRDA;
		else if (strcmp(slots->name, "Modem") == 0)
			uap->flags |= PMACZILOG_FLAG_IS_INTMODEM;
	}
	if (ZS_IS_IRDA(uap))
		uap->port_type = PMAC_SCC_IRDA;
	if (ZS_IS_INTMODEM(uap)) {
		struct device_node* i2c_modem = find_devices("i2c-modem");
		if (i2c_modem) {
			char* mid = get_property(i2c_modem, "modem-id", NULL);
			if (mid) switch(*mid) {
			case 0x04 :
			case 0x05 :
			case 0x07 :
			case 0x08 :
			case 0x0b :
			case 0x0c :
				uap->port_type = PMAC_SCC_I2S1;
			}
			printk(KERN_INFO "pmac_zilog: i2c-modem detected, id: %d\n",
				mid ? (*mid) : 0);
		} else {
			printk(KERN_INFO "pmac_zilog: serial modem detected\n");
		}
	}

	/*
	 * Init remaining bits of "port" structure
	 */
	uap->port.iotype = SERIAL_IO_MEM;
	uap->port.irq = np->intrs[0].line;
	uap->port.uartclk = ZS_CLOCK;
	uap->port.fifosize = 1;
	uap->port.ops = &pmz_pops;
	uap->port.type = PORT_PMAC_ZILOG;
	uap->port.flags = 0;

	/* Setup some valid baud rate information in the register
	 * shadows so we don't write crap there before baud rate is
	 * first initialized.
	 */
	pmz_convert_to_zs(uap, CS8, 0, 9600);

	return 0;
}

/*
 * Get rid of a port on module removal
 */
static void pmz_dispose_port(struct uart_pmac_port *uap)
{
	struct device_node *np;

	np = uap->node;
	iounmap(uap->rx_dma_regs);
	iounmap(uap->tx_dma_regs);
	iounmap(uap->control_reg);
	uap->node = NULL;
	of_node_put(np);
	memset(uap, 0, sizeof(struct uart_pmac_port));
}

/*
 * Called upon match with an escc node in the devive-tree.
 */
static int pmz_attach(struct macio_dev *mdev, const struct of_device_id *match)
{
	int i;
	
	/* Iterate the pmz_ports array to find a matching entry
	 */
	for (i = 0; i < MAX_ZS_PORTS; i++)
		if (pmz_ports[i].node == mdev->ofdev.node) {
			struct uart_pmac_port *uap = &pmz_ports[i];

			uap->dev = mdev;
			dev_set_drvdata(&mdev->ofdev.dev, uap);
			if (macio_request_resources(uap->dev, "pmac_zilog"))
				printk(KERN_WARNING "%s: Failed to request resource"
				       ", port still active\n",
				       uap->node->name);
			else
				uap->flags |= PMACZILOG_FLAG_RSRC_REQUESTED;				
			return 0;
		}
	return -ENODEV;
}

/*
 * That one should not be called, macio isn't really a hotswap device,
 * we don't expect one of those serial ports to go away...
 */
static int pmz_detach(struct macio_dev *mdev)
{
	struct uart_pmac_port	*uap = dev_get_drvdata(&mdev->ofdev.dev);
	
	if (!uap)
		return -ENODEV;

	if (uap->flags & PMACZILOG_FLAG_RSRC_REQUESTED) {
		macio_release_resources(uap->dev);
		uap->flags &= ~PMACZILOG_FLAG_RSRC_REQUESTED;
	}
	dev_set_drvdata(&mdev->ofdev.dev, NULL);
	uap->dev = NULL;
	
	return 0;
}


static int pmz_suspend(struct macio_dev *mdev, pm_message_t pm_state)
{
	struct uart_pmac_port *uap = dev_get_drvdata(&mdev->ofdev.dev);
	struct uart_state *state;
	unsigned long flags;

	if (uap == NULL) {
		printk("HRM... pmz_suspend with NULL uap\n");
		return 0;
	}

	if (pm_state.event == mdev->ofdev.dev.power.power_state.event)
		return 0;

	pmz_debug("suspend, switching to state %d\n", pm_state);

	state = pmz_uart_reg.state + uap->port.line;

	mutex_lock(&pmz_irq_mutex);
	mutex_lock(&state->mutex);

	spin_lock_irqsave(&uap->port.lock, flags);

	if (ZS_IS_OPEN(uap) || ZS_IS_CONS(uap)) {
		/* Disable receiver and transmitter.  */
		uap->curregs[R3] &= ~RxENABLE;
		uap->curregs[R5] &= ~TxENABLE;

		/* Disable all interrupts and BRK assertion.  */
		uap->curregs[R1] &= ~(EXT_INT_ENAB | TxINT_ENAB | RxINT_MASK);
		uap->curregs[R5] &= ~SND_BRK;
		pmz_load_zsregs(uap, uap->curregs);
		uap->flags |= PMACZILOG_FLAG_IS_ASLEEP;
		mb();
	}

	spin_unlock_irqrestore(&uap->port.lock, flags);

	if (ZS_IS_OPEN(uap) || ZS_IS_OPEN(uap->mate))
		if (ZS_IS_ASLEEP(uap->mate) && ZS_IS_IRQ_ON(pmz_get_port_A(uap))) {
			pmz_get_port_A(uap)->flags &= ~PMACZILOG_FLAG_IS_IRQ_ON;
			disable_irq(uap->port.irq);
		}

	if (ZS_IS_CONS(uap))
		uap->port.cons->flags &= ~CON_ENABLED;

	/* Shut the chip down */
	pmz_set_scc_power(uap, 0);

	mutex_unlock(&state->mutex);
	mutex_unlock(&pmz_irq_mutex);

	pmz_debug("suspend, switching complete\n");

	mdev->ofdev.dev.power.power_state = pm_state;

	return 0;
}


static int pmz_resume(struct macio_dev *mdev)
{
	struct uart_pmac_port *uap = dev_get_drvdata(&mdev->ofdev.dev);
	struct uart_state *state;
	unsigned long flags;
	int pwr_delay = 0;

	if (uap == NULL)
		return 0;

	if (mdev->ofdev.dev.power.power_state.event == PM_EVENT_ON)
		return 0;
	
	pmz_debug("resume, switching to state 0\n");

	state = pmz_uart_reg.state + uap->port.line;

	mutex_lock(&pmz_irq_mutex);
	mutex_lock(&state->mutex);

	spin_lock_irqsave(&uap->port.lock, flags);
	if (!ZS_IS_OPEN(uap) && !ZS_IS_CONS(uap)) {
		spin_unlock_irqrestore(&uap->port.lock, flags);
		goto bail;
	}
	pwr_delay = __pmz_startup(uap);

	/* Take care of config that may have changed while asleep */
	__pmz_set_termios(&uap->port, &uap->termios_cache, NULL);

	if (ZS_IS_OPEN(uap)) {
		/* Enable interrupts */		
		uap->curregs[R1] |= INT_ALL_Rx | TxINT_ENAB;
		if (!ZS_IS_EXTCLK(uap))
			uap->curregs[R1] |= EXT_INT_ENAB;
		write_zsreg(uap, R1, uap->curregs[R1]);
	}

	spin_unlock_irqrestore(&uap->port.lock, flags);

	if (ZS_IS_CONS(uap))
		uap->port.cons->flags |= CON_ENABLED;

	/* Re-enable IRQ on the controller */
	if (ZS_IS_OPEN(uap) && !ZS_IS_IRQ_ON(pmz_get_port_A(uap))) {
		pmz_get_port_A(uap)->flags |= PMACZILOG_FLAG_IS_IRQ_ON;
		enable_irq(uap->port.irq);
	}

 bail:
	mutex_unlock(&state->mutex);
	mutex_unlock(&pmz_irq_mutex);

	/* Right now, we deal with delay by blocking here, I'll be
	 * smarter later on
	 */
	if (pwr_delay != 0) {
		pmz_debug("pmz: delaying %d ms\n", pwr_delay);
		msleep(pwr_delay);
	}

	pmz_debug("resume, switching complete\n");

	mdev->ofdev.dev.power.power_state.event = PM_EVENT_ON;

	return 0;
}

/*
 * Probe all ports in the system and build the ports array, we register
 * with the serial layer at this point, the macio-type probing is only
 * used later to "attach" to the sysfs tree so we get power management
 * events
 */
static int __init pmz_probe(void)
{
	struct device_node	*node_p, *node_a, *node_b, *np;
	int			count = 0;
	int			rc;

	/*
	 * Find all escc chips in the system
	 */
	node_p = of_find_node_by_name(NULL, "escc");
	while (node_p) {
		/*
		 * First get channel A/B node pointers
		 * 
		 * TODO: Add routines with proper locking to do that...
		 */
		node_a = node_b = NULL;
		for (np = NULL; (np = of_get_next_child(node_p, np)) != NULL;) {
			if (strncmp(np->name, "ch-a", 4) == 0)
				node_a = of_node_get(np);
			else if (strncmp(np->name, "ch-b", 4) == 0)
				node_b = of_node_get(np);
		}
		if (!node_a && !node_b) {
			of_node_put(node_a);
			of_node_put(node_b);
			printk(KERN_ERR "pmac_zilog: missing node %c for escc %s\n",
				(!node_a) ? 'a' : 'b', node_p->full_name);
			goto next;
		}

		/*
		 * Fill basic fields in the port structures
		 */
		pmz_ports[count].mate		= &pmz_ports[count+1];
		pmz_ports[count+1].mate		= &pmz_ports[count];
		pmz_ports[count].flags		= PMACZILOG_FLAG_IS_CHANNEL_A;
		pmz_ports[count].node		= node_a;
		pmz_ports[count+1].node		= node_b;
		pmz_ports[count].port.line	= count;
		pmz_ports[count+1].port.line   	= count+1;

		/*
		 * Setup the ports for real
		 */
		rc = pmz_init_port(&pmz_ports[count]);
		if (rc == 0 && node_b != NULL)
			rc = pmz_init_port(&pmz_ports[count+1]);
		if (rc != 0) {
			of_node_put(node_a);
			of_node_put(node_b);
			memset(&pmz_ports[count], 0, sizeof(struct uart_pmac_port));
			memset(&pmz_ports[count+1], 0, sizeof(struct uart_pmac_port));
			goto next;
		}
		count += 2;
next:
		node_p = of_find_node_by_name(node_p, "escc");
	}
	pmz_ports_count = count;

	return 0;
}

#ifdef CONFIG_SERIAL_PMACZILOG_CONSOLE

static void pmz_console_write(struct console *con, const char *s, unsigned int count);
static int __init pmz_console_setup(struct console *co, char *options);

static struct console pmz_console = {
	.name	=	"ttyS",
	.write	=	pmz_console_write,
	.device	=	uart_console_device,
	.setup	=	pmz_console_setup,
	.flags	=	CON_PRINTBUFFER,
	.index	=	-1,
	.data   =	&pmz_uart_reg,
};

#define PMACZILOG_CONSOLE	&pmz_console
#else /* CONFIG_SERIAL_PMACZILOG_CONSOLE */
#define PMACZILOG_CONSOLE	(NULL)
#endif /* CONFIG_SERIAL_PMACZILOG_CONSOLE */

/*
 * Register the driver, console driver and ports with the serial
 * core
 */
static int __init pmz_register(void)
{
	int i, rc;
	
	pmz_uart_reg.nr = pmz_ports_count;
	pmz_uart_reg.cons = PMACZILOG_CONSOLE;
	pmz_uart_reg.minor = 64;

	/*
	 * Register this driver with the serial core
	 */
	rc = uart_register_driver(&pmz_uart_reg);
	if (rc)
		return rc;

	/*
	 * Register each port with the serial core
	 */
	for (i = 0; i < pmz_ports_count; i++) {
		struct uart_pmac_port *uport = &pmz_ports[i];
		/* NULL node may happen on wallstreet */
		if (uport->node != NULL)
			rc = uart_add_one_port(&pmz_uart_reg, &uport->port);
		if (rc)
			goto err_out;
	}

	return 0;
err_out:
	while (i-- > 0) {
		struct uart_pmac_port *uport = &pmz_ports[i];
		uart_remove_one_port(&pmz_uart_reg, &uport->port);
	}
	uart_unregister_driver(&pmz_uart_reg);
	return rc;
}

static struct of_device_id pmz_match[] = 
{
	{
	.name 		= "ch-a",
	},
	{
	.name 		= "ch-b",
	},
	{},
};
MODULE_DEVICE_TABLE (of, pmz_match);

static struct macio_driver pmz_driver = 
{
	.name 		= "pmac_zilog",
	.match_table	= pmz_match,
	.probe		= pmz_attach,
	.remove		= pmz_detach,
	.suspend	= pmz_suspend,
       	.resume		= pmz_resume,
};

static int __init init_pmz(void)
{
	int rc, i;
	printk(KERN_INFO "%s\n", version);

	/* 
	 * First, we need to do a direct OF-based probe pass. We
	 * do that because we want serial console up before the
	 * macio stuffs calls us back, and since that makes it
	 * easier to pass the proper number of channels to
	 * uart_register_driver()
	 */
	if (pmz_ports_count == 0)
		pmz_probe();

	/*
	 * Bail early if no port found
	 */
	if (pmz_ports_count == 0)
		return -ENODEV;

	/*
	 * Now we register with the serial layer
	 */
	rc = pmz_register();
	if (rc) {
		printk(KERN_ERR 
			"pmac_zilog: Error registering serial device, disabling pmac_zilog.\n"
		 	"pmac_zilog: Did another serial driver already claim the minors?\n"); 
		/* effectively "pmz_unprobe()" */
		for (i=0; i < pmz_ports_count; i++)
			pmz_dispose_port(&pmz_ports[i]);
		return rc;
	}
	
	/*
	 * Then we register the macio driver itself
	 */
	return macio_register_driver(&pmz_driver);
}

static void __exit exit_pmz(void)
{
	int i;

	/* Get rid of macio-driver (detach from macio) */
	macio_unregister_driver(&pmz_driver);

	for (i = 0; i < pmz_ports_count; i++) {
		struct uart_pmac_port *uport = &pmz_ports[i];
		if (uport->node != NULL) {
			uart_remove_one_port(&pmz_uart_reg, &uport->port);
			pmz_dispose_port(uport);
		}
	}
	/* Unregister UART driver */
	uart_unregister_driver(&pmz_uart_reg);
}

#ifdef CONFIG_SERIAL_PMACZILOG_CONSOLE

/*
 * Print a string to the serial port trying not to disturb
 * any possible real use of the port...
 */
static void pmz_console_write(struct console *con, const char *s, unsigned int count)
{
	struct uart_pmac_port *uap = &pmz_ports[con->index];
	unsigned long flags;
	int i;

	if (ZS_IS_ASLEEP(uap))
		return;
	spin_lock_irqsave(&uap->port.lock, flags);

	/* Turn of interrupts and enable the transmitter. */
	write_zsreg(uap, R1, uap->curregs[1] & ~TxINT_ENAB);
	write_zsreg(uap, R5, uap->curregs[5] | TxENABLE | RTS | DTR);

	for (i = 0; i < count; i++) {
		/* Wait for the transmit buffer to empty. */
		while ((read_zsreg(uap, R0) & Tx_BUF_EMP) == 0)
			udelay(5);
		write_zsdata(uap, s[i]);
		if (s[i] == 10) {
			while ((read_zsreg(uap, R0) & Tx_BUF_EMP) == 0)
				udelay(5);
			write_zsdata(uap, R13);
		}
	}

	/* Restore the values in the registers. */
	write_zsreg(uap, R1, uap->curregs[1]);
	/* Don't disable the transmitter. */

	spin_unlock_irqrestore(&uap->port.lock, flags);
}

/*
 * Setup the serial console
 */
static int __init pmz_console_setup(struct console *co, char *options)
{
	struct uart_pmac_port *uap;
	struct uart_port *port;
	int baud = 38400;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	unsigned long pwr_delay;

	/*
	 * XServe's default to 57600 bps
	 */
	if (machine_is_compatible("RackMac1,1")
	    || machine_is_compatible("RackMac1,2")
	    || machine_is_compatible("MacRISC4"))
	 	baud = 57600;

	/*
	 * Check whether an invalid uart number has been specified, and
	 * if so, search for the first available port that does have
	 * console support.
	 */
	if (co->index >= pmz_ports_count)
		co->index = 0;
	uap = &pmz_ports[co->index];
	if (uap->node == NULL)
		return -ENODEV;
	port = &uap->port;

	/*
	 * Mark port as beeing a console
	 */
	uap->flags |= PMACZILOG_FLAG_IS_CONS;

	/*
	 * Temporary fix for uart layer who didn't setup the spinlock yet
	 */
	spin_lock_init(&port->lock);

	/*
	 * Enable the hardware
	 */
	pwr_delay = __pmz_startup(uap);
	if (pwr_delay)
		mdelay(pwr_delay);
	
	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static int __init pmz_console_init(void)
{
	/* Probe ports */
	pmz_probe();

	/* TODO: Autoprobe console based on OF */
	/* pmz_console.index = i; */
	register_console(&pmz_console);

	return 0;

}
console_initcall(pmz_console_init);
#endif /* CONFIG_SERIAL_PMACZILOG_CONSOLE */

module_init(init_pmz);
module_exit(exit_pmz);
