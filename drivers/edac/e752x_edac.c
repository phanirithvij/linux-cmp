/*
 * Intel e752x Memory Controller kernel module
 * (C) 2004 Linux Networx (http://lnxi.com)
 * This file may be distributed under the terms of the
 * GNU General Public License.
 *
 * See "enum e752x_chips" below for supported chipsets
 *
 * Written by Tom Zimmerman
 *
 * Contributors:
 * 	Thayne Harbaugh at realmsys.com (?)
 * 	Wang Zhenyu at intel.com
 * 	Dave Jiang at mvista.com
 *
 * $Id: edac_e752x.c,v 1.5.2.11 2005/10/05 00:43:44 dsp_llnl Exp $
 *
 */


#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/pci.h>
#include <linux/pci_ids.h>

#include <linux/slab.h>

#include "edac_mc.h"


#ifndef PCI_DEVICE_ID_INTEL_7520_0
#define PCI_DEVICE_ID_INTEL_7520_0      0x3590
#endif				/* PCI_DEVICE_ID_INTEL_7520_0      */

#ifndef PCI_DEVICE_ID_INTEL_7520_1_ERR
#define PCI_DEVICE_ID_INTEL_7520_1_ERR  0x3591
#endif				/* PCI_DEVICE_ID_INTEL_7520_1_ERR  */

#ifndef PCI_DEVICE_ID_INTEL_7525_0
#define PCI_DEVICE_ID_INTEL_7525_0      0x359E
#endif				/* PCI_DEVICE_ID_INTEL_7525_0      */

#ifndef PCI_DEVICE_ID_INTEL_7525_1_ERR
#define PCI_DEVICE_ID_INTEL_7525_1_ERR  0x3593
#endif				/* PCI_DEVICE_ID_INTEL_7525_1_ERR  */

#ifndef PCI_DEVICE_ID_INTEL_7320_0
#define PCI_DEVICE_ID_INTEL_7320_0	0x3592
#endif				/* PCI_DEVICE_ID_INTEL_7320_0 */

#ifndef PCI_DEVICE_ID_INTEL_7320_1_ERR
#define PCI_DEVICE_ID_INTEL_7320_1_ERR	0x3593
#endif				/* PCI_DEVICE_ID_INTEL_7320_1_ERR */

#define E752X_NR_CSROWS		8	/* number of csrows */


/* E752X register addresses - device 0 function 0 */
#define E752X_DRB		0x60	/* DRAM row boundary register (8b) */
#define E752X_DRA		0x70	/* DRAM row attribute register (8b) */
					/*
					 * 31:30   Device width row 7
					 *      01=x8 10=x4 11=x8 DDR2
					 * 27:26   Device width row 6
					 * 23:22   Device width row 5
					 * 19:20   Device width row 4
					 * 15:14   Device width row 3
					 * 11:10   Device width row 2
					 *  7:6    Device width row 1
					 *  3:2    Device width row 0
					 */
#define E752X_DRC		0x7C	/* DRAM controller mode reg (32b) */
					/* FIXME:IS THIS RIGHT? */
					/*
					 * 22    Number channels 0=1,1=2
					 * 19:18 DRB Granularity 32/64MB
					 */
#define E752X_DRM		0x80	/* Dimm mapping register */
#define E752X_DDRCSR		0x9A	/* DDR control and status reg (16b) */
					/*
					 * 14:12 1 single A, 2 single B, 3 dual
					 */
#define E752X_TOLM		0xC4	/* DRAM top of low memory reg (16b) */
#define E752X_REMAPBASE		0xC6	/* DRAM remap base address reg (16b) */
#define E752X_REMAPLIMIT	0xC8	/* DRAM remap limit address reg (16b) */
#define E752X_REMAPOFFSET	0xCA	/* DRAM remap limit offset reg (16b) */

/* E752X register addresses - device 0 function 1 */
#define E752X_FERR_GLOBAL	0x40	/* Global first error register (32b) */
#define E752X_NERR_GLOBAL	0x44	/* Global next error register (32b) */
#define E752X_HI_FERR		0x50	/* Hub interface first error reg (8b) */
#define E752X_HI_NERR		0x52	/* Hub interface next error reg (8b) */
#define E752X_HI_ERRMASK	0x54	/* Hub interface error mask reg (8b) */
#define E752X_HI_SMICMD		0x5A	/* Hub interface SMI command reg (8b) */
#define E752X_SYSBUS_FERR	0x60	/* System buss first error reg (16b) */
#define E752X_SYSBUS_NERR	0x62	/* System buss next error reg (16b) */
#define E752X_SYSBUS_ERRMASK	0x64	/* System buss error mask reg (16b) */
#define E752X_SYSBUS_SMICMD	0x6A	/* System buss SMI command reg (16b) */
#define E752X_BUF_FERR		0x70	/* Memory buffer first error reg (8b) */
#define E752X_BUF_NERR		0x72	/* Memory buffer next error reg (8b) */
#define E752X_BUF_ERRMASK	0x74	/* Memory buffer error mask reg (8b) */
#define E752X_BUF_SMICMD	0x7A	/* Memory buffer SMI command reg (8b) */
#define E752X_DRAM_FERR		0x80	/* DRAM first error register (16b) */
#define E752X_DRAM_NERR		0x82	/* DRAM next error register (16b) */
#define E752X_DRAM_ERRMASK	0x84	/* DRAM error mask register (8b) */
#define E752X_DRAM_SMICMD	0x8A	/* DRAM SMI command register (8b) */
#define E752X_DRAM_RETR_ADD	0xAC	/* DRAM Retry address register (32b) */
#define E752X_DRAM_SEC1_ADD	0xA0	/* DRAM first correctable memory */
					/*     error address register (32b) */
					/*
					 * 31    Reserved
					 * 30:2  CE address (64 byte block 34:6)
					 * 1     Reserved
					 * 0     HiLoCS
					 */
