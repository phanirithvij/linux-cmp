/*
 * Prolific PL2303 USB to serial adaptor driver
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2003 IBM Corp.
 *
 * Original driver for 2.2.x by anonymous
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License.
 *
 * See Documentation/usb/usb-serial.txt for more information on using this driver
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <asm/uaccess.h>
#include <linux/usb.h>
#include "usb-serial.h"
#include "pl2303.h"

/*
 * Version Information
 */
#define DRIVER_DESC "Prolific PL2303 USB to serial adaptor driver"

static int debug;

#define PL2303_CLOSING_WAIT	(30*HZ)

#define PL2303_BUF_SIZE		1024
#define PL2303_TMP_BUF_SIZE	1024

struct pl2303_buf {
	unsigned int	buf_size;
	char		*buf_buf;
	char		*buf_get;
	char		*buf_put;
};

static struct usb_device_id id_table [] = {
	{ USB_DEVICE(PL2303_VENDOR_ID, PL2303_PRODUCT_ID) },
	{ USB_DEVICE(PL2303_VENDOR_ID, PL2303_PRODUCT_ID_RSAQ2) },
	{ USB_DEVICE(PL2303_VENDOR_ID, PL2303_PRODUCT_ID_RSAQ3) },
	{ USB_DEVICE(PL2303_VENDOR_ID, PL2303_PRODUCT_ID_PHAROS) },
	{ USB_DEVICE(IODATA_VENDOR_ID, IODATA_PRODUCT_ID) },
	{ USB_DEVICE(ATEN_VENDOR_ID, ATEN_PRODUCT_ID) },
	{ USB_DEVICE(ATEN_VENDOR_ID2, ATEN_PRODUCT_ID) },
	{ USB_DEVICE(ELCOM_VENDOR_ID, ELCOM_PRODUCT_ID) },
	{ USB_DEVICE(ELCOM_VENDOR_ID, ELCOM_PRODUCT_ID_UCSGT) },
	{ USB_DEVICE(ITEGNO_VENDOR_ID, ITEGNO_PRODUCT_ID) },
	{ USB_DEVICE(MA620_VENDOR_ID, MA620_PRODUCT_ID) },
	{ USB_DEVICE(RATOC_VENDOR_ID, RATOC_PRODUCT_ID) },
	{ USB_DEVICE(TRIPP_VENDOR_ID, TRIPP_PRODUCT_ID) },
	{ USB_DEVICE(RADIOSHACK_VENDOR_ID, RADIOSHACK_PRODUCT_ID) },
	{ USB_DEVICE(DCU10_VENDOR_ID, DCU10_PRODUCT_ID) },
	{ USB_DEVICE(SITECOM_VENDOR_ID, SITECOM_PRODUCT_ID) },
	{ USB_DEVICE(ALCATEL_VENDOR_ID, ALCATEL_PRODUCT_ID) },
	{ USB_DEVICE(SAMSUNG_VENDOR_ID, SAMSUNG_PRODUCT_ID) },
	{ USB_DEVICE(SIEMENS_VENDOR_ID, SIEMENS_PRODUCT_ID_SX1) },
	{ USB_DEVICE(SIEMENS_VENDOR_ID, SIEMENS_PRODUCT_ID_X65) },
	{ USB_DEVICE(SIEMENS_VENDOR_ID, SIEMENS_PRODUCT_ID_X75) },
	{ USB_DEVICE(SYNTECH_VENDOR_ID, SYNTECH_PRODUCT_ID) },
	{ USB_DEVICE(NOKIA_CA42_VENDOR_ID, NOKIA_CA42_PRODUCT_ID ) },
	{ USB_DEVICE(CA_42_CA42_VENDOR_ID, CA_42_CA42_PRODUCT_ID ) },
	{ USB_DEVICE(SAGEM_VENDOR_ID, SAGEM_PRODUCT_ID) },
	{ }					/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, id_table);

static struct usb_driver pl2303_driver = {
	.name =		"pl2303",
	.probe =	usb_serial_probe,
	.disconnect =	usb_serial_disconnect,
	.id_table =	id_table,
	.no_dynamic_id = 	1,
};

#define SET_LINE_REQUEST_TYPE		0x21
#define SET_LINE_REQUEST		0x20

#define SET_CONTROL_REQUEST_TYPE	0x21
#define SET_CONTROL_REQUEST		0x22
#define CONTROL_DTR			0x01
#define CONTROL_RTS			0x02

#define BREAK_REQUEST_TYPE		0x21
#define BREAK_REQUEST			0x23	
#define BREAK_ON			0xffff
#define BREAK_OFF			0x0000

#define GET_LINE_REQUEST_TYPE		0xa1
#define GET_LINE_REQUEST		0x21

#define VENDOR_WRITE_REQUEST_TYPE	0x40
#define VENDOR_WRITE_REQUEST		0x01

#define VENDOR_READ_REQUEST_TYPE	0xc0
#define VENDOR_READ_REQUEST		0x01

