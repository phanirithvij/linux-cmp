/*
 * $Id: synclink_gt.c,v 4.22 2006/01/09 20:16:06 paulkf Exp $
 *
 * Device driver for Microgate SyncLink GT serial adapters.
 *
 * written by Paul Fulghum for Microgate Corporation
 * paulkf@microgate.com
 *
 * Microgate and SyncLink are trademarks of Microgate Corporation
 *
 * This code is released under the GNU General Public License (GPL)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * DEBUG OUTPUT DEFINITIONS
 *
 * uncomment lines below to enable specific types of debug output
 *
 * DBGINFO   information - most verbose output
 * DBGERR    serious errors
 * DBGBH     bottom half service routine debugging
 * DBGISR    interrupt service routine debugging
 * DBGDATA   output receive and transmit data
 * DBGTBUF   output transmit DMA buffers and registers
 * DBGRBUF   output receive DMA buffers and registers
 */

#define DBGINFO(fmt) if (debug_level >= DEBUG_LEVEL_INFO) printk fmt
#define DBGERR(fmt) if (debug_level >= DEBUG_LEVEL_ERROR) printk fmt
#define DBGBH(fmt) if (debug_level >= DEBUG_LEVEL_BH) printk fmt
#define DBGISR(fmt) if (debug_level >= DEBUG_LEVEL_ISR) printk fmt
#define DBGDATA(info, buf, size, label) if (debug_level >= DEBUG_LEVEL_DATA) trace_block((info), (buf), (size), (label))
//#define DBGTBUF(info) dump_tbufs(info)
//#define DBGRBUF(info) dump_rbufs(info)


#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/ioctl.h>
#include <linux/termios.h>
#include <linux/bitops.h>
#include <linux/workqueue.h>
#include <linux/hdlc.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/dma.h>
#include <asm/types.h>
#include <asm/uaccess.h>

#include "linux/synclink.h"

#ifdef CONFIG_HDLC_MODULE
#define CONFIG_HDLC 1
#endif

/*
 * module identification
 */
static char *driver_name     = "SyncLink GT";
static char *driver_version  = "$Revision: 4.22 $";
static char *tty_driver_name = "synclink_gt";
static char *tty_dev_prefix  = "ttySLG";
MODULE_LICENSE("GPL");
#define MGSL_MAGIC 0x5401
#define MAX_DEVICES 12

static struct pci_device_id pci_table[] = {
	{PCI_VENDOR_ID_MICROGATE, SYNCLINK_GT_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID,},
	{PCI_VENDOR_ID_MICROGATE, SYNCLINK_GT4_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID,},
	{PCI_VENDOR_ID_MICROGATE, SYNCLINK_AC_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID,},
	{0,}, /* terminate list */
};
MODULE_DEVICE_TABLE(pci, pci_table);

static int  init_one(struct pci_dev *dev,const struct pci_device_id *ent);
static void remove_one(struct pci_dev *dev);
static struct pci_driver pci_driver = {
	.name		= "synclink_gt",
	.id_table	= pci_table,
	.probe		= init_one,
	.remove		= __devexit_p(remove_one),
};

static int pci_registered;

/*
 * module configuration and status
 */
static struct slgt_info *slgt_device_list;
static int slgt_device_count;

static int ttymajor;
static int debug_level;
static int maxframe[MAX_DEVICES];
static int dosyncppp[MAX_DEVICES];

module_param(ttymajor, int, 0);
module_param(debug_level, int, 0);
module_param_array(maxframe, int, NULL, 0);
module_param_array(dosyncppp, int, NULL, 0);

MODULE_PARM_DESC(ttymajor, "TTY major device number override: 0=auto assigned");
MODULE_PARM_DESC(debug_level, "Debug syslog output: 0=disabled, 1 to 5=increasing detail");
MODULE_PARM_DESC(maxframe, "Maximum frame size used by device (4096 to 65535)");
MODULE_PARM_DESC(dosyncppp, "Enable synchronous net device, 0=disable 1=enable");

/*
 * tty support and callbacks
 */
#define RELEVANT_IFLAG(iflag) (iflag & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

static struct tty_driver *serial_driver;

static int  open(struct tty_struct *tty, struct file * filp);
static void close(struct tty_struct *tty, struct file * filp);
static void hangup(struct tty_struct *tty);
static void set_termios(struct tty_struct *tty, struct termios *old_termios);

static int  write(struct tty_struct *tty, const unsigned char *buf, int count);
static void put_char(struct tty_struct *tty, unsigned char ch);
static void send_xchar(struct tty_struct *tty, char ch);
static void wait_until_sent(struct tty_struct *tty, int timeout);
static int  write_room(struct tty_struct *tty);
static void flush_chars(struct tty_struct *tty);
static void flush_buffer(struct tty_struct *tty);
static void tx_hold(struct tty_struct *tty);
static void tx_release(struct tty_struct *tty);

static int  ioctl(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg);
static int  read_proc(char *page, char **start, off_t off, int count,int *eof, void *data);
static int  chars_in_buffer(struct tty_struct *tty);
static void throttle(struct tty_struct * tty);
static void unthrottle(struct tty_struct * tty);
static void set_break(struct tty_struct *tty, int break_state);

/*
 * generic HDLC support and callbacks
 */
#ifdef CONFIG_HDLC
#define dev_to_port(D) (dev_to_hdlc(D)->priv)
static void hdlcdev_tx_done(struct slgt_info *info);
static void hdlcdev_rx(struct slgt_info *info, char *buf, int size);
static int  hdlcdev_init(struct slgt_info *info);
static void hdlcdev_exit(struct slgt_info *info);
#endif


/*
 * device specific structures, macros and functions
 */

#define SLGT_MAX_PORTS 4
#define SLGT_REG_SIZE  256

/*
 * DMA buffer descriptor and access macros
 */
struct slgt_desc
{
	unsigned short count;
	unsigned short status;
	unsigned int pbuf;  /* physical address of data buffer */
	unsigned int next;  /* physical address of next descriptor */

	/* driver book keeping */
	char *buf;          /* virtual  address of data buffer */
    	unsigned int pdesc; /* physical address of this descriptor */
	dma_addr_t buf_dma_addr;
};

#define set_desc_buffer(a,b) (a).pbuf = cpu_to_le32((unsigned int)(b))
#define set_desc_next(a,b) (a).next   = cpu_to_le32((unsigned int)(b))
#define set_desc_count(a,b)(a).count  = cpu_to_le16((unsigned short)(b))
#define set_desc_eof(a,b)  (a).status = cpu_to_le16((b) ? (le16_to_cpu((a).status) | BIT0) : (le16_to_cpu((a).status) & ~BIT0))
#define desc_count(a)      (le16_to_cpu((a).count))
#define desc_status(a)     (le16_to_cpu((a).status))
#define desc_complete(a)   (le16_to_cpu((a).status) & BIT15)
#define desc_eof(a)        (le16_to_cpu((a).status) & BIT2)
#define desc_crc_error(a)  (le16_to_cpu((a).status) & BIT1)
#define desc_abort(a)      (le16_to_cpu((a).status) & BIT0)
#define desc_residue(a)    ((le16_to_cpu((a).status) & 0x38) >> 3)

struct _input_signal_events {
	int ri_up;
	int ri_down;
	int dsr_up;
	int dsr_down;
	int dcd_up;
	int dcd_down;
	int cts_up;
	int cts_down;
};

/*
 * device instance data structure
 */
struct slgt_info {
	void *if_ptr;		/* General purpose pointer (used by SPPP) */

	struct slgt_info *next_device;	/* device list link */

	int magic;
	int flags;

	char device_name[25];
	struct pci_dev *pdev;

	int port_count;  /* count of ports on adapter */
	int adapter_num; /* adapter instance number */
	int port_num;    /* port instance number */

	/* array of pointers to port contexts on this adapter */
	struct slgt_info *port_array[SLGT_MAX_PORTS];

	int			count;		/* count of opens */
	int			line;		/* tty line instance number */
	unsigned short		close_delay;
	unsigned short		closing_wait;	/* time to wait before closing */

	struct mgsl_icount	icount;

	struct tty_struct 	*tty;
	int			timeout;
	int			x_char;		/* xon/xoff character */
	int			blocked_open;	/* # of blocked opens */
	unsigned int		read_status_mask;
	unsigned int 		ignore_status_mask;

	wait_queue_head_t	open_wait;
	wait_queue_head_t	close_wait;

	wait_queue_head_t	status_event_wait_q;
	wait_queue_head_t	event_wait_q;
	struct timer_list	tx_timer;
	struct timer_list	rx_timer;

	spinlock_t lock;	/* spinlock for synchronizing with ISR */

	struct work_struct task;
	u32 pending_bh;
	int bh_requested;
	int bh_running;

	int isr_overflow;
	int irq_requested;	/* nonzero if IRQ requested */
	int irq_occurred;	/* for diagnostics use */

	/* device configuration */

	unsigned int bus_type;
	unsigned int irq_level;
	unsigned long irq_flags;

	unsigned char __iomem * reg_addr;  /* memory mapped registers address */
	u32 phys_reg_addr;
	int reg_addr_requested;

	MGSL_PARAMS params;       /* communications parameters */
	u32 idle_mode;
	u32 max_frame_size;       /* as set by device config */

	unsigned int raw_rx_size;
	unsigned int if_mode;

	/* device status */

	int rx_enabled;
	int rx_restart;

	int tx_enabled;
	int tx_active;

	unsigned char signals;    /* serial signal states */
	unsigned int init_error;  /* initialization error */

	unsigned char *tx_buf;
	int tx_count;

	char flag_buf[MAX_ASYNC_BUFFER_SIZE];
	char char_buf[MAX_ASYNC_BUFFER_SIZE];
	BOOLEAN drop_rts_on_tx_done;
	struct	_input_signal_events	input_signal_events;

	int dcd_chkcount;	/* check counts to prevent */
	int cts_chkcount;	/* too many IRQs if a signal */
	int dsr_chkcount;	/* is floating */
	int ri_chkcount;

	char *bufs;		/* virtual address of DMA buffer lists */
	dma_addr_t bufs_dma_addr; /* physical address of buffer descriptors */

	unsigned int rbuf_count;
	struct slgt_desc *rbufs;
	unsigned int rbuf_current;
	unsigned int rbuf_index;

	unsigned int tbuf_count;
	struct slgt_desc *tbufs;
	unsigned int tbuf_current;
	unsigned int tbuf_start;

	unsigned char *tmp_rbuf;
	unsigned int tmp_rbuf_count;

	/* SPPP/Cisco HDLC device parts */

	int netcount;
	int dosyncppp;
	spinlock_t netlock;
#ifdef CONFIG_HDLC
	struct net_device *netdev;
#endif

};

static MGSL_PARAMS default_params = {
	.mode            = MGSL_MODE_HDLC,
	.loopback        = 0,
	.flags           = HDLC_FLAG_UNDERRUN_ABORT15,
	.encoding        = HDLC_ENCODING_NRZI_SPACE,
	.clock_speed     = 0,
	.addr_filter     = 0xff,
	.crc_type        = HDLC_CRC_16_CCITT,
	.preamble_length = HDLC_PREAMBLE_LENGTH_8BITS,
	.preamble        = HDLC_PREAMBLE_PATTERN_NONE,
	.data_rate       = 9600,
	.data_bits       = 8,
	.stop_bits       = 1,
	.parity          = ASYNC_PARITY_NONE
};


#define BH_RECEIVE  1
#define BH_TRANSMIT 2
#define BH_STATUS   4
#define IO_PIN_SHUTDOWN_LIMIT 100

#define DMABUFSIZE 256
#define DESC_LIST_SIZE 4096

#define MASK_PARITY  BIT1
#define MASK_FRAMING BIT2
#define MASK_BREAK   BIT3
#define MASK_OVERRUN BIT4

#define GSR   0x00 /* global status */
#define TDR   0x80 /* tx data */
#define RDR   0x80 /* rx data */
#define TCR   0x82 /* tx control */
#define TIR   0x84 /* tx idle */
#define TPR   0x85 /* tx preamble */
#define RCR   0x86 /* rx control */
#define VCR   0x88 /* V.24 control */
#define CCR   0x89 /* clock control */
#define BDR   0x8a /* baud divisor */
#define SCR   0x8c /* serial control */
#define SSR   0x8e /* serial status */
#define RDCSR 0x90 /* rx DMA control/status */
#define TDCSR 0x94 /* tx DMA control/status */
#define RDDAR 0x98 /* rx DMA descriptor address */
#define TDDAR 0x9c /* tx DMA descriptor address */

#define RXIDLE      BIT14
#define RXBREAK     BIT14
#define IRQ_TXDATA  BIT13
#define IRQ_TXIDLE  BIT12
#define IRQ_TXUNDER BIT11 /* HDLC */
#define IRQ_RXDATA  BIT10
#define IRQ_RXIDLE  BIT9  /* HDLC */
#define IRQ_RXBREAK BIT9  /* async */
#define IRQ_RXOVER  BIT8
#define IRQ_DSR     BIT7
#define IRQ_CTS     BIT6
#define IRQ_DCD     BIT5
#define IRQ_RI      BIT4
#define IRQ_ALL     0x3ff0
#define IRQ_MASTER  BIT0

#define slgt_irq_on(info, mask) \
	wr_reg16((info), SCR, (unsigned short)(rd_reg16((info), SCR) | (mask)))
#define slgt_irq_off(info, mask) \
	wr_reg16((info), SCR, (unsigned short)(rd_reg16((info), SCR) & ~(mask)))

static __u8  rd_reg8(struct slgt_info *info, unsigned int addr);
static void  wr_reg8(struct slgt_info *info, unsigned int addr, __u8 value);
static __u16 rd_reg16(struct slgt_info *info, unsigned int addr);
static void  wr_reg16(struct slgt_info *info, unsigned int addr, __u16 value);
static __u32 rd_reg32(struct slgt_info *info, unsigned int addr);
static void  wr_reg32(struct slgt_info *info, unsigned int addr, __u32 value);

static void  msc_set_vcr(struct slgt_info *info);

static int  startup(struct slgt_info *info);
static int  block_til_ready(struct tty_struct *tty, struct file * filp,struct slgt_info *info);
static void shutdown(struct slgt_info *info);
static void program_hw(struct slgt_info *info);
static void change_params(struct slgt_info *info);

static int  register_test(struct slgt_info *info);
static int  irq_test(struct slgt_info *info);
static int  loopback_test(struct slgt_info *info);
static int  adapter_test(struct slgt_info *info);

static void reset_adapter(struct slgt_info *info);
static void reset_port(struct slgt_info *info);
static void async_mode(struct slgt_info *info);
static void hdlc_mode(struct slgt_info *info);

static void rx_stop(struct slgt_info *info);
static void rx_start(struct slgt_info *info);
static void reset_rbufs(struct slgt_info *info);
static void free_rbufs(struct slgt_info *info, unsigned int first, unsigned int last);
static void rdma_reset(struct slgt_info *info);
static int  rx_get_frame(struct slgt_info *info);
static int  rx_get_buf(struct slgt_info *info);

static void tx_start(struct slgt_info *info);
static void tx_stop(struct slgt_info *info);
static void tx_set_idle(struct slgt_info *info);
static unsigned int free_tbuf_count(struct slgt_info *info);
static void reset_tbufs(struct slgt_info *info);
static void tdma_reset(struct slgt_info *info);
static void tx_load(struct slgt_info *info, const char *buf, unsigned int count);

static void get_signals(struct slgt_info *info);
static void set_signals(struct slgt_info *info);
static void enable_loopback(struct slgt_info *info);
static void set_rate(struct slgt_info *info, u32 data_rate);

static int  bh_action(struct slgt_info *info);
static void bh_handler(void* context);
static void bh_transmit(struct slgt_info *info);
static void isr_serial(struct slgt_info *info);
static void isr_rdma(struct slgt_info *info);
static void isr_txeom(struct slgt_info *info, unsigned short status);
static void isr_tdma(struct slgt_info *info);
static irqreturn_t slgt_interrupt(int irq, void *dev_id, struct pt_regs * regs);

static int  alloc_dma_bufs(struct slgt_info *info);
static void free_dma_bufs(struct slgt_info *info);
static int  alloc_desc(struct slgt_info *info);
static void free_desc(struct slgt_info *info);
static int  alloc_bufs(struct slgt_info *info, struct slgt_desc *bufs, int count);
static void free_bufs(struct slgt_info *info, struct slgt_desc *bufs, int count);

static int  alloc_tmp_rbuf(struct slgt_info *info);
static void free_tmp_rbuf(struct slgt_info *info);

static void tx_timeout(unsigned long context);
static void rx_timeout(unsigned long context);

/*
 * ioctl handlers
 */
static int  get_stats(struct slgt_info *info, struct mgsl_icount __user *user_icount);
static int  get_params(struct slgt_info *info, MGSL_PARAMS __user *params);
static int  set_params(struct slgt_info *info, MGSL_PARAMS __user *params);
static int  get_txidle(struct slgt_info *info, int __user *idle_mode);
static int  set_txidle(struct slgt_info *info, int idle_mode);
static int  tx_enable(struct slgt_info *info, int enable);
static int  tx_abort(struct slgt_info *info);
static int  rx_enable(struct slgt_info *info, int enable);
static int  modem_input_wait(struct slgt_info *info,int arg);
static int  wait_mgsl_event(struct slgt_info *info, int __user *mask_ptr);
static int  tiocmget(struct tty_struct *tty, struct file *file);
static int  tiocmset(struct tty_struct *tty, struct file *file,
		     unsigned int set, unsigned int clear);