#define E752X_DRAM_SEC2_ADD	0xC8	/* DRAM first correctable memory */
					/*     error address register (32b) */
					/*
					 * 31    Reserved
					 * 30:2  CE address (64 byte block 34:6)
					 * 1     Reserved
					 * 0     HiLoCS
					 */
#define E752X_DRAM_DED_ADD	0xA4	/* DRAM first uncorrectable memory */
					/*     error address register (32b) */
					/*
					 * 31    Reserved
					 * 30:2  CE address (64 byte block 34:6)
					 * 1     Reserved
					 * 0     HiLoCS
					 */
#define E752X_DRAM_SCRB_ADD	0xA8	/* DRAM first uncorrectable scrub memory */
					/*     error address register (32b) */
					/*
					 * 31    Reserved
					 * 30:2  CE address (64 byte block 34:6)
					 * 1     Reserved
					 * 0     HiLoCS
					 */
#define E752X_DRAM_SEC1_SYNDROME 0xC4	/* DRAM first correctable memory */
					/*     error syndrome register (16b) */
#define E752X_DRAM_SEC2_SYNDROME 0xC6	/* DRAM second correctable memory */
					/*     error syndrome register (16b) */
#define E752X_DEVPRES1		0xF4	/* Device Present 1 register (8b) */

/* ICH5R register addresses - device 30 function 0 */
#define ICH5R_PCI_STAT		0x06	/* PCI status register (16b) */
#define ICH5R_PCI_2ND_STAT	0x1E	/* PCI status secondary reg (16b) */
#define ICH5R_PCI_BRIDGE_CTL	0x3E	/* PCI bridge control register (16b) */

enum e752x_chips {
	E7520 = 0,
	E7525 = 1,
	E7320 = 2
};


struct e752x_pvt {
	struct pci_dev *bridge_ck;
	struct pci_dev *dev_d0f0;
	struct pci_dev *dev_d0f1;
	u32 tolm;
	u32 remapbase;
	u32 remaplimit;
	int mc_symmetric;
	u8 map[8];
	int map_type;
	const struct e752x_dev_info *dev_info;
};


struct e752x_dev_info {
	u16 err_dev;
	const char *ctl_name;
};

struct e752x_error_info {
	u32 ferr_global;
	u32 nerr_global;
	u8 hi_ferr;
	u8 hi_nerr;
	u16 sysbus_ferr;
	u16 sysbus_nerr;
	u8 buf_ferr;
	u8 buf_nerr;
	u16 dram_ferr;
	u16 dram_nerr;
	u32 dram_sec1_add;
	u32 dram_sec2_add;
	u16 dram_sec1_syndrome;
	u16 dram_sec2_syndrome;
	u32 dram_ded_add;
	u32 dram_scrb_add;
	u32 dram_retr_add;
};

static const struct e752x_dev_info e752x_devs[] = {
	[E7520] = {
		   .err_dev = PCI_DEVICE_ID_INTEL_7520_1_ERR,
		   .ctl_name = "E7520"},
	[E7525] = {
		   .err_dev = PCI_DEVICE_ID_INTEL_7525_1_ERR,
		   .ctl_name = "E7525"},
	[E7320] = {
		   .err_dev = PCI_DEVICE_ID_INTEL_7320_1_ERR,
		   .ctl_name = "E7320"},
};


static unsigned long ctl_page_to_phys(struct mem_ctl_info *mci,
				      unsigned long page)
{
	u32 remap;
	struct e752x_pvt *pvt = (struct e752x_pvt *) mci->pvt_info;

	debugf3("MC: " __FILE__ ": %s()\n", __func__);

	if (page < pvt->tolm)
		return page;
	if ((page >= 0x100000) && (page < pvt->remapbase))
		return page;
	remap = (page - pvt->tolm) + pvt->remapbase;
	if (remap < pvt->remaplimit)
		return remap;
	printk(KERN_ERR "Invalid page %lx - out of range\n", page);
	return pvt->tolm - 1;
}