#define UART_STATE			0x08
#define UART_STATE_TRANSIENT_MASK	0x74
#define UART_DCD			0x01
#define UART_DSR			0x02
#define UART_BREAK_ERROR		0x04
#define UART_RING			0x08
#define UART_FRAME_ERROR		0x10
#define UART_PARITY_ERROR		0x20
#define UART_OVERRUN_ERROR		0x40
#define UART_CTS			0x80

/* function prototypes for a PL2303 serial converter */
static int pl2303_open (struct usb_serial_port *port, struct file *filp);
static void pl2303_close (struct usb_serial_port *port, struct file *filp);
static void pl2303_set_termios (struct usb_serial_port *port,
				struct termios *old);
static int pl2303_ioctl (struct usb_serial_port *port, struct file *file,
			 unsigned int cmd, unsigned long arg);
static void pl2303_read_int_callback (struct urb *urb, struct pt_regs *regs);
static void pl2303_read_bulk_callback (struct urb *urb, struct pt_regs *regs);
static void pl2303_write_bulk_callback (struct urb *urb, struct pt_regs *regs);
static int pl2303_write (struct usb_serial_port *port,
			 const unsigned char *buf, int count);
static void pl2303_send (struct usb_serial_port *port);
static int pl2303_write_room(struct usb_serial_port *port);
static int pl2303_chars_in_buffer(struct usb_serial_port *port);
static void pl2303_break_ctl(struct usb_serial_port *port,int break_state);
static int pl2303_tiocmget (struct usb_serial_port *port, struct file *file);
static int pl2303_tiocmset (struct usb_serial_port *port, struct file *file,
			    unsigned int set, unsigned int clear);
static int pl2303_startup (struct usb_serial *serial);
static void pl2303_shutdown (struct usb_serial *serial);
static struct pl2303_buf *pl2303_buf_alloc(unsigned int size);
static void pl2303_buf_free(struct pl2303_buf *pb);
static void pl2303_buf_clear(struct pl2303_buf *pb);
static unsigned int pl2303_buf_data_avail(struct pl2303_buf *pb);
static unsigned int pl2303_buf_space_avail(struct pl2303_buf *pb);
static unsigned int pl2303_buf_put(struct pl2303_buf *pb, const char *buf,
	unsigned int count);
static unsigned int pl2303_buf_get(struct pl2303_buf *pb, char *buf,
	unsigned int count);


/* All of the device info needed for the PL2303 SIO serial converter */
static struct usb_serial_driver pl2303_device = {
	.driver = {
		.owner =	THIS_MODULE,
		.name =		"pl2303",
	},
	.id_table =		id_table,
	.num_interrupt_in =	NUM_DONT_CARE,
	.num_bulk_in =		1,
	.num_bulk_out =		1,
	.num_ports =		1,
	.open =			pl2303_open,
	.close =		pl2303_close,
	.write =		pl2303_write,
	.ioctl =		pl2303_ioctl,
	.break_ctl =		pl2303_break_ctl,
	.set_termios =		pl2303_set_termios,
	.tiocmget =		pl2303_tiocmget,
	.tiocmset =		pl2303_tiocmset,
	.read_bulk_callback =	pl2303_read_bulk_callback,
	.read_int_callback =	pl2303_read_int_callback,
	.write_bulk_callback =	pl2303_write_bulk_callback,
	.write_room =		pl2303_write_room,
	.chars_in_buffer =	pl2303_chars_in_buffer,
	.attach =		pl2303_startup,
	.shutdown =		pl2303_shutdown,
};

enum pl2303_type {
	type_0,		/* don't know the difference between type 0 and */
	type_1,		/* type 1, until someone from prolific tells us... */
	HX,		/* HX version of the pl2303 chip */
};

struct pl2303_private {
	spinlock_t lock;
	struct pl2303_buf *buf;
	int write_urb_in_use;
	wait_queue_head_t delta_msr_wait;
	u8 line_control;
	u8 line_status;
	u8 termios_initialized;
	enum pl2303_type type;
};


static int pl2303_startup (struct usb_serial *serial)
{
	struct pl2303_private *priv;
	enum pl2303_type type = type_0;
	int i;

	if (serial->dev->descriptor.bDeviceClass == 0x02)
		type = type_0;
	else if (serial->dev->descriptor.bMaxPacketSize0 == 0x40)
		type = HX;
	else if (serial->dev->descriptor.bDeviceClass == 0x00)
		type = type_1;
	else if (serial->dev->descriptor.bDeviceClass == 0xFF)
		type = type_1;
	dbg("device type: %d", type);

	for (i = 0; i < serial->num_ports; ++i) {
		priv = kmalloc (sizeof (struct pl2303_private), GFP_KERNEL);
		if (!priv)
			goto cleanup;
		memset (priv, 0x00, sizeof (struct pl2303_private));
		spin_lock_init(&priv->lock);
		priv->buf = pl2303_buf_alloc(PL2303_BUF_SIZE);
		if (priv->buf == NULL) {
			kfree(priv);
			goto cleanup;
		}
		init_waitqueue_head(&priv->delta_msr_wait);
		priv->type = type;
		usb_set_serial_port_data(serial->port[i], priv);
	}
	return 0;

cleanup:
	for (--i; i>=0; --i) {
		priv = usb_get_serial_port_data(serial->port[i]);
		pl2303_buf_free(priv->buf);
		kfree(priv);
		usb_set_serial_port_data(serial->port[i], NULL);
	}
	return -ENOMEM;
}

