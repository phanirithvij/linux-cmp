/*
   em28xx-video.c - driver for Empia EM2800/EM2820/2840 USB video capture devices

   Copyright (C) 2005 Ludovico Cavedon <cavedon@sssup.it>
		      Markus Rechberger <mrechberger@gmail.com>
		      Mauro Carvalho Chehab <mchehab@brturbo.com.br>
		      Sascha Sommer <saschasommer@freenet.de>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/i2c.h>
#include <linux/version.h>
#include <linux/video_decoder.h>
#include <linux/mutex.h>

#include "em28xx.h"
#include <media/tuner.h>
#include <media/v4l2-common.h>

#define DRIVER_AUTHOR "Ludovico Cavedon <cavedon@sssup.it>, " \
		      "Markus Rechberger <mrechberger@gmail.com>, " \
		      "Mauro Carvalho Chehab <mchehab@brturbo.com.br>, " \
		      "Sascha Sommer <saschasommer@freenet.de>"

#define DRIVER_NAME         "em28xx"
#define DRIVER_DESC         "Empia em28xx based USB video device driver"
#define EM28XX_VERSION_CODE  KERNEL_VERSION(0, 0, 1)

#define em28xx_videodbg(fmt, arg...) do {\
	if (video_debug) \
		printk(KERN_INFO "%s %s :"fmt, \
			 dev->name, __FUNCTION__ , ##arg); } while (0)

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

static LIST_HEAD(em28xx_devlist);

static unsigned int card[]     = {[0 ... (EM28XX_MAXBOARDS - 1)] = UNSET };
module_param_array(card,  int, NULL, 0444);
MODULE_PARM_DESC(card,"card type");

static int tuner = -1;
module_param(tuner, int, 0444);
MODULE_PARM_DESC(tuner, "tuner type");

static unsigned int video_debug = 0;
module_param(video_debug,int,0644);
MODULE_PARM_DESC(video_debug,"enable debug messages [video]");

/* supported tv norms */
static struct em28xx_tvnorm tvnorms[] = {
	{
		.name = "PAL",
		.id = V4L2_STD_PAL,
		.mode = VIDEO_MODE_PAL,
	 }, {
		.name = "NTSC",
		.id = V4L2_STD_NTSC,
		.mode = VIDEO_MODE_NTSC,
	}, {
		 .name = "SECAM",
		 .id = V4L2_STD_SECAM,
		 .mode = VIDEO_MODE_SECAM,
	}, {
		.name = "PAL-M",
		.id = V4L2_STD_PAL_M,
		.mode = VIDEO_MODE_PAL,
	}
};

static const unsigned char saa7114_i2c_init[] = {
	0x00,0x00,0x01,0x08,0x02,0xc4,0x03,0x30,0x04,0x90,0x05,0x90,0x06,0xeb,0x07,0xe0,
	0x08,0x88,0x09,0x40,0x0a,0x80,0x0b,0x44,0x0c,0x40,0x0d,0x00,0x0e,0x81,0x0f,0x2a,
	0x10,0x06,0x11,0x00,0x12,0xc8,0x13,0x80,0x14,0x00,0x15,0x11,0x16,0x01,0x17,0x42,
	0x18,0x40,0x19,0x80,0x40,0x00,0x41,0xff,0x42,0xff,0x43,0xff,0x44,0xff,0x45,0xff,
	0x46,0xff,0x47,0xff,0x48,0xff,0x49,0xff,0x4a,0xff,0x4b,0xff,0x4c,0xff,0x4d,0xff,
	0x4e,0xff,0x4f,0xff,0x50,0xff,0x51,0xff,0x52,0xff,0x53,0xff,0x54,0x5f,0x55,0xff,
	0x56,0xff,0x57,0xff,0x58,0x00,0x59,0x47,0x5a,0x03,0x5b,0x03,0x5d,0x3e,0x5e,0x00,
	0x80,0x1c,0x83,0x01,0x84,0xa5,0x85,0x10,0x86,0x45,0x87,0x41,0x88,0xf0,0x88,0x00,
	0x88,0xf0,0x90,0x00,0x91,0x08,0x92,0x00,0x93,0x80,0x94,0x08,0x95,0x00,0x96,0xc0,
	0x97,0x02,0x98,0x13,0x99,0x00,0x9a,0x38,0x9b,0x01,0x9c,0x80,0x9d,0x02,0x9e,0x06,
	0x9f,0x01,0xa0,0x01,0xa1,0x00,0xa2,0x00,0xa4,0x80,0xa5,0x36,0xa6,0x36,0xa8,0x67,
	0xa9,0x04,0xaa,0x00,0xac,0x33,0xad,0x02,0xae,0x00,0xb0,0xcd,0xb1,0x04,0xb2,0xcd,
	0xb3,0x04,0xb4,0x01,0xb8,0x00,0xb9,0x00,0xba,0x00,0xbb,0x00,0xbc,0x00,0xbd,0x00,
	0xbe,0x00,0xbf,0x00
};

#define TVNORMS ARRAY_SIZE(tvnorms)

/* supported controls */
/* Common to all boards */
static struct v4l2_queryctrl em28xx_qctrl[] = {
	{
		.id = V4L2_CID_AUDIO_VOLUME,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Volume",
		.minimum = 0x0,
		.maximum = 0x1f,
		.step = 0x1,
		.default_value = 0x1f,
		.flags = 0,
	},{
		.id = V4L2_CID_AUDIO_MUTE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "Mute",
		.minimum = 0,
		.maximum = 1,
		.step = 1,
		.default_value = 1,
		.flags = 0,
	}
};

/* FIXME: These are specific to saa711x - should be moved to its code */
static struct v4l2_queryctrl saa711x_qctrl[] = {
	{
		.id = V4L2_CID_BRIGHTNESS,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Brightness",
		.minimum = -128,
		.maximum = 127,
		.step = 1,
		.default_value = 0,
		.flags = 0,
	},{
		.id = V4L2_CID_CONTRAST,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Contrast",
		.minimum = 0x0,
		.maximum = 0x1f,
		.step = 0x1,
		.default_value = 0x10,
		.flags = 0,
	},{
		.id = V4L2_CID_SATURATION,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Saturation",
		.minimum = 0x0,
		.maximum = 0x1f,
		.step = 0x1,
		.default_value = 0x10,
		.flags = 0,
	},{
		.id = V4L2_CID_RED_BALANCE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Red chroma balance",
		.minimum = -128,
		.maximum = 127,
		.step = 1,
		.default_value = 0,
		.flags = 0,
	},{
		.id = V4L2_CID_BLUE_BALANCE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Blue chroma balance",
		.minimum = -128,
		.maximum = 127,
		.step = 1,
		.default_value = 0,
		.flags = 0,
	},{
		.id = V4L2_CID_GAMMA,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Gamma",
		.minimum = 0x0,
		.maximum = 0x3f,
		.step = 0x1,
		.default_value = 0x20,
		.flags = 0,
	}
};

static struct usb_driver em28xx_usb_driver;

static DEFINE_MUTEX(em28xx_sysfs_lock);
static DECLARE_RWSEM(em28xx_disconnect);

/*********************  v4l2 interface  ******************************************/

/*
 * em28xx_config()
 * inits registers with sane defaults
 */
static int em28xx_config(struct em28xx *dev)
{

	/* Sets I2C speed to 100 KHz */
	em28xx_write_regs_req(dev, 0x00, 0x06, "\x40", 1);

	/* enable vbi capturing */
	em28xx_audio_usb_mute(dev, 1);
	dev->mute = 1;		/* maybe not the right place... */
	dev->volume = 0x1f;
	em28xx_audio_analog_set(dev);
	em28xx_audio_analog_setup(dev);
	em28xx_outfmt_set_yuv422(dev);
	em28xx_colorlevels_set_default(dev);
	em28xx_compression_disable(dev);

	return 0;
}

/*
 * em28xx_config_i2c()
 * configure i2c attached devices
 */