static void do_process_ce(struct mem_ctl_info *mci, u16 error_one,
		       u32 sec1_add, u16 sec1_syndrome)
{
	u32 page;
	int row;
	int channel;
	int i;
	struct e752x_pvt *pvt = (struct e752x_pvt *) mci->pvt_info;

	debugf3("MC: " __FILE__ ": %s()\n", __func__);

	/* convert the addr to 4k page */
	page = sec1_add >> (PAGE_SHIFT - 4);

	/* FIXME - check for -1 */
	if (pvt->mc_symmetric) {
		/* chip select are bits 14 & 13 */
		row = ((page >> 1) & 3);
		printk(KERN_WARNING
		       "Test row %d Table %d %d %d %d %d %d %d %d\n",
		       row, pvt->map[0], pvt->map[1], pvt->map[2],
		       pvt->map[3], pvt->map[4], pvt->map[5],
		       pvt->map[6], pvt->map[7]);

		/* test for channel remapping */
		for (i = 0; i < 8; i++) {
			if (pvt->map[i] == row)
				break;
		}
		printk(KERN_WARNING "Test computed row %d\n", i);
		if (i < 8)
			row = i;
		else
			printk(KERN_WARNING
			       "MC%d: row %d not found in remap table\n",
			       mci->mc_idx, row);
	} else
		row = edac_mc_find_csrow_by_page(mci, page);
	/* 0 = channel A, 1 = channel B */
	channel = !(error_one & 1);

	if (!pvt->map_type)
		row = 7 - row;
	edac_mc_handle_ce(mci, page, 0, sec1_syndrome, row, channel,
	    "e752x CE");
}


static inline void process_ce(struct mem_ctl_info *mci, u16 error_one,
		u32 sec1_add, u16 sec1_syndrome, int *error_found,
		int handle_error)
{
	*error_found = 1;

	if (handle_error)
		do_process_ce(mci, error_one, sec1_add, sec1_syndrome);
}

static void do_process_ue(struct mem_ctl_info *mci, u16 error_one, u32 ded_add,
		u32 scrb_add)
{
	u32 error_2b, block_page;
	int row;
	struct e752x_pvt *pvt = (struct e752x_pvt *) mci->pvt_info;

	debugf3("MC: " __FILE__ ": %s()\n", __func__);

	if (error_one & 0x0202) {
		error_2b = ded_add;
		/* convert to 4k address */
		block_page = error_2b >> (PAGE_SHIFT - 4);
		row = pvt->mc_symmetric ?
		    /* chip select are bits 14 & 13 */
		    ((block_page >> 1) & 3) :
		    edac_mc_find_csrow_by_page(mci, block_page);
		edac_mc_handle_ue(mci, block_page, 0, row,
				       "e752x UE from Read");
	}
	if (error_one & 0x0404) {
		error_2b = scrb_add;
		/* convert to 4k address */
		block_page = error_2b >> (PAGE_SHIFT - 4);
		row = pvt->mc_symmetric ?
		    /* chip select are bits 14 & 13 */
		    ((block_page >> 1) & 3) :
		    edac_mc_find_csrow_by_page(mci, block_page);
		edac_mc_handle_ue(mci, block_page, 0, row,
				       "e752x UE from Scruber");
	}
}

static inline void process_ue(struct mem_ctl_info *mci, u16 error_one,
		u32 ded_add, u32 scrb_add, int *error_found, int handle_error)
{
	*error_found = 1;

	if (handle_error)
		do_process_ue(mci, error_one, ded_add, scrb_add);
}

static inline void process_ue_no_info_wr(struct mem_ctl_info *mci,
		int *error_found, int handle_error)
{
	*error_found = 1;

	if (!handle_error)
		return;

	debugf3("MC: " __FILE__ ": %s()\n", __func__);
	edac_mc_handle_ue_no_info(mci, "e752x UE log memory write");
}

static void do_process_ded_retry(struct mem_ctl_info *mci, u16 error,
		u32 retry_add)
{
	u32 error_1b, page;
	int row;
	struct e752x_pvt *pvt = (struct e752x_pvt *) mci->pvt_info;

	error_1b = retry_add;
	page = error_1b >> (PAGE_SHIFT - 4);	/* convert the addr to 4k page */
	row = pvt->mc_symmetric ?
	    ((page >> 1) & 3) :	/* chip select are bits 14 & 13 */
	    edac_mc_find_csrow_by_page(mci, page);
	printk(KERN_WARNING
	       "MC%d: CE page 0x%lx, row %d : Memory read retry\n",
	       mci->mc_idx, (long unsigned int) page, row);
}

static inline void process_ded_retry(struct mem_ctl_info *mci, u16 error,
		u32 retry_add, int *error_found, int handle_error)
{
	*error_found = 1;

	if (handle_error)
		do_process_ded_retry(mci, error, retry_add);
}

static inline void process_threshold_ce(struct mem_ctl_info *mci, u16 error,
		int *error_found, int handle_error)
{
	*error_found = 1;

	if (handle_error)
		printk(KERN_WARNING "MC%d: Memory threshold CE\n",
		       mci->mc_idx);
}

static char *global_message[11] = {
	"PCI Express C1", "PCI Express C", "PCI Express B1",
	"PCI Express B", "PCI Express A1", "PCI Express A",
	"DMA Controler", "HUB Interface", "System Bus",
	"DRAM Controler", "Internal Buffer"
};

static char *fatal_message[2] = { "Non-Fatal ", "Fatal " };

static void do_global_error(int fatal, u32 errors)
{
	int i;

	for (i = 0; i < 11; i++) {
		if (errors & (1 << i))
			printk(KERN_WARNING "%sError %s\n",
			       fatal_message[fatal], global_message[i]);
	}
}