static int set_control_lines (struct usb_device *dev, u8 value)
{
	int retval;
	
	retval = usb_control_msg (dev, usb_sndctrlpipe (dev, 0),
				  SET_CONTROL_REQUEST, SET_CONTROL_REQUEST_TYPE,
				  value, 0, NULL, 0, 100);
	dbg("%s - value = %d, retval = %d", __FUNCTION__, value, retval);
	return retval;
}

static int pl2303_write (struct usb_serial_port *port,  const unsigned char *buf, int count)
{
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;

	dbg("%s - port %d, %d bytes", __FUNCTION__, port->number, count);

	if (!count)
		return count;

	spin_lock_irqsave(&priv->lock, flags);
	count = pl2303_buf_put(priv->buf, buf, count);
	spin_unlock_irqrestore(&priv->lock, flags);

	pl2303_send(port);

	return count;
}

static void pl2303_send(struct usb_serial_port *port)
{
	int count, result;
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;

	dbg("%s - port %d", __FUNCTION__, port->number);

	spin_lock_irqsave(&priv->lock, flags);

	if (priv->write_urb_in_use) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return;
	}

	count = pl2303_buf_get(priv->buf, port->write_urb->transfer_buffer,
		port->bulk_out_size);

	if (count == 0) {
		spin_unlock_irqrestore(&priv->lock, flags);
		return;
	}

	priv->write_urb_in_use = 1;

	spin_unlock_irqrestore(&priv->lock, flags);

	usb_serial_debug_data(debug, &port->dev, __FUNCTION__, count, port->write_urb->transfer_buffer);

	port->write_urb->transfer_buffer_length = count;
	port->write_urb->dev = port->serial->dev;
	result = usb_submit_urb (port->write_urb, GFP_ATOMIC);
	if (result) {
		dev_err(&port->dev, "%s - failed submitting write urb, error %d\n", __FUNCTION__, result);
		priv->write_urb_in_use = 0;
		// TODO: reschedule pl2303_send
	}

	schedule_work(&port->work);
}

static int pl2303_write_room(struct usb_serial_port *port)
{
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	int room = 0;
	unsigned long flags;

	dbg("%s - port %d", __FUNCTION__, port->number);

	spin_lock_irqsave(&priv->lock, flags);
	room = pl2303_buf_space_avail(priv->buf);
	spin_unlock_irqrestore(&priv->lock, flags);

	dbg("%s - returns %d", __FUNCTION__, room);
	return room;
}

static int pl2303_chars_in_buffer(struct usb_serial_port *port)
{
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	int chars = 0;
	unsigned long flags;

	dbg("%s - port %d", __FUNCTION__, port->number);

	spin_lock_irqsave(&priv->lock, flags);
	chars = pl2303_buf_data_avail(priv->buf);
	spin_unlock_irqrestore(&priv->lock, flags);

	dbg("%s - returns %d", __FUNCTION__, chars);
	return chars;
}