static void em28xx_config_i2c(struct em28xx *dev)
{
	struct v4l2_frequency f;
	struct video_decoder_init em28xx_vdi = {.data = NULL };


	/* configure decoder */
	if(dev->model == EM2820_BOARD_MSI_VOX_USB_2){
		em28xx_vdi.data=saa7114_i2c_init;
		em28xx_vdi.len=sizeof(saa7114_i2c_init);
	}


	em28xx_i2c_call_clients(dev, DECODER_INIT, &em28xx_vdi);
	em28xx_i2c_call_clients(dev, DECODER_SET_INPUT, &dev->ctl_input);
/*	em28xx_i2c_call_clients(dev,DECODER_SET_PICTURE, &dev->vpic); */
/*	em28xx_i2c_call_clients(dev,DECODER_SET_NORM,&dev->tvnorm->id); */
/*	em28xx_i2c_call_clients(dev,DECODER_ENABLE_OUTPUT,&output); */
/*	em28xx_i2c_call_clients(dev,DECODER_DUMP, NULL); */

	/* configure tuner */
	f.tuner = 0;
	f.type = V4L2_TUNER_ANALOG_TV;
	f.frequency = 9076;	/* FIXME:remove magic number */
	dev->ctl_freq = f.frequency;
	em28xx_i2c_call_clients(dev, VIDIOC_S_FREQUENCY, &f);

	/* configure tda9887 */


/*	em28xx_i2c_call_clients(dev,VIDIOC_S_STD,&dev->tvnorm->id); */
}

/*
 * em28xx_empty_framequeues()
 * prepare queues for incoming and outgoing frames
 */
static void em28xx_empty_framequeues(struct em28xx *dev)
{
	u32 i;

	INIT_LIST_HEAD(&dev->inqueue);
	INIT_LIST_HEAD(&dev->outqueue);

	for (i = 0; i < EM28XX_NUM_FRAMES; i++) {
		dev->frame[i].state = F_UNUSED;
		dev->frame[i].buf.bytesused = 0;
	}
}

static void video_mux(struct em28xx *dev, int index)
{
	int input, ainput;

	input = INPUT(index)->vmux;
	dev->ctl_input = index;
	dev->ctl_ainput = INPUT(index)->amux;

	em28xx_i2c_call_clients(dev, DECODER_SET_INPUT, &input);


	em28xx_videodbg("Setting input index=%d, vmux=%d, amux=%d\n",index,input,dev->ctl_ainput);

	if (dev->has_msp34xx) {
		if (dev->i2s_speed)
			em28xx_i2c_call_clients(dev, VIDIOC_INT_I2S_CLOCK_FREQ, &dev->i2s_speed);
		em28xx_i2c_call_clients(dev, VIDIOC_S_AUDIO, &dev->ctl_ainput);
		ainput = EM28XX_AUDIO_SRC_TUNER;
		em28xx_audio_source(dev, ainput);
	} else {
		switch (dev->ctl_ainput) {
		case 0:
			ainput = EM28XX_AUDIO_SRC_TUNER;
			break;
		default:
			ainput = EM28XX_AUDIO_SRC_LINE;
		}
		em28xx_audio_source(dev, ainput);
	}
}

/*
 * em28xx_v4l2_open()
 * inits the device and starts isoc transfer
 */
static int em28xx_v4l2_open(struct inode *inode, struct file *filp)
{
	int minor = iminor(inode);
	int errCode = 0;
	struct em28xx *h,*dev = NULL;
	struct list_head *list;

	list_for_each(list,&em28xx_devlist) {
		h = list_entry(list, struct em28xx, devlist);
		if (h->vdev->minor == minor) {
			dev  = h;
		}
	}

	filp->private_data=dev;


	em28xx_videodbg("users=%d\n", dev->users);

	if (!down_read_trylock(&em28xx_disconnect))
		return -ERESTARTSYS;

	if (dev->users) {
		em28xx_warn("this driver can be opened only once\n");
		up_read(&em28xx_disconnect);
		return -EBUSY;
	}

/*	if(dev->vbi_dev->minor == minor){
		dev->type=V4L2_BUF_TYPE_VBI_CAPTURE;
	}*/
	if (dev->vdev->minor == minor) {
		dev->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	}

	init_MUTEX(&dev->fileop_lock);	/* to 1 == available */
	spin_lock_init(&dev->queue_lock);
	init_waitqueue_head(&dev->wait_frame);
	init_waitqueue_head(&dev->wait_stream);

	down(&dev->lock);

	em28xx_set_alternate(dev);

	dev->width = norm_maxw(dev);
	dev->height = norm_maxh(dev);
	dev->frame_size = dev->width * dev->height * 2;
	dev->field_size = dev->frame_size >> 1;	/*both_fileds ? dev->frame_size>>1 : dev->frame_size; */
	dev->bytesperline = dev->width * 2;
	dev->hscale = 0;
	dev->vscale = 0;

	em28xx_capture_start(dev, 1);
	em28xx_resolution_set(dev);

	/* start the transfer */
	errCode = em28xx_init_isoc(dev);
	if (errCode)
		goto err;

	dev->users++;
	filp->private_data = dev;
	dev->io = IO_NONE;
	dev->stream = STREAM_OFF;
	dev->num_frames = 0;

	/* prepare queues */
	em28xx_empty_framequeues(dev);

	dev->state |= DEV_INITIALIZED;

	video_mux(dev, 0);

      err:
	up(&dev->lock);
	up_read(&em28xx_disconnect);
	return errCode;
}

/*
 * em28xx_realease_resources()
 * unregisters the v4l2,i2c and usb devices
 * called when the device gets disconected or at module unload
*/
static void em28xx_release_resources(struct em28xx *dev)
{
	mutex_lock(&em28xx_sysfs_lock);

	em28xx_info("V4L2 device /dev/video%d deregistered\n",
		    dev->vdev->minor);
	list_del(&dev->devlist);
	video_unregister_device(dev->vdev);
/*	video_unregister_device(dev->vbi_dev); */
	em28xx_i2c_unregister(dev);
	usb_put_dev(dev->udev);
	mutex_unlock(&em28xx_sysfs_lock);
}

/*
 * em28xx_v4l2_close()
 * stops streaming and deallocates all resources allocated by the v4l2 calls and ioctls
 */
static int em28xx_v4l2_close(struct inode *inode, struct file *filp)
{
	int errCode;
	struct em28xx *dev=filp->private_data;

	em28xx_videodbg("users=%d\n", dev->users);

	down(&dev->lock);

	em28xx_uninit_isoc(dev);

	em28xx_release_buffers(dev);

	/* the device is already disconnect, free the remaining resources */
	if (dev->state & DEV_DISCONNECTED) {
		em28xx_release_resources(dev);
		up(&dev->lock);
		kfree(dev);
		return 0;
	}

	/* set alternate 0 */
	dev->alt = 0;
	em28xx_videodbg("setting alternate 0\n");
	errCode = usb_set_interface(dev->udev, 0, 0);
	if (errCode < 0) {
		em28xx_errdev ("cannot change alternate number to 0 (error=%i)\n",
		     errCode);
	}

	dev->users--;
	wake_up_interruptible_nr(&dev->open, 1);
	up(&dev->lock);
	return 0;
}

/*
 * em28xx_v4l2_read()
 * will allocate buffers when called for the first time
 */