static void set_break(struct tty_struct *tty, int break_state);
static int  get_interface(struct slgt_info *info, int __user *if_mode);
static int  set_interface(struct slgt_info *info, int if_mode);

/*
 * driver functions
 */
static void add_device(struct slgt_info *info);
static void device_init(int adapter_num, struct pci_dev *pdev);
static int  claim_resources(struct slgt_info *info);
static void release_resources(struct slgt_info *info);

/*
 * DEBUG OUTPUT CODE
 */
#ifndef DBGINFO
#define DBGINFO(fmt)
#endif
#ifndef DBGERR
#define DBGERR(fmt)
#endif
#ifndef DBGBH
#define DBGBH(fmt)
#endif
#ifndef DBGISR
#define DBGISR(fmt)
#endif

#ifdef DBGDATA
static void trace_block(struct slgt_info *info, const char *data, int count, const char *label)
{
	int i;
	int linecount;
	printk("%s %s data:\n",info->device_name, label);
	while(count) {
		linecount = (count > 16) ? 16 : count;
		for(i=0; i < linecount; i++)
			printk("%02X ",(unsigned char)data[i]);
		for(;i<17;i++)
			printk("   ");
		for(i=0;i<linecount;i++) {
			if (data[i]>=040 && data[i]<=0176)
				printk("%c",data[i]);
			else
				printk(".");
		}
		printk("\n");
		data  += linecount;
		count -= linecount;
	}
}
#else
#define DBGDATA(info, buf, size, label)
#endif

#ifdef DBGTBUF
static void dump_tbufs(struct slgt_info *info)
{
	int i;
	printk("tbuf_current=%d\n", info->tbuf_current);
	for (i=0 ; i < info->tbuf_count ; i++) {
		printk("%d: count=%04X status=%04X\n",
			i, le16_to_cpu(info->tbufs[i].count), le16_to_cpu(info->tbufs[i].status));
	}
}
#else
#define DBGTBUF(info)
#endif

#ifdef DBGRBUF
static void dump_rbufs(struct slgt_info *info)
{
	int i;
	printk("rbuf_current=%d\n", info->rbuf_current);
	for (i=0 ; i < info->rbuf_count ; i++) {
		printk("%d: count=%04X status=%04X\n",
			i, le16_to_cpu(info->rbufs[i].count), le16_to_cpu(info->rbufs[i].status));
	}
}
#else
#define DBGRBUF(info)
#endif

static inline int sanity_check(struct slgt_info *info, char *devname, const char *name)
{
#ifdef SANITY_CHECK
	if (!info) {
		printk("null struct slgt_info for (%s) in %s\n", devname, name);
		return 1;
	}
	if (info->magic != MGSL_MAGIC) {
		printk("bad magic number struct slgt_info (%s) in %s\n", devname, name);
		return 1;
	}
#else
	if (!info)
		return 1;
#endif
	return 0;
}

/**
 * line discipline callback wrappers
 *
 * The wrappers maintain line discipline references
 * while calling into the line discipline.
 *
 * ldisc_receive_buf  - pass receive data to line discipline
 */
static void ldisc_receive_buf(struct tty_struct *tty,
			      const __u8 *data, char *flags, int count)
{
	struct tty_ldisc *ld;
	if (!tty)
		return;
	ld = tty_ldisc_ref(tty);
	if (ld) {
		if (ld->receive_buf)
			ld->receive_buf(tty, data, flags, count);
		tty_ldisc_deref(ld);
	}
}

/* tty callbacks */

static int open(struct tty_struct *tty, struct file *filp)
{
	struct slgt_info *info;
	int retval, line;
	unsigned long flags;

	line = tty->index;
	if ((line < 0) || (line >= slgt_device_count)) {
		DBGERR(("%s: open with invalid line #%d.\n", driver_name, line));
		return -ENODEV;
	}

	info = slgt_device_list;
	while(info && info->line != line)
		info = info->next_device;
	if (sanity_check(info, tty->name, "open"))
		return -ENODEV;
	if (info->init_error) {
		DBGERR(("%s init error=%d\n", info->device_name, info->init_error));
		return -ENODEV;
	}

	tty->driver_data = info;
	info->tty = tty;

	DBGINFO(("%s open, old ref count = %d\n", info->device_name, info->count));

	/* If port is closing, signal caller to try again */
	if (tty_hung_up_p(filp) || info->flags & ASYNC_CLOSING){
		if (info->flags & ASYNC_CLOSING)
			interruptible_sleep_on(&info->close_wait);
		retval = ((info->flags & ASYNC_HUP_NOTIFY) ?
			-EAGAIN : -ERESTARTSYS);
		goto cleanup;
	}

	info->tty->low_latency = (info->flags & ASYNC_LOW_LATENCY) ? 1 : 0;

	spin_lock_irqsave(&info->netlock, flags);
	if (info->netcount) {
		retval = -EBUSY;
		spin_unlock_irqrestore(&info->netlock, flags);
		goto cleanup;
	}
	info->count++;
	spin_unlock_irqrestore(&info->netlock, flags);

	if (info->count == 1) {
		/* 1st open on this device, init hardware */
		retval = startup(info);
		if (retval < 0)
			goto cleanup;
	}

	retval = block_til_ready(tty, filp, info);
	if (retval) {
		DBGINFO(("%s block_til_ready rc=%d\n", info->device_name, retval));
		goto cleanup;
	}

	retval = 0;

cleanup:
	if (retval) {
		if (tty->count == 1)
			info->tty = NULL; /* tty layer will release tty struct */
		if(info->count)
			info->count--;
	}

	DBGINFO(("%s open rc=%d\n", info->device_name, retval));
	return retval;
}

static void close(struct tty_struct *tty, struct file *filp)
{
	struct slgt_info *info = tty->driver_data;

	if (sanity_check(info, tty->name, "close"))
		return;
	DBGINFO(("%s close entry, count=%d\n", info->device_name, info->count));

	if (!info->count)
		return;

	if (tty_hung_up_p(filp))
		goto cleanup;

	if ((tty->count == 1) && (info->count != 1)) {
		/*
		 * tty->count is 1 and the tty structure will be freed.
		 * info->count should be one in this case.
		 * if it's not, correct it so that the port is shutdown.
		 */
		DBGERR(("%s close: bad refcount; tty->count=1, "
		       "info->count=%d\n", info->device_name, info->count));
		info->count = 1;
	}

	info->count--;

	/* if at least one open remaining, leave hardware active */
	if (info->count)
		goto cleanup;

	info->flags |= ASYNC_CLOSING;

	/* set tty->closing to notify line discipline to
	 * only process XON/XOFF characters. Only the N_TTY
	 * discipline appears to use this (ppp does not).
	 */
	tty->closing = 1;

	/* wait for transmit data to clear all layers */

	if (info->closing_wait != ASYNC_CLOSING_WAIT_NONE) {
		DBGINFO(("%s call tty_wait_until_sent\n", info->device_name));
		tty_wait_until_sent(tty, info->closing_wait);
	}

 	if (info->flags & ASYNC_INITIALIZED)
 		wait_until_sent(tty, info->timeout);
	if (tty->driver->flush_buffer)
		tty->driver->flush_buffer(tty);
	tty_ldisc_flush(tty);

	shutdown(info);

	tty->closing = 0;
	info->tty = NULL;

	if (info->blocked_open) {
		if (info->close_delay) {
			msleep_interruptible(jiffies_to_msecs(info->close_delay));
		}
		wake_up_interruptible(&info->open_wait);
	}

	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CLOSING);

	wake_up_interruptible(&info->close_wait);

cleanup:
	DBGINFO(("%s close exit, count=%d\n", tty->driver->name, info->count));
}

static void hangup(struct tty_struct *tty)
{
	struct slgt_info *info = tty->driver_data;

	if (sanity_check(info, tty->name, "hangup"))
		return;
	DBGINFO(("%s hangup\n", info->device_name));

	flush_buffer(tty);
	shutdown(info);

	info->count = 0;
	info->flags &= ~ASYNC_NORMAL_ACTIVE;
	info->tty = NULL;

	wake_up_interruptible(&info->open_wait);
}

static void set_termios(struct tty_struct *tty, struct termios *old_termios)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	DBGINFO(("%s set_termios\n", tty->driver->name));

	/* just return if nothing has changed */
	if ((tty->termios->c_cflag == old_termios->c_cflag)
	    && (RELEVANT_IFLAG(tty->termios->c_iflag)
		== RELEVANT_IFLAG(old_termios->c_iflag)))
		return;

	change_params(info);

	/* Handle transition to B0 status */
	if (old_termios->c_cflag & CBAUD &&
	    !(tty->termios->c_cflag & CBAUD)) {
		info->signals &= ~(SerialSignal_RTS + SerialSignal_DTR);
		spin_lock_irqsave(&info->lock,flags);
		set_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}

	/* Handle transition away from B0 status */
	if (!(old_termios->c_cflag & CBAUD) &&
	    tty->termios->c_cflag & CBAUD) {
		info->signals |= SerialSignal_DTR;
 		if (!(tty->termios->c_cflag & CRTSCTS) ||
 		    !test_bit(TTY_THROTTLED, &tty->flags)) {
			info->signals |= SerialSignal_RTS;
 		}
		spin_lock_irqsave(&info->lock,flags);
	 	set_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}

	/* Handle turning off CRTSCTS */
	if (old_termios->c_cflag & CRTSCTS &&
	    !(tty->termios->c_cflag & CRTSCTS)) {
		tty->hw_stopped = 0;
		tx_release(tty);
	}
}

static int write(struct tty_struct *tty,
		 const unsigned char *buf, int count)
{
	int ret = 0;
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "write"))
		goto cleanup;
	DBGINFO(("%s write count=%d\n", info->device_name, count));

	if (!tty || !info->tx_buf)
		goto cleanup;

	if (count > info->max_frame_size) {
		ret = -EIO;
		goto cleanup;
	}

	if (!count)
		goto cleanup;

	if (info->params.mode == MGSL_MODE_RAW) {
		unsigned int bufs_needed = (count/DMABUFSIZE);
		unsigned int bufs_free = free_tbuf_count(info);
		if (count % DMABUFSIZE)
			++bufs_needed;
		if (bufs_needed > bufs_free)
			goto cleanup;
	} else {
		if (info->tx_active)
			goto cleanup;
		if (info->tx_count) {
			/* send accumulated data from send_char() calls */
			/* as frame and wait before accepting more data. */
			tx_load(info, info->tx_buf, info->tx_count);
			goto start;
		}
	}

	ret = info->tx_count = count;
	tx_load(info, buf, count);
	goto start;

start:
 	if (info->tx_count && !tty->stopped && !tty->hw_stopped) {
		spin_lock_irqsave(&info->lock,flags);
		if (!info->tx_active)
		 	tx_start(info);
		spin_unlock_irqrestore(&info->lock,flags);
 	}

cleanup:
	DBGINFO(("%s write rc=%d\n", info->device_name, ret));
	return ret;
}

static void put_char(struct tty_struct *tty, unsigned char ch)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "put_char"))
		return;
	DBGINFO(("%s put_char(%d)\n", info->device_name, ch));
	if (!tty || !info->tx_buf)
		return;
	spin_lock_irqsave(&info->lock,flags);
	if (!info->tx_active && (info->tx_count < info->max_frame_size))
		info->tx_buf[info->tx_count++] = ch;
	spin_unlock_irqrestore(&info->lock,flags);
}

static void send_xchar(struct tty_struct *tty, char ch)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "send_xchar"))
		return;
	DBGINFO(("%s send_xchar(%d)\n", info->device_name, ch));
	info->x_char = ch;
	if (ch) {
		spin_lock_irqsave(&info->lock,flags);
		if (!info->tx_enabled)
		 	tx_start(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}
}

static void wait_until_sent(struct tty_struct *tty, int timeout)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long orig_jiffies, char_time;

	if (!info )
		return;
	if (sanity_check(info, tty->name, "wait_until_sent"))
		return;
	DBGINFO(("%s wait_until_sent entry\n", info->device_name));
	if (!(info->flags & ASYNC_INITIALIZED))
		goto exit;

	orig_jiffies = jiffies;

	/* Set check interval to 1/5 of estimated time to
	 * send a character, and make it at least 1. The check
	 * interval should also be less than the timeout.
	 * Note: use tight timings here to satisfy the NIST-PCTS.
	 */

	if (info->params.data_rate) {
	       	char_time = info->timeout/(32 * 5);
		if (!char_time)
			char_time++;
	} else
		char_time = 1;

	if (timeout)
		char_time = min_t(unsigned long, char_time, timeout);

	while (info->tx_active) {
		msleep_interruptible(jiffies_to_msecs(char_time));
		if (signal_pending(current))
			break;
		if (timeout && time_after(jiffies, orig_jiffies + timeout))
			break;
	}

exit:
	DBGINFO(("%s wait_until_sent exit\n", info->device_name));
}

static int write_room(struct tty_struct *tty)
{
	struct slgt_info *info = tty->driver_data;
	int ret;

	if (sanity_check(info, tty->name, "write_room"))
		return 0;
	ret = (info->tx_active) ? 0 : HDLC_MAX_FRAME_SIZE;
	DBGINFO(("%s write_room=%d\n", info->device_name, ret));
	return ret;
}

static void flush_chars(struct tty_struct *tty)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "flush_chars"))
		return;
	DBGINFO(("%s flush_chars entry tx_count=%d\n", info->device_name, info->tx_count));

	if (info->tx_count <= 0 || tty->stopped ||
	    tty->hw_stopped || !info->tx_buf)
		return;

	DBGINFO(("%s flush_chars start transmit\n", info->device_name));

	spin_lock_irqsave(&info->lock,flags);
	if (!info->tx_active && info->tx_count) {
		tx_load(info, info->tx_buf,info->tx_count);
	 	tx_start(info);
	}
	spin_unlock_irqrestore(&info->lock,flags);
}

static void flush_buffer(struct tty_struct *tty)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "flush_buffer"))
		return;
	DBGINFO(("%s flush_buffer\n", info->device_name));

	spin_lock_irqsave(&info->lock,flags);
	if (!info->tx_active)
		info->tx_count = 0;
	spin_unlock_irqrestore(&info->lock,flags);

	wake_up_interruptible(&tty->write_wait);
	tty_wakeup(tty);
}

/*
 * throttle (stop) transmitter
 */
static void tx_hold(struct tty_struct *tty)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "tx_hold"))
		return;
	DBGINFO(("%s tx_hold\n", info->device_name));
	spin_lock_irqsave(&info->lock,flags);
	if (info->tx_enabled && info->params.mode == MGSL_MODE_ASYNC)
	 	tx_stop(info);
	spin_unlock_irqrestore(&info->lock,flags);
}

/*
 * release (start) transmitter
 */
static void tx_release(struct tty_struct *tty)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "tx_release"))
		return;
	DBGINFO(("%s tx_release\n", info->device_name));
	spin_lock_irqsave(&info->lock,flags);
	if (!info->tx_active && info->tx_count) {
		tx_load(info, info->tx_buf, info->tx_count);
	 	tx_start(info);
	}
	spin_unlock_irqrestore(&info->lock,flags);
}

/*
 * Service an IOCTL request
 *
 * Arguments
 *
 * 	tty	pointer to tty instance data
 * 	file	pointer to associated file object for device
 * 	cmd	IOCTL command code
 * 	arg	command argument/context
 *
 * Return 0 if success, otherwise error code
 */