static void pl2303_set_termios (struct usb_serial_port *port, struct termios *old_termios)
{
	struct usb_serial *serial = port->serial;
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned int cflag;
	unsigned char *buf;
	int baud;
	int i;
	u8 control;

	dbg("%s -  port %d", __FUNCTION__, port->number);

	if ((!port->tty) || (!port->tty->termios)) {
		dbg("%s - no tty structures", __FUNCTION__);
		return;
	}

	spin_lock_irqsave(&priv->lock, flags);
	if (!priv->termios_initialized) {
		*(port->tty->termios) = tty_std_termios;
		port->tty->termios->c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
		priv->termios_initialized = 1;
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	cflag = port->tty->termios->c_cflag;
	/* check that they really want us to change something */
	if (old_termios) {
		if ((cflag == old_termios->c_cflag) &&
		    (RELEVANT_IFLAG(port->tty->termios->c_iflag) == RELEVANT_IFLAG(old_termios->c_iflag))) {
		    dbg("%s - nothing to change...", __FUNCTION__);
		    return;
		}
	}

	buf = kmalloc (7, GFP_KERNEL);
	if (!buf) {
		dev_err(&port->dev, "%s - out of memory.\n", __FUNCTION__);
		return;
	}
	memset (buf, 0x00, 0x07);
	
	i = usb_control_msg (serial->dev, usb_rcvctrlpipe (serial->dev, 0),
			     GET_LINE_REQUEST, GET_LINE_REQUEST_TYPE,
			     0, 0, buf, 7, 100);
	dbg ("0xa1:0x21:0:0  %d - %x %x %x %x %x %x %x", i,
	     buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);


	if (cflag & CSIZE) {
		switch (cflag & CSIZE) {
			case CS5:	buf[6] = 5;	break;
			case CS6:	buf[6] = 6;	break;
			case CS7:	buf[6] = 7;	break;
			default:
			case CS8:	buf[6] = 8;	break;
		}
		dbg("%s - data bits = %d", __FUNCTION__, buf[6]);
	}

	baud = 0;
	switch (cflag & CBAUD) {
		case B0:	baud = 0;	break;
		case B75:	baud = 75;	break;
		case B150:	baud = 150;	break;
		case B300:	baud = 300;	break;
		case B600:	baud = 600;	break;
		case B1200:	baud = 1200;	break;
		case B1800:	baud = 1800;	break;
		case B2400:	baud = 2400;	break;
		case B4800:	baud = 4800;	break;
		case B9600:	baud = 9600;	break;
		case B19200:	baud = 19200;	break;
		case B38400:	baud = 38400;	break;
		case B57600:	baud = 57600;	break;
		case B115200:	baud = 115200;	break;
		case B230400:	baud = 230400;	break;
		case B460800:	baud = 460800;	break;
		default:
			dev_err(&port->dev, "pl2303 driver does not support the baudrate requested (fix it)\n");
			break;
	}
	dbg("%s - baud = %d", __FUNCTION__, baud);
	if (baud) {
		buf[0] = baud & 0xff;
		buf[1] = (baud >> 8) & 0xff;
		buf[2] = (baud >> 16) & 0xff;
		buf[3] = (baud >> 24) & 0xff;
	}

	/* For reference buf[4]=0 is 1 stop bits */
	/* For reference buf[4]=1 is 1.5 stop bits */
	/* For reference buf[4]=2 is 2 stop bits */
	if (cflag & CSTOPB) {
		buf[4] = 2;
		dbg("%s - stop bits = 2", __FUNCTION__);
	} else {
		buf[4] = 0;
		dbg("%s - stop bits = 1", __FUNCTION__);
	}

	if (cflag & PARENB) {
		/* For reference buf[5]=0 is none parity */
		/* For reference buf[5]=1 is odd parity */
		/* For reference buf[5]=2 is even parity */
		/* For reference buf[5]=3 is mark parity */
		/* For reference buf[5]=4 is space parity */
		if (cflag & PARODD) {
			buf[5] = 1;
			dbg("%s - parity = odd", __FUNCTION__);
		} else {
			buf[5] = 2;
			dbg("%s - parity = even", __FUNCTION__);
		}
	} else {
		buf[5] = 0;
		dbg("%s - parity = none", __FUNCTION__);
	}

	i = usb_control_msg (serial->dev, usb_sndctrlpipe (serial->dev, 0),
			     SET_LINE_REQUEST, SET_LINE_REQUEST_TYPE, 
			     0, 0, buf, 7, 100);
	dbg ("0x21:0x20:0:0  %d", i);

	/* change control lines if we are switching to or from B0 */
	spin_lock_irqsave(&priv->lock, flags);
	control = priv->line_control;
	if ((cflag & CBAUD) == B0)
		priv->line_control &= ~(CONTROL_DTR | CONTROL_RTS);
	else
		priv->line_control |= (CONTROL_DTR | CONTROL_RTS);
	if (control != priv->line_control) {
		control = priv->line_control;
		spin_unlock_irqrestore(&priv->lock, flags);
		set_control_lines(serial->dev, control);
	} else {
		spin_unlock_irqrestore(&priv->lock, flags);
	}
	
	buf[0] = buf[1] = buf[2] = buf[3] = buf[4] = buf[5] = buf[6] = 0;

	i = usb_control_msg (serial->dev, usb_rcvctrlpipe (serial->dev, 0),
			     GET_LINE_REQUEST, GET_LINE_REQUEST_TYPE,
			     0, 0, buf, 7, 100);
	dbg ("0xa1:0x21:0:0  %d - %x %x %x %x %x %x %x", i,
	     buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

	if (cflag & CRTSCTS) {
		__u16 index;
		if (priv->type == HX)
			index = 0x61;
		else
			index = 0x41;
		i = usb_control_msg(serial->dev, 
				    usb_sndctrlpipe(serial->dev, 0),
				    VENDOR_WRITE_REQUEST,
				    VENDOR_WRITE_REQUEST_TYPE,
				    0x0, index, NULL, 0, 100);
		dbg ("0x40:0x1:0x0:0x%x  %d", index, i);
	}

	kfree (buf);
}

static int pl2303_open (struct usb_serial_port *port, struct file *filp)
{
	struct termios tmp_termios;
	struct usb_serial *serial = port->serial;
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned char *buf;
	int result;

	dbg("%s -  port %d", __FUNCTION__, port->number);

	if (priv->type != HX) {
		usb_clear_halt(serial->dev, port->write_urb->pipe);
		usb_clear_halt(serial->dev, port->read_urb->pipe);
	}

	buf = kmalloc(10, GFP_KERNEL);
	if (buf==NULL)
		return -ENOMEM;

#define FISH(a,b,c,d)								\
	result=usb_control_msg(serial->dev, usb_rcvctrlpipe(serial->dev,0),	\
			       b, a, c, d, buf, 1, 100);			\
	dbg("0x%x:0x%x:0x%x:0x%x  %d - %x",a,b,c,d,result,buf[0]);

#define SOUP(a,b,c,d)								\
	result=usb_control_msg(serial->dev, usb_sndctrlpipe(serial->dev,0),	\
			       b, a, c, d, NULL, 0, 100);			\
	dbg("0x%x:0x%x:0x%x:0x%x  %d",a,b,c,d,result);

	FISH (VENDOR_READ_REQUEST_TYPE, VENDOR_READ_REQUEST, 0x8484, 0);
	SOUP (VENDOR_WRITE_REQUEST_TYPE, VENDOR_WRITE_REQUEST, 0x0404, 0);
	FISH (VENDOR_READ_REQUEST_TYPE, VENDOR_READ_REQUEST, 0x8484, 0);
	FISH (VENDOR_READ_REQUEST_TYPE, VENDOR_READ_REQUEST, 0x8383, 0);
	FISH (VENDOR_READ_REQUEST_TYPE, VENDOR_READ_REQUEST, 0x8484, 0);
	SOUP (VENDOR_WRITE_REQUEST_TYPE, VENDOR_WRITE_REQUEST, 0x0404, 1);
	FISH (VENDOR_READ_REQUEST_TYPE, VENDOR_READ_REQUEST, 0x8484, 0);
	FISH (VENDOR_READ_REQUEST_TYPE, VENDOR_READ_REQUEST, 0x8383, 0);
	SOUP (VENDOR_WRITE_REQUEST_TYPE, VENDOR_WRITE_REQUEST, 0, 1);
	SOUP (VENDOR_WRITE_REQUEST_TYPE, VENDOR_WRITE_REQUEST, 1, 0);

	if (priv->type == HX) {
		/* HX chip */
		SOUP (VENDOR_WRITE_REQUEST_TYPE, VENDOR_WRITE_REQUEST, 2, 0x44);
		/* reset upstream data pipes */
          	SOUP (VENDOR_WRITE_REQUEST_TYPE, VENDOR_WRITE_REQUEST, 8, 0);
        	SOUP (VENDOR_WRITE_REQUEST_TYPE, VENDOR_WRITE_REQUEST, 9, 0);
	} else {
		SOUP (VENDOR_WRITE_REQUEST_TYPE, VENDOR_WRITE_REQUEST, 2, 0x24);
	}

	kfree(buf);

	/* Setup termios */
	if (port->tty) {
		pl2303_set_termios (port, &tmp_termios);
	}

	//FIXME: need to assert RTS and DTR if CRTSCTS off

	dbg("%s - submitting read urb", __FUNCTION__);
	port->read_urb->dev = serial->dev;
	result = usb_submit_urb (port->read_urb, GFP_KERNEL);
	if (result) {
		dev_err(&port->dev, "%s - failed submitting read urb, error %d\n", __FUNCTION__, result);
		pl2303_close (port, NULL);
		return -EPROTO;
	}

	dbg("%s - submitting interrupt urb", __FUNCTION__);
	port->interrupt_in_urb->dev = serial->dev;
	result = usb_submit_urb (port->interrupt_in_urb, GFP_KERNEL);
	if (result) {
		dev_err(&port->dev, "%s - failed submitting interrupt urb, error %d\n", __FUNCTION__, result);
		pl2303_close (port, NULL);
		return -EPROTO;
	}
	return 0;
}


static void pl2303_close (struct usb_serial_port *port, struct file *filp)
{
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned int c_cflag;
	int bps;
	long timeout;
	wait_queue_t wait;						\

	dbg("%s - port %d", __FUNCTION__, port->number);

	/* wait for data to drain from the buffer */
	spin_lock_irqsave(&priv->lock, flags);
	timeout = PL2303_CLOSING_WAIT;
	init_waitqueue_entry(&wait, current);
	add_wait_queue(&port->tty->write_wait, &wait);
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (pl2303_buf_data_avail(priv->buf) == 0
		|| timeout == 0 || signal_pending(current)
		|| !usb_get_intfdata(port->serial->interface))	/* disconnect */
			break;
		spin_unlock_irqrestore(&priv->lock, flags);
		timeout = schedule_timeout(timeout);
		spin_lock_irqsave(&priv->lock, flags);
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&port->tty->write_wait, &wait);
	/* clear out any remaining data in the buffer */
	pl2303_buf_clear(priv->buf);
	spin_unlock_irqrestore(&priv->lock, flags);

	/* wait for characters to drain from the device */
	/* (this is long enough for the entire 256 byte */
	/* pl2303 hardware buffer to drain with no flow */
	/* control for data rates of 1200 bps or more, */
	/* for lower rates we should really know how much */
	/* data is in the buffer to compute a delay */
	/* that is not unnecessarily long) */
	bps = tty_get_baud_rate(port->tty);
	if (bps > 1200)
		timeout = max((HZ*2560)/bps,HZ/10);
	else
		timeout = 2*HZ;
	schedule_timeout_interruptible(timeout);

	/* shutdown our urbs */
	dbg("%s - shutting down urbs", __FUNCTION__);
	usb_kill_urb(port->write_urb);
	usb_kill_urb(port->read_urb);
	usb_kill_urb(port->interrupt_in_urb);

	if (port->tty) {
		c_cflag = port->tty->termios->c_cflag;
		if (c_cflag & HUPCL) {
			/* drop DTR and RTS */
			spin_lock_irqsave(&priv->lock, flags);
			priv->line_control = 0;
			spin_unlock_irqrestore (&priv->lock, flags);
			set_control_lines (port->serial->dev, 0);
		}
	}
}

static int pl2303_tiocmset (struct usb_serial_port *port, struct file *file,
			    unsigned int set, unsigned int clear)
{
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	u8 control;

	if (!usb_get_intfdata(port->serial->interface))
		return -ENODEV;

	spin_lock_irqsave (&priv->lock, flags);
	if (set & TIOCM_RTS)
		priv->line_control |= CONTROL_RTS;
	if (set & TIOCM_DTR)
		priv->line_control |= CONTROL_DTR;
	if (clear & TIOCM_RTS)
		priv->line_control &= ~CONTROL_RTS;
	if (clear & TIOCM_DTR)
		priv->line_control &= ~CONTROL_DTR;
	control = priv->line_control;
	spin_unlock_irqrestore (&priv->lock, flags);

	return set_control_lines (port->serial->dev, control);
}

static int pl2303_tiocmget (struct usb_serial_port *port, struct file *file)
{
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned int mcr;
	unsigned int status;
	unsigned int result;

	dbg("%s (%d)", __FUNCTION__, port->number);

	if (!usb_get_intfdata(port->serial->interface))
		return -ENODEV;

	spin_lock_irqsave (&priv->lock, flags);
	mcr = priv->line_control;
	status = priv->line_status;
	spin_unlock_irqrestore (&priv->lock, flags);

	result = ((mcr & CONTROL_DTR)		? TIOCM_DTR : 0)
		  | ((mcr & CONTROL_RTS)	? TIOCM_RTS : 0)
		  | ((status & UART_CTS)	? TIOCM_CTS : 0)
		  | ((status & UART_DSR)	? TIOCM_DSR : 0)
		  | ((status & UART_RING)	? TIOCM_RI  : 0)
		  | ((status & UART_DCD)	? TIOCM_CD  : 0);

	dbg("%s - result = %x", __FUNCTION__, result);

	return result;
}

static int wait_modem_info(struct usb_serial_port *port, unsigned int arg)
{
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned int prevstatus;
	unsigned int status;
	unsigned int changed;

	spin_lock_irqsave (&priv->lock, flags);
	prevstatus = priv->line_status;
	spin_unlock_irqrestore (&priv->lock, flags);

	while (1) {
		interruptible_sleep_on(&priv->delta_msr_wait);
		/* see if a signal did it */
		if (signal_pending(current))
			return -ERESTARTSYS;
		
		spin_lock_irqsave (&priv->lock, flags);
		status = priv->line_status;
		spin_unlock_irqrestore (&priv->lock, flags);
		
		changed=prevstatus^status;
		
		if (((arg & TIOCM_RNG) && (changed & UART_RING)) ||
		    ((arg & TIOCM_DSR) && (changed & UART_DSR)) ||
		    ((arg & TIOCM_CD)  && (changed & UART_DCD)) ||
		    ((arg & TIOCM_CTS) && (changed & UART_CTS)) ) {
			return 0;
		}
		prevstatus = status;
	}
	/* NOTREACHED */
	return 0;
}

static int pl2303_ioctl (struct usb_serial_port *port, struct file *file, unsigned int cmd, unsigned long arg)
{
	dbg("%s (%d) cmd = 0x%04x", __FUNCTION__, port->number, cmd);

	switch (cmd) {
		case TIOCMIWAIT:
			dbg("%s (%d) TIOCMIWAIT", __FUNCTION__,  port->number);
			return wait_modem_info(port, arg);

		default:
			dbg("%s not supported = 0x%04x", __FUNCTION__, cmd);
			break;
	}

	return -ENOIOCTLCMD;
}

static void pl2303_break_ctl (struct usb_serial_port *port, int break_state)
{
	struct usb_serial *serial = port->serial;
	u16 state;
	int result;

	dbg("%s - port %d", __FUNCTION__, port->number);

	if (break_state == 0)
		state = BREAK_OFF;
	else
		state = BREAK_ON;
	dbg("%s - turning break %s", __FUNCTION__, state==BREAK_OFF ? "off" : "on");

	result = usb_control_msg (serial->dev, usb_sndctrlpipe (serial->dev, 0),
				  BREAK_REQUEST, BREAK_REQUEST_TYPE, state, 
				  0, NULL, 0, 100);
	if (result)
		dbg("%s - error sending break = %d", __FUNCTION__, result);
}


static void pl2303_shutdown (struct usb_serial *serial)
{
	int i;
	struct pl2303_private *priv;

	dbg("%s", __FUNCTION__);

	for (i = 0; i < serial->num_ports; ++i) {
		priv = usb_get_serial_port_data(serial->port[i]);
		if (priv) {
			pl2303_buf_free(priv->buf);
			kfree(priv);
			usb_set_serial_port_data(serial->port[i], NULL);
		}
	}		
}

static void pl2303_update_line_status(struct usb_serial_port *port,
				      unsigned char *data,
				      unsigned int actual_length)
{

	struct pl2303_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	u8 status_idx = UART_STATE;
	u8 length = UART_STATE + 1;

	if ((le16_to_cpu(port->serial->dev->descriptor.idVendor) == SIEMENS_VENDOR_ID) &&
	    (le16_to_cpu(port->serial->dev->descriptor.idProduct) == SIEMENS_PRODUCT_ID_X65 ||
	     le16_to_cpu(port->serial->dev->descriptor.idProduct) == SIEMENS_PRODUCT_ID_SX1 ||
	     le16_to_cpu(port->serial->dev->descriptor.idProduct) == SIEMENS_PRODUCT_ID_X75)) {
		length = 1;
		status_idx = 0;
	}

	if (actual_length < length)
		goto exit;

        /* Save off the uart status for others to look at */
	spin_lock_irqsave(&priv->lock, flags);
	priv->line_status = data[status_idx];
	spin_unlock_irqrestore(&priv->lock, flags);

exit:
	return;
}

static void pl2303_read_int_callback (struct urb *urb, struct pt_regs *regs)
{
	struct usb_serial_port *port = (struct usb_serial_port *) urb->context;
	unsigned char *data = urb->transfer_buffer;
	unsigned int actual_length = urb->actual_length;
	int status;

	dbg("%s (%d)", __FUNCTION__, port->number);

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d", __FUNCTION__, urb->status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d", __FUNCTION__, urb->status);
		goto exit;
	}

	usb_serial_debug_data(debug, &port->dev, __FUNCTION__, urb->actual_length, urb->transfer_buffer);
	pl2303_update_line_status(port, data, actual_length);