static ssize_t
em28xx_v4l2_read(struct file *filp, char __user * buf, size_t count,
		 loff_t * f_pos)
{
	struct em28xx_frame_t *f, *i;
	unsigned long lock_flags;
	int ret = 0;
	struct em28xx *dev = filp->private_data;

	if (down_interruptible(&dev->fileop_lock))
		return -ERESTARTSYS;

	if (dev->state & DEV_DISCONNECTED) {
		em28xx_videodbg("device not present\n");
		up(&dev->fileop_lock);
		return -ENODEV;
	}

	if (dev->state & DEV_MISCONFIGURED) {
		em28xx_videodbg("device misconfigured; close and open it again\n");
		up(&dev->fileop_lock);
		return -EIO;
	}

	if (dev->io == IO_MMAP) {
		em28xx_videodbg ("IO method is set to mmap; close and open"
				" the device again to choose the read method\n");
		up(&dev->fileop_lock);
		return -EINVAL;
	}

	if (dev->io == IO_NONE) {
		if (!em28xx_request_buffers(dev, EM28XX_NUM_READ_FRAMES)) {
			em28xx_errdev("read failed, not enough memory\n");
			up(&dev->fileop_lock);
			return -ENOMEM;
		}
		dev->io = IO_READ;
		dev->stream = STREAM_ON;
		em28xx_queue_unusedframes(dev);
	}

	if (!count) {
		up(&dev->fileop_lock);
		return 0;
	}

	if (list_empty(&dev->outqueue)) {
		if (filp->f_flags & O_NONBLOCK) {
			up(&dev->fileop_lock);
			return -EAGAIN;
		}
		ret = wait_event_interruptible
		    (dev->wait_frame,
		     (!list_empty(&dev->outqueue)) ||
		     (dev->state & DEV_DISCONNECTED));
		if (ret) {
			up(&dev->fileop_lock);
			return ret;
		}
		if (dev->state & DEV_DISCONNECTED) {
			up(&dev->fileop_lock);
			return -ENODEV;
		}
	}

	f = list_entry(dev->outqueue.prev, struct em28xx_frame_t, frame);

	spin_lock_irqsave(&dev->queue_lock, lock_flags);
	list_for_each_entry(i, &dev->outqueue, frame)
	    i->state = F_UNUSED;
	INIT_LIST_HEAD(&dev->outqueue);
	spin_unlock_irqrestore(&dev->queue_lock, lock_flags);

	em28xx_queue_unusedframes(dev);

	if (count > f->buf.length)
		count = f->buf.length;

	if (copy_to_user(buf, f->bufmem, count)) {
		up(&dev->fileop_lock);
		return -EFAULT;
	}
	*f_pos += count;

	up(&dev->fileop_lock);

	return count;
}

/*
 * em28xx_v4l2_poll()
 * will allocate buffers when called for the first time
 */
static unsigned int em28xx_v4l2_poll(struct file *filp, poll_table * wait)
{
	unsigned int mask = 0;
	struct em28xx *dev = filp->private_data;

	if (down_interruptible(&dev->fileop_lock))
		return POLLERR;

	if (dev->state & DEV_DISCONNECTED) {
		em28xx_videodbg("device not present\n");
	} else if (dev->state & DEV_MISCONFIGURED) {
		em28xx_videodbg("device is misconfigured; close and open it again\n");
	} else {
		if (dev->io == IO_NONE) {
			if (!em28xx_request_buffers
			    (dev, EM28XX_NUM_READ_FRAMES)) {
				em28xx_warn
				    ("poll() failed, not enough memory\n");
			} else {
				dev->io = IO_READ;
				dev->stream = STREAM_ON;
			}
		}

		if (dev->io == IO_READ) {
			em28xx_queue_unusedframes(dev);
			poll_wait(filp, &dev->wait_frame, wait);

			if (!list_empty(&dev->outqueue))
				mask |= POLLIN | POLLRDNORM;

			up(&dev->fileop_lock);

			return mask;
		}
	}

	up(&dev->fileop_lock);
	return POLLERR;
}

/*
 * em28xx_vm_open()
 */
static void em28xx_vm_open(struct vm_area_struct *vma)
{
	struct em28xx_frame_t *f = vma->vm_private_data;
	f->vma_use_count++;
}

/*
 * em28xx_vm_close()
 */
static void em28xx_vm_close(struct vm_area_struct *vma)
{
	/* NOTE: buffers are not freed here */
	struct em28xx_frame_t *f = vma->vm_private_data;
	f->vma_use_count--;
}

static struct vm_operations_struct em28xx_vm_ops = {
	.open = em28xx_vm_open,
	.close = em28xx_vm_close,
};

/*
 * em28xx_v4l2_mmap()
 */
static int em28xx_v4l2_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start,
	    start = vma->vm_start;
	void *pos;
	u32 i;

	struct em28xx *dev = filp->private_data;

	if (down_interruptible(&dev->fileop_lock))
		return -ERESTARTSYS;

	if (dev->state & DEV_DISCONNECTED) {
		em28xx_videodbg("mmap: device not present\n");
		up(&dev->fileop_lock);
		return -ENODEV;
	}

	if (dev->state & DEV_MISCONFIGURED) {
		em28xx_videodbg ("mmap: Device is misconfigured; close and "
						"open it again\n");
		up(&dev->fileop_lock);
		return -EIO;
	}

	if (dev->io != IO_MMAP || !(vma->vm_flags & VM_WRITE) ||
	    size != PAGE_ALIGN(dev->frame[0].buf.length)) {
		up(&dev->fileop_lock);
		return -EINVAL;
	}

	for (i = 0; i < dev->num_frames; i++) {
		if ((dev->frame[i].buf.m.offset >> PAGE_SHIFT) == vma->vm_pgoff)
			break;
	}
	if (i == dev->num_frames) {
		em28xx_videodbg("mmap: user supplied mapping address is out of range\n");
		up(&dev->fileop_lock);
		return -EINVAL;
	}

	/* VM_IO is eventually going to replace PageReserved altogether */
	vma->vm_flags |= VM_IO;
	vma->vm_flags |= VM_RESERVED;	/* avoid to swap out this VMA */

	pos = dev->frame[i].bufmem;
	while (size > 0) {	/* size is page-aligned */
		if (vm_insert_page(vma, start, vmalloc_to_page(pos))) {
			em28xx_videodbg("mmap: vm_insert_page failed\n");
			up(&dev->fileop_lock);
			return -EAGAIN;
		}
		start += PAGE_SIZE;
		pos += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	vma->vm_ops = &em28xx_vm_ops;
	vma->vm_private_data = &dev->frame[i];

	em28xx_vm_open(vma);
	up(&dev->fileop_lock);
	return 0;
}

/*
 * em28xx_get_ctrl()
 * return the current saturation, brightness or contrast, mute state
 */
static int em28xx_get_ctrl(struct em28xx *dev, struct v4l2_control *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		ctrl->value = dev->mute;
		return 0;
	case V4L2_CID_AUDIO_VOLUME:
		ctrl->value = dev->volume;
		return 0;
	default:
		return -EINVAL;
	}
}

/*FIXME: should be moved to saa711x */
static int saa711x_get_ctrl(struct em28xx *dev, struct v4l2_control *ctrl)
{
	s32 tmp;
	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		if ((tmp = em28xx_brightness_get(dev)) < 0)
			return -EIO;
		ctrl->value = (s32) ((s8) tmp);	/* FIXME: clenaer way to extend sign? */
		return 0;
	case V4L2_CID_CONTRAST:
		if ((ctrl->value = em28xx_contrast_get(dev)) < 0)
			return -EIO;
		return 0;
	case V4L2_CID_SATURATION:
		if ((ctrl->value = em28xx_saturation_get(dev)) < 0)
			return -EIO;
		return 0;
	case V4L2_CID_RED_BALANCE:
		if ((tmp = em28xx_v_balance_get(dev)) < 0)
			return -EIO;
		ctrl->value = (s32) ((s8) tmp);	/* FIXME: clenaer way to extend sign? */
		return 0;
	case V4L2_CID_BLUE_BALANCE:
		if ((tmp = em28xx_u_balance_get(dev)) < 0)
			return -EIO;
		ctrl->value = (s32) ((s8) tmp);	/* FIXME: clenaer way to extend sign? */
		return 0;
	case V4L2_CID_GAMMA:
		if ((ctrl->value = em28xx_gamma_get(dev)) < 0)
			return -EIO;
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * em28xx_set_ctrl()
 * mute or set new saturation, brightness or contrast
 */
static int em28xx_set_ctrl(struct em28xx *dev, const struct v4l2_control *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		if (ctrl->value != dev->mute) {
			dev->mute = ctrl->value;
			em28xx_audio_usb_mute(dev, ctrl->value);
			return em28xx_audio_analog_set(dev);
		}
		return 0;
	case V4L2_CID_AUDIO_VOLUME:
		dev->volume = ctrl->value;
		return em28xx_audio_analog_set(dev);
	default:
		return -EINVAL;
	}
}