static int ioctl(struct tty_struct *tty, struct file *file,
		 unsigned int cmd, unsigned long arg)
{
	struct slgt_info *info = tty->driver_data;
	struct mgsl_icount cnow;	/* kernel counter temps */
	struct serial_icounter_struct __user *p_cuser;	/* user space */
	unsigned long flags;
	void __user *argp = (void __user *)arg;

	if (sanity_check(info, tty->name, "ioctl"))
		return -ENODEV;
	DBGINFO(("%s ioctl() cmd=%08X\n", info->device_name, cmd));

	if ((cmd != TIOCGSERIAL) && (cmd != TIOCSSERIAL) &&
	    (cmd != TIOCMIWAIT) && (cmd != TIOCGICOUNT)) {
		if (tty->flags & (1 << TTY_IO_ERROR))
		    return -EIO;
	}

	switch (cmd) {
	case MGSL_IOCGPARAMS:
		return get_params(info, argp);
	case MGSL_IOCSPARAMS:
		return set_params(info, argp);
	case MGSL_IOCGTXIDLE:
		return get_txidle(info, argp);
	case MGSL_IOCSTXIDLE:
		return set_txidle(info, (int)arg);
	case MGSL_IOCTXENABLE:
		return tx_enable(info, (int)arg);
	case MGSL_IOCRXENABLE:
		return rx_enable(info, (int)arg);
	case MGSL_IOCTXABORT:
		return tx_abort(info);
	case MGSL_IOCGSTATS:
		return get_stats(info, argp);
	case MGSL_IOCWAITEVENT:
		return wait_mgsl_event(info, argp);
	case TIOCMIWAIT:
		return modem_input_wait(info,(int)arg);
	case MGSL_IOCGIF:
		return get_interface(info, argp);
	case MGSL_IOCSIF:
		return set_interface(info,(int)arg);
	case TIOCGICOUNT:
		spin_lock_irqsave(&info->lock,flags);
		cnow = info->icount;
		spin_unlock_irqrestore(&info->lock,flags);
		p_cuser = argp;
		if (put_user(cnow.cts, &p_cuser->cts) ||
		    put_user(cnow.dsr, &p_cuser->dsr) ||
		    put_user(cnow.rng, &p_cuser->rng) ||
		    put_user(cnow.dcd, &p_cuser->dcd) ||
		    put_user(cnow.rx, &p_cuser->rx) ||
		    put_user(cnow.tx, &p_cuser->tx) ||
		    put_user(cnow.frame, &p_cuser->frame) ||
		    put_user(cnow.overrun, &p_cuser->overrun) ||
		    put_user(cnow.parity, &p_cuser->parity) ||
		    put_user(cnow.brk, &p_cuser->brk) ||
		    put_user(cnow.buf_overrun, &p_cuser->buf_overrun))
			return -EFAULT;
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

/*
 * proc fs support
 */
static inline int line_info(char *buf, struct slgt_info *info)
{
	char stat_buf[30];
	int ret;
	unsigned long flags;

	ret = sprintf(buf, "%s: IO=%08X IRQ=%d MaxFrameSize=%u\n",
		      info->device_name, info->phys_reg_addr,
		      info->irq_level, info->max_frame_size);

	/* output current serial signal states */
	spin_lock_irqsave(&info->lock,flags);
	get_signals(info);
	spin_unlock_irqrestore(&info->lock,flags);

	stat_buf[0] = 0;
	stat_buf[1] = 0;
	if (info->signals & SerialSignal_RTS)
		strcat(stat_buf, "|RTS");
	if (info->signals & SerialSignal_CTS)
		strcat(stat_buf, "|CTS");
	if (info->signals & SerialSignal_DTR)
		strcat(stat_buf, "|DTR");
	if (info->signals & SerialSignal_DSR)
		strcat(stat_buf, "|DSR");
	if (info->signals & SerialSignal_DCD)
		strcat(stat_buf, "|CD");
	if (info->signals & SerialSignal_RI)
		strcat(stat_buf, "|RI");

	if (info->params.mode != MGSL_MODE_ASYNC) {
		ret += sprintf(buf+ret, "\tHDLC txok:%d rxok:%d",
			       info->icount.txok, info->icount.rxok);
		if (info->icount.txunder)
			ret += sprintf(buf+ret, " txunder:%d", info->icount.txunder);
		if (info->icount.txabort)
			ret += sprintf(buf+ret, " txabort:%d", info->icount.txabort);
		if (info->icount.rxshort)
			ret += sprintf(buf+ret, " rxshort:%d", info->icount.rxshort);
		if (info->icount.rxlong)
			ret += sprintf(buf+ret, " rxlong:%d", info->icount.rxlong);
		if (info->icount.rxover)
			ret += sprintf(buf+ret, " rxover:%d", info->icount.rxover);
		if (info->icount.rxcrc)
			ret += sprintf(buf+ret, " rxcrc:%d", info->icount.rxcrc);
	} else {
		ret += sprintf(buf+ret, "\tASYNC tx:%d rx:%d",
			       info->icount.tx, info->icount.rx);
		if (info->icount.frame)
			ret += sprintf(buf+ret, " fe:%d", info->icount.frame);
		if (info->icount.parity)
			ret += sprintf(buf+ret, " pe:%d", info->icount.parity);
		if (info->icount.brk)
			ret += sprintf(buf+ret, " brk:%d", info->icount.brk);
		if (info->icount.overrun)
			ret += sprintf(buf+ret, " oe:%d", info->icount.overrun);
	}

	/* Append serial signal status to end */
	ret += sprintf(buf+ret, " %s\n", stat_buf+1);

	ret += sprintf(buf+ret, "\ttxactive=%d bh_req=%d bh_run=%d pending_bh=%x\n",
		       info->tx_active,info->bh_requested,info->bh_running,
		       info->pending_bh);

	return ret;
}

/* Called to print information about devices
 */
static int read_proc(char *page, char **start, off_t off, int count,
		     int *eof, void *data)
{
	int len = 0, l;
	off_t	begin = 0;
	struct slgt_info *info;

	len += sprintf(page, "synclink_gt driver:%s\n", driver_version);

	info = slgt_device_list;
	while( info ) {
		l = line_info(page + len, info);
		len += l;
		if (len+begin > off+count)
			goto done;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
		info = info->next_device;
	}

	*eof = 1;
done:
	if (off >= len+begin)
		return 0;
	*start = page + (off-begin);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * return count of bytes in transmit buffer
 */
static int chars_in_buffer(struct tty_struct *tty)
{
	struct slgt_info *info = tty->driver_data;
	if (sanity_check(info, tty->name, "chars_in_buffer"))
		return 0;
	DBGINFO(("%s chars_in_buffer()=%d\n", info->device_name, info->tx_count));
	return info->tx_count;
}

/*
 * signal remote device to throttle send data (our receive data)
 */
static void throttle(struct tty_struct * tty)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "throttle"))
		return;
	DBGINFO(("%s throttle\n", info->device_name));
	if (I_IXOFF(tty))
		send_xchar(tty, STOP_CHAR(tty));
 	if (tty->termios->c_cflag & CRTSCTS) {
		spin_lock_irqsave(&info->lock,flags);
		info->signals &= ~SerialSignal_RTS;
	 	set_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}
}

/*
 * signal remote device to stop throttling send data (our receive data)
 */
static void unthrottle(struct tty_struct * tty)
{
	struct slgt_info *info = tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->name, "unthrottle"))
		return;
	DBGINFO(("%s unthrottle\n", info->device_name));
	if (I_IXOFF(tty)) {
		if (info->x_char)
			info->x_char = 0;
		else
			send_xchar(tty, START_CHAR(tty));
	}
 	if (tty->termios->c_cflag & CRTSCTS) {
		spin_lock_irqsave(&info->lock,flags);
		info->signals |= SerialSignal_RTS;
	 	set_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}
}

/*
 * set or clear transmit break condition
 * break_state	-1=set break condition, 0=clear
 */
static void set_break(struct tty_struct *tty, int break_state)
{
	struct slgt_info *info = tty->driver_data;
	unsigned short value;
	unsigned long flags;

	if (sanity_check(info, tty->name, "set_break"))
		return;
	DBGINFO(("%s set_break(%d)\n", info->device_name, break_state));

	spin_lock_irqsave(&info->lock,flags);
	value = rd_reg16(info, TCR);
 	if (break_state == -1)
		value |= BIT6;
	else
		value &= ~BIT6;
	wr_reg16(info, TCR, value);
	spin_unlock_irqrestore(&info->lock,flags);
}

#ifdef CONFIG_HDLC

/**
 * called by generic HDLC layer when protocol selected (PPP, frame relay, etc.)
 * set encoding and frame check sequence (FCS) options
 *
 * dev       pointer to network device structure
 * encoding  serial encoding setting
 * parity    FCS setting
 *
 * returns 0 if success, otherwise error code
 */
static int hdlcdev_attach(struct net_device *dev, unsigned short encoding,
			  unsigned short parity)
{
	struct slgt_info *info = dev_to_port(dev);
	unsigned char  new_encoding;
	unsigned short new_crctype;

	/* return error if TTY interface open */
	if (info->count)
		return -EBUSY;

	DBGINFO(("%s hdlcdev_attach\n", info->device_name));

	switch (encoding)
	{
	case ENCODING_NRZ:        new_encoding = HDLC_ENCODING_NRZ; break;
	case ENCODING_NRZI:       new_encoding = HDLC_ENCODING_NRZI_SPACE; break;
	case ENCODING_FM_MARK:    new_encoding = HDLC_ENCODING_BIPHASE_MARK; break;
	case ENCODING_FM_SPACE:   new_encoding = HDLC_ENCODING_BIPHASE_SPACE; break;
	case ENCODING_MANCHESTER: new_encoding = HDLC_ENCODING_BIPHASE_LEVEL; break;
	default: return -EINVAL;
	}

	switch (parity)
	{
	case PARITY_NONE:            new_crctype = HDLC_CRC_NONE; break;
	case PARITY_CRC16_PR1_CCITT: new_crctype = HDLC_CRC_16_CCITT; break;
	case PARITY_CRC32_PR1_CCITT: new_crctype = HDLC_CRC_32_CCITT; break;
	default: return -EINVAL;
	}

	info->params.encoding = new_encoding;
	info->params.crc_type = new_crctype;;

	/* if network interface up, reprogram hardware */
	if (info->netcount)
		program_hw(info);

	return 0;
}

/**
 * called by generic HDLC layer to send frame
 *
 * skb  socket buffer containing HDLC frame
 * dev  pointer to network device structure
 *
 * returns 0 if success, otherwise error code
 */
static int hdlcdev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct slgt_info *info = dev_to_port(dev);
	struct net_device_stats *stats = hdlc_stats(dev);
	unsigned long flags;

	DBGINFO(("%s hdlc_xmit\n", dev->name));

	/* stop sending until this frame completes */
	netif_stop_queue(dev);

	/* copy data to device buffers */
	info->tx_count = skb->len;
	tx_load(info, skb->data, skb->len);

	/* update network statistics */
	stats->tx_packets++;
	stats->tx_bytes += skb->len;

	/* done with socket buffer, so free it */
	dev_kfree_skb(skb);

	/* save start time for transmit timeout detection */
	dev->trans_start = jiffies;

	/* start hardware transmitter if necessary */
	spin_lock_irqsave(&info->lock,flags);
	if (!info->tx_active)
	 	tx_start(info);
	spin_unlock_irqrestore(&info->lock,flags);

	return 0;
}

/**
 * called by network layer when interface enabled
 * claim resources and initialize hardware
 *
 * dev  pointer to network device structure
 *
 * returns 0 if success, otherwise error code
 */
static int hdlcdev_open(struct net_device *dev)
{
	struct slgt_info *info = dev_to_port(dev);
	int rc;
	unsigned long flags;

	DBGINFO(("%s hdlcdev_open\n", dev->name));

	/* generic HDLC layer open processing */
	if ((rc = hdlc_open(dev)))
		return rc;

	/* arbitrate between network and tty opens */
	spin_lock_irqsave(&info->netlock, flags);
	if (info->count != 0 || info->netcount != 0) {
		DBGINFO(("%s hdlc_open busy\n", dev->name));
		spin_unlock_irqrestore(&info->netlock, flags);
		return -EBUSY;
	}
	info->netcount=1;
	spin_unlock_irqrestore(&info->netlock, flags);

	/* claim resources and init adapter */
	if ((rc = startup(info)) != 0) {
		spin_lock_irqsave(&info->netlock, flags);
		info->netcount=0;
		spin_unlock_irqrestore(&info->netlock, flags);
		return rc;
	}

	/* assert DTR and RTS, apply hardware settings */
	info->signals |= SerialSignal_RTS + SerialSignal_DTR;
	program_hw(info);

	/* enable network layer transmit */
	dev->trans_start = jiffies;
	netif_start_queue(dev);

	/* inform generic HDLC layer of current DCD status */
	spin_lock_irqsave(&info->lock, flags);
	get_signals(info);
	spin_unlock_irqrestore(&info->lock, flags);
	hdlc_set_carrier(info->signals & SerialSignal_DCD, dev);

	return 0;
}

/**
 * called by network layer when interface is disabled
 * shutdown hardware and release resources
 *
 * dev  pointer to network device structure
 *
 * returns 0 if success, otherwise error code
 */
static int hdlcdev_close(struct net_device *dev)
{
	struct slgt_info *info = dev_to_port(dev);
	unsigned long flags;

	DBGINFO(("%s hdlcdev_close\n", dev->name));

	netif_stop_queue(dev);

	/* shutdown adapter and release resources */
	shutdown(info);

	hdlc_close(dev);

	spin_lock_irqsave(&info->netlock, flags);
	info->netcount=0;
	spin_unlock_irqrestore(&info->netlock, flags);

	return 0;
}

/**
 * called by network layer to process IOCTL call to network device
 *
 * dev  pointer to network device structure
 * ifr  pointer to network interface request structure
 * cmd  IOCTL command code
 *
 * returns 0 if success, otherwise error code
 */
static int hdlcdev_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	const size_t size = sizeof(sync_serial_settings);
	sync_serial_settings new_line;
	sync_serial_settings __user *line = ifr->ifr_settings.ifs_ifsu.sync;
	struct slgt_info *info = dev_to_port(dev);
	unsigned int flags;

	DBGINFO(("%s hdlcdev_ioctl\n", dev->name));

	/* return error if TTY interface open */
	if (info->count)
		return -EBUSY;

	if (cmd != SIOCWANDEV)
		return hdlc_ioctl(dev, ifr, cmd);

	switch(ifr->ifr_settings.type) {
	case IF_GET_IFACE: /* return current sync_serial_settings */

		ifr->ifr_settings.type = IF_IFACE_SYNC_SERIAL;
		if (ifr->ifr_settings.size < size) {
			ifr->ifr_settings.size = size; /* data size wanted */
			return -ENOBUFS;
		}

		flags = info->params.flags & (HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_RXC_DPLL |
					      HDLC_FLAG_RXC_BRG    | HDLC_FLAG_RXC_TXCPIN |
					      HDLC_FLAG_TXC_TXCPIN | HDLC_FLAG_TXC_DPLL |
					      HDLC_FLAG_TXC_BRG    | HDLC_FLAG_TXC_RXCPIN);

		switch (flags){
		case (HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_TXC_TXCPIN): new_line.clock_type = CLOCK_EXT; break;
		case (HDLC_FLAG_RXC_BRG    | HDLC_FLAG_TXC_BRG):    new_line.clock_type = CLOCK_INT; break;
		case (HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_TXC_BRG):    new_line.clock_type = CLOCK_TXINT; break;
		case (HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_TXC_RXCPIN): new_line.clock_type = CLOCK_TXFROMRX; break;
		default: new_line.clock_type = CLOCK_DEFAULT;
		}

		new_line.clock_rate = info->params.clock_speed;
		new_line.loopback   = info->params.loopback ? 1:0;

		if (copy_to_user(line, &new_line, size))
			return -EFAULT;
		return 0;

	case IF_IFACE_SYNC_SERIAL: /* set sync_serial_settings */

		if(!capable(CAP_NET_ADMIN))
			return -EPERM;
		if (copy_from_user(&new_line, line, size))
			return -EFAULT;

		switch (new_line.clock_type)
		{
		case CLOCK_EXT:      flags = HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_TXC_TXCPIN; break;
		case CLOCK_TXFROMRX: flags = HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_TXC_RXCPIN; break;
		case CLOCK_INT:      flags = HDLC_FLAG_RXC_BRG    | HDLC_FLAG_TXC_BRG;    break;
		case CLOCK_TXINT:    flags = HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_TXC_BRG;    break;
		case CLOCK_DEFAULT:  flags = info->params.flags &
					     (HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_RXC_DPLL |
					      HDLC_FLAG_RXC_BRG    | HDLC_FLAG_RXC_TXCPIN |
					      HDLC_FLAG_TXC_TXCPIN | HDLC_FLAG_TXC_DPLL |
					      HDLC_FLAG_TXC_BRG    | HDLC_FLAG_TXC_RXCPIN); break;
		default: return -EINVAL;
		}

		if (new_line.loopback != 0 && new_line.loopback != 1)
			return -EINVAL;

		info->params.flags &= ~(HDLC_FLAG_RXC_RXCPIN | HDLC_FLAG_RXC_DPLL |
					HDLC_FLAG_RXC_BRG    | HDLC_FLAG_RXC_TXCPIN |
					HDLC_FLAG_TXC_TXCPIN | HDLC_FLAG_TXC_DPLL |
					HDLC_FLAG_TXC_BRG    | HDLC_FLAG_TXC_RXCPIN);
		info->params.flags |= flags;

		info->params.loopback = new_line.loopback;

		if (flags & (HDLC_FLAG_RXC_BRG | HDLC_FLAG_TXC_BRG))
			info->params.clock_speed = new_line.clock_rate;
		else
			info->params.clock_speed = 0;

		/* if network interface up, reprogram hardware */
		if (info->netcount)
			program_hw(info);
		return 0;

	default:
		return hdlc_ioctl(dev, ifr, cmd);
	}
}

/**
 * called by network layer when transmit timeout is detected
 *
 * dev  pointer to network device structure
 */
static void hdlcdev_tx_timeout(struct net_device *dev)
{
	struct slgt_info *info = dev_to_port(dev);
	struct net_device_stats *stats = hdlc_stats(dev);
	unsigned long flags;

	DBGINFO(("%s hdlcdev_tx_timeout\n", dev->name));

	stats->tx_errors++;
	stats->tx_aborted_errors++;

	spin_lock_irqsave(&info->lock,flags);
	tx_stop(info);
	spin_unlock_irqrestore(&info->lock,flags);

	netif_wake_queue(dev);
}

/**
 * called by device driver when transmit completes
 * reenable network layer transmit if stopped
 *
 * info  pointer to device instance information
 */
static void hdlcdev_tx_done(struct slgt_info *info)
{
	if (netif_queue_stopped(info->netdev))
		netif_wake_queue(info->netdev);
}

/**
 * called by device driver when frame received
 * pass frame to network layer
 *
 * info  pointer to device instance information
 * buf   pointer to buffer contianing frame data
 * size  count of data bytes in buf
 */