exit:
	status = usb_submit_urb (urb, GFP_ATOMIC);
	if (status)
		dev_err(&urb->dev->dev, "%s - usb_submit_urb failed with result %d\n",
			__FUNCTION__, status);
}


static void pl2303_read_bulk_callback (struct urb *urb, struct pt_regs *regs)
{
	struct usb_serial_port *port = (struct usb_serial_port *) urb->context;
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	struct tty_struct *tty;
	unsigned char *data = urb->transfer_buffer;
	unsigned long flags;
	int i;
	int result;
	u8 status;
	char tty_flag;

	dbg("%s - port %d", __FUNCTION__, port->number);

	if (urb->status) {
		dbg("%s - urb->status = %d", __FUNCTION__, urb->status);
		if (!port->open_count) {
			dbg("%s - port is closed, exiting.", __FUNCTION__);
			return;
		}
		if (urb->status == -EPROTO) {
			/* PL2303 mysteriously fails with -EPROTO reschedule the read */
			dbg("%s - caught -EPROTO, resubmitting the urb", __FUNCTION__);
			urb->status = 0;
			urb->dev = port->serial->dev;
			result = usb_submit_urb(urb, GFP_ATOMIC);
			if (result)
				dev_err(&urb->dev->dev, "%s - failed resubmitting read urb, error %d\n", __FUNCTION__, result);
			return;
		}
		dbg("%s - unable to handle the error, exiting.", __FUNCTION__);
		return;
	}

	usb_serial_debug_data(debug, &port->dev, __FUNCTION__, urb->actual_length, data);

	/* get tty_flag from status */
	tty_flag = TTY_NORMAL;

	spin_lock_irqsave(&priv->lock, flags);
	status = priv->line_status;
	priv->line_status &= ~UART_STATE_TRANSIENT_MASK;
	spin_unlock_irqrestore(&priv->lock, flags);
	wake_up_interruptible (&priv->delta_msr_wait);

	/* break takes precedence over parity, */
	/* which takes precedence over framing errors */
	if (status & UART_BREAK_ERROR )
		tty_flag = TTY_BREAK;
	else if (status & UART_PARITY_ERROR)
		tty_flag = TTY_PARITY;
	else if (status & UART_FRAME_ERROR)
		tty_flag = TTY_FRAME;
	dbg("%s - tty_flag = %d", __FUNCTION__, tty_flag);

	tty = port->tty;
	if (tty && urb->actual_length) {
		tty_buffer_request_room(tty, urb->actual_length + 1);
		/* overrun is special, not associated with a char */
		if (status & UART_OVERRUN_ERROR)
			tty_insert_flip_char(tty, 0, TTY_OVERRUN);
		for (i = 0; i < urb->actual_length; ++i)
			tty_insert_flip_char (tty, data[i], tty_flag);
		tty_flip_buffer_push (tty);
	}

	/* Schedule the next read _if_ we are still open */
	if (port->open_count) {
		urb->dev = port->serial->dev;
		result = usb_submit_urb(urb, GFP_ATOMIC);
		if (result)
			dev_err(&urb->dev->dev, "%s - failed resubmitting read urb, error %d\n", __FUNCTION__, result);
	}

	return;
}