/*FIXME: should be moved to saa711x */
static int saa711x_set_ctrl(struct em28xx *dev, const struct v4l2_control *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		return em28xx_brightness_set(dev, ctrl->value);
	case V4L2_CID_CONTRAST:
		return em28xx_contrast_set(dev, ctrl->value);
	case V4L2_CID_SATURATION:
		return em28xx_saturation_set(dev, ctrl->value);
	case V4L2_CID_RED_BALANCE:
		return em28xx_v_balance_set(dev, ctrl->value);
	case V4L2_CID_BLUE_BALANCE:
		return em28xx_u_balance_set(dev, ctrl->value);
	case V4L2_CID_GAMMA:
		return em28xx_gamma_set(dev, ctrl->value);
	default:
		return -EINVAL;
	}
}

/*
 * em28xx_stream_interrupt()
 * stops streaming
 */
static int em28xx_stream_interrupt(struct em28xx *dev)
{
	int ret = 0;

	/* stop reading from the device */

	dev->stream = STREAM_INTERRUPT;
	ret = wait_event_timeout(dev->wait_stream,
				 (dev->stream == STREAM_OFF) ||
				 (dev->state & DEV_DISCONNECTED),
				 EM28XX_URB_TIMEOUT);
	if (dev->state & DEV_DISCONNECTED)
		return -ENODEV;
	else if (ret) {
		dev->state |= DEV_MISCONFIGURED;
		em28xx_videodbg("device is misconfigured; close and "
			"open /dev/video%d again\n", dev->vdev->minor);
		return ret;
	}

	return 0;
}

static int em28xx_set_norm(struct em28xx *dev, int width, int height)
{
	unsigned int hscale, vscale;
	unsigned int maxh, maxw;

	maxw = norm_maxw(dev);
	maxh = norm_maxh(dev);

	/* width must even because of the YUYV format */
	/* height must be even because of interlacing */
	height &= 0xfffe;
	width &= 0xfffe;

	if (height < 32)
		height = 32;
	if (height > maxh)
		height = maxh;
	if (width < 48)
		width = 48;
	if (width > maxw)
		width = maxw;

	if ((hscale = (((unsigned long)maxw) << 12) / width - 4096L) >= 0x4000)
		hscale = 0x3fff;
	width = (((unsigned long)maxw) << 12) / (hscale + 4096L);

	if ((vscale = (((unsigned long)maxh) << 12) / height - 4096L) >= 0x4000)
		vscale = 0x3fff;
	height = (((unsigned long)maxh) << 12) / (vscale + 4096L);

	/* set new image size */
	dev->width = width;
	dev->height = height;
	dev->frame_size = dev->width * dev->height * 2;
	dev->field_size = dev->frame_size >> 1;	/*both_fileds ? dev->frame_size>>1 : dev->frame_size; */
	dev->bytesperline = dev->width * 2;
	dev->hscale = hscale;
	dev->vscale = vscale;

	em28xx_resolution_set(dev);

	return 0;
}

/*
 * em28xx_v4l2_do_ioctl()
 * This function is _not_ called directly, but from
 * em28xx_v4l2_ioctl. Userspace
 * copying is done already, arg is a kernel pointer.
 */