static inline void global_error(int fatal, u32 errors, int *error_found,
		int handle_error)
{
	*error_found = 1;

	if (handle_error)
		do_global_error(fatal, errors);
}

static char *hub_message[7] = {
	"HI Address or Command Parity", "HI Illegal Access",
	"HI Internal Parity", "Out of Range Access",
	"HI Data Parity", "Enhanced Config Access",
	"Hub Interface Target Abort"
};

static void do_hub_error(int fatal, u8 errors)
{
	int i;

	for (i = 0; i < 7; i++) {
		if (errors & (1 << i))
			printk(KERN_WARNING "%sError %s\n",
			       fatal_message[fatal], hub_message[i]);
	}
}

static inline void hub_error(int fatal, u8 errors, int *error_found,
		int handle_error)
{
	*error_found = 1;

	if (handle_error)
		do_hub_error(fatal, errors);
}

static char *membuf_message[4] = {
	"Internal PMWB to DRAM parity",
	"Internal PMWB to System Bus Parity",
	"Internal System Bus or IO to PMWB Parity",
	"Internal DRAM to PMWB Parity"
};

static void do_membuf_error(u8 errors)
{
	int i;

	for (i = 0; i < 4; i++) {
		if (errors & (1 << i))
			printk(KERN_WARNING "Non-Fatal Error %s\n",
			       membuf_message[i]);
	}
}

static inline void membuf_error(u8 errors, int *error_found, int handle_error)
{
	*error_found = 1;

	if (handle_error)
		do_membuf_error(errors);
}

#if 0
char *sysbus_message[10] = {
	"Addr or Request Parity",
	"Data Strobe Glitch",
	"Addr Strobe Glitch",
	"Data Parity",
	"Addr Above TOM",
	"Non DRAM Lock Error",
	"MCERR", "BINIT",
	"Memory Parity",
	"IO Subsystem Parity"
};
#endif  /*  0  */

static void do_sysbus_error(int fatal, u32 errors)
{
	int i;

	for (i = 0; i < 10; i++) {
		if (errors & (1 << i))
			printk(KERN_WARNING "%sError System Bus %s\n",
			       fatal_message[fatal], global_message[i]);
	}
}

static inline void sysbus_error(int fatal, u32 errors, int *error_found,
		int handle_error)
{
	*error_found = 1;

	if (handle_error)
		do_sysbus_error(fatal, errors);
}

static void e752x_check_hub_interface (struct e752x_error_info *info,
		int *error_found, int handle_error)
{
	u8 stat8;

	//pci_read_config_byte(dev,E752X_HI_FERR,&stat8);
	stat8 = info->hi_ferr;
	if(stat8 & 0x7f) { /* Error, so process */
		stat8 &= 0x7f;
		if(stat8 & 0x2b)
			hub_error(1, stat8 & 0x2b, error_found, handle_error);
		if(stat8 & 0x54)
			hub_error(0, stat8 & 0x54, error_found, handle_error);
	}
	//pci_read_config_byte(dev,E752X_HI_NERR,&stat8);
	stat8 = info->hi_nerr;
	if(stat8 & 0x7f) { /* Error, so process */
		stat8 &= 0x7f;
		if (stat8 & 0x2b)
			hub_error(1, stat8 & 0x2b, error_found, handle_error);
		if(stat8 & 0x54)
			hub_error(0, stat8 & 0x54, error_found, handle_error);
	}
}

static void e752x_check_sysbus (struct e752x_error_info *info, int *error_found,
		int handle_error)
{
	u32 stat32, error32;

	//pci_read_config_dword(dev,E752X_SYSBUS_FERR,&stat32);
	stat32 = info->sysbus_ferr + (info->sysbus_nerr << 16);

	if (stat32 == 0)
		return;  /* no errors */

	error32 = (stat32 >> 16) & 0x3ff;
	stat32 = stat32 & 0x3ff;
	if(stat32 & 0x083)
		sysbus_error(1, stat32 & 0x083, error_found, handle_error);
	if(stat32 & 0x37c)
		sysbus_error(0, stat32 & 0x37c, error_found, handle_error);
	if(error32 & 0x083)
		sysbus_error(1, error32 & 0x083, error_found, handle_error);
	if(error32 & 0x37c)
		sysbus_error(0, error32 & 0x37c, error_found, handle_error);
}

static void e752x_check_membuf (struct e752x_error_info *info, int *error_found,
		int handle_error)
{
	u8 stat8;

	stat8 = info->buf_ferr;
	if (stat8 & 0x0f) { /* Error, so process */
		stat8 &= 0x0f;
		membuf_error(stat8, error_found, handle_error);
	}
	stat8 = info->buf_nerr;
	if (stat8 & 0x0f) { /* Error, so process */
		stat8 &= 0x0f;
		membuf_error(stat8, error_found, handle_error);
	}
}

static void e752x_check_dram (struct mem_ctl_info *mci,
		struct e752x_error_info *info, int *error_found, int handle_error)
{
	u16 error_one, error_next;

	error_one = info->dram_ferr;
	error_next = info->dram_nerr;