static void hdlcdev_rx(struct slgt_info *info, char *buf, int size)
{
	struct sk_buff *skb = dev_alloc_skb(size);
	struct net_device *dev = info->netdev;
	struct net_device_stats *stats = hdlc_stats(dev);

	DBGINFO(("%s hdlcdev_rx\n", dev->name));

	if (skb == NULL) {
		DBGERR(("%s: can't alloc skb, drop packet\n", dev->name));
		stats->rx_dropped++;
		return;
	}

	memcpy(skb_put(skb, size),buf,size);

	skb->protocol = hdlc_type_trans(skb, info->netdev);

	stats->rx_packets++;
	stats->rx_bytes += size;

	netif_rx(skb);

	info->netdev->last_rx = jiffies;
}

/**
 * called by device driver when adding device instance
 * do generic HDLC initialization
 *
 * info  pointer to device instance information
 *
 * returns 0 if success, otherwise error code
 */
static int hdlcdev_init(struct slgt_info *info)
{
	int rc;
	struct net_device *dev;
	hdlc_device *hdlc;

	/* allocate and initialize network and HDLC layer objects */

	if (!(dev = alloc_hdlcdev(info))) {
		printk(KERN_ERR "%s hdlc device alloc failure\n", info->device_name);
		return -ENOMEM;
	}

	/* for network layer reporting purposes only */
	dev->mem_start = info->phys_reg_addr;
	dev->mem_end   = info->phys_reg_addr + SLGT_REG_SIZE - 1;
	dev->irq       = info->irq_level;

	/* network layer callbacks and settings */
	dev->do_ioctl       = hdlcdev_ioctl;
	dev->open           = hdlcdev_open;
	dev->stop           = hdlcdev_close;
	dev->tx_timeout     = hdlcdev_tx_timeout;
	dev->watchdog_timeo = 10*HZ;
	dev->tx_queue_len   = 50;

	/* generic HDLC layer callbacks and settings */
	hdlc         = dev_to_hdlc(dev);
	hdlc->attach = hdlcdev_attach;
	hdlc->xmit   = hdlcdev_xmit;

	/* register objects with HDLC layer */
	if ((rc = register_hdlc_device(dev))) {
		printk(KERN_WARNING "%s:unable to register hdlc device\n",__FILE__);
		free_netdev(dev);
		return rc;
	}

	info->netdev = dev;
	return 0;
}

/**
 * called by device driver when removing device instance
 * do generic HDLC cleanup
 *
 * info  pointer to device instance information
 */
static void hdlcdev_exit(struct slgt_info *info)
{
	unregister_hdlc_device(info->netdev);
	free_netdev(info->netdev);
	info->netdev = NULL;
}

#endif /* ifdef CONFIG_HDLC */

/*
 * get async data from rx DMA buffers
 */
static void rx_async(struct slgt_info *info)
{
 	struct tty_struct *tty = info->tty;
 	struct mgsl_icount *icount = &info->icount;
	unsigned int start, end;
	unsigned char *p;
	unsigned char status;
	struct slgt_desc *bufs = info->rbufs;
	int i, count;
	int chars = 0;
	int stat;
	unsigned char ch;

	start = end = info->rbuf_current;

	while(desc_complete(bufs[end])) {
		count = desc_count(bufs[end]) - info->rbuf_index;
		p     = bufs[end].buf + info->rbuf_index;

		DBGISR(("%s rx_async count=%d\n", info->device_name, count));
		DBGDATA(info, p, count, "rx");

		for(i=0 ; i < count; i+=2, p+=2) {
			if (tty && chars) {
				tty_flip_buffer_push(tty);
				chars = 0;
			}
			ch = *p;
			icount->rx++;

			stat = 0;

			if ((status = *(p+1) & (BIT9 + BIT8))) {
				if (status & BIT9)
					icount->parity++;
				else if (status & BIT8)
					icount->frame++;
				/* discard char if tty control flags say so */
				if (status & info->ignore_status_mask)
					continue;
				if (status & BIT9)
					stat = TTY_PARITY;
				else if (status & BIT8)
					stat = TTY_FRAME;
			}
			if (tty) {
				tty_insert_flip_char(tty, ch, stat);
				chars++;
			}
		}

		if (i < count) {
			/* receive buffer not completed */
			info->rbuf_index += i;
			info->rx_timer.expires = jiffies + 1;
			add_timer(&info->rx_timer);
			break;
		}

		info->rbuf_index = 0;
		free_rbufs(info, end, end);

		if (++end == info->rbuf_count)
			end = 0;

		/* if entire list searched then no frame available */
		if (end == start)
			break;
	}

	if (tty && chars)
		tty_flip_buffer_push(tty);
}

/*
 * return next bottom half action to perform
 */
static int bh_action(struct slgt_info *info)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&info->lock,flags);

	if (info->pending_bh & BH_RECEIVE) {
		info->pending_bh &= ~BH_RECEIVE;
		rc = BH_RECEIVE;
	} else if (info->pending_bh & BH_TRANSMIT) {
		info->pending_bh &= ~BH_TRANSMIT;
		rc = BH_TRANSMIT;
	} else if (info->pending_bh & BH_STATUS) {
		info->pending_bh &= ~BH_STATUS;
		rc = BH_STATUS;
	} else {
		/* Mark BH routine as complete */
		info->bh_running   = 0;
		info->bh_requested = 0;
		rc = 0;
	}

	spin_unlock_irqrestore(&info->lock,flags);

	return rc;
}

/*
 * perform bottom half processing
 */
static void bh_handler(void* context)
{
	struct slgt_info *info = context;
	int action;

	if (!info)
		return;
	info->bh_running = 1;

	while((action = bh_action(info))) {
		switch (action) {
		case BH_RECEIVE:
			DBGBH(("%s bh receive\n", info->device_name));
			switch(info->params.mode) {
			case MGSL_MODE_ASYNC:
				rx_async(info);
				break;
			case MGSL_MODE_HDLC:
				while(rx_get_frame(info));
				break;
			case MGSL_MODE_RAW:
				while(rx_get_buf(info));
				break;
			}
			/* restart receiver if rx DMA buffers exhausted */
			if (info->rx_restart)
				rx_start(info);
			break;
		case BH_TRANSMIT:
			bh_transmit(info);
			break;
		case BH_STATUS:
			DBGBH(("%s bh status\n", info->device_name));
			info->ri_chkcount = 0;
			info->dsr_chkcount = 0;
			info->dcd_chkcount = 0;
			info->cts_chkcount = 0;
			break;
		default:
			DBGBH(("%s unknown action\n", info->device_name));
			break;
		}
	}
	DBGBH(("%s bh_handler exit\n", info->device_name));
}

static void bh_transmit(struct slgt_info *info)
{
	struct tty_struct *tty = info->tty;

	DBGBH(("%s bh_transmit\n", info->device_name));
	if (tty) {
		tty_wakeup(tty);
		wake_up_interruptible(&tty->write_wait);
	}
}

static void dsr_change(struct slgt_info *info)
{
	get_signals(info);
	DBGISR(("dsr_change %s signals=%04X\n", info->device_name, info->signals));
	if ((info->dsr_chkcount)++ == IO_PIN_SHUTDOWN_LIMIT) {
		slgt_irq_off(info, IRQ_DSR);
		return;
	}
	info->icount.dsr++;
	if (info->signals & SerialSignal_DSR)
		info->input_signal_events.dsr_up++;
	else
		info->input_signal_events.dsr_down++;
	wake_up_interruptible(&info->status_event_wait_q);
	wake_up_interruptible(&info->event_wait_q);
	info->pending_bh |= BH_STATUS;
}

static void cts_change(struct slgt_info *info)
{
	get_signals(info);
	DBGISR(("cts_change %s signals=%04X\n", info->device_name, info->signals));
	if ((info->cts_chkcount)++ == IO_PIN_SHUTDOWN_LIMIT) {
		slgt_irq_off(info, IRQ_CTS);
		return;
	}
	info->icount.cts++;
	if (info->signals & SerialSignal_CTS)
		info->input_signal_events.cts_up++;
	else
		info->input_signal_events.cts_down++;
	wake_up_interruptible(&info->status_event_wait_q);
	wake_up_interruptible(&info->event_wait_q);
	info->pending_bh |= BH_STATUS;

	if (info->flags & ASYNC_CTS_FLOW) {
		if (info->tty) {
			if (info->tty->hw_stopped) {
				if (info->signals & SerialSignal_CTS) {
		 			info->tty->hw_stopped = 0;
					info->pending_bh |= BH_TRANSMIT;
					return;
				}
			} else {
				if (!(info->signals & SerialSignal_CTS))
		 			info->tty->hw_stopped = 1;
			}
		}
	}
}

static void dcd_change(struct slgt_info *info)
{
	get_signals(info);
	DBGISR(("dcd_change %s signals=%04X\n", info->device_name, info->signals));
	if ((info->dcd_chkcount)++ == IO_PIN_SHUTDOWN_LIMIT) {
		slgt_irq_off(info, IRQ_DCD);
		return;
	}
	info->icount.dcd++;
	if (info->signals & SerialSignal_DCD) {
		info->input_signal_events.dcd_up++;
	} else {
		info->input_signal_events.dcd_down++;
	}
#ifdef CONFIG_HDLC
	if (info->netcount)
		hdlc_set_carrier(info->signals & SerialSignal_DCD, info->netdev);
#endif
	wake_up_interruptible(&info->status_event_wait_q);
	wake_up_interruptible(&info->event_wait_q);
	info->pending_bh |= BH_STATUS;

	if (info->flags & ASYNC_CHECK_CD) {
		if (info->signals & SerialSignal_DCD)
			wake_up_interruptible(&info->open_wait);
		else {
			if (info->tty)
				tty_hangup(info->tty);
		}
	}
}

static void ri_change(struct slgt_info *info)
{
	get_signals(info);
	DBGISR(("ri_change %s signals=%04X\n", info->device_name, info->signals));
	if ((info->ri_chkcount)++ == IO_PIN_SHUTDOWN_LIMIT) {
		slgt_irq_off(info, IRQ_RI);
		return;
	}
	info->icount.dcd++;
	if (info->signals & SerialSignal_RI) {
		info->input_signal_events.ri_up++;
	} else {
		info->input_signal_events.ri_down++;
	}
	wake_up_interruptible(&info->status_event_wait_q);
	wake_up_interruptible(&info->event_wait_q);
	info->pending_bh |= BH_STATUS;
}

static void isr_serial(struct slgt_info *info)
{
	unsigned short status = rd_reg16(info, SSR);

	DBGISR(("%s isr_serial status=%04X\n", info->device_name, status));

	wr_reg16(info, SSR, status); /* clear pending */

	info->irq_occurred = 1;

	if (info->params.mode == MGSL_MODE_ASYNC) {
		if (status & IRQ_TXIDLE) {
			if (info->tx_count)
				isr_txeom(info, status);
		}
		if ((status & IRQ_RXBREAK) && (status & RXBREAK)) {
			info->icount.brk++;
			/* process break detection if tty control allows */
			if (info->tty) {
				if (!(status & info->ignore_status_mask)) {
					if (info->read_status_mask & MASK_BREAK) {
						tty_insert_flip_char(info->tty, 0, TTY_BREAK);
						if (info->flags & ASYNC_SAK)
							do_SAK(info->tty);
					}
				}
			}
		}
	} else {
		if (status & (IRQ_TXIDLE + IRQ_TXUNDER))
			isr_txeom(info, status);

		if (status & IRQ_RXIDLE) {
			if (status & RXIDLE)
				info->icount.rxidle++;
			else
				info->icount.exithunt++;
			wake_up_interruptible(&info->event_wait_q);
		}

		if (status & IRQ_RXOVER)
			rx_start(info);
	}

	if (status & IRQ_DSR)
		dsr_change(info);
	if (status & IRQ_CTS)
		cts_change(info);
	if (status & IRQ_DCD)
		dcd_change(info);
	if (status & IRQ_RI)
		ri_change(info);
}

static void isr_rdma(struct slgt_info *info)
{
	unsigned int status = rd_reg32(info, RDCSR);

	DBGISR(("%s isr_rdma status=%08x\n", info->device_name, status));

	/* RDCSR (rx DMA control/status)
	 *
	 * 31..07  reserved
	 * 06      save status byte to DMA buffer
	 * 05      error
	 * 04      eol (end of list)
	 * 03      eob (end of buffer)
	 * 02      IRQ enable
	 * 01      reset
	 * 00      enable
	 */
	wr_reg32(info, RDCSR, status);	/* clear pending */

	if (status & (BIT5 + BIT4)) {
		DBGISR(("%s isr_rdma rx_restart=1\n", info->device_name));
		info->rx_restart = 1;
	}
	info->pending_bh |= BH_RECEIVE;
}

static void isr_tdma(struct slgt_info *info)
{
	unsigned int status = rd_reg32(info, TDCSR);

	DBGISR(("%s isr_tdma status=%08x\n", info->device_name, status));

	/* TDCSR (tx DMA control/status)
	 *
	 * 31..06  reserved
	 * 05      error
	 * 04      eol (end of list)
	 * 03      eob (end of buffer)
	 * 02      IRQ enable
	 * 01      reset
	 * 00      enable
	 */
	wr_reg32(info, TDCSR, status);	/* clear pending */

	if (status & (BIT5 + BIT4 + BIT3)) {
		// another transmit buffer has completed
		// run bottom half to get more send data from user
		info->pending_bh |= BH_TRANSMIT;
	}
}

static void isr_txeom(struct slgt_info *info, unsigned short status)
{
	DBGISR(("%s txeom status=%04x\n", info->device_name, status));

	slgt_irq_off(info, IRQ_TXDATA + IRQ_TXIDLE + IRQ_TXUNDER);
	tdma_reset(info);
	reset_tbufs(info);
	if (status & IRQ_TXUNDER) {
		unsigned short val = rd_reg16(info, TCR);
		wr_reg16(info, TCR, (unsigned short)(val | BIT2)); /* set reset bit */
		wr_reg16(info, TCR, val); /* clear reset bit */
	}

	if (info->tx_active) {
		if (info->params.mode != MGSL_MODE_ASYNC) {
			if (status & IRQ_TXUNDER)
				info->icount.txunder++;
			else if (status & IRQ_TXIDLE)
				info->icount.txok++;
		}

		info->tx_active = 0;
		info->tx_count = 0;

		del_timer(&info->tx_timer);

		if (info->params.mode != MGSL_MODE_ASYNC && info->drop_rts_on_tx_done) {
			info->signals &= ~SerialSignal_RTS;
			info->drop_rts_on_tx_done = 0;
			set_signals(info);
		}

#ifdef CONFIG_HDLC
		if (info->netcount)
			hdlcdev_tx_done(info);
		else
#endif
		{
			if (info->tty && (info->tty->stopped || info->tty->hw_stopped)) {
				tx_stop(info);
				return;
			}
			info->pending_bh |= BH_TRANSMIT;
		}
	}
}

/* interrupt service routine
 *
 * 	irq	interrupt number
 * 	dev_id	device ID supplied during interrupt registration
 * 	regs	interrupted processor context
 */
static irqreturn_t slgt_interrupt(int irq, void *dev_id, struct pt_regs * regs)
{
	struct slgt_info *info;
	unsigned int gsr;
	unsigned int i;

	DBGISR(("slgt_interrupt irq=%d entry\n", irq));

	info = dev_id;
	if (!info)
		return IRQ_NONE;

	spin_lock(&info->lock);

	while((gsr = rd_reg32(info, GSR) & 0xffffff00)) {
		DBGISR(("%s gsr=%08x\n", info->device_name, gsr));
		info->irq_occurred = 1;
		for(i=0; i < info->port_count ; i++) {
			if (info->port_array[i] == NULL)
				continue;
			if (gsr & (BIT8 << i))
				isr_serial(info->port_array[i]);
			if (gsr & (BIT16 << (i*2)))
				isr_rdma(info->port_array[i]);
			if (gsr & (BIT17 << (i*2)))
				isr_tdma(info->port_array[i]);
		}
	}

	for(i=0; i < info->port_count ; i++) {
		struct slgt_info *port = info->port_array[i];

		if (port && (port->count || port->netcount) &&
		    port->pending_bh && !port->bh_running &&
		    !port->bh_requested) {
			DBGISR(("%s bh queued\n", port->device_name));
			schedule_work(&port->task);
			port->bh_requested = 1;
		}
	}

	spin_unlock(&info->lock);

	DBGISR(("slgt_interrupt irq=%d exit\n", irq));
	return IRQ_HANDLED;
}

static int startup(struct slgt_info *info)
{
	DBGINFO(("%s startup\n", info->device_name));

	if (info->flags & ASYNC_INITIALIZED)
		return 0;

	if (!info->tx_buf) {
		info->tx_buf = kmalloc(info->max_frame_size, GFP_KERNEL);
		if (!info->tx_buf) {
			DBGERR(("%s can't allocate tx buffer\n", info->device_name));
			return -ENOMEM;
		}
	}

	info->pending_bh = 0;

	memset(&info->icount, 0, sizeof(info->icount));

	/* program hardware for current parameters */
	change_params(info);

	if (info->tty)
		clear_bit(TTY_IO_ERROR, &info->tty->flags);

	info->flags |= ASYNC_INITIALIZED;

	return 0;
}

/*
 *  called by close() and hangup() to shutdown hardware
 */