static int em28xx_do_ioctl(struct inode *inode, struct file *filp,
			   struct em28xx *dev, unsigned int cmd, void *arg,
			   v4l2_kioctl driver_ioctl)
{
	int ret;

	switch (cmd) {
		/* ---------- tv norms ---------- */
	case VIDIOC_ENUMSTD:
		{
			struct v4l2_standard *e = arg;
			unsigned int i;

			i = e->index;
			if (i >= TVNORMS)
				return -EINVAL;
			ret = v4l2_video_std_construct(e, tvnorms[e->index].id,
						       tvnorms[e->index].name);
			e->index = i;
			if (ret < 0)
				return ret;
			return 0;
		}
	case VIDIOC_G_STD:
		{
			v4l2_std_id *id = arg;

			*id = dev->tvnorm->id;
			return 0;
		}
	case VIDIOC_S_STD:
		{
			v4l2_std_id *id = arg;
			unsigned int i;

			for (i = 0; i < TVNORMS; i++)
				if (*id == tvnorms[i].id)
					break;
			if (i == TVNORMS)
				for (i = 0; i < TVNORMS; i++)
					if (*id & tvnorms[i].id)
						break;
			if (i == TVNORMS)
				return -EINVAL;

			down(&dev->lock);
			dev->tvnorm = &tvnorms[i];

			em28xx_set_norm(dev, dev->width, dev->height);

/*
		dev->width=norm_maxw(dev);
		dev->height=norm_maxh(dev);
		dev->frame_size=dev->width*dev->height*2;
		dev->field_size=dev->frame_size>>1;
		dev->bytesperline=dev->width*2;
		dev->hscale=0;
		dev->vscale=0;

		em28xx_resolution_set(dev);
*/
/*
		em28xx_uninit_isoc(dev);
		em28xx_set_alternate(dev);
		em28xx_capture_start(dev, 1);
		em28xx_resolution_set(dev);
		em28xx_init_isoc(dev);
*/
			em28xx_i2c_call_clients(dev, DECODER_SET_NORM,
						&tvnorms[i].mode);
			em28xx_i2c_call_clients(dev, VIDIOC_S_STD,
						&dev->tvnorm->id);

			up(&dev->lock);

			return 0;
		}

		/* ------ input switching ---------- */
	case VIDIOC_ENUMINPUT:
		{
			struct v4l2_input *i = arg;
			unsigned int n;
			static const char *iname[] = {
				[EM28XX_VMUX_COMPOSITE1] = "Composite1",
				[EM28XX_VMUX_COMPOSITE2] = "Composite2",
				[EM28XX_VMUX_COMPOSITE3] = "Composite3",
				[EM28XX_VMUX_COMPOSITE4] = "Composite4",
				[EM28XX_VMUX_SVIDEO] = "S-Video",
				[EM28XX_VMUX_TELEVISION] = "Television",
				[EM28XX_VMUX_CABLE] = "Cable TV",
				[EM28XX_VMUX_DVB] = "DVB",
				[EM28XX_VMUX_DEBUG] = "for debug only",
			};

			n = i->index;
			if (n >= MAX_EM28XX_INPUT)
				return -EINVAL;
			if (0 == INPUT(n)->type)
				return -EINVAL;
			memset(i, 0, sizeof(*i));
			i->index = n;
			i->type = V4L2_INPUT_TYPE_CAMERA;
			strcpy(i->name, iname[INPUT(n)->type]);
			if ((EM28XX_VMUX_TELEVISION == INPUT(n)->type) ||
			    (EM28XX_VMUX_CABLE == INPUT(n)->type))
				i->type = V4L2_INPUT_TYPE_TUNER;
			for (n = 0; n < ARRAY_SIZE(tvnorms); n++)
				i->std |= tvnorms[n].id;
			return 0;
		}

	case VIDIOC_G_INPUT:
		{
			int *i = arg;
			*i = dev->ctl_input;

			return 0;
		}

	case VIDIOC_S_INPUT:
		{
			int *index = arg;

			if (*index >= MAX_EM28XX_INPUT)
				return -EINVAL;
			if (0 == INPUT(*index)->type)
				return -EINVAL;

			down(&dev->lock);
			video_mux(dev, *index);
			up(&dev->lock);

			return 0;
		}

	case VIDIOC_G_AUDIO:
		{
			struct v4l2_audio *a = arg;
			unsigned int index = a->index;

			if (a->index > 1)
				return -EINVAL;
			memset(a, 0, sizeof(*a));
			index = dev->ctl_ainput;

			if (index == 0) {
				strcpy(a->name, "Television");
			} else {
				strcpy(a->name, "Line In");
			}
			a->capability = V4L2_AUDCAP_STEREO;
			a->index = index;
			return 0;
		}

	case VIDIOC_S_AUDIO:
		{
			struct v4l2_audio *a = arg;
			if (a->index != dev->ctl_ainput)
				return -EINVAL;

			return 0;
		}

		/* --- controls ---------------------------------------------- */
	case VIDIOC_QUERYCTRL:
		{
			struct v4l2_queryctrl *qc = arg;
			int i, id=qc->id;

			memset(qc,0,sizeof(*qc));
			qc->id=id;

			if (!dev->has_msp34xx) {
				for (i = 0; i < ARRAY_SIZE(em28xx_qctrl); i++) {
					if (qc->id && qc->id == em28xx_qctrl[i].id) {
						memcpy(qc, &(em28xx_qctrl[i]),
						sizeof(*qc));
						return 0;
					}
				}
			}
			if (dev->decoder == EM28XX_TVP5150) {
				em28xx_i2c_call_clients(dev,cmd,qc);
				if (qc->type)
					return 0;
				else
					return -EINVAL;
			}
			for (i = 0; i < ARRAY_SIZE(saa711x_qctrl); i++) {
				if (qc->id && qc->id == saa711x_qctrl[i].id) {
					memcpy(qc, &(saa711x_qctrl[i]),
					       sizeof(*qc));
					return 0;
				}
			}

			return -EINVAL;
		}

	case VIDIOC_G_CTRL:
		{
			struct v4l2_control *ctrl = arg;
			int retval=-EINVAL;

			if (!dev->has_msp34xx)
				retval=em28xx_get_ctrl(dev, ctrl);
			if (retval==-EINVAL) {
				if (dev->decoder == EM28XX_TVP5150) {
					em28xx_i2c_call_clients(dev,cmd,arg);
					return 0;
				}

				return saa711x_get_ctrl(dev, ctrl);
			} else return retval;
		}

	case VIDIOC_S_CTRL:
		{
			struct v4l2_control *ctrl = arg;
			u8 i;

			if (!dev->has_msp34xx){
				for (i = 0; i < ARRAY_SIZE(em28xx_qctrl); i++) {
					if (ctrl->id == em28xx_qctrl[i].id) {
						if (ctrl->value <
						em28xx_qctrl[i].minimum
						|| ctrl->value >
						em28xx_qctrl[i].maximum)
							return -ERANGE;
						return em28xx_set_ctrl(dev, ctrl);
					}
				}
			}

			if (dev->decoder == EM28XX_TVP5150) {
				em28xx_i2c_call_clients(dev,cmd,arg);
				return 0;
			} else if (!dev->has_msp34xx) {
				for (i = 0; i < ARRAY_SIZE(em28xx_qctrl); i++) {
					if (ctrl->id == em28xx_qctrl[i].id) {
						if (ctrl->value <
						em28xx_qctrl[i].minimum
						|| ctrl->value >
						em28xx_qctrl[i].maximum)
							return -ERANGE;
						return em28xx_set_ctrl(dev, ctrl);
					}
				}
				for (i = 0; i < ARRAY_SIZE(saa711x_qctrl); i++) {
					if (ctrl->id == saa711x_qctrl[i].id) {
						if (ctrl->value <
						saa711x_qctrl[i].minimum
						|| ctrl->value >
						saa711x_qctrl[i].maximum)
							return -ERANGE;
						return saa711x_set_ctrl(dev, ctrl);
					}
				}
			}

			return -EINVAL;
		}

		/* --- tuner ioctls ------------------------------------------ */
	case VIDIOC_G_TUNER:
		{
			struct v4l2_tuner *t = arg;
			int status = 0;

			if (0 != t->index)
				return -EINVAL;

			memset(t, 0, sizeof(*t));
			strcpy(t->name, "Tuner");
			t->type = V4L2_TUNER_ANALOG_TV;
			t->capability = V4L2_TUNER_CAP_NORM;
			t->rangehigh = 0xffffffffUL;	/* FIXME: set correct range */
/*		t->signal = 0xffff;*/
/*		em28xx_i2c_call_clients(dev,VIDIOC_G_TUNER,t);*/
			/* No way to get signal strength? */
			down(&dev->lock);
			em28xx_i2c_call_clients(dev, DECODER_GET_STATUS,
						&status);
			up(&dev->lock);
			t->signal =
			    (status & DECODER_STATUS_GOOD) != 0 ? 0xffff : 0;

			em28xx_videodbg("VIDIO_G_TUNER: signal=%x, afc=%x\n", t->signal,
				 t->afc);
			return 0;
		}
	case VIDIOC_S_TUNER:
		{
			struct v4l2_tuner *t = arg;
			int status = 0;

			if (0 != t->index)
				return -EINVAL;
			memset(t, 0, sizeof(*t));
			strcpy(t->name, "Tuner");
			t->type = V4L2_TUNER_ANALOG_TV;
			t->capability = V4L2_TUNER_CAP_NORM;
			t->rangehigh = 0xffffffffUL;	/* FIXME: set correct range */
/*		t->signal = 0xffff; */
			/* No way to get signal strength? */
			down(&dev->lock);
			em28xx_i2c_call_clients(dev, DECODER_GET_STATUS,
						&status);
			up(&dev->lock);
			t->signal =
			    (status & DECODER_STATUS_GOOD) != 0 ? 0xffff : 0;

			em28xx_videodbg("VIDIO_S_TUNER: signal=%x, afc=%x\n",
				 t->signal, t->afc);
			return 0;
		}
	case VIDIOC_G_FREQUENCY:
		{
			struct v4l2_frequency *f = arg;

			memset(f, 0, sizeof(*f));
			f->type = V4L2_TUNER_ANALOG_TV;
			f->frequency = dev->ctl_freq;

			return 0;
		}
	case VIDIOC_S_FREQUENCY:
		{
			struct v4l2_frequency *f = arg;

			if (0 != f->tuner)
				return -EINVAL;

			if (V4L2_TUNER_ANALOG_TV != f->type)
				return -EINVAL;

			down(&dev->lock);
			dev->ctl_freq = f->frequency;
			em28xx_i2c_call_clients(dev, VIDIOC_S_FREQUENCY, f);
			up(&dev->lock);
			return 0;
		}

	case VIDIOC_CROPCAP:
		{
			struct v4l2_cropcap *cc = arg;

			if (cc->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
				return -EINVAL;
			cc->bounds.left = 0;
			cc->bounds.top = 0;
			cc->bounds.width = dev->width;
			cc->bounds.height = dev->height;
			cc->defrect = cc->bounds;
			cc->pixelaspect.numerator = 54;	/* 4:3 FIXME: remove magic numbers */
			cc->pixelaspect.denominator = 59;
			return 0;
		}
	case VIDIOC_STREAMON:
		{
			int *type = arg;

			if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE
			    || dev->io != IO_MMAP)
				return -EINVAL;

			if (list_empty(&dev->inqueue))
				return -EINVAL;

			dev->stream = STREAM_ON;	/* FIXME: Start video capture here? */

			em28xx_videodbg("VIDIOC_STREAMON: starting stream\n");

			return 0;
		}
	case VIDIOC_STREAMOFF:
		{
			int *type = arg;
			int ret;

			if (*type != V4L2_BUF_TYPE_VIDEO_CAPTURE
			    || dev->io != IO_MMAP)
				return -EINVAL;

			if (dev->stream == STREAM_ON) {
				em28xx_videodbg ("VIDIOC_STREAMOFF: interrupting stream\n");
				if ((ret = em28xx_stream_interrupt(dev)))
					return ret;
			}
			em28xx_empty_framequeues(dev);

			return 0;
		}
	default:
		return v4l_compat_translate_ioctl(inode, filp, cmd, arg,
						  driver_ioctl);
	}
	return 0;
}

/*
 * em28xx_v4l2_do_ioctl()
 * This function is _not_ called directly, but from
 * em28xx_v4l2_ioctl. Userspace
 * copying is done already, arg is a kernel pointer.
 */