	/* decode and report errors */
	if(error_one & 0x0101)  /* check first error correctable */
		process_ce(mci, error_one, info->dram_sec1_add,
			   info->dram_sec1_syndrome, error_found,
			   handle_error);

	if(error_next & 0x0101)  /* check next error correctable */
		process_ce(mci, error_next, info->dram_sec2_add,
			   info->dram_sec2_syndrome, error_found,
			   handle_error);

	if(error_one & 0x4040)
		process_ue_no_info_wr(mci, error_found, handle_error);

	if(error_next & 0x4040)
		process_ue_no_info_wr(mci, error_found, handle_error);

	if(error_one & 0x2020)
		process_ded_retry(mci, error_one, info->dram_retr_add,
				  error_found, handle_error);

	if(error_next & 0x2020)
		process_ded_retry(mci, error_next, info->dram_retr_add,
				  error_found, handle_error);

	if(error_one & 0x0808)
		process_threshold_ce(mci, error_one, error_found,
				     handle_error);

	if(error_next & 0x0808)
		process_threshold_ce(mci, error_next, error_found,
				     handle_error);

	if(error_one & 0x0606)
		process_ue(mci, error_one, info->dram_ded_add,
			   info->dram_scrb_add, error_found, handle_error);

	if(error_next & 0x0606)
		process_ue(mci, error_next, info->dram_ded_add,
			   info->dram_scrb_add, error_found, handle_error);
}

static void e752x_get_error_info (struct mem_ctl_info *mci,
				  struct e752x_error_info *info)
{
	struct pci_dev *dev;
	struct e752x_pvt *pvt;

	memset(info, 0, sizeof(*info));
	pvt = (struct e752x_pvt *) mci->pvt_info;
	dev = pvt->dev_d0f1;

	pci_read_config_dword(dev, E752X_FERR_GLOBAL, &info->ferr_global);

	if (info->ferr_global) {
		pci_read_config_byte(dev, E752X_HI_FERR, &info->hi_ferr);
		pci_read_config_word(dev, E752X_SYSBUS_FERR,
				&info->sysbus_ferr);
		pci_read_config_byte(dev, E752X_BUF_FERR, &info->buf_ferr);
		pci_read_config_word(dev, E752X_DRAM_FERR,
				&info->dram_ferr);
		pci_read_config_dword(dev, E752X_DRAM_SEC1_ADD,
				&info->dram_sec1_add);
		pci_read_config_word(dev, E752X_DRAM_SEC1_SYNDROME,
				&info->dram_sec1_syndrome);
		pci_read_config_dword(dev, E752X_DRAM_DED_ADD,
				&info->dram_ded_add);
		pci_read_config_dword(dev, E752X_DRAM_SCRB_ADD,
				&info->dram_scrb_add);
		pci_read_config_dword(dev, E752X_DRAM_RETR_ADD,
				&info->dram_retr_add);

		if (info->hi_ferr & 0x7f)
			pci_write_config_byte(dev, E752X_HI_FERR,
					info->hi_ferr);

		if (info->sysbus_ferr)
			pci_write_config_word(dev, E752X_SYSBUS_FERR,
					info->sysbus_ferr);

		if (info->buf_ferr & 0x0f)
			pci_write_config_byte(dev, E752X_BUF_FERR,
					info->buf_ferr);

		if (info->dram_ferr)
			pci_write_bits16(pvt->bridge_ck, E752X_DRAM_FERR,
					info->dram_ferr, info->dram_ferr);

		pci_write_config_dword(dev, E752X_FERR_GLOBAL,
				info->ferr_global);
	}

	pci_read_config_dword(dev, E752X_NERR_GLOBAL, &info->nerr_global);

	if (info->nerr_global) {
		pci_read_config_byte(dev, E752X_HI_NERR, &info->hi_nerr);
		pci_read_config_word(dev, E752X_SYSBUS_NERR,
				&info->sysbus_nerr);
		pci_read_config_byte(dev, E752X_BUF_NERR, &info->buf_nerr);
		pci_read_config_word(dev, E752X_DRAM_NERR,
				&info->dram_nerr);
		pci_read_config_dword(dev, E752X_DRAM_SEC2_ADD,
				&info->dram_sec2_add);
		pci_read_config_word(dev, E752X_DRAM_SEC2_SYNDROME,
				&info->dram_sec2_syndrome);

		if (info->hi_nerr & 0x7f)
			pci_write_config_byte(dev, E752X_HI_NERR,
					info->hi_nerr);

		if (info->sysbus_nerr)
			pci_write_config_word(dev, E752X_SYSBUS_NERR,
					info->sysbus_nerr);

		if (info->buf_nerr & 0x0f)
			pci_write_config_byte(dev, E752X_BUF_NERR,
					info->buf_nerr);

		if (info->dram_nerr)
			pci_write_bits16(pvt->bridge_ck, E752X_DRAM_NERR,
					info->dram_nerr, info->dram_nerr);

		pci_write_config_dword(dev, E752X_NERR_GLOBAL,
				info->nerr_global);
	}
}

static int e752x_process_error_info (struct mem_ctl_info *mci,
		struct e752x_error_info *info, int handle_errors)
{
	u32 error32, stat32;
	int error_found;