static void pl2303_write_bulk_callback (struct urb *urb, struct pt_regs *regs)
{
	struct usb_serial_port *port = (struct usb_serial_port *) urb->context;
	struct pl2303_private *priv = usb_get_serial_port_data(port);
	int result;

	dbg("%s - port %d", __FUNCTION__, port->number);

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d", __FUNCTION__, urb->status);
		priv->write_urb_in_use = 0;
		return;
	default:
		/* error in the urb, so we have to resubmit it */
		dbg("%s - Overflow in write", __FUNCTION__);
		dbg("%s - nonzero write bulk status received: %d", __FUNCTION__, urb->status);
		port->write_urb->transfer_buffer_length = 1;
		port->write_urb->dev = port->serial->dev;
		result = usb_submit_urb (port->write_urb, GFP_ATOMIC);
		if (result)
			dev_err(&urb->dev->dev, "%s - failed resubmitting write urb, error %d\n", __FUNCTION__, result);
		else
			return;
	}

	priv->write_urb_in_use = 0;

	/* send any buffered data */
	pl2303_send(port);
}


/*
 * pl2303_buf_alloc
 *
 * Allocate a circular buffer and all associated memory.
 */

static struct pl2303_buf *pl2303_buf_alloc(unsigned int size)
{

	struct pl2303_buf *pb;