static int em28xx_video_do_ioctl(struct inode *inode, struct file *filp,
				 unsigned int cmd, void *arg)
{
	struct em28xx *dev = filp->private_data;

	if (!dev)
		return -ENODEV;

	if (video_debug > 1)
		v4l_print_ioctl(dev->name,cmd);

	switch (cmd) {

		/* --- capabilities ------------------------------------------ */
	case VIDIOC_QUERYCAP:
		{
			struct v4l2_capability *cap = arg;

			memset(cap, 0, sizeof(*cap));
			strlcpy(cap->driver, "em28xx", sizeof(cap->driver));
			strlcpy(cap->card, em28xx_boards[dev->model].name,
				sizeof(cap->card));
			strlcpy(cap->bus_info, dev->udev->dev.bus_id,
				sizeof(cap->bus_info));
			cap->version = EM28XX_VERSION_CODE;
			cap->capabilities =
			    V4L2_CAP_VIDEO_CAPTURE |
			    V4L2_CAP_AUDIO |
			    V4L2_CAP_READWRITE | V4L2_CAP_STREAMING;
			if (dev->has_tuner)
				cap->capabilities |= V4L2_CAP_TUNER;
			return 0;
		}

		/* --- capture ioctls ---------------------------------------- */
	case VIDIOC_ENUM_FMT:
		{
			struct v4l2_fmtdesc *fmtd = arg;

			if (fmtd->index != 0)
				return -EINVAL;
			memset(fmtd, 0, sizeof(*fmtd));
			fmtd->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			strcpy(fmtd->description, "Packed YUY2");
			fmtd->pixelformat = V4L2_PIX_FMT_YUYV;
			memset(fmtd->reserved, 0, sizeof(fmtd->reserved));
			return 0;
		}

	case VIDIOC_G_FMT:
		{
			struct v4l2_format *format = arg;

			em28xx_videodbg("VIDIOC_G_FMT: type=%s\n",
				 format->type ==
				 V4L2_BUF_TYPE_VIDEO_CAPTURE ?
				 "V4L2_BUF_TYPE_VIDEO_CAPTURE" : format->type ==
				 V4L2_BUF_TYPE_VBI_CAPTURE ?
				 "V4L2_BUF_TYPE_VBI_CAPTURE " :
				 "not supported");

			if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
				return -EINVAL;

			format->fmt.pix.width = dev->width;
			format->fmt.pix.height = dev->height;
			format->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
			format->fmt.pix.bytesperline = dev->bytesperline;
			format->fmt.pix.sizeimage = dev->frame_size;
			format->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
			format->fmt.pix.field = dev->interlaced ? V4L2_FIELD_INTERLACED : V4L2_FIELD_TOP;	/* FIXME: TOP? NONE? BOTTOM? ALTENATE? */

			em28xx_videodbg("VIDIOC_G_FMT: %dx%d\n", dev->width,
				 dev->height);
			return 0;
		}

	case VIDIOC_TRY_FMT:
	case VIDIOC_S_FMT:
		{
			struct v4l2_format *format = arg;
			u32 i;
			int ret = 0;
			int width = format->fmt.pix.width;
			int height = format->fmt.pix.height;
			unsigned int hscale, vscale;
			unsigned int maxh, maxw;

			maxw = norm_maxw(dev);
			maxh = norm_maxh(dev);

/*		int both_fields; */

			em28xx_videodbg("%s: type=%s\n",
				 cmd ==
				 VIDIOC_TRY_FMT ? "VIDIOC_TRY_FMT" :
				 "VIDIOC_S_FMT",
				 format->type ==
				 V4L2_BUF_TYPE_VIDEO_CAPTURE ?
				 "V4L2_BUF_TYPE_VIDEO_CAPTURE" : format->type ==
				 V4L2_BUF_TYPE_VBI_CAPTURE ?
				 "V4L2_BUF_TYPE_VBI_CAPTURE " :
				 "not supported");

			if (format->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
				return -EINVAL;

			em28xx_videodbg("%s: requested %dx%d\n",
				 cmd ==
				 VIDIOC_TRY_FMT ? "VIDIOC_TRY_FMT" :
				 "VIDIOC_S_FMT", format->fmt.pix.width,
				 format->fmt.pix.height);

			/* FIXME: Move some code away from here */
			/* width must even because of the YUYV format */
			/* height must be even because of interlacing */
			height &= 0xfffe;
			width &= 0xfffe;

			if (height < 32)
				height = 32;
			if (height > maxh)
				height = maxh;
			if (width < 48)
				width = 48;
			if (width > maxw)
				width = maxw;

			if(dev->is_em2800){
				/* the em2800 can only scale down to 50% */
				if(height % (maxh / 2))
					height=maxh;
				if(width % (maxw / 2))
					width=maxw;
				/* according to empiatech support */
				/* the MaxPacketSize is to small to support */
				/* framesizes larger than 640x480 @ 30 fps */
				/* or 640x576 @ 25 fps. As this would cut */
				/* of a part of the image we prefer */
				/* 360x576 or 360x480 for now */
				if(width == maxw && height == maxh)
					width /= 2;
			}

			if ((hscale =
			     (((unsigned long)maxw) << 12) / width - 4096L) >=
			    0x4000)
				hscale = 0x3fff;
			width =
			    (((unsigned long)maxw) << 12) / (hscale + 4096L);

			if ((vscale =
			     (((unsigned long)maxh) << 12) / height - 4096L) >=
			    0x4000)
				vscale = 0x3fff;
			height =
			    (((unsigned long)maxh) << 12) / (vscale + 4096L);

			format->fmt.pix.width = width;
			format->fmt.pix.height = height;
			format->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
			format->fmt.pix.bytesperline = width * 2;
			format->fmt.pix.sizeimage = width * 2 * height;
			format->fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
			format->fmt.pix.field = V4L2_FIELD_INTERLACED;

			em28xx_videodbg("%s: returned %dx%d (%d, %d)\n",
				 cmd ==
				 VIDIOC_TRY_FMT ? "VIDIOC_TRY_FMT" :
				 "VIDIOC_S_FMT", format->fmt.pix.width,
				 format->fmt.pix.height, hscale, vscale);

			if (cmd == VIDIOC_TRY_FMT)
				return 0;

			for (i = 0; i < dev->num_frames; i++)
				if (dev->frame[i].vma_use_count) {
					em28xx_videodbg("VIDIOC_S_FMT failed. "
						"Unmap the buffers first.\n");
					return -EINVAL;
				}

			/* stop io in case it is already in progress */
			if (dev->stream == STREAM_ON) {
				em28xx_videodbg("VIDIOC_SET_FMT: interupting stream\n");
				if ((ret = em28xx_stream_interrupt(dev)))
					return ret;
			}

			em28xx_release_buffers(dev);
			dev->io = IO_NONE;

			/* set new image size */
			dev->width = width;
			dev->height = height;
			dev->frame_size = dev->width * dev->height * 2;
			dev->field_size = dev->frame_size >> 1;	/*both_fileds ? dev->frame_size>>1 : dev->frame_size; */
			dev->bytesperline = dev->width * 2;
			dev->hscale = hscale;
			dev->vscale = vscale;
/*			dev->both_fileds = both_fileds; */
			em28xx_uninit_isoc(dev);
			em28xx_set_alternate(dev);
			em28xx_capture_start(dev, 1);
			em28xx_resolution_set(dev);
			em28xx_init_isoc(dev);

			return 0;
		}

		/* --- streaming capture ------------------------------------- */
	case VIDIOC_REQBUFS:
		{
			struct v4l2_requestbuffers *rb = arg;
			u32 i;
			int ret;

			if (rb->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
			    rb->memory != V4L2_MEMORY_MMAP)
				return -EINVAL;

			if (dev->io == IO_READ) {
				em28xx_videodbg ("method is set to read;"
					" close and open the device again to"
					" choose the mmap I/O method\n");
				return -EINVAL;
			}

			for (i = 0; i < dev->num_frames; i++)
				if (dev->frame[i].vma_use_count) {
					em28xx_videodbg ("VIDIOC_REQBUFS failed; previous buffers are still mapped\n");
					return -EINVAL;
				}

			if (dev->stream == STREAM_ON) {
				em28xx_videodbg("VIDIOC_REQBUFS: interrupting stream\n");
				if ((ret = em28xx_stream_interrupt(dev)))
					return ret;
			}

			em28xx_empty_framequeues(dev);

			em28xx_release_buffers(dev);
			if (rb->count)
				rb->count =
				    em28xx_request_buffers(dev, rb->count);

			dev->frame_current = NULL;

			em28xx_videodbg ("VIDIOC_REQBUFS: setting io method to mmap: num bufs %i\n",
						     rb->count);
			dev->io = rb->count ? IO_MMAP : IO_NONE;
			return 0;
		}

	case VIDIOC_QUERYBUF:
		{
			struct v4l2_buffer *b = arg;

			if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
			    b->index >= dev->num_frames || dev->io != IO_MMAP)
				return -EINVAL;

			memcpy(b, &dev->frame[b->index].buf, sizeof(*b));

			if (dev->frame[b->index].vma_use_count) {
				b->flags |= V4L2_BUF_FLAG_MAPPED;
			}
			if (dev->frame[b->index].state == F_DONE)
				b->flags |= V4L2_BUF_FLAG_DONE;
			else if (dev->frame[b->index].state != F_UNUSED)
				b->flags |= V4L2_BUF_FLAG_QUEUED;
			return 0;
		}
	case VIDIOC_QBUF:
		{
			struct v4l2_buffer *b = arg;
			unsigned long lock_flags;

			if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
			    b->index >= dev->num_frames || dev->io != IO_MMAP) {
				return -EINVAL;
			}

			if (dev->frame[b->index].state != F_UNUSED) {
				return -EAGAIN;
			}
			dev->frame[b->index].state = F_QUEUED;

			/* add frame to fifo */
			spin_lock_irqsave(&dev->queue_lock, lock_flags);
			list_add_tail(&dev->frame[b->index].frame,
				      &dev->inqueue);
			spin_unlock_irqrestore(&dev->queue_lock, lock_flags);

			return 0;
		}
	case VIDIOC_DQBUF:
		{
			struct v4l2_buffer *b = arg;
			struct em28xx_frame_t *f;
			unsigned long lock_flags;
			int ret = 0;

			if (b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE
			    || dev->io != IO_MMAP)
				return -EINVAL;

			if (list_empty(&dev->outqueue)) {
				if (dev->stream == STREAM_OFF)
					return -EINVAL;
				if (filp->f_flags & O_NONBLOCK)
					return -EAGAIN;
				ret = wait_event_interruptible
				    (dev->wait_frame,
				     (!list_empty(&dev->outqueue)) ||
				     (dev->state & DEV_DISCONNECTED));
				if (ret)
					return ret;
				if (dev->state & DEV_DISCONNECTED)
					return -ENODEV;
			}

			spin_lock_irqsave(&dev->queue_lock, lock_flags);
			f = list_entry(dev->outqueue.next,
				       struct em28xx_frame_t, frame);
			list_del(dev->outqueue.next);
			spin_unlock_irqrestore(&dev->queue_lock, lock_flags);

			f->state = F_UNUSED;
			memcpy(b, &f->buf, sizeof(*b));

			if (f->vma_use_count)
				b->flags |= V4L2_BUF_FLAG_MAPPED;

			return 0;
		}
	default:
		return em28xx_do_ioctl(inode, filp, dev, cmd, arg,
				       em28xx_video_do_ioctl);
	}
	return 0;
}