	error_found = 0;
	error32 = (info->ferr_global >> 18) & 0x3ff;
	stat32 = (info->ferr_global >> 4) & 0x7ff;

	if (error32)
		global_error(1, error32, &error_found, handle_errors);

	if (stat32)
		global_error(0, stat32, &error_found, handle_errors);

	error32 = (info->nerr_global >> 18) & 0x3ff;
	stat32 = (info->nerr_global >> 4) & 0x7ff;

	if (error32)
		global_error(1, error32, &error_found, handle_errors);

	if (stat32)
		global_error(0, stat32, &error_found, handle_errors);

	e752x_check_hub_interface(info, &error_found, handle_errors);
	e752x_check_sysbus(info, &error_found, handle_errors);
	e752x_check_membuf(info, &error_found, handle_errors);
	e752x_check_dram(mci, info, &error_found, handle_errors);
	return error_found;
}

static void e752x_check(struct mem_ctl_info *mci)
{
	struct e752x_error_info info;
	debugf3("MC: " __FILE__ ": %s()\n", __func__);
	e752x_get_error_info(mci, &info);
	e752x_process_error_info(mci, &info, 1);
}

static int e752x_probe1(struct pci_dev *pdev, int dev_idx)
{
	int rc = -ENODEV;
	int index;
	u16 pci_data, stat;
	u32 stat32;
	u16 stat16;
	u8 stat8;
	struct mem_ctl_info *mci = NULL;
	struct e752x_pvt *pvt = NULL;
	u16 ddrcsr;
	u32 drc;
	int drc_chan;		/* Number of channels 0=1chan,1=2chan */
	int drc_drbg;		/* DRB granularity 0=64mb,1=128mb */
	int drc_ddim;		/* DRAM Data Integrity Mode 0=none,2=edac */
	u32 dra;
	unsigned long last_cumul_size;
	struct pci_dev *pres_dev;
	struct pci_dev *dev = NULL;

	debugf0("MC: " __FILE__ ": %s(): mci\n", __func__);
	debugf0("Starting Probe1\n");

	/* enable device 0 function 1 */
	pci_read_config_byte(pdev, E752X_DEVPRES1, &stat8);
	stat8 |= (1 << 5);
	pci_write_config_byte(pdev, E752X_DEVPRES1, stat8);

	/* need to find out the number of channels */
	pci_read_config_dword(pdev, E752X_DRC, &drc);
	pci_read_config_word(pdev, E752X_DDRCSR, &ddrcsr);
	/* FIXME: should check >>12 or 0xf, true for all? */
	/* Dual channel = 1, Single channel = 0 */
	drc_chan = (((ddrcsr >> 12) & 3) == 3);
	drc_drbg = drc_chan + 1;	/* 128 in dual mode, 64 in single */
	drc_ddim = (drc >> 20) & 0x3;

	mci = edac_mc_alloc(sizeof(*pvt), E752X_NR_CSROWS, drc_chan + 1);

	if (mci == NULL) {
		rc = -ENOMEM;
		goto fail;
	}

	debugf3("MC: " __FILE__ ": %s(): init mci\n", __func__);

	mci->mtype_cap = MEM_FLAG_RDDR;
	mci->edac_ctl_cap = EDAC_FLAG_NONE | EDAC_FLAG_SECDED |
	    EDAC_FLAG_S4ECD4ED;
	/* FIXME - what if different memory types are in different csrows? */
	mci->mod_name = BS_MOD_STR;
	mci->mod_ver = "$Revision: 1.5.2.11 $";
	mci->pdev = pdev;

	debugf3("MC: " __FILE__ ": %s(): init pvt\n", __func__);
	pvt = (struct e752x_pvt *) mci->pvt_info;
	pvt->dev_info = &e752x_devs[dev_idx];
	pvt->bridge_ck = pci_get_device(PCI_VENDOR_ID_INTEL,
					 pvt->dev_info->err_dev,
					 pvt->bridge_ck);
	if (pvt->bridge_ck == NULL)
		pvt->bridge_ck = pci_scan_single_device(pdev->bus,
							PCI_DEVFN(0, 1));
	if (pvt->bridge_ck == NULL) {
		printk(KERN_ERR "MC: error reporting device not found:"
		       "vendor %x device 0x%x (broken BIOS?)\n",
		       PCI_VENDOR_ID_INTEL, e752x_devs[dev_idx].err_dev);
		goto fail;
	}
	pvt->mc_symmetric = ((ddrcsr & 0x10) != 0);

	debugf3("MC: " __FILE__ ": %s(): more mci init\n", __func__);
	mci->ctl_name = pvt->dev_info->ctl_name;
	mci->edac_check = e752x_check;
	mci->ctl_page_to_phys = ctl_page_to_phys;

	/* find out the device types */
	pci_read_config_dword(pdev, E752X_DRA, &dra);

	/*
	 * The dram row boundary (DRB) reg values are boundary address for
	 * each DRAM row with a granularity of 64 or 128MB (single/dual
	 * channel operation).  DRB regs are cumulative; therefore DRB7 will
	 * contain the total memory contained in all eight rows.
	 */
	for (last_cumul_size = index = 0; index < mci->nr_csrows; index++) {
		u8 value;
		u32 cumul_size;
		/* mem_dev 0=x8, 1=x4 */
		int mem_dev = (dra >> (index * 4 + 2)) & 0x3;
		struct csrow_info *csrow = &mci->csrows[index];

		mem_dev = (mem_dev == 2);
		pci_read_config_byte(mci->pdev, E752X_DRB + index, &value);
		/* convert a 128 or 64 MiB DRB to a page size. */
		cumul_size = value << (25 + drc_drbg - PAGE_SHIFT);
		debugf3("MC: " __FILE__ ": %s(): (%d) cumul_size 0x%x\n",
			__func__, index, cumul_size);
		if (cumul_size == last_cumul_size)
			continue;	/* not populated */

		csrow->first_page = last_cumul_size;
		csrow->last_page = cumul_size - 1;
		csrow->nr_pages = cumul_size - last_cumul_size;
		last_cumul_size = cumul_size;
		csrow->grain = 1 << 12;	/* 4KiB - resolution of CELOG */
		csrow->mtype = MEM_RDDR;	/* only one type supported */
		csrow->dtype = mem_dev ? DEV_X4 : DEV_X8;

		/*
		 * if single channel or x8 devices then SECDED
		 * if dual channel and x4 then S4ECD4ED
		 */
		if (drc_ddim) {
			if (drc_chan && mem_dev) {
				csrow->edac_mode = EDAC_S4ECD4ED;
				mci->edac_cap |= EDAC_FLAG_S4ECD4ED;
			} else {
				csrow->edac_mode = EDAC_SECDED;
				mci->edac_cap |= EDAC_FLAG_SECDED;
			}
		} else
			csrow->edac_mode = EDAC_NONE;
	}

	/* Fill in the memory map table */
	{
		u8 value;
		u8 last = 0;
		u8 row = 0;
		for (index = 0; index < 8; index += 2) {

			pci_read_config_byte(mci->pdev, E752X_DRB + index,
					     &value);
			/* test if there is a dimm in this slot */
			if (value == last) {
				/* no dimm in the slot, so flag it as empty */
				pvt->map[index] = 0xff;
				pvt->map[index + 1] = 0xff;
			} else {	/* there is a dimm in the slot */
				pvt->map[index] = row;
				row++;
				last = value;
				/* test the next value to see if the dimm is
				   double sided */
				pci_read_config_byte(mci->pdev,
						     E752X_DRB + index + 1,
						     &value);
				pvt->map[index + 1] = (value == last) ?
				    0xff :	/* the dimm is single sided,
						   so flag as empty */
				    row;	/* this is a double sided dimm
						   to save the next row # */
				row++;
				last = value;
			}
		}
	}

	/* set the map type.  1 = normal, 0 = reversed */
	pci_read_config_byte(mci->pdev, E752X_DRM, &stat8);
	pvt->map_type = ((stat8 & 0x0f) > ((stat8 >> 4) & 0x0f));

	mci->edac_cap |= EDAC_FLAG_NONE;

	debugf3("MC: " __FILE__ ": %s(): tolm, remapbase, remaplimit\n",
		__func__);
	/* load the top of low memory, remap base, and remap limit vars */
	pci_read_config_word(mci->pdev, E752X_TOLM, &pci_data);
	pvt->tolm = ((u32) pci_data) << 4;
	pci_read_config_word(mci->pdev, E752X_REMAPBASE, &pci_data);
	pvt->remapbase = ((u32) pci_data) << 14;
	pci_read_config_word(mci->pdev, E752X_REMAPLIMIT, &pci_data);
	pvt->remaplimit = ((u32) pci_data) << 14;
	printk("tolm = %x, remapbase = %x, remaplimit = %x\n", pvt->tolm,
	       pvt->remapbase, pvt->remaplimit);

	if (edac_mc_add_mc(mci)) {
		debugf3("MC: " __FILE__
			": %s(): failed edac_mc_add_mc()\n",
			__func__);
		goto fail;
	}

	/* Walk through the PCI table and clear errors */
	switch (dev_idx) {
	case E7520:
		dev = pci_get_device(PCI_VENDOR_ID_INTEL,
				      PCI_DEVICE_ID_INTEL_7520_0, NULL);
		break;
	case E7525:
		dev = pci_get_device(PCI_VENDOR_ID_INTEL,
				      PCI_DEVICE_ID_INTEL_7525_0, NULL);
		break;
	case E7320:
		dev = pci_get_device(PCI_VENDOR_ID_INTEL,
				      PCI_DEVICE_ID_INTEL_7320_0, NULL);
		break;
	}


	pvt->dev_d0f0 = dev;
	for (pres_dev = dev;
	     ((struct pci_dev *) pres_dev->global_list.next != dev);
	     pres_dev = (struct pci_dev *) pres_dev->global_list.next) {
		pci_read_config_dword(pres_dev, PCI_COMMAND, &stat32);
		stat = (u16) (stat32 >> 16);
		/* clear any error bits */
		if (stat32 & ((1 << 6) + (1 << 8)))
			pci_write_config_word(pres_dev, PCI_STATUS, stat);
	}
	/* find the error reporting device and clear errors */
	dev = pvt->dev_d0f1 = pci_dev_get(pvt->bridge_ck);
	/* Turn off error disable & SMI in case the BIOS turned it on */
	pci_write_config_byte(dev, E752X_HI_ERRMASK, 0x00);
	pci_write_config_byte(dev, E752X_HI_SMICMD, 0x00);
	pci_write_config_word(dev, E752X_SYSBUS_ERRMASK, 0x00);
	pci_write_config_word(dev, E752X_SYSBUS_SMICMD, 0x00);
	pci_write_config_byte(dev, E752X_BUF_ERRMASK, 0x00);
	pci_write_config_byte(dev, E752X_BUF_SMICMD, 0x00);
	pci_write_config_byte(dev, E752X_DRAM_ERRMASK, 0x00);
	pci_write_config_byte(dev, E752X_DRAM_SMICMD, 0x00);
	/* clear other MCH errors */
	pci_read_config_dword(dev, E752X_FERR_GLOBAL, &stat32);
	pci_write_config_dword(dev, E752X_FERR_GLOBAL, stat32);
	pci_read_config_dword(dev, E752X_NERR_GLOBAL, &stat32);
	pci_write_config_dword(dev, E752X_NERR_GLOBAL, stat32);
	pci_read_config_byte(dev, E752X_HI_FERR, &stat8);
	pci_write_config_byte(dev, E752X_HI_FERR, stat8);
	pci_read_config_byte(dev, E752X_HI_NERR, &stat8);
	pci_write_config_byte(dev, E752X_HI_NERR, stat8);
	pci_read_config_dword(dev, E752X_SYSBUS_FERR, &stat32);
	pci_write_config_dword(dev, E752X_SYSBUS_FERR, stat32);
	pci_read_config_byte(dev, E752X_BUF_FERR, &stat8);
	pci_write_config_byte(dev, E752X_BUF_FERR, stat8);
	pci_read_config_byte(dev, E752X_BUF_NERR, &stat8);
	pci_write_config_byte(dev, E752X_BUF_NERR, stat8);
	pci_read_config_word(dev, E752X_DRAM_FERR, &stat16);
	pci_write_config_word(dev, E752X_DRAM_FERR, stat16);
	pci_read_config_word(dev, E752X_DRAM_NERR, &stat16);
	pci_write_config_word(dev, E752X_DRAM_NERR, stat16);

	/* get this far and it's successful */
	debugf3("MC: " __FILE__ ": %s(): success\n", __func__);
	return 0;

fail:
	if (mci) {
		if (pvt->dev_d0f0)
			pci_dev_put(pvt->dev_d0f0);
		if (pvt->dev_d0f1)
			pci_dev_put(pvt->dev_d0f1);
		if (pvt->bridge_ck)
			pci_dev_put(pvt->bridge_ck);
		edac_mc_free(mci);
	}
	return rc;
}