	if (size == 0)
		return NULL;

	pb = (struct pl2303_buf *)kmalloc(sizeof(struct pl2303_buf), GFP_KERNEL);
	if (pb == NULL)
		return NULL;

	pb->buf_buf = kmalloc(size, GFP_KERNEL);
	if (pb->buf_buf == NULL) {
		kfree(pb);
		return NULL;
	}

	pb->buf_size = size;
	pb->buf_get = pb->buf_put = pb->buf_buf;

	return pb;

}


/*
 * pl2303_buf_free
 *
 * Free the buffer and all associated memory.
 */

static void pl2303_buf_free(struct pl2303_buf *pb)
{
	if (pb) {
		kfree(pb->buf_buf);
		kfree(pb);
	}
}


/*
 * pl2303_buf_clear
 *
 * Clear out all data in the circular buffer.
 */

static void pl2303_buf_clear(struct pl2303_buf *pb)
{
	if (pb != NULL)
		pb->buf_get = pb->buf_put;
		/* equivalent to a get of all data available */
}


/*
 * pl2303_buf_data_avail
 *
 * Return the number of bytes of data available in the circular
 * buffer.
 */

static unsigned int pl2303_buf_data_avail(struct pl2303_buf *pb)
{
	if (pb != NULL)
		return ((pb->buf_size + pb->buf_put - pb->buf_get) % pb->buf_size);
	else
		return 0;
}