/*
 * em28xx_v4l2_ioctl()
 * handle v4l2 ioctl the main action happens in em28xx_v4l2_do_ioctl()
 */
static int em28xx_v4l2_ioctl(struct inode *inode, struct file *filp,
			     unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct em28xx *dev = filp->private_data;

	if (down_interruptible(&dev->fileop_lock))
		return -ERESTARTSYS;

	if (dev->state & DEV_DISCONNECTED) {
		em28xx_errdev("v4l2 ioctl: device not present\n");
		up(&dev->fileop_lock);
		return -ENODEV;
	}

	if (dev->state & DEV_MISCONFIGURED) {
		em28xx_errdev
		    ("v4l2 ioctl: device is misconfigured; close and open it again\n");
		up(&dev->fileop_lock);
		return -EIO;
	}

	ret = video_usercopy(inode, filp, cmd, arg, em28xx_video_do_ioctl);

	up(&dev->fileop_lock);

	return ret;
}

static struct file_operations em28xx_v4l_fops = {
	.owner = THIS_MODULE,
	.open = em28xx_v4l2_open,
	.release = em28xx_v4l2_close,
	.ioctl = em28xx_v4l2_ioctl,
	.read = em28xx_v4l2_read,
	.poll = em28xx_v4l2_poll,
	.mmap = em28xx_v4l2_mmap,
	.llseek = no_llseek,
	.compat_ioctl   = v4l_compat_ioctl32,

};

/******************************** usb interface *****************************************/

/*
 * em28xx_init_dev()
 * allocates and inits the device structs, registers i2c bus and v4l device
 */
static int em28xx_init_dev(struct em28xx **devhandle, struct usb_device *udev,
			   int minor, int model)
{
	struct em28xx *dev = *devhandle;
	int retval = -ENOMEM;
	int errCode, i;
	unsigned int maxh, maxw;

	dev->udev = udev;
	dev->model = model;
	init_MUTEX(&dev->lock);
	init_waitqueue_head(&dev->open);

	dev->em28xx_write_regs = em28xx_write_regs;
	dev->em28xx_read_reg = em28xx_read_reg;
	dev->em28xx_read_reg_req_len = em28xx_read_reg_req_len;
	dev->em28xx_write_regs_req = em28xx_write_regs_req;
	dev->em28xx_read_reg_req = em28xx_read_reg_req;
	dev->is_em2800 = em28xx_boards[model].is_em2800;
	dev->has_tuner = em28xx_boards[model].has_tuner;
	dev->has_msp34xx = em28xx_boards[model].has_msp34xx;
	dev->tda9887_conf = em28xx_boards[model].tda9887_conf;
	dev->decoder = em28xx_boards[model].decoder;

	if (tuner >= 0)
		dev->tuner_type = tuner;
	else
		dev->tuner_type = em28xx_boards[model].tuner_type;

	dev->video_inputs = em28xx_boards[model].vchannels;

	for (i = 0; i < TVNORMS; i++)
		if (em28xx_boards[model].norm == tvnorms[i].mode)
			break;
	if (i == TVNORMS)
		i = 0;

	dev->tvnorm = &tvnorms[i];	/* set default norm */

	em28xx_videodbg("tvnorm=%s\n", dev->tvnorm->name);

	maxw = norm_maxw(dev);
	maxh = norm_maxh(dev);

	/* set default image size */
	dev->width = maxw;
	dev->height = maxh;
	dev->interlaced = EM28XX_INTERLACED_DEFAULT;
	dev->field_size = dev->width * dev->height;
	dev->frame_size =
	    dev->interlaced ? dev->field_size << 1 : dev->field_size;
	dev->bytesperline = dev->width * 2;
	dev->hscale = 0;
	dev->vscale = 0;
	dev->ctl_input = 2;

	/* setup video picture settings for saa7113h */
	memset(&dev->vpic, 0, sizeof(dev->vpic));
	dev->vpic.colour = 128 << 8;
	dev->vpic.hue = 128 << 8;
	dev->vpic.brightness = 128 << 8;
	dev->vpic.contrast = 192 << 8;
	dev->vpic.whiteness = 128 << 8;	/* This one isn't used */
	dev->vpic.depth = 16;
	dev->vpic.palette = VIDEO_PALETTE_YUV422;

#ifdef CONFIG_MODULES
	/* request some modules */
	if (dev->decoder == EM28XX_SAA7113 || dev->decoder == EM28XX_SAA7114)
		request_module("saa711x");
	if (dev->decoder == EM28XX_TVP5150)
		request_module("tvp5150");
	if (dev->has_tuner)
		request_module("tuner");
	if (dev->tda9887_conf)
		request_module("tda9887");
#endif
	errCode = em28xx_config(dev);
	if (errCode) {
		em28xx_errdev("error configuring device\n");
		kfree(dev);
		return -ENOMEM;
	}

	down(&dev->lock);
	/* register i2c bus */
	em28xx_i2c_register(dev);

	/* Do board specific init and eeprom reading */
	em28xx_card_setup(dev);

	/* configure the device */
	em28xx_config_i2c(dev);

	up(&dev->lock);

	errCode = em28xx_config(dev);

#ifdef CONFIG_MODULES
	if (dev->has_msp34xx)
		request_module("msp3400");
#endif
	/* allocate and fill v4l2 device struct */
	dev->vdev = video_device_alloc();
	if (NULL == dev->vdev) {
		em28xx_errdev("cannot allocate video_device.\n");
		kfree(dev);
		return -ENOMEM;
	}

	dev->vdev->type = VID_TYPE_CAPTURE;
	if (dev->has_tuner)
		dev->vdev->type |= VID_TYPE_TUNER;
	dev->vdev->hardware = 0;
	dev->vdev->fops = &em28xx_v4l_fops;
	dev->vdev->minor = -1;
	dev->vdev->dev = &dev->udev->dev;
	dev->vdev->release = video_device_release;
	snprintf(dev->vdev->name, sizeof(dev->vdev->name), "%s",
		 "em28xx video");
	list_add_tail(&dev->devlist,&em28xx_devlist);

	/* register v4l2 device */
	down(&dev->lock);
	if ((retval = video_register_device(dev->vdev, VFL_TYPE_GRABBER, -1))) {
		em28xx_errdev("unable to register video device (error=%i).\n",
			      retval);
		up(&dev->lock);
		list_del(&dev->devlist);
		video_device_release(dev->vdev);
		kfree(dev);
		return -ENODEV;
	}
	if (dev->has_msp34xx) {
		/* Send a reset to other chips via gpio */
		em28xx_write_regs_req(dev, 0x00, 0x08, "\xf7", 1);
		udelay(2500);
		em28xx_write_regs_req(dev, 0x00, 0x08, "\xff", 1);
		udelay(2500);

	}
	video_mux(dev, 0);

	up(&dev->lock);

	em28xx_info("V4L2 device registered as /dev/video%d\n",
		    dev->vdev->minor);

	return 0;
}