static void shutdown(struct slgt_info *info)
{
	unsigned long flags;

	if (!(info->flags & ASYNC_INITIALIZED))
		return;

	DBGINFO(("%s shutdown\n", info->device_name));

	/* clear status wait queue because status changes */
	/* can't happen after shutting down the hardware */
	wake_up_interruptible(&info->status_event_wait_q);
	wake_up_interruptible(&info->event_wait_q);

	del_timer_sync(&info->tx_timer);
	del_timer_sync(&info->rx_timer);

	kfree(info->tx_buf);
	info->tx_buf = NULL;

	spin_lock_irqsave(&info->lock,flags);

	tx_stop(info);
	rx_stop(info);

	slgt_irq_off(info, IRQ_ALL | IRQ_MASTER);

 	if (!info->tty || info->tty->termios->c_cflag & HUPCL) {
 		info->signals &= ~(SerialSignal_DTR + SerialSignal_RTS);
		set_signals(info);
	}

	spin_unlock_irqrestore(&info->lock,flags);

	if (info->tty)
		set_bit(TTY_IO_ERROR, &info->tty->flags);

	info->flags &= ~ASYNC_INITIALIZED;
}

static void program_hw(struct slgt_info *info)
{
	unsigned long flags;

	spin_lock_irqsave(&info->lock,flags);

	rx_stop(info);
	tx_stop(info);

	if (info->params.mode == MGSL_MODE_HDLC ||
	    info->params.mode == MGSL_MODE_RAW ||
	    info->netcount)
		hdlc_mode(info);
	else
		async_mode(info);

	set_signals(info);

	info->dcd_chkcount = 0;
	info->cts_chkcount = 0;
	info->ri_chkcount = 0;
	info->dsr_chkcount = 0;

	slgt_irq_on(info, IRQ_DCD | IRQ_CTS | IRQ_DSR);
	get_signals(info);

	if (info->netcount ||
	    (info->tty && info->tty->termios->c_cflag & CREAD))
		rx_start(info);

	spin_unlock_irqrestore(&info->lock,flags);
}

/*
 * reconfigure adapter based on new parameters
 */