/*
 * pl2303_buf_space_avail
 *
 * Return the number of bytes of space available in the circular
 * buffer.
 */

static unsigned int pl2303_buf_space_avail(struct pl2303_buf *pb)
{
	if (pb != NULL)
		return ((pb->buf_size + pb->buf_get - pb->buf_put - 1) % pb->buf_size);
	else
		return 0;
}


/*
 * pl2303_buf_put
 *
 * Copy data data from a user buffer and put it into the circular buffer.
 * Restrict to the amount of space available.
 *
 * Return the number of bytes copied.
 */

static unsigned int pl2303_buf_put(struct pl2303_buf *pb, const char *buf,
	unsigned int count)
{

	unsigned int len;


	if (pb == NULL)
		return 0;

	len  = pl2303_buf_space_avail(pb);
	if (count > len)
		count = len;

	if (count == 0)
		return 0;

	len = pb->buf_buf + pb->buf_size - pb->buf_put;
	if (count > len) {
		memcpy(pb->buf_put, buf, len);
		memcpy(pb->buf_buf, buf+len, count - len);
		pb->buf_put = pb->buf_buf + count - len;
	} else {
		memcpy(pb->buf_put, buf, count);
		if (count < len)
			pb->buf_put += count;
		else /* count == len */
			pb->buf_put = pb->buf_buf;
	}

	return count;

}


/*
 * pl2303_buf_get
 *
 * Get data from the circular buffer and copy to the given buffer.
 * Restrict to the amount of data available.
 *
 * Return the number of bytes copied.
 */

static unsigned int pl2303_buf_get(struct pl2303_buf *pb, char *buf,
	unsigned int count)
{

	unsigned int len;


	if (pb == NULL)
		return 0;

	len = pl2303_buf_data_avail(pb);
	if (count > len)
		count = len;

	if (count == 0)
		return 0;

	len = pb->buf_buf + pb->buf_size - pb->buf_get;
	if (count > len) {
		memcpy(buf, pb->buf_get, len);
		memcpy(buf+len, pb->buf_buf, count - len);
		pb->buf_get = pb->buf_buf + count - len;
	} else {
		memcpy(buf, pb->buf_get, count);
		if (count < len)
			pb->buf_get += count;
		else /* count == len */
			pb->buf_get = pb->buf_buf;
	}

	return count;

}

static int __init pl2303_init (void)
{
	int retval;
	retval = usb_serial_register(&pl2303_device);
	if (retval)
		goto failed_usb_serial_register;
	retval = usb_register(&pl2303_driver);
	if (retval)
		goto failed_usb_register;
	info(DRIVER_DESC);
	return 0;
failed_usb_register:
	usb_serial_deregister(&pl2303_device);
failed_usb_serial_register:
	return retval;
}


static void __exit pl2303_exit (void)
{
	usb_deregister (&pl2303_driver);
	usb_serial_deregister (&pl2303_device);
}


module_init(pl2303_init);
module_exit(pl2303_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug enabled or not");