/*
 * em28xx_usb_probe()
 * checks for supported devices
 */
static int em28xx_usb_probe(struct usb_interface *interface,
			    const struct usb_device_id *id)
{
	const struct usb_endpoint_descriptor *endpoint;
	struct usb_device *udev;
	struct usb_interface *uif;
	struct em28xx *dev = NULL;
	int retval = -ENODEV;
	int model,i,nr,ifnum;

	udev = usb_get_dev(interface_to_usbdev(interface));
	ifnum = interface->altsetting[0].desc.bInterfaceNumber;


	/* Don't register audio interfaces */
	if (interface->altsetting[0].desc.bInterfaceClass == USB_CLASS_AUDIO) {
		em28xx_err(DRIVER_NAME " audio device (%04x:%04x): interface %i, class %i\n",
				udev->descriptor.idVendor,udev->descriptor.idProduct,
				ifnum,
				interface->altsetting[0].desc.bInterfaceClass);
		return -ENODEV;
	}

	em28xx_err(DRIVER_NAME " new video device (%04x:%04x): interface %i, class %i\n",
			udev->descriptor.idVendor,udev->descriptor.idProduct,
			ifnum,
			interface->altsetting[0].desc.bInterfaceClass);

	endpoint = &interface->cur_altsetting->endpoint[1].desc;

	/* check if the the device has the iso in endpoint at the correct place */
	if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) !=
	    USB_ENDPOINT_XFER_ISOC) {
		em28xx_err(DRIVER_NAME " probing error: endpoint is non-ISO endpoint!\n");
		return -ENODEV;
	}
	if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT) {
		em28xx_err(DRIVER_NAME " probing error: endpoint is ISO OUT endpoint!\n");
		return -ENODEV;
	}

	model=id->driver_info;
	nr=interface->minor;

	if (nr>EM28XX_MAXBOARDS) {
		printk (DRIVER_NAME ": Supports only %i em28xx boards.\n",EM28XX_MAXBOARDS);
		return -ENOMEM;
	}

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (dev == NULL) {
		em28xx_err(DRIVER_NAME ": out of memory!\n");
		return -ENOMEM;
	}

	/* compute alternate max packet sizes */
	uif = udev->actconfig->interface[0];

	dev->num_alt=uif->num_altsetting;
	printk(DRIVER_NAME ": Alternate settings: %i\n",dev->num_alt);
//	dev->alt_max_pkt_size = kmalloc(sizeof(*dev->alt_max_pkt_size)*
	dev->alt_max_pkt_size = kmalloc(32*
						dev->num_alt,GFP_KERNEL);
	if (dev->alt_max_pkt_size == NULL) {
		em28xx_err(DRIVER_NAME ": out of memory!\n");
		return -ENOMEM;
	}

	for (i = 0; i < dev->num_alt ; i++) {
		u16 tmp = le16_to_cpu(uif->altsetting[i].endpoint[1].desc.
							wMaxPacketSize);
		dev->alt_max_pkt_size[i] =
		    (tmp & 0x07ff) * (((tmp & 0x1800) >> 11) + 1);
		printk(DRIVER_NAME ": Alternate setting %i, max size= %i\n",i,
							dev->alt_max_pkt_size[i]);
	}

	snprintf(dev->name, 29, "em28xx #%d", nr);

	if ((card[nr]>=0)&&(card[nr]<em28xx_bcount))
		model=card[nr];

	if ((model==EM2800_BOARD_UNKNOWN)||(model==EM2820_BOARD_UNKNOWN)) {
		printk( "%s: Your board has no eeprom inside it and thus can't\n"
			"%s: be autodetected.  Please pass card=<n> insmod option to\n"
			"%s: workaround that.  Redirect complaints to the vendor of\n"
			"%s: the TV card.  Best regards,\n"
			"%s:         -- tux\n",
			dev->name,dev->name,dev->name,dev->name,dev->name);
		printk("%s: Here is a list of valid choices for the card=<n> insmod option:\n",
			dev->name);
		for (i = 0; i < em28xx_bcount; i++) {
			printk("%s:    card=%d -> %s\n",
				dev->name, i, em28xx_boards[i].name);
		}
	}

	/* allocate device struct */
	retval = em28xx_init_dev(&dev, udev, nr, model);
	if (retval)
		return retval;

	em28xx_info("Found %s\n", em28xx_boards[model].name);

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);
	return 0;
}

/*
 * em28xx_usb_disconnect()
 * called when the device gets diconencted
 * video device will be unregistered on v4l2_close in case it is still open
 */
static void em28xx_usb_disconnect(struct usb_interface *interface)
{
	struct em28xx *dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

/*FIXME: IR should be disconnected */

	if (!dev)
		return;


	down_write(&em28xx_disconnect);

	down(&dev->lock);

	em28xx_info("disconnecting %s\n", dev->vdev->name);

	wake_up_interruptible_all(&dev->open);

	if (dev->users) {
		em28xx_warn
		    ("device /dev/video%d is open! Deregistration and memory "
		     "deallocation are deferred on close.\n", dev->vdev->minor);
		dev->state |= DEV_MISCONFIGURED;
		em28xx_uninit_isoc(dev);
		dev->state |= DEV_DISCONNECTED;
		wake_up_interruptible(&dev->wait_frame);
		wake_up_interruptible(&dev->wait_stream);
	} else {
		dev->state |= DEV_DISCONNECTED;
		em28xx_release_resources(dev);
	}

	up(&dev->lock);

	if (!dev->users) {
		kfree(dev->alt_max_pkt_size);
		kfree(dev);
	}

	up_write(&em28xx_disconnect);
}

static struct usb_driver em28xx_usb_driver = {
	.name = "em28xx",
	.probe = em28xx_usb_probe,
	.disconnect = em28xx_usb_disconnect,
	.id_table = em28xx_id_table,
};

static int __init em28xx_module_init(void)
{
	int result;

	printk(KERN_INFO DRIVER_NAME " v4l2 driver version %d.%d.%d loaded\n",
	       (EM28XX_VERSION_CODE >> 16) & 0xff,
	       (EM28XX_VERSION_CODE >> 8) & 0xff, EM28XX_VERSION_CODE & 0xff);
#ifdef SNAPSHOT
	printk(KERN_INFO DRIVER_NAME " snapshot date %04d-%02d-%02d\n",
	       SNAPSHOT / 10000, (SNAPSHOT / 100) % 100, SNAPSHOT % 100);
#endif

	/* register this driver with the USB subsystem */
	result = usb_register(&em28xx_usb_driver);
	if (result)
		em28xx_err(DRIVER_NAME
			   " usb_register failed. Error number %d.\n", result);

	return result;
}

static void __exit em28xx_module_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&em28xx_usb_driver);
}

module_init(em28xx_module_init);
module_exit(em28xx_module_exit);