static void change_params(struct slgt_info *info)
{
	unsigned cflag;
	int bits_per_char;

	if (!info->tty || !info->tty->termios)
		return;
	DBGINFO(("%s change_params\n", info->device_name));

	cflag = info->tty->termios->c_cflag;

	/* if B0 rate (hangup) specified then negate DTR and RTS */
	/* otherwise assert DTR and RTS */
 	if (cflag & CBAUD)
		info->signals |= SerialSignal_RTS + SerialSignal_DTR;
	else
		info->signals &= ~(SerialSignal_RTS + SerialSignal_DTR);

	/* byte size and parity */

	switch (cflag & CSIZE) {
	case CS5: info->params.data_bits = 5; break;
	case CS6: info->params.data_bits = 6; break;
	case CS7: info->params.data_bits = 7; break;
	case CS8: info->params.data_bits = 8; break;
	default:  info->params.data_bits = 7; break;
	}

	info->params.stop_bits = (cflag & CSTOPB) ? 2 : 1;

	if (cflag & PARENB)
		info->params.parity = (cflag & PARODD) ? ASYNC_PARITY_ODD : ASYNC_PARITY_EVEN;
	else
		info->params.parity = ASYNC_PARITY_NONE;

	/* calculate number of jiffies to transmit a full
	 * FIFO (32 bytes) at specified data rate
	 */
	bits_per_char = info->params.data_bits +
			info->params.stop_bits + 1;

	info->params.data_rate = tty_get_baud_rate(info->tty);

	if (info->params.data_rate) {
		info->timeout = (32*HZ*bits_per_char) /
				info->params.data_rate;
	}
	info->timeout += HZ/50;		/* Add .02 seconds of slop */

	if (cflag & CRTSCTS)
		info->flags |= ASYNC_CTS_FLOW;
	else
		info->flags &= ~ASYNC_CTS_FLOW;

	if (cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else
		info->flags |= ASYNC_CHECK_CD;

	/* process tty input control flags */

	info->read_status_mask = IRQ_RXOVER;
	if (I_INPCK(info->tty))
		info->read_status_mask |= MASK_PARITY | MASK_FRAMING;
 	if (I_BRKINT(info->tty) || I_PARMRK(info->tty))
 		info->read_status_mask |= MASK_BREAK;
	if (I_IGNPAR(info->tty))
		info->ignore_status_mask |= MASK_PARITY | MASK_FRAMING;
	if (I_IGNBRK(info->tty)) {
		info->ignore_status_mask |= MASK_BREAK;
		/* If ignoring parity and break indicators, ignore
		 * overruns too.  (For real raw support).
		 */
		if (I_IGNPAR(info->tty))
			info->ignore_status_mask |= MASK_OVERRUN;
	}

	program_hw(info);
}

static int get_stats(struct slgt_info *info, struct mgsl_icount __user *user_icount)
{
	DBGINFO(("%s get_stats\n",  info->device_name));
	if (!user_icount) {
		memset(&info->icount, 0, sizeof(info->icount));
	} else {
		if (copy_to_user(user_icount, &info->icount, sizeof(struct mgsl_icount)))
			return -EFAULT;
	}
	return 0;
}

static int get_params(struct slgt_info *info, MGSL_PARAMS __user *user_params)
{
	DBGINFO(("%s get_params\n", info->device_name));
	if (copy_to_user(user_params, &info->params, sizeof(MGSL_PARAMS)))
		return -EFAULT;
	return 0;
}

static int set_params(struct slgt_info *info, MGSL_PARAMS __user *new_params)
{
 	unsigned long flags;
	MGSL_PARAMS tmp_params;

	DBGINFO(("%s set_params\n", info->device_name));
	if (copy_from_user(&tmp_params, new_params, sizeof(MGSL_PARAMS)))
		return -EFAULT;

	spin_lock_irqsave(&info->lock, flags);
	memcpy(&info->params, &tmp_params, sizeof(MGSL_PARAMS));
	spin_unlock_irqrestore(&info->lock, flags);

 	change_params(info);

	return 0;
}

static int get_txidle(struct slgt_info *info, int __user *idle_mode)
{
	DBGINFO(("%s get_txidle=%d\n", info->device_name, info->idle_mode));
	if (put_user(info->idle_mode, idle_mode))
		return -EFAULT;
	return 0;
}

static int set_txidle(struct slgt_info *info, int idle_mode)
{
 	unsigned long flags;
	DBGINFO(("%s set_txidle(%d)\n", info->device_name, idle_mode));
	spin_lock_irqsave(&info->lock,flags);
	info->idle_mode = idle_mode;
	tx_set_idle(info);
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

static int tx_enable(struct slgt_info *info, int enable)
{
 	unsigned long flags;
	DBGINFO(("%s tx_enable(%d)\n", info->device_name, enable));
	spin_lock_irqsave(&info->lock,flags);
	if (enable) {
		if (!info->tx_enabled)
			tx_start(info);
	} else {
		if (info->tx_enabled)
			tx_stop(info);
	}
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

/*
 * abort transmit HDLC frame
 */
static int tx_abort(struct slgt_info *info)
{
 	unsigned long flags;
	DBGINFO(("%s tx_abort\n", info->device_name));
	spin_lock_irqsave(&info->lock,flags);
	tdma_reset(info);
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

static int rx_enable(struct slgt_info *info, int enable)
{
 	unsigned long flags;
	DBGINFO(("%s rx_enable(%d)\n", info->device_name, enable));
	spin_lock_irqsave(&info->lock,flags);
	if (enable) {
		if (!info->rx_enabled)
			rx_start(info);
	} else {
		if (info->rx_enabled)
			rx_stop(info);
	}
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

/*
 *  wait for specified event to occur
 */
static int wait_mgsl_event(struct slgt_info *info, int __user *mask_ptr)
{
 	unsigned long flags;
	int s;
	int rc=0;
	struct mgsl_icount cprev, cnow;
	int events;
	int mask;
	struct	_input_signal_events oldsigs, newsigs;
	DECLARE_WAITQUEUE(wait, current);

	if (get_user(mask, mask_ptr))
		return -EFAULT;

	DBGINFO(("%s wait_mgsl_event(%d)\n", info->device_name, mask));

	spin_lock_irqsave(&info->lock,flags);

	/* return immediately if state matches requested events */
	get_signals(info);
	s = info->signals;

	events = mask &
		( ((s & SerialSignal_DSR) ? MgslEvent_DsrActive:MgslEvent_DsrInactive) +
 		  ((s & SerialSignal_DCD) ? MgslEvent_DcdActive:MgslEvent_DcdInactive) +
		  ((s & SerialSignal_CTS) ? MgslEvent_CtsActive:MgslEvent_CtsInactive) +
		  ((s & SerialSignal_RI)  ? MgslEvent_RiActive :MgslEvent_RiInactive) );
	if (events) {
		spin_unlock_irqrestore(&info->lock,flags);
		goto exit;
	}

	/* save current irq counts */
	cprev = info->icount;
	oldsigs = info->input_signal_events;

	/* enable hunt and idle irqs if needed */
	if (mask & (MgslEvent_ExitHuntMode+MgslEvent_IdleReceived)) {
		unsigned short val = rd_reg16(info, SCR);
		if (!(val & IRQ_RXIDLE))
			wr_reg16(info, SCR, (unsigned short)(val | IRQ_RXIDLE));
	}

	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&info->event_wait_q, &wait);

	spin_unlock_irqrestore(&info->lock,flags);

	for(;;) {
		schedule();
		if (signal_pending(current)) {
			rc = -ERESTARTSYS;
			break;
		}

		/* get current irq counts */
		spin_lock_irqsave(&info->lock,flags);
		cnow = info->icount;
		newsigs = info->input_signal_events;
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&info->lock,flags);

		/* if no change, wait aborted for some reason */
		if (newsigs.dsr_up   == oldsigs.dsr_up   &&
		    newsigs.dsr_down == oldsigs.dsr_down &&
		    newsigs.dcd_up   == oldsigs.dcd_up   &&
		    newsigs.dcd_down == oldsigs.dcd_down &&
		    newsigs.cts_up   == oldsigs.cts_up   &&
		    newsigs.cts_down == oldsigs.cts_down &&
		    newsigs.ri_up    == oldsigs.ri_up    &&
		    newsigs.ri_down  == oldsigs.ri_down  &&
		    cnow.exithunt    == cprev.exithunt   &&
		    cnow.rxidle      == cprev.rxidle) {
			rc = -EIO;
			break;
		}

		events = mask &
			( (newsigs.dsr_up   != oldsigs.dsr_up   ? MgslEvent_DsrActive:0)   +
			  (newsigs.dsr_down != oldsigs.dsr_down ? MgslEvent_DsrInactive:0) +
			  (newsigs.dcd_up   != oldsigs.dcd_up   ? MgslEvent_DcdActive:0)   +
			  (newsigs.dcd_down != oldsigs.dcd_down ? MgslEvent_DcdInactive:0) +
			  (newsigs.cts_up   != oldsigs.cts_up   ? MgslEvent_CtsActive:0)   +
			  (newsigs.cts_down != oldsigs.cts_down ? MgslEvent_CtsInactive:0) +
			  (newsigs.ri_up    != oldsigs.ri_up    ? MgslEvent_RiActive:0)    +
			  (newsigs.ri_down  != oldsigs.ri_down  ? MgslEvent_RiInactive:0)  +
			  (cnow.exithunt    != cprev.exithunt   ? MgslEvent_ExitHuntMode:0) +
			  (cnow.rxidle      != cprev.rxidle     ? MgslEvent_IdleReceived:0) );
		if (events)
			break;

		cprev = cnow;
		oldsigs = newsigs;
	}

	remove_wait_queue(&info->event_wait_q, &wait);
	set_current_state(TASK_RUNNING);


	if (mask & (MgslEvent_ExitHuntMode + MgslEvent_IdleReceived)) {
		spin_lock_irqsave(&info->lock,flags);
		if (!waitqueue_active(&info->event_wait_q)) {
			/* disable enable exit hunt mode/idle rcvd IRQs */
			wr_reg16(info, SCR,
				(unsigned short)(rd_reg16(info, SCR) & ~IRQ_RXIDLE));
		}
		spin_unlock_irqrestore(&info->lock,flags);
	}
exit:
	if (rc == 0)
		rc = put_user(events, mask_ptr);
	return rc;
}

static int get_interface(struct slgt_info *info, int __user *if_mode)
{
	DBGINFO(("%s get_interface=%x\n", info->device_name, info->if_mode));
	if (put_user(info->if_mode, if_mode))
		return -EFAULT;
	return 0;
}

static int set_interface(struct slgt_info *info, int if_mode)
{
 	unsigned long flags;
	unsigned short val;

	DBGINFO(("%s set_interface=%x)\n", info->device_name, if_mode));
	spin_lock_irqsave(&info->lock,flags);
	info->if_mode = if_mode;

	msc_set_vcr(info);

	/* TCR (tx control) 07  1=RTS driver control */
	val = rd_reg16(info, TCR);
	if (info->if_mode & MGSL_INTERFACE_RTS_EN)
		val |= BIT7;
	else
		val &= ~BIT7;
	wr_reg16(info, TCR, val);

	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

static int modem_input_wait(struct slgt_info *info,int arg)
{
 	unsigned long flags;
	int rc;
	struct mgsl_icount cprev, cnow;
	DECLARE_WAITQUEUE(wait, current);

	/* save current irq counts */
	spin_lock_irqsave(&info->lock,flags);
	cprev = info->icount;
	add_wait_queue(&info->status_event_wait_q, &wait);
	set_current_state(TASK_INTERRUPTIBLE);
	spin_unlock_irqrestore(&info->lock,flags);

	for(;;) {
		schedule();
		if (signal_pending(current)) {
			rc = -ERESTARTSYS;
			break;
		}

		/* get new irq counts */
		spin_lock_irqsave(&info->lock,flags);
		cnow = info->icount;
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&info->lock,flags);

		/* if no change, wait aborted for some reason */
		if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr &&
		    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts) {
			rc = -EIO;
			break;
		}

		/* check for change in caller specified modem input */
		if ((arg & TIOCM_RNG && cnow.rng != cprev.rng) ||
		    (arg & TIOCM_DSR && cnow.dsr != cprev.dsr) ||
		    (arg & TIOCM_CD  && cnow.dcd != cprev.dcd) ||
		    (arg & TIOCM_CTS && cnow.cts != cprev.cts)) {
			rc = 0;
			break;
		}

		cprev = cnow;
	}
	remove_wait_queue(&info->status_event_wait_q, &wait);
	set_current_state(TASK_RUNNING);
	return rc;
}

/*
 *  return state of serial control and status signals
 */
static int tiocmget(struct tty_struct *tty, struct file *file)
{
	struct slgt_info *info = tty->driver_data;
	unsigned int result;
 	unsigned long flags;

	spin_lock_irqsave(&info->lock,flags);
 	get_signals(info);
	spin_unlock_irqrestore(&info->lock,flags);

	result = ((info->signals & SerialSignal_RTS) ? TIOCM_RTS:0) +
		((info->signals & SerialSignal_DTR) ? TIOCM_DTR:0) +
		((info->signals & SerialSignal_DCD) ? TIOCM_CAR:0) +
		((info->signals & SerialSignal_RI)  ? TIOCM_RNG:0) +
		((info->signals & SerialSignal_DSR) ? TIOCM_DSR:0) +
		((info->signals & SerialSignal_CTS) ? TIOCM_CTS:0);

	DBGINFO(("%s tiocmget value=%08X\n", info->device_name, result));
	return result;
}

/*
 * set modem control signals (DTR/RTS)
 *
 * 	cmd	signal command: TIOCMBIS = set bit TIOCMBIC = clear bit
 *		TIOCMSET = set/clear signal values
 * 	value	bit mask for command
 */
static int tiocmset(struct tty_struct *tty, struct file *file,
		    unsigned int set, unsigned int clear)
{
	struct slgt_info *info = tty->driver_data;
 	unsigned long flags;

	DBGINFO(("%s tiocmset(%x,%x)\n", info->device_name, set, clear));

	if (set & TIOCM_RTS)
		info->signals |= SerialSignal_RTS;
	if (set & TIOCM_DTR)
		info->signals |= SerialSignal_DTR;
	if (clear & TIOCM_RTS)
		info->signals &= ~SerialSignal_RTS;
	if (clear & TIOCM_DTR)
		info->signals &= ~SerialSignal_DTR;

	spin_lock_irqsave(&info->lock,flags);
 	set_signals(info);
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

/*
 *  block current process until the device is ready to open
 */
static int block_til_ready(struct tty_struct *tty, struct file *filp,
			   struct slgt_info *info)
{
	DECLARE_WAITQUEUE(wait, current);
	int		retval;
	int		do_clocal = 0, extra_count = 0;
	unsigned long	flags;

	DBGINFO(("%s block_til_ready\n", tty->driver->name));

	if (filp->f_flags & O_NONBLOCK || tty->flags & (1 << TTY_IO_ERROR)){
		/* nonblock mode is set or port is not enabled */
		info->flags |= ASYNC_NORMAL_ACTIVE;
		return 0;
	}

	if (tty->termios->c_cflag & CLOCAL)
		do_clocal = 1;

	/* Wait for carrier detect and the line to become
	 * free (i.e., not in use by the callout).  While we are in
	 * this loop, info->count is dropped by one, so that
	 * close() knows when to free things.  We restore it upon
	 * exit, either normal or abnormal.
	 */

	retval = 0;
	add_wait_queue(&info->open_wait, &wait);

	spin_lock_irqsave(&info->lock, flags);
	if (!tty_hung_up_p(filp)) {
		extra_count = 1;
		info->count--;
	}
	spin_unlock_irqrestore(&info->lock, flags);
	info->blocked_open++;

	while (1) {
		if ((tty->termios->c_cflag & CBAUD)) {
			spin_lock_irqsave(&info->lock,flags);
			info->signals |= SerialSignal_RTS + SerialSignal_DTR;
		 	set_signals(info);
			spin_unlock_irqrestore(&info->lock,flags);
		}

		set_current_state(TASK_INTERRUPTIBLE);

		if (tty_hung_up_p(filp) || !(info->flags & ASYNC_INITIALIZED)){
			retval = (info->flags & ASYNC_HUP_NOTIFY) ?
					-EAGAIN : -ERESTARTSYS;
			break;
		}

		spin_lock_irqsave(&info->lock,flags);
	 	get_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);

 		if (!(info->flags & ASYNC_CLOSING) &&
 		    (do_clocal || (info->signals & SerialSignal_DCD)) ) {
 			break;
		}

		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			break;
		}

		DBGINFO(("%s block_til_ready wait\n", tty->driver->name));
		schedule();
	}

	set_current_state(TASK_RUNNING);
	remove_wait_queue(&info->open_wait, &wait);

	if (extra_count)
		info->count++;
	info->blocked_open--;

	if (!retval)
		info->flags |= ASYNC_NORMAL_ACTIVE;

	DBGINFO(("%s block_til_ready ready, rc=%d\n", tty->driver->name, retval));
	return retval;
}

static int alloc_tmp_rbuf(struct slgt_info *info)
{
	info->tmp_rbuf = kmalloc(info->max_frame_size, GFP_KERNEL);
	if (info->tmp_rbuf == NULL)
		return -ENOMEM;
	return 0;
}

static void free_tmp_rbuf(struct slgt_info *info)
{
	kfree(info->tmp_rbuf);
	info->tmp_rbuf = NULL;
}

/*
 * allocate DMA descriptor lists.
 */
static int alloc_desc(struct slgt_info *info)
{
	unsigned int i;
	unsigned int pbufs;

	/* allocate memory to hold descriptor lists */
	info->bufs = pci_alloc_consistent(info->pdev, DESC_LIST_SIZE, &info->bufs_dma_addr);
	if (info->bufs == NULL)
		return -ENOMEM;

	memset(info->bufs, 0, DESC_LIST_SIZE);

	info->rbufs = (struct slgt_desc*)info->bufs;
	info->tbufs = ((struct slgt_desc*)info->bufs) + info->rbuf_count;

	pbufs = (unsigned int)info->bufs_dma_addr;

	/*
	 * Build circular lists of descriptors
	 */

	for (i=0; i < info->rbuf_count; i++) {
		/* physical address of this descriptor */
		info->rbufs[i].pdesc = pbufs + (i * sizeof(struct slgt_desc));

		/* physical address of next descriptor */
		if (i == info->rbuf_count - 1)
			info->rbufs[i].next = cpu_to_le32(pbufs);
		else
			info->rbufs[i].next = cpu_to_le32(pbufs + ((i+1) * sizeof(struct slgt_desc)));
		set_desc_count(info->rbufs[i], DMABUFSIZE);
	}

	for (i=0; i < info->tbuf_count; i++) {
		/* physical address of this descriptor */
		info->tbufs[i].pdesc = pbufs + ((info->rbuf_count + i) * sizeof(struct slgt_desc));

		/* physical address of next descriptor */
		if (i == info->tbuf_count - 1)
			info->tbufs[i].next = cpu_to_le32(pbufs + info->rbuf_count * sizeof(struct slgt_desc));
		else
			info->tbufs[i].next = cpu_to_le32(pbufs + ((info->rbuf_count + i + 1) * sizeof(struct slgt_desc)));
	}

	return 0;
}

static void free_desc(struct slgt_info *info)
{
	if (info->bufs != NULL) {
		pci_free_consistent(info->pdev, DESC_LIST_SIZE, info->bufs, info->bufs_dma_addr);
		info->bufs  = NULL;
		info->rbufs = NULL;
		info->tbufs = NULL;
	}
}

static int alloc_bufs(struct slgt_info *info, struct slgt_desc *bufs, int count)
{
	int i;
	for (i=0; i < count; i++) {
		if ((bufs[i].buf = pci_alloc_consistent(info->pdev, DMABUFSIZE, &bufs[i].buf_dma_addr)) == NULL)
			return -ENOMEM;
		bufs[i].pbuf  = cpu_to_le32((unsigned int)bufs[i].buf_dma_addr);
	}
	return 0;
}

static void free_bufs(struct slgt_info *info, struct slgt_desc *bufs, int count)
{
	int i;
	for (i=0; i < count; i++) {
		if (bufs[i].buf == NULL)
			continue;
		pci_free_consistent(info->pdev, DMABUFSIZE, bufs[i].buf, bufs[i].buf_dma_addr);
		bufs[i].buf = NULL;
	}
}

static int alloc_dma_bufs(struct slgt_info *info)
{
	info->rbuf_count = 32;
	info->tbuf_count = 32;

	if (alloc_desc(info) < 0 ||
	    alloc_bufs(info, info->rbufs, info->rbuf_count) < 0 ||
	    alloc_bufs(info, info->tbufs, info->tbuf_count) < 0 ||
	    alloc_tmp_rbuf(info) < 0) {
		DBGERR(("%s DMA buffer alloc fail\n", info->device_name));
		return -ENOMEM;
	}
	reset_rbufs(info);
	return 0;
}

static void free_dma_bufs(struct slgt_info *info)
{
	if (info->bufs) {
		free_bufs(info, info->rbufs, info->rbuf_count);
		free_bufs(info, info->tbufs, info->tbuf_count);
		free_desc(info);
	}
	free_tmp_rbuf(info);
}

static int claim_resources(struct slgt_info *info)
{
	if (request_mem_region(info->phys_reg_addr, SLGT_REG_SIZE, "synclink_gt") == NULL) {
		DBGERR(("%s reg addr conflict, addr=%08X\n",
			info->device_name, info->phys_reg_addr));
		info->init_error = DiagStatus_AddressConflict;
		goto errout;
	}
	else
		info->reg_addr_requested = 1;

	info->reg_addr = ioremap(info->phys_reg_addr, SLGT_REG_SIZE);
	if (!info->reg_addr) {
		DBGERR(("%s cant map device registers, addr=%08X\n",
			info->device_name, info->phys_reg_addr));
		info->init_error = DiagStatus_CantAssignPciResources;
		goto errout;
	}
	return 0;

errout:
	release_resources(info);
	return -ENODEV;
}

static void release_resources(struct slgt_info *info)
{
	if (info->irq_requested) {
		free_irq(info->irq_level, info);
		info->irq_requested = 0;
	}

	if (info->reg_addr_requested) {
		release_mem_region(info->phys_reg_addr, SLGT_REG_SIZE);
		info->reg_addr_requested = 0;
	}

	if (info->reg_addr) {
		iounmap(info->reg_addr);
		info->reg_addr = NULL;
	}
}

/* Add the specified device instance data structure to the
 * global linked list of devices and increment the device count.
 */
static void add_device(struct slgt_info *info)
{
	char *devstr;

	info->next_device = NULL;
	info->line = slgt_device_count;
	sprintf(info->device_name, "%s%d", tty_dev_prefix, info->line);

	if (info->line < MAX_DEVICES) {
		if (maxframe[info->line])
			info->max_frame_size = maxframe[info->line];
		info->dosyncppp = dosyncppp[info->line];
	}

	slgt_device_count++;

	if (!slgt_device_list)
		slgt_device_list = info;
	else {
		struct slgt_info *current_dev = slgt_device_list;
		while(current_dev->next_device)
			current_dev = current_dev->next_device;
		current_dev->next_device = info;
	}

	if (info->max_frame_size < 4096)
		info->max_frame_size = 4096;
	else if (info->max_frame_size > 65535)
		info->max_frame_size = 65535;

	switch(info->pdev->device) {
	case SYNCLINK_GT_DEVICE_ID:
		devstr = "GT";
		break;
	case SYNCLINK_GT4_DEVICE_ID:
		devstr = "GT4";
		break;
	case SYNCLINK_AC_DEVICE_ID:
		devstr = "AC";
		info->params.mode = MGSL_MODE_ASYNC;
		break;
	default:
		devstr = "(unknown model)";
	}
	printk("SyncLink %s %s IO=%08x IRQ=%d MaxFrameSize=%u\n",
		devstr, info->device_name, info->phys_reg_addr,
		info->irq_level, info->max_frame_size);

#ifdef CONFIG_HDLC
	hdlcdev_init(info);
#endif
}

/*
 *  allocate device instance structure, return NULL on failure
 */
static struct slgt_info *alloc_dev(int adapter_num, int port_num, struct pci_dev *pdev)
{
	struct slgt_info *info;

	info = kmalloc(sizeof(struct slgt_info), GFP_KERNEL);

	if (!info) {
		DBGERR(("%s device alloc failed adapter=%d port=%d\n",
			driver_name, adapter_num, port_num));
	} else {
		memset(info, 0, sizeof(struct slgt_info));
		info->magic = MGSL_MAGIC;
		INIT_WORK(&info->task, bh_handler, info);
		info->max_frame_size = 4096;
		info->raw_rx_size = DMABUFSIZE;
		info->close_delay = 5*HZ/10;
		info->closing_wait = 30*HZ;
		init_waitqueue_head(&info->open_wait);
		init_waitqueue_head(&info->close_wait);
		init_waitqueue_head(&info->status_event_wait_q);
		init_waitqueue_head(&info->event_wait_q);
		spin_lock_init(&info->netlock);
		memcpy(&info->params,&default_params,sizeof(MGSL_PARAMS));
		info->idle_mode = HDLC_TXIDLE_FLAGS;
		info->adapter_num = adapter_num;
		info->port_num = port_num;

		init_timer(&info->tx_timer);
		info->tx_timer.data = (unsigned long)info;
		info->tx_timer.function = tx_timeout;

		init_timer(&info->rx_timer);
		info->rx_timer.data = (unsigned long)info;
		info->rx_timer.function = rx_timeout;

		/* Copy configuration info to device instance data */
		info->pdev = pdev;
		info->irq_level = pdev->irq;
		info->phys_reg_addr = pci_resource_start(pdev,0);

		info->bus_type = MGSL_BUS_TYPE_PCI;
		info->irq_flags = SA_SHIRQ;

		info->init_error = -1; /* assume error, set to 0 on successful init */
	}

	return info;
}

static void device_init(int adapter_num, struct pci_dev *pdev)
{
	struct slgt_info *port_array[SLGT_MAX_PORTS];
	int i;
	int port_count = 1;

	if (pdev->device == SYNCLINK_GT4_DEVICE_ID)
		port_count = 4;

	/* allocate device instances for all ports */
	for (i=0; i < port_count; ++i) {
		port_array[i] = alloc_dev(adapter_num, i, pdev);
		if (port_array[i] == NULL) {
			for (--i; i >= 0; --i)
				kfree(port_array[i]);
			return;
		}
	}

	/* give copy of port_array to all ports and add to device list  */
	for (i=0; i < port_count; ++i) {
		memcpy(port_array[i]->port_array, port_array, sizeof(port_array));
		add_device(port_array[i]);
		port_array[i]->port_count = port_count;
		spin_lock_init(&port_array[i]->lock);
	}

	/* Allocate and claim adapter resources */
	if (!claim_resources(port_array[0])) {

		alloc_dma_bufs(port_array[0]);

		/* copy resource information from first port to others */
		for (i = 1; i < port_count; ++i) {
			port_array[i]->lock      = port_array[0]->lock;
			port_array[i]->irq_level = port_array[0]->irq_level;
			port_array[i]->reg_addr  = port_array[0]->reg_addr;
			alloc_dma_bufs(port_array[i]);
		}

		if (request_irq(port_array[0]->irq_level,
					slgt_interrupt,
					port_array[0]->irq_flags,
					port_array[0]->device_name,
					port_array[0]) < 0) {
			DBGERR(("%s request_irq failed IRQ=%d\n",
				port_array[0]->device_name,
				port_array[0]->irq_level));
		} else {
			port_array[0]->irq_requested = 1;
			adapter_test(port_array[0]);
			for (i=1 ; i < port_count ; i++)
				port_array[i]->init_error = port_array[0]->init_error;
		}
	}
}

static int __devinit init_one(struct pci_dev *dev,
			      const struct pci_device_id *ent)
{
	if (pci_enable_device(dev)) {
		printk("error enabling pci device %p\n", dev);
		return -EIO;
	}
	pci_set_master(dev);
	device_init(slgt_device_count, dev);
	return 0;
}

static void __devexit remove_one(struct pci_dev *dev)
{
}

static struct tty_operations ops = {
	.open = open,
	.close = close,
	.write = write,
	.put_char = put_char,
	.flush_chars = flush_chars,
	.write_room = write_room,
	.chars_in_buffer = chars_in_buffer,
	.flush_buffer = flush_buffer,
	.ioctl = ioctl,
	.throttle = throttle,
	.unthrottle = unthrottle,
	.send_xchar = send_xchar,
	.break_ctl = set_break,
	.wait_until_sent = wait_until_sent,
 	.read_proc = read_proc,
	.set_termios = set_termios,
	.stop = tx_hold,
	.start = tx_release,
	.hangup = hangup,
	.tiocmget = tiocmget,
	.tiocmset = tiocmset,
};

static void slgt_cleanup(void)
{
	int rc;
	struct slgt_info *info;
	struct slgt_info *tmp;

	printk("unload %s %s\n", driver_name, driver_version);

	if (serial_driver) {
		if ((rc = tty_unregister_driver(serial_driver)))
			DBGERR(("tty_unregister_driver error=%d\n", rc));
		put_tty_driver(serial_driver);
	}

	/* reset devices */
	info = slgt_device_list;
	while(info) {
		reset_port(info);
		info = info->next_device;
	}

	/* release devices */
	info = slgt_device_list;
	while(info) {
#ifdef CONFIG_HDLC
		hdlcdev_exit(info);
#endif
		free_dma_bufs(info);
		free_tmp_rbuf(info);
		if (info->port_num == 0)
			release_resources(info);
		tmp = info;
		info = info->next_device;
		kfree(tmp);
	}

	if (pci_registered)
		pci_unregister_driver(&pci_driver);
}

/*
 *  Driver initialization entry point.
 */
static int __init slgt_init(void)
{
	int rc;

 	printk("%s %s\n", driver_name, driver_version);

	slgt_device_count = 0;
	if ((rc = pci_register_driver(&pci_driver)) < 0) {
		printk("%s pci_register_driver error=%d\n", driver_name, rc);
		return rc;
	}
	pci_registered = 1;

	if (!slgt_device_list) {
		printk("%s no devices found\n",driver_name);
		return -ENODEV;
	}

	serial_driver = alloc_tty_driver(MAX_DEVICES);
	if (!serial_driver) {
		rc = -ENOMEM;
		goto error;
	}

	/* Initialize the tty_driver structure */

	serial_driver->owner = THIS_MODULE;
	serial_driver->driver_name = tty_driver_name;
	serial_driver->name = tty_dev_prefix;
	serial_driver->major = ttymajor;
	serial_driver->minor_start = 64;
	serial_driver->type = TTY_DRIVER_TYPE_SERIAL;
	serial_driver->subtype = SERIAL_TYPE_NORMAL;
	serial_driver->init_termios = tty_std_termios;
	serial_driver->init_termios.c_cflag =
		B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	serial_driver->flags = TTY_DRIVER_REAL_RAW;
	tty_set_operations(serial_driver, &ops);
	if ((rc = tty_register_driver(serial_driver)) < 0) {
		DBGERR(("%s can't register serial driver\n", driver_name));
		put_tty_driver(serial_driver);
		serial_driver = NULL;
		goto error;
	}

 	printk("%s %s, tty major#%d\n",
		driver_name, driver_version,
		serial_driver->major);

	return 0;

error:
	slgt_cleanup();
	return rc;
}

static void __exit slgt_exit(void)
{
	slgt_cleanup();
}

module_init(slgt_init);
module_exit(slgt_exit);

/*
 * register access routines
 */

#define CALC_REGADDR() \
	unsigned long reg_addr = ((unsigned long)info->reg_addr) + addr; \
	if (addr >= 0x80) \
		reg_addr += (info->port_num) * 32;

static __u8 rd_reg8(struct slgt_info *info, unsigned int addr)
{
	CALC_REGADDR();
	return readb((void __iomem *)reg_addr);
}

static void wr_reg8(struct slgt_info *info, unsigned int addr, __u8 value)
{
	CALC_REGADDR();
	writeb(value, (void __iomem *)reg_addr);
}

static __u16 rd_reg16(struct slgt_info *info, unsigned int addr)
{
	CALC_REGADDR();
	return readw((void __iomem *)reg_addr);
}

static void wr_reg16(struct slgt_info *info, unsigned int addr, __u16 value)
{
	CALC_REGADDR();
	writew(value, (void __iomem *)reg_addr);
}

static __u32 rd_reg32(struct slgt_info *info, unsigned int addr)
{
	CALC_REGADDR();
	return readl((void __iomem *)reg_addr);
}

static void wr_reg32(struct slgt_info *info, unsigned int addr, __u32 value)
{
	CALC_REGADDR();
	writel(value, (void __iomem *)reg_addr);
}

static void rdma_reset(struct slgt_info *info)
{
	unsigned int i;

	/* set reset bit */
	wr_reg32(info, RDCSR, BIT1);

	/* wait for enable bit cleared */
	for(i=0 ; i < 1000 ; i++)
		if (!(rd_reg32(info, RDCSR) & BIT0))
			break;
}

static void tdma_reset(struct slgt_info *info)
{
	unsigned int i;

	/* set reset bit */
	wr_reg32(info, TDCSR, BIT1);

	/* wait for enable bit cleared */
	for(i=0 ; i < 1000 ; i++)
		if (!(rd_reg32(info, TDCSR) & BIT0))
			break;
}

/*
 * enable internal loopback
 * TxCLK and RxCLK are generated from BRG
 * and TxD is looped back to RxD internally.
 */
static void enable_loopback(struct slgt_info *info)
{
	/* SCR (serial control) BIT2=looopback enable */
	wr_reg16(info, SCR, (unsigned short)(rd_reg16(info, SCR) | BIT2));

	if (info->params.mode != MGSL_MODE_ASYNC) {
		/* CCR (clock control)
		 * 07..05  tx clock source (010 = BRG)
		 * 04..02  rx clock source (010 = BRG)
		 * 01      auxclk enable   (0 = disable)
		 * 00      BRG enable      (1 = enable)
		 *
		 * 0100 1001
		 */
		wr_reg8(info, CCR, 0x49);

		/* set speed if available, otherwise use default */
		if (info->params.clock_speed)
			set_rate(info, info->params.clock_speed);
		else
			set_rate(info, 3686400);
	}
}

/*
 *  set baud rate generator to specified rate
 */
static void set_rate(struct slgt_info *info, u32 rate)
{
	unsigned int div;
	static unsigned int osc = 14745600;

	/* div = osc/rate - 1
	 *
	 * Round div up if osc/rate is not integer to
	 * force to next slowest rate.
	 */

	if (rate) {
		div = osc/rate;
		if (!(osc % rate) && div)
			div--;
		wr_reg16(info, BDR, (unsigned short)div);
	}
}

static void rx_stop(struct slgt_info *info)
{
	unsigned short val;

	/* disable and reset receiver */
	val = rd_reg16(info, RCR) & ~BIT1;          /* clear enable bit */
	wr_reg16(info, RCR, (unsigned short)(val | BIT2)); /* set reset bit */
	wr_reg16(info, RCR, val);                  /* clear reset bit */

	slgt_irq_off(info, IRQ_RXOVER + IRQ_RXDATA + IRQ_RXIDLE);

	/* clear pending rx interrupts */
	wr_reg16(info, SSR, IRQ_RXIDLE + IRQ_RXOVER);

	rdma_reset(info);

	info->rx_enabled = 0;
	info->rx_restart = 0;
}

static void rx_start(struct slgt_info *info)
{
	unsigned short val;

	slgt_irq_off(info, IRQ_RXOVER + IRQ_RXDATA);

	/* clear pending rx overrun IRQ */
	wr_reg16(info, SSR, IRQ_RXOVER);

	/* reset and disable receiver */
	val = rd_reg16(info, RCR) & ~BIT1; /* clear enable bit */
	wr_reg16(info, RCR, (unsigned short)(val | BIT2)); /* set reset bit */
	wr_reg16(info, RCR, val);                  /* clear reset bit */

	rdma_reset(info);
	reset_rbufs(info);

	/* set 1st descriptor address */
	wr_reg32(info, RDDAR, info->rbufs[0].pdesc);

	if (info->params.mode != MGSL_MODE_ASYNC) {
		/* enable rx DMA and DMA interrupt */
		wr_reg32(info, RDCSR, (BIT2 + BIT0));
	} else {
		/* enable saving of rx status, rx DMA and DMA interrupt */
		wr_reg32(info, RDCSR, (BIT6 + BIT2 + BIT0));
	}

	slgt_irq_on(info, IRQ_RXOVER);

	/* enable receiver */
	wr_reg16(info, RCR, (unsigned short)(rd_reg16(info, RCR) | BIT1));

	info->rx_restart = 0;
	info->rx_enabled = 1;
}

static void tx_start(struct slgt_info *info)
{
	if (!info->tx_enabled) {
		wr_reg16(info, TCR,
			(unsigned short)(rd_reg16(info, TCR) | BIT1));
		info->tx_enabled = TRUE;
	}

	if (info->tx_count) {
		info->drop_rts_on_tx_done = 0;

		if (info->params.mode != MGSL_MODE_ASYNC) {
			if (info->params.flags & HDLC_FLAG_AUTO_RTS) {
				get_signals(info);
				if (!(info->signals & SerialSignal_RTS)) {
					info->signals |= SerialSignal_RTS;
					set_signals(info);
					info->drop_rts_on_tx_done = 1;
				}
			}

			slgt_irq_off(info, IRQ_TXDATA);
			slgt_irq_on(info, IRQ_TXUNDER + IRQ_TXIDLE);
			/* clear tx idle and underrun status bits */
			wr_reg16(info, SSR, (unsigned short)(IRQ_TXIDLE + IRQ_TXUNDER));

			if (!(rd_reg32(info, TDCSR) & BIT0)) {
				/* tx DMA stopped, restart tx DMA */
				tdma_reset(info);
				/* set 1st descriptor address */
				wr_reg32(info, TDDAR, info->tbufs[info->tbuf_start].pdesc);
				if (info->params.mode == MGSL_MODE_RAW)
					wr_reg32(info, TDCSR, BIT2 + BIT0); /* IRQ + DMA enable */
				else
					wr_reg32(info, TDCSR, BIT0); /* DMA enable */
			}

			if (info->params.mode != MGSL_MODE_RAW) {
				info->tx_timer.expires = jiffies + msecs_to_jiffies(5000);
				add_timer(&info->tx_timer);
			}
		} else {
			tdma_reset(info);
			/* set 1st descriptor address */
			wr_reg32(info, TDDAR, info->tbufs[info->tbuf_start].pdesc);

			slgt_irq_off(info, IRQ_TXDATA);
			slgt_irq_on(info, IRQ_TXIDLE);
			/* clear tx idle status bit */
			wr_reg16(info, SSR, IRQ_TXIDLE);

			/* enable tx DMA */
			wr_reg32(info, TDCSR, BIT0);
		}

		info->tx_active = 1;
	}
}

static void tx_stop(struct slgt_info *info)
{
	unsigned short val;

	del_timer(&info->tx_timer);

	tdma_reset(info);

	/* reset and disable transmitter */
	val = rd_reg16(info, TCR) & ~BIT1;          /* clear enable bit */
	wr_reg16(info, TCR, (unsigned short)(val | BIT2)); /* set reset bit */
	wr_reg16(info, TCR, val);                  /* clear reset */

	slgt_irq_off(info, IRQ_TXDATA + IRQ_TXIDLE + IRQ_TXUNDER);

	/* clear tx idle and underrun status bit */
	wr_reg16(info, SSR, (unsigned short)(IRQ_TXIDLE + IRQ_TXUNDER));

	reset_tbufs(info);

	info->tx_enabled = 0;
	info->tx_active  = 0;
}

static void reset_port(struct slgt_info *info)
{
	if (!info->reg_addr)
		return;

	tx_stop(info);
	rx_stop(info);

	info->signals &= ~(SerialSignal_DTR + SerialSignal_RTS);
	set_signals(info);

	slgt_irq_off(info, IRQ_ALL | IRQ_MASTER);
}

static void reset_adapter(struct slgt_info *info)
{
	int i;
	for (i=0; i < info->port_count; ++i) {
		if (info->port_array[i])
			reset_port(info->port_array[i]);
	}
}

static void async_mode(struct slgt_info *info)
{
  	unsigned short val;

	slgt_irq_off(info, IRQ_ALL | IRQ_MASTER);
	tx_stop(info);
	rx_stop(info);

	/* TCR (tx control)
	 *
	 * 15..13  mode, 010=async
	 * 12..10  encoding, 000=NRZ
	 * 09      parity enable
	 * 08      1=odd parity, 0=even parity
	 * 07      1=RTS driver control
	 * 06      1=break enable
	 * 05..04  character length
	 *         00=5 bits
	 *         01=6 bits
	 *         10=7 bits
	 *         11=8 bits
	 * 03      0=1 stop bit, 1=2 stop bits
	 * 02      reset
	 * 01      enable
	 * 00      auto-CTS enable
	 */
	val = 0x4000;

	if (info->if_mode & MGSL_INTERFACE_RTS_EN)
		val |= BIT7;

	if (info->params.parity != ASYNC_PARITY_NONE) {
		val |= BIT9;
		if (info->params.parity == ASYNC_PARITY_ODD)
			val |= BIT8;
	}

	switch (info->params.data_bits)
	{
	case 6: val |= BIT4; break;
	case 7: val |= BIT5; break;
	case 8: val |= BIT5 + BIT4; break;
	}

	if (info->params.stop_bits != 1)
		val |= BIT3;

	if (info->params.flags & HDLC_FLAG_AUTO_CTS)
		val |= BIT0;

	wr_reg16(info, TCR, val);

	/* RCR (rx control)
	 *
	 * 15..13  mode, 010=async
	 * 12..10  encoding, 000=NRZ
	 * 09      parity enable
	 * 08      1=odd parity, 0=even parity
	 * 07..06  reserved, must be 0
	 * 05..04  character length
	 *         00=5 bits
	 *         01=6 bits
	 *         10=7 bits
	 *         11=8 bits
	 * 03      reserved, must be zero
	 * 02      reset
	 * 01      enable
	 * 00      auto-DCD enable
	 */
	val = 0x4000;

	if (info->params.parity != ASYNC_PARITY_NONE) {
		val |= BIT9;
		if (info->params.parity == ASYNC_PARITY_ODD)
			val |= BIT8;
	}

	switch (info->params.data_bits)
	{
	case 6: val |= BIT4; break;
	case 7: val |= BIT5; break;
	case 8: val |= BIT5 + BIT4; break;
	}

	if (info->params.flags & HDLC_FLAG_AUTO_DCD)
		val |= BIT0;

	wr_reg16(info, RCR, val);

	/* CCR (clock control)
	 *
	 * 07..05  011 = tx clock source is BRG/16
	 * 04..02  010 = rx clock source is BRG
	 * 01      0 = auxclk disabled
	 * 00      1 = BRG enabled
	 *
	 * 0110 1001
	 */
	wr_reg8(info, CCR, 0x69);

	msc_set_vcr(info);

	tx_set_idle(info);

	/* SCR (serial control)
	 *
	 * 15  1=tx req on FIFO half empty
	 * 14  1=rx req on FIFO half full
	 * 13  tx data  IRQ enable
	 * 12  tx idle  IRQ enable
	 * 11  rx break on IRQ enable
	 * 10  rx data  IRQ enable
	 * 09  rx break off IRQ enable
	 * 08  overrun  IRQ enable
	 * 07  DSR      IRQ enable
	 * 06  CTS      IRQ enable
	 * 05  DCD      IRQ enable
	 * 04  RI       IRQ enable
	 * 03  reserved, must be zero
	 * 02  1=txd->rxd internal loopback enable
	 * 01  reserved, must be zero
	 * 00  1=master IRQ enable
	 */
	val = BIT15 + BIT14 + BIT0;
	wr_reg16(info, SCR, val);

	slgt_irq_on(info, IRQ_RXBREAK | IRQ_RXOVER);

	set_rate(info, info->params.data_rate * 16);

	if (info->params.loopback)
		enable_loopback(info);
}

static void hdlc_mode(struct slgt_info *info)
{
	unsigned short val;

	slgt_irq_off(info, IRQ_ALL | IRQ_MASTER);
	tx_stop(info);
	rx_stop(info);

	/* TCR (tx control)
	 *
	 * 15..13  mode, 000=HDLC 001=raw sync
	 * 12..10  encoding
	 * 09      CRC enable
	 * 08      CRC32
	 * 07      1=RTS driver control
	 * 06      preamble enable
	 * 05..04  preamble length
	 * 03      share open/close flag
	 * 02      reset
	 * 01      enable
	 * 00      auto-CTS enable
	 */
	val = 0;

	if (info->params.mode == MGSL_MODE_RAW)
		val |= BIT13;
	if (info->if_mode & MGSL_INTERFACE_RTS_EN)
		val |= BIT7;

	switch(info->params.encoding)
	{
	case HDLC_ENCODING_NRZB:          val |= BIT10; break;
	case HDLC_ENCODING_NRZI_MARK:     val |= BIT11; break;
	case HDLC_ENCODING_NRZI:          val |= BIT11 + BIT10; break;
	case HDLC_ENCODING_BIPHASE_MARK:  val |= BIT12; break;
	case HDLC_ENCODING_BIPHASE_SPACE: val |= BIT12 + BIT10; break;
	case HDLC_ENCODING_BIPHASE_LEVEL: val |= BIT12 + BIT11; break;
	case HDLC_ENCODING_DIFF_BIPHASE_LEVEL: val |= BIT12 + BIT11 + BIT10; break;
	}

	switch (info->params.crc_type)
	{
	case HDLC_CRC_16_CCITT: val |= BIT9; break;
	case HDLC_CRC_32_CCITT: val |= BIT9 + BIT8; break;
	}

	if (info->params.preamble != HDLC_PREAMBLE_PATTERN_NONE)
		val |= BIT6;

	switch (info->params.preamble_length)
	{
	case HDLC_PREAMBLE_LENGTH_16BITS: val |= BIT5; break;
	case HDLC_PREAMBLE_LENGTH_32BITS: val |= BIT4; break;
	case HDLC_PREAMBLE_LENGTH_64BITS: val |= BIT5 + BIT4; break;
	}

	if (info->params.flags & HDLC_FLAG_AUTO_CTS)
		val |= BIT0;

	wr_reg16(info, TCR, val);

	/* TPR (transmit preamble) */

	switch (info->params.preamble)
	{
	case HDLC_PREAMBLE_PATTERN_FLAGS: val = 0x7e; break;
	case HDLC_PREAMBLE_PATTERN_ONES:  val = 0xff; break;
	case HDLC_PREAMBLE_PATTERN_ZEROS: val = 0x00; break;
	case HDLC_PREAMBLE_PATTERN_10:    val = 0x55; break;
	case HDLC_PREAMBLE_PATTERN_01:    val = 0xaa; break;
	default:                          val = 0x7e; break;
	}
	wr_reg8(info, TPR, (unsigned char)val);

	/* RCR (rx control)
	 *
	 * 15..13  mode, 000=HDLC 001=raw sync
	 * 12..10  encoding
	 * 09      CRC enable
	 * 08      CRC32
	 * 07..03  reserved, must be 0
	 * 02      reset
	 * 01      enable
	 * 00      auto-DCD enable
	 */
	val = 0;

	if (info->params.mode == MGSL_MODE_RAW)
		val |= BIT13;

	switch(info->params.encoding)
	{
	case HDLC_ENCODING_NRZB:          val |= BIT10; break;
	case HDLC_ENCODING_NRZI_MARK:     val |= BIT11; break;
	case HDLC_ENCODING_NRZI:          val |= BIT11 + BIT10; break;
	case HDLC_ENCODING_BIPHASE_MARK:  val |= BIT12; break;
	case HDLC_ENCODING_BIPHASE_SPACE: val |= BIT12 + BIT10; break;
	case HDLC_ENCODING_BIPHASE_LEVEL: val |= BIT12 + BIT11; break;
	case HDLC_ENCODING_DIFF_BIPHASE_LEVEL: val |= BIT12 + BIT11 + BIT10; break;
	}

	switch (info->params.crc_type)
	{
	case HDLC_CRC_16_CCITT: val |= BIT9; break;
	case HDLC_CRC_32_CCITT: val |= BIT9 + BIT8; break;
	}

	if (info->params.flags & HDLC_FLAG_AUTO_DCD)
		val |= BIT0;

	wr_reg16(info, RCR, val);

	/* CCR (clock control)
	 *
	 * 07..05  tx clock source
	 * 04..02  rx clock source
	 * 01      auxclk enable
	 * 00      BRG enable
	 */
	val = 0;

	if (info->params.flags & HDLC_FLAG_TXC_BRG)
	{
		// when RxC source is DPLL, BRG generates 16X DPLL
		// reference clock, so take TxC from BRG/16 to get
		// transmit clock at actual data rate
		if (info->params.flags & HDLC_FLAG_RXC_DPLL)
			val |= BIT6 + BIT5;	/* 011, txclk = BRG/16 */
		else
			val |= BIT6;	/* 010, txclk = BRG */
	}
	else if (info->params.flags & HDLC_FLAG_TXC_DPLL)
		val |= BIT7;	/* 100, txclk = DPLL Input */
	else if (info->params.flags & HDLC_FLAG_TXC_RXCPIN)
		val |= BIT5;	/* 001, txclk = RXC Input */

	if (info->params.flags & HDLC_FLAG_RXC_BRG)
		val |= BIT3;	/* 010, rxclk = BRG */
	else if (info->params.flags & HDLC_FLAG_RXC_DPLL)
		val |= BIT4;	/* 100, rxclk = DPLL */
	else if (info->params.flags & HDLC_FLAG_RXC_TXCPIN)
		val |= BIT2;	/* 001, rxclk = TXC Input */

	if (info->params.clock_speed)
		val |= BIT1 + BIT0;

	wr_reg8(info, CCR, (unsigned char)val);

	if (info->params.flags & (HDLC_FLAG_TXC_DPLL + HDLC_FLAG_RXC_DPLL))
	{
		// program DPLL mode
		switch(info->params.encoding)
		{
		case HDLC_ENCODING_BIPHASE_MARK:
		case HDLC_ENCODING_BIPHASE_SPACE:
			val = BIT7; break;
		case HDLC_ENCODING_BIPHASE_LEVEL:
		case HDLC_ENCODING_DIFF_BIPHASE_LEVEL:
			val = BIT7 + BIT6; break;
		default: val = BIT6;	// NRZ encodings
		}
		wr_reg16(info, RCR, (unsigned short)(rd_reg16(info, RCR) | val));

		// DPLL requires a 16X reference clock from BRG
		set_rate(info, info->params.clock_speed * 16);
	}
	else
		set_rate(info, info->params.clock_speed);

	tx_set_idle(info);

	msc_set_vcr(info);

	/* SCR (serial control)
	 *
	 * 15  1=tx req on FIFO half empty
	 * 14  1=rx req on FIFO half full
	 * 13  tx data  IRQ enable
	 * 12  tx idle  IRQ enable
	 * 11  underrun IRQ enable
	 * 10  rx data  IRQ enable
	 * 09  rx idle  IRQ enable
	 * 08  overrun  IRQ enable
	 * 07  DSR      IRQ enable
	 * 06  CTS      IRQ enable
	 * 05  DCD      IRQ enable
	 * 04  RI       IRQ enable
	 * 03  reserved, must be zero
	 * 02  1=txd->rxd internal loopback enable
	 * 01  reserved, must be zero
	 * 00  1=master IRQ enable
	 */
	wr_reg16(info, SCR, BIT15 + BIT14 + BIT0);

	if (info->params.loopback)
		enable_loopback(info);
}

/*
 *  set transmit idle mode
 */
static void tx_set_idle(struct slgt_info *info)
{
	unsigned char val = 0xff;

	switch(info->idle_mode)
	{
	case HDLC_TXIDLE_FLAGS:          val = 0x7e; break;
	case HDLC_TXIDLE_ALT_ZEROS_ONES: val = 0xaa; break;
	case HDLC_TXIDLE_ZEROS:          val = 0x00; break;
	case HDLC_TXIDLE_ONES:           val = 0xff; break;
	case HDLC_TXIDLE_ALT_MARK_SPACE: val = 0xaa; break;
	case HDLC_TXIDLE_SPACE:          val = 0x00; break;
	case HDLC_TXIDLE_MARK:           val = 0xff; break;
	}

	wr_reg8(info, TIR, val);
}

/*
 * get state of V24 status (input) signals
 */
static void get_signals(struct slgt_info *info)
{
	unsigned short status = rd_reg16(info, SSR);

	/* clear all serial signals except DTR and RTS */
	info->signals &= SerialSignal_DTR + SerialSignal_RTS;

	if (status & BIT3)
		info->signals |= SerialSignal_DSR;
	if (status & BIT2)
		info->signals |= SerialSignal_CTS;
	if (status & BIT1)
		info->signals |= SerialSignal_DCD;
	if (status & BIT0)
		info->signals |= SerialSignal_RI;
}

/*
 * set V.24 Control Register based on current configuration
 */
static void msc_set_vcr(struct slgt_info *info)
{
	unsigned char val = 0;

	/* VCR (V.24 control)
	 *
	 * 07..04  serial IF select
	 * 03      DTR
	 * 02      RTS
	 * 01      LL
	 * 00      RL
	 */

	switch(info->if_mode & MGSL_INTERFACE_MASK)
	{
	case MGSL_INTERFACE_RS232:
		val |= BIT5; /* 0010 */
		break;
	case MGSL_INTERFACE_V35:
		val |= BIT7 + BIT6 + BIT5; /* 1110 */
		break;
	case MGSL_INTERFACE_RS422:
		val |= BIT6; /* 0100 */
		break;
	}

	if (info->signals & SerialSignal_DTR)
		val |= BIT3;
	if (info->signals & SerialSignal_RTS)
		val |= BIT2;
	if (info->if_mode & MGSL_INTERFACE_LL)
		val |= BIT1;
	if (info->if_mode & MGSL_INTERFACE_RL)
		val |= BIT0;
	wr_reg8(info, VCR, val);
}

/*
 * set state of V24 control (output) signals
 */
static void set_signals(struct slgt_info *info)
{
	unsigned char val = rd_reg8(info, VCR);
	if (info->signals & SerialSignal_DTR)
		val |= BIT3;
	else
		val &= ~BIT3;
	if (info->signals & SerialSignal_RTS)
		val |= BIT2;
	else
		val &= ~BIT2;
	wr_reg8(info, VCR, val);
}

/*
 * free range of receive DMA buffers (i to last)
 */
static void free_rbufs(struct slgt_info *info, unsigned int i, unsigned int last)
{
	int done = 0;

	while(!done) {
		/* reset current buffer for reuse */
		info->rbufs[i].status = 0;
		if (info->params.mode == MGSL_MODE_RAW)
			set_desc_count(info->rbufs[i], info->raw_rx_size);
		else
			set_desc_count(info->rbufs[i], DMABUFSIZE);

		if (i == last)
			done = 1;
		if (++i == info->rbuf_count)
			i = 0;
	}
	info->rbuf_current = i;
}

/*
 * mark all receive DMA buffers as free
 */
static void reset_rbufs(struct slgt_info *info)
{
	free_rbufs(info, 0, info->rbuf_count - 1);
}

/*
 * pass receive HDLC frame to upper layer
 *
 * return 1 if frame available, otherwise 0
 */
static int rx_get_frame(struct slgt_info *info)
{
	unsigned int start, end;
	unsigned short status;
	unsigned int framesize = 0;
	int rc = 0;
	unsigned long flags;
	struct tty_struct *tty = info->tty;
	unsigned char addr_field = 0xff;

check_again:

	framesize = 0;
	addr_field = 0xff;
	start = end = info->rbuf_current;

	for (;;) {
		if (!desc_complete(info->rbufs[end]))
			goto cleanup;

		if (framesize == 0 && info->params.addr_filter != 0xff)
			addr_field = info->rbufs[end].buf[0];

		framesize += desc_count(info->rbufs[end]);

		if (desc_eof(info->rbufs[end]))
			break;

		if (++end == info->rbuf_count)
			end = 0;

		if (end == info->rbuf_current) {
			if (info->rx_enabled){
				spin_lock_irqsave(&info->lock,flags);
				rx_start(info);
				spin_unlock_irqrestore(&info->lock,flags);
			}
			goto cleanup;
		}
	}

	/* status
	 *
	 * 15      buffer complete
	 * 14..06  reserved
	 * 05..04  residue
	 * 02      eof (end of frame)
	 * 01      CRC error
	 * 00      abort
	 */
	status = desc_status(info->rbufs[end]);

	/* ignore CRC bit if not using CRC (bit is undefined) */
	if (info->params.crc_type == HDLC_CRC_NONE)
		status &= ~BIT1;

	if (framesize == 0 ||
		 (addr_field != 0xff && addr_field != info->params.addr_filter)) {
		free_rbufs(info, start, end);
		goto check_again;
	}

	if (framesize < 2 || status & (BIT1+BIT0)) {
		if (framesize < 2 || (status & BIT0))
			info->icount.rxshort++;
		else
			info->icount.rxcrc++;
		framesize = 0;

#ifdef CONFIG_HDLC
		{
			struct net_device_stats *stats = hdlc_stats(info->netdev);
			stats->rx_errors++;
			stats->rx_frame_errors++;
		}
#endif
	} else {
		/* adjust frame size for CRC, if any */
		if (info->params.crc_type == HDLC_CRC_16_CCITT)
			framesize -= 2;
		else if (info->params.crc_type == HDLC_CRC_32_CCITT)
			framesize -= 4;
	}

	DBGBH(("%s rx frame status=%04X size=%d\n",
		info->device_name, status, framesize));
	DBGDATA(info, info->rbufs[start].buf, min_t(int, framesize, DMABUFSIZE), "rx");

	if (framesize) {
		if (framesize > info->max_frame_size)
			info->icount.rxlong++;
		else {
			/* copy dma buffer(s) to contiguous temp buffer */
			int copy_count = framesize;
			int i = start;
			unsigned char *p = info->tmp_rbuf;
			info->tmp_rbuf_count = framesize;

			info->icount.rxok++;

			while(copy_count) {
				int partial_count = min(copy_count, DMABUFSIZE);
				memcpy(p, info->rbufs[i].buf, partial_count);
				p += partial_count;
				copy_count -= partial_count;
				if (++i == info->rbuf_count)
					i = 0;
			}

#ifdef CONFIG_HDLC
			if (info->netcount)
				hdlcdev_rx(info,info->tmp_rbuf, framesize);
			else
#endif
				ldisc_receive_buf(tty, info->tmp_rbuf, info->flag_buf, framesize);
		}
	}
	free_rbufs(info, start, end);
	rc = 1;

cleanup:
	return rc;
}

/*
 * pass receive buffer (RAW synchronous mode) to tty layer
 * return 1 if buffer available, otherwise 0
 */
static int rx_get_buf(struct slgt_info *info)
{
	unsigned int i = info->rbuf_current;

	if (!desc_complete(info->rbufs[i]))
		return 0;
	DBGDATA(info, info->rbufs[i].buf, desc_count(info->rbufs[i]), "rx");
	DBGINFO(("rx_get_buf size=%d\n", desc_count(info->rbufs[i])));
	ldisc_receive_buf(info->tty, info->rbufs[i].buf,
			  info->flag_buf, desc_count(info->rbufs[i]));
	free_rbufs(info, i, i);
	return 1;
}

static void reset_tbufs(struct slgt_info *info)
{
	unsigned int i;
	info->tbuf_current = 0;
	for (i=0 ; i < info->tbuf_count ; i++) {
		info->tbufs[i].status = 0;
		info->tbufs[i].count  = 0;
	}
}

/*
 * return number of free transmit DMA buffers
 */
static unsigned int free_tbuf_count(struct slgt_info *info)
{
	unsigned int count = 0;
	unsigned int i = info->tbuf_current;

	do
	{
		if (desc_count(info->tbufs[i]))
			break; /* buffer in use */
		++count;
		if (++i == info->tbuf_count)
			i=0;
	} while (i != info->tbuf_current);

	/* last buffer with zero count may be in use, assume it is */
	if (count)
		--count;

	return count;
}

/*
 * load transmit DMA buffer(s) with data
 */
static void tx_load(struct slgt_info *info, const char *buf, unsigned int size)
{
	unsigned short count;
	unsigned int i;
	struct slgt_desc *d;

	if (size == 0)
		return;

	DBGDATA(info, buf, size, "tx");

	info->tbuf_start = i = info->tbuf_current;

	while (size) {
		d = &info->tbufs[i];
		if (++i == info->tbuf_count)
			i = 0;

		count = (unsigned short)((size > DMABUFSIZE) ? DMABUFSIZE : size);
		memcpy(d->buf, buf, count);

		size -= count;
		buf  += count;

		if (!size && info->params.mode != MGSL_MODE_RAW)
			set_desc_eof(*d, 1); /* HDLC: set EOF of last desc */
		else
			set_desc_eof(*d, 0);

		set_desc_count(*d, count);
	}

	info->tbuf_current = i;
}

static int register_test(struct slgt_info *info)
{
	static unsigned short patterns[] =
		{0x0000, 0xffff, 0xaaaa, 0x5555, 0x6969, 0x9696};
	static unsigned int count = sizeof(patterns)/sizeof(patterns[0]);
	unsigned int i;
	int rc = 0;

	for (i=0 ; i < count ; i++) {
		wr_reg16(info, TIR, patterns[i]);
		wr_reg16(info, BDR, patterns[(i+1)%count]);
		if ((rd_reg16(info, TIR) != patterns[i]) ||
		    (rd_reg16(info, BDR) != patterns[(i+1)%count])) {
			rc = -ENODEV;
			break;
		}
	}

	info->init_error = rc ? 0 : DiagStatus_AddressFailure;
	return rc;
}

static int irq_test(struct slgt_info *info)
{
	unsigned long timeout;
	unsigned long flags;
	struct tty_struct *oldtty = info->tty;
	u32 speed = info->params.data_rate;

	info->params.data_rate = 921600;
	info->tty = NULL;

	spin_lock_irqsave(&info->lock, flags);
	async_mode(info);
	slgt_irq_on(info, IRQ_TXIDLE);

	/* enable transmitter */
	wr_reg16(info, TCR,
		(unsigned short)(rd_reg16(info, TCR) | BIT1));

	/* write one byte and wait for tx idle */
	wr_reg16(info, TDR, 0);

	/* assume failure */
	info->init_error = DiagStatus_IrqFailure;
	info->irq_occurred = FALSE;

	spin_unlock_irqrestore(&info->lock, flags);

	timeout=100;
	while(timeout-- && !info->irq_occurred)
		msleep_interruptible(10);

	spin_lock_irqsave(&info->lock,flags);
	reset_port(info);
	spin_unlock_irqrestore(&info->lock,flags);

	info->params.data_rate = speed;
	info->tty = oldtty;

	info->init_error = info->irq_occurred ? 0 : DiagStatus_IrqFailure;
	return info->irq_occurred ? 0 : -ENODEV;
}

static int loopback_test_rx(struct slgt_info *info)
{
	unsigned char *src, *dest;
	int count;

	if (desc_complete(info->rbufs[0])) {
		count = desc_count(info->rbufs[0]);
		src   = info->rbufs[0].buf;
		dest  = info->tmp_rbuf;

		for( ; count ; count-=2, src+=2) {
			/* src=data byte (src+1)=status byte */
			if (!(*(src+1) & (BIT9 + BIT8))) {
				*dest = *src;
				dest++;
				info->tmp_rbuf_count++;
			}
		}
		DBGDATA(info, info->tmp_rbuf, info->tmp_rbuf_count, "rx");
		return 1;
	}
	return 0;
}

static int loopback_test(struct slgt_info *info)
{
#define TESTFRAMESIZE 20

	unsigned long timeout;
	u16 count = TESTFRAMESIZE;
	unsigned char buf[TESTFRAMESIZE];
	int rc = -ENODEV;
	unsigned long flags;

	struct tty_struct *oldtty = info->tty;
	MGSL_PARAMS params;

	memcpy(&params, &info->params, sizeof(params));

	info->params.mode = MGSL_MODE_ASYNC;
	info->params.data_rate = 921600;
	info->params.loopback = 1;
	info->tty = NULL;

	/* build and send transmit frame */
	for (count = 0; count < TESTFRAMESIZE; ++count)
		buf[count] = (unsigned char)count;

	info->tmp_rbuf_count = 0;
	memset(info->tmp_rbuf, 0, TESTFRAMESIZE);

	/* program hardware for HDLC and enabled receiver */
	spin_lock_irqsave(&info->lock,flags);
	async_mode(info);
	rx_start(info);
	info->tx_count = count;
	tx_load(info, buf, count);
	tx_start(info);
	spin_unlock_irqrestore(&info->lock, flags);

	/* wait for receive complete */
	for (timeout = 100; timeout; --timeout) {
		msleep_interruptible(10);
		if (loopback_test_rx(info)) {
			rc = 0;
			break;
		}
	}

	/* verify received frame length and contents */
	if (!rc && (info->tmp_rbuf_count != count ||
		  memcmp(buf, info->tmp_rbuf, count))) {
		rc = -ENODEV;
	}

	spin_lock_irqsave(&info->lock,flags);
	reset_adapter(info);
	spin_unlock_irqrestore(&info->lock,flags);

	memcpy(&info->params, &params, sizeof(info->params));
	info->tty = oldtty;

	info->init_error = rc ? DiagStatus_DmaFailure : 0;
	return rc;
}

static int adapter_test(struct slgt_info *info)
{
	DBGINFO(("testing %s\n", info->device_name));
	if ((info->init_error = register_test(info)) < 0) {
		printk("register test failure %s addr=%08X\n",
			info->device_name, info->phys_reg_addr);
	} else if ((info->init_error = irq_test(info)) < 0) {
		printk("IRQ test failure %s IRQ=%d\n",
			info->device_name, info->irq_level);
	} else if ((info->init_error = loopback_test(info)) < 0) {
		printk("loopback test failure %s\n", info->device_name);
	}
	return info->init_error;
}

/*
 * transmit timeout handler
 */
static void tx_timeout(unsigned long context)
{
	struct slgt_info *info = (struct slgt_info*)context;
	unsigned long flags;

	DBGINFO(("%s tx_timeout\n", info->device_name));
	if(info->tx_active && info->params.mode == MGSL_MODE_HDLC) {
		info->icount.txtimeout++;
	}
	spin_lock_irqsave(&info->lock,flags);
	info->tx_active = 0;
	info->tx_count = 0;
	spin_unlock_irqrestore(&info->lock,flags);

#ifdef CONFIG_HDLC
	if (info->netcount)
		hdlcdev_tx_done(info);
	else
#endif
		bh_transmit(info);
}

/*
 * receive buffer polling timer
 */
static void rx_timeout(unsigned long context)
{
	struct slgt_info *info = (struct slgt_info*)context;
	unsigned long flags;

	DBGINFO(("%s rx_timeout\n", info->device_name));
	spin_lock_irqsave(&info->lock, flags);
	info->pending_bh |= BH_RECEIVE;
	spin_unlock_irqrestore(&info->lock, flags);
	bh_handler(info);
}