/* returns count (>= 0), or negative on error */
static int __devinit e752x_init_one(struct pci_dev *pdev,
				    const struct pci_device_id *ent)
{
	debugf0("MC: " __FILE__ ": %s()\n", __func__);

	/* wake up and enable device */
	if(pci_enable_device(pdev) < 0)
		return -EIO;
	return e752x_probe1(pdev, ent->driver_data);
}


static void __devexit e752x_remove_one(struct pci_dev *pdev)
{
	struct mem_ctl_info *mci;
	struct e752x_pvt *pvt;

	debugf0(__FILE__ ": %s()\n", __func__);

	if ((mci = edac_mc_find_mci_by_pdev(pdev)) == NULL)
		return;

	if (edac_mc_del_mc(mci))
		return;

	pvt = (struct e752x_pvt *) mci->pvt_info;
	pci_dev_put(pvt->dev_d0f0);
	pci_dev_put(pvt->dev_d0f1);
	pci_dev_put(pvt->bridge_ck);
	edac_mc_free(mci);
}


static const struct pci_device_id e752x_pci_tbl[] __devinitdata = {
	{PCI_VEND_DEV(INTEL, 7520_0), PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	 E7520},
	{PCI_VEND_DEV(INTEL, 7525_0), PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	 E7525},
	{PCI_VEND_DEV(INTEL, 7320_0), PCI_ANY_ID, PCI_ANY_ID, 0, 0,
	 E7320},
	{0,}			/* 0 terminated list. */
};

MODULE_DEVICE_TABLE(pci, e752x_pci_tbl);


static struct pci_driver e752x_driver = {
      name: BS_MOD_STR,
      probe: e752x_init_one,
      remove: __devexit_p(e752x_remove_one),
      id_table: e752x_pci_tbl,
};


static int __init e752x_init(void)
{
	int pci_rc;

	debugf3("MC: " __FILE__ ": %s()\n", __func__);
	pci_rc = pci_register_driver(&e752x_driver);
	return (pci_rc < 0) ? pci_rc : 0;
}


static void __exit e752x_exit(void)
{
	debugf3("MC: " __FILE__ ": %s()\n", __func__);
	pci_unregister_driver(&e752x_driver);
}


module_init(e752x_init);
module_exit(e752x_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Networx (http://lnxi.com) Tom Zimmerman\n");
MODULE_DESCRIPTION("MC support for Intel e752x memory controllers");
