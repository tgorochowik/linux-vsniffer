/*
 * Video Sniffer
 *
 * Copyright (C) 2015 Antmicro Ltd.
 *
 * Author(s): Tomasz Gorochowik <tgorochowik@antmicro.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/completion.h>

#include <linux/fs.h>
#include <asm/uaccess.h>

#include <linux/amba/xilinx_dma.h>
#include <linux/dmaengine.h>

/* Video 4 Linux */
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-event.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

#include "video-sniffer.h"

/* Global variables for chrdev */
static int vsniff_chrdev_is_open;
static struct vsniff_private_data *private;

static int vsniff_chrdev_open(struct inode *inode, struct file *file)
{
	if (vsniff_chrdev_is_open)
		return -EBUSY;

	vsniff_chrdev_is_open++;
	try_module_get(THIS_MODULE);

	return 0;
}

static int vsniff_chrdev_release(struct inode *inode, struct file *file)
{
	vsniff_chrdev_is_open--;
	module_put(THIS_MODULE);

	return 0;
}

static void vsniff_chrdev_dma_transfer_done(void *arg)
{
	complete((struct completion*)arg);
}

static ssize_t vsniff_chrdev_read(struct file *file, char *buffer,
				  size_t length, loff_t *offset)
{
	struct dma_interleaved_template *xt;
	struct dma_async_tx_descriptor *desc;
	struct completion dma_transfer_complete;
	dma_cookie_t cookie;
	uint32_t frame_size;

	/* Calculate frame size */
	frame_size = private->regs->res_y *
		private->regs->res_x *
		VSNIFF_BPP;

	/* Check limits */
	if (*offset >= frame_size)
		*offset = frame_size;

	if ((*offset + length) >= frame_size)
		length = (frame_size - *offset);

	/* Check if there is anything to send */
	if (!length)
		return 0;

	/* New DMA transfer if it is the first chunk */
	if (*offset == 0) {
		xt = kzalloc(sizeof(struct dma_async_tx_descriptor) +
			     sizeof(struct data_chunk), GFP_KERNEL);

		xt->dst_start = private->buffer_phys;
		xt->src_inc = false;
		xt->dst_inc = true;
		xt->src_sgl = false;
		xt->dst_sgl = true;
		xt->frame_size = 1;
		xt->numf = private->regs->res_y;
		xt->sgl[0].size = private->regs->res_x * VSNIFF_BPP;
		xt->sgl[0].icg = 0;
		xt->dir = DMA_DEV_TO_MEM;

		desc = dmaengine_prep_interleaved_dma(private->dma, xt,
						      DMA_PREP_INTERRUPT);
		kfree(xt);
		if (!desc) {
			printk(KERN_ERR "Internal VDMA error\n");
			return -EIO;
		}

		/* Register dma callback */
		desc->callback = vsniff_chrdev_dma_transfer_done;

		/* Prepare completion struct */
		init_completion(&dma_transfer_complete);

		/* Register callback param */
		desc->callback_param = &dma_transfer_complete;

		/* Submit the prepared transfer */
		cookie = dmaengine_submit(desc);
		if (cookie < 0) {
			printk(KERN_ERR "Internal VDMA error \n");
			return -EIO;
		}

		/* Start internal transfer */
		dma_async_issue_pending(private->dma);

		/* Wait until the transfer is done */
		if (!wait_for_completion_timeout(&dma_transfer_complete,
						 msecs_to_jiffies(2000)))
			return -ETIMEDOUT;

		/* Terminate transfer */
		dmaengine_terminate_all(private->dma);
	}

	/* Copy the data to user */
	if (copy_to_user(buffer, (private->buffer_virt + *offset), length))
		return -EFAULT;

	/* Update the reading offset */
	*offset += length;

	return length;
}

static long vsniff_chrdev_ioctl(struct file *file,
				unsigned int cmd,
				unsigned long arg)
{
	uint32_t buf = 0;
	switch(cmd) {
	case VSNIFF_SETMODE_RGB:
		/* Change the mode on fpga */
		private->regs->mode = VSNIFF_REG_MODE_RGB;
		break;
	case VSNIFF_SETMODE_TMDS:
		/* Change the mode on fpga */
		private->regs->mode = VSNIFF_REG_MODE_TMDS;
		break;
	case VSNIFF_GETRES:
		/* Width on the MSB half, height on the LSB half */
		buf = (private->regs->res_x & 0xffff) << 16;
		buf |= (private->regs->res_y & 0xffff);

		copy_to_user((uint32_t*)arg, &buf, sizeof(buf));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* File operations struct for the chrdev */
struct file_operations vsniff_chrdev_fops = {
	.open = vsniff_chrdev_open,
	.release = vsniff_chrdev_release,
	.read = vsniff_chrdev_read,
	.unlocked_ioctl = vsniff_chrdev_ioctl
};

static struct vsniff_v4l2_buffer *vb2_buf_to_vsniff_buf(struct vb2_buffer *vb)
{
	return container_of(vb, struct vsniff_v4l2_buffer, vb);
}

static void vsniff_v4l2_dma_transfer_done(void *arg)
{
	struct vsniff_v4l2_buffer *buf = arg;
	struct vsniff_v4l2_private_data *priv;
	unsigned long flags;

	priv = vb2_get_drv_priv(buf->vb.vb2_queue);
	spin_lock_irqsave(&priv->spinlock, flags);
	list_del(&buf->head);
	spin_unlock_irqrestore(&priv->spinlock, flags);

	v4l2_get_timestamp(&buf->vb.v4l2_buf.timestamp);
	vb2_buffer_done(&buf->vb, VB2_BUF_STATE_DONE);
}

/* File operations struct for the v4l2 dev */
static const struct v4l2_file_operations vsniff_v4l2_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static int vsniff_v4l2_queue_setup(struct vb2_queue *q,
				   const struct v4l2_format *fmt,
				   unsigned int *num_buffers,
				   unsigned int *num_planes,
				   unsigned int sizes[],
				   void *alloc_ctxs[])
{
	struct vsniff_v4l2_private_data *priv = vb2_get_drv_priv(q);

	if (*num_buffers < 1)
		*num_buffers = 1;
	*num_planes = 1;

	if (fmt) {
		printk(KERN_ERR "We have format and we're going to use it\n");
		sizes[0] = fmt->fmt.pix.sizeimage;
	} else {
		printk(KERN_ERR "Format is taken from channel settings\n");
		sizes[0] = private->regs->res_x *
			private->regs->res_y * VSNIFF_BPP;
	}

	if (sizes[0] == 0)
		return -EINVAL;

	alloc_ctxs[0] = priv->alloc_ctx;
	return 0;
}

static int vsniff_v4l2_buf_prepare(struct vb2_buffer *vb)
{
	unsigned size;

	size = private->regs->res_x * private->regs->res_y * VSNIFF_BPP;
	if (vb2_plane_size(vb, 0) < size) {
		printk(KERN_ERR" data will not fit the plane (%lu < %u)\n",
		       vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}
	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void vsniff_v4l2_buf_queue(struct vb2_buffer *vb)
{
	unsigned long size;
	dma_addr_t addr;
	unsigned long flags;

	struct dma_interleaved_template *xt;
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;

	struct vsniff_v4l2_buffer *buf = vb2_buf_to_vsniff_buf(vb);

	addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	size = vb2_get_plane_payload(vb, 0);

	xt = kzalloc(sizeof(struct dma_async_tx_descriptor) +
		     sizeof(struct data_chunk), GFP_KERNEL);

	xt->dst_start = addr;
	xt->src_inc = false;
	xt->dst_inc = true;
	xt->src_sgl = false;
	xt->dst_sgl = true;
	xt->frame_size = 1;
	xt->numf = private->regs->res_y;
	xt->sgl[0].size = private->regs->res_x * VSNIFF_BPP;
	xt->sgl[0].icg = 0;
	xt->dir = DMA_DEV_TO_MEM;

	desc = dmaengine_prep_interleaved_dma(private->dma, xt,
					      DMA_PREP_INTERRUPT);
	kfree(xt);
	if (!desc) {
		printk(KERN_ERR "Internal VDMA error\n");
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	/* Register dma callback */
	desc->callback = vsniff_v4l2_dma_transfer_done;
	desc->callback_param = buf;

	/* Submit the prepared transfer */
	cookie = dmaengine_submit(desc);
	if (cookie < 0) {
		printk(KERN_ERR "Internal VDMA error\n");
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	spin_lock_irqsave(&private->v4l2.spinlock, flags);
	list_add_tail(&buf->head, &private->v4l2.queued_buffers);
	spin_unlock_irqrestore(&private->v4l2.spinlock, flags);

	if (vb2_is_streaming(vb->vb2_queue)) {
		dma_async_issue_pending(private->v4l2.dma);
	}
}

static int vsniff_v4l2_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vsniff_v4l2_private_data *priv = vb2_get_drv_priv(q);
	dma_async_issue_pending(priv->dma);
	return 0;
}

static void vsniff_v4l2_stop_streaming(struct vb2_queue *q)
{
	struct vsniff_v4l2_private_data *priv = vb2_get_drv_priv(q);
	struct vsniff_v4l2_buffer *buf;
	unsigned long flags;

	dmaengine_terminate_all(priv->dma);

	spin_lock_irqsave(&priv->spinlock, flags);
	list_for_each_entry(buf, &priv->queued_buffers, head)
		vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
	INIT_LIST_HEAD(&priv->queued_buffers);
	spin_unlock_irqrestore(&priv->spinlock, flags);

	vb2_wait_for_all_buffers(q);
}

/* Queue operations struct for the v4l2 dev */
static const struct vb2_ops vsniff_v4l2_qops = {
	.queue_setup = vsniff_v4l2_queue_setup,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,

	.buf_prepare = vsniff_v4l2_buf_prepare,
	.buf_queue = vsniff_v4l2_buf_queue,
	.start_streaming = vsniff_v4l2_start_streaming,
	.stop_streaming = vsniff_v4l2_stop_streaming,
};

static int vsniff_v4l2_log_status(struct file *file, void *priv)
{
	return 0;
}

static int vsniff_v4l2_querycap(struct file *file, void *priv_fh,
				struct v4l2_capability *vcap)
{
	strlcpy(vcap->driver, "vsniff_v4l2_streamer", sizeof(vcap->driver));
	strlcpy(vcap->card, "vsniff_v4l2_streamer", sizeof(vcap->card));
	vcap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	vcap->version = KERNEL_VERSION(0, 0, 5);
	return 0;
}

static int vsniff_v4l2_streamon(struct file *file, void *priv_fh,
				enum v4l2_buf_type buffer_type)
{
	if (buffer_type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return vb2_streamon(&private->v4l2.queue, buffer_type);
}

static int vsniff_v4l2_streamoff(struct file *file, void *priv_fh,
				 enum v4l2_buf_type buffer_type)
{
	return 0;
}

static int vsniff_v4l2_enum_fmt_vid_cap(struct file *file, void *priv_fh,
					struct v4l2_fmtdesc *f)
{
	if(f->index == 0) {
		strlcpy(f->description, "RGBA888 + 8b padding", sizeof(f->description));
		f->pixelformat = V4L2_PIX_FMT_ARGB32;
	}
	else return -EINVAL;
	printk(KERN_ERR "index[%d] = %s \n", f->index, f->description);

	return 0;
}

static int vsniff_v4l2_g_fmt_vid_cap(struct file *file, void *priv_fh,
				     struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;

	pix->width = private->regs->res_x;
	pix->height = private->regs->res_y;
	pix->bytesperline = private->regs->res_x * VSNIFF_BPP;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->pixelformat = V4L2_PIX_FMT_ARGB32;
	pix->sizeimage =  pix->bytesperline * pix->height;
	pix->field = V4L2_FIELD_NONE;

	return 0;
}

static int vsniff_v4l2_try_fmt_vid_cap(struct file *file, void *priv_fh,
				       struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;

	v4l_bound_align_image(&pix->width, 176, private->regs->res_x, 0,
			      &pix->height, 144, private->regs->res_y, 0, 0);
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->pixelformat = V4L2_PIX_FMT_ARGB32;
	pix->bytesperline = pix->width * VSNIFF_BPP;
	pix->sizeimage =  pix->bytesperline * pix->height;
	pix->field = V4L2_FIELD_NONE;
	pix->priv = 0;
	return 0;
}

static int vsniff_v4l2_s_fmt_vid_cap(struct file *file, void *priv_fh,
				     struct v4l2_format *f)
{
	return 0;
}

static int vsniff_v4l2_enum_input(struct file *file, void *priv_fh,
				  struct v4l2_input *inp)
{
	if (inp->index == 0) {
		snprintf(inp->name, sizeof(inp->name), "Video Sniffer Streaming");
		inp->type = V4L2_INPUT_TYPE_CAMERA;
		inp->std = V4L2_STD_UNKNOWN;
	} else return -EINVAL;
	return 0;
}

static int vsniff_v4l2_g_input(struct file *file, void *priv_fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vsniff_v4l2_s_input(struct file *file, void *priv_fh, unsigned int i)
{
	return 0;
}

/* Video operations struct for the v4l2 dev */
static const struct v4l2_ioctl_ops vsniff_v4l2_ioctl_ops = {
	.vidioc_querycap = vsniff_v4l2_querycap,
	.vidioc_log_status = vsniff_v4l2_log_status,
	.vidioc_streamon = vsniff_v4l2_streamon,
	.vidioc_streamoff = vsniff_v4l2_streamoff,
	.vidioc_enum_input = vsniff_v4l2_enum_input,
	.vidioc_g_input = vsniff_v4l2_g_input,
	.vidioc_s_input = vsniff_v4l2_s_input,
	.vidioc_enum_fmt_vid_cap = vsniff_v4l2_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = vsniff_v4l2_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = vsniff_v4l2_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = vsniff_v4l2_try_fmt_vid_cap,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
};

static int vsniff_probe(struct platform_device *pdev)
{
	struct resource *resource;
	struct video_device *vdev;
	int result;

	/* Allocate memory for private data */
	private = devm_kzalloc(&pdev->dev, sizeof(*private), GFP_KERNEL);
	if (!private) {
		printk(KERN_ERR "Memory allocation failure (private data)\n");
		return -ENOMEM;
	}

	/* Get the resource */
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	private->regs = devm_ioremap_resource(&pdev->dev, resource);

	if (IS_ERR(private->regs)) {
		printk(KERN_ERR "IO mapping failed\n");
		return PTR_ERR(private->regs);
	}

	/* Set the sniffer to RGB mode by default */
	private->regs->mode = VSNIFF_REG_MODE_RGB;

	/* Allocate memory for the buffer */
	private->buffer_virt = dmam_alloc_coherent(&pdev->dev,
						   VSNIFF_DMA_MEM_SIZE * 2,
						   &(private->buffer_phys),
						   GFP_DMA | GFP_KERNEL);

	if (!private->buffer_virt) {
		printk(KERN_ERR "Memory allocation failure (dma buffer)\n");
		return -ENOMEM;
	}

	/* Request DMA channel */
	private->dma = dma_request_slave_channel(&pdev->dev, "captured-video");

	/* Xilinx DMA driver might be not initialized yet - defer if failed */
	if (private->dma == NULL)
		return -EPROBE_DEFER;

	/* Initialize chrdev driver */
	vsniff_chrdev_is_open = 0;

	/* Attempt to alloc char device region */
	result = alloc_chrdev_region(&(private->chrdev.dev), 0, 1,
				     VSNIFF_CHRDEV_NAME);
	if (result < 0) {
		printk(KERN_ERR "Failed to create chrdev region\n");
		return result;
	}

	/* Attempt to alloc mem for cdev */
	private->chrdev.cdev = cdev_alloc();
	if (!private->chrdev.cdev) {
		printk(KERN_ERR "Memory allocation failure (cdev)\n");
		unregister_chrdev_region(private->chrdev.dev, 1);
		return -ENOMEM;
	}

	/* Attempt to create cdev */
	cdev_init(private->chrdev.cdev, &vsniff_chrdev_fops);
	result = cdev_add(private->chrdev.cdev, private->chrdev.dev, 1);
	if (result < 0) {
		printk(KERN_ERR "Failed to create chrdev cdev\n");
		unregister_chrdev_region(private->chrdev.dev, 1);
		return result;
	}

	/* Attempt to create chrdev class */
	private->chrdev.cl = class_create(THIS_MODULE, VSNIFF_CHRDEV_NAME);
	if (!private->chrdev.cl) {
		printk(KERN_ERR "Failed to create chrdev class\n");
		cdev_del(private->chrdev.cdev);
		unregister_chrdev_region(private->chrdev.dev, 1);
		return -EEXIST;
	}

	/* Create the actual device */
	if (!device_create(private->chrdev.cl, NULL,
			   private->chrdev.dev, NULL,
			   VSNIFF_CHRDEV_NAME"-%d",
			   MINOR(private->chrdev.dev))) {
		printk(KERN_ERR "Failed to create chrdev\n");
		class_destroy(private->chrdev.cl);
		cdev_del(private->chrdev.cdev);
		unregister_chrdev_region(private->chrdev.dev, 1);
		return -EINVAL;
	}

	/* Iinitalize v4l2 driver */
	result = v4l2_device_register(&pdev->dev, &private->v4l2.dev);
	if (result) {
		printk(KERN_ERR "Failed to register v4l2 dev: %d\n", result);
		return -result;
	}

	/* Give the same DMA pointer to v4l2 driver */
	private->v4l2.dma = private->dma;

	private->v4l2.alloc_ctx = vb2_dma_contig_init_ctx(&pdev->dev);
	if (IS_ERR(private->v4l2.alloc_ctx)) {
		result = PTR_ERR(private->v4l2.alloc_ctx);
		printk(KERN_ERR "Failed to init ctx\n");
		return -result;
	}

	vdev = &private->v4l2.vdev;
	mutex_init(&private->v4l2.lock);

	snprintf(vdev->name, sizeof(vdev->name),
		 "%s", private->v4l2.dev.name);

	vdev->v4l2_dev = &private->v4l2.dev;
	vdev->fops = &vsniff_v4l2_fops;
	vdev->release = video_device_release_empty;
	vdev->ctrl_handler = NULL;
	vdev->lock = &private->v4l2.lock;
	vdev->queue = &private->v4l2.queue;
	vdev->queue->lock = &private->v4l2.lock;

	INIT_LIST_HEAD(&private->v4l2.queued_buffers);

	vdev->queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vdev->queue->io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	vdev->queue->drv_priv = &private->v4l2;
	vdev->queue->buf_struct_size = sizeof(struct vsniff_v4l2_private_data);
	vdev->queue->ops = &vsniff_v4l2_qops;
	vdev->queue->mem_ops = &vb2_dma_contig_memops;
	vdev->queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	result = vb2_queue_init(vdev->queue);
	if (result)
		return -result;

	vdev->ioctl_ops = &vsniff_v4l2_ioctl_ops;

	result = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (result)
		return -result;

	/* Set the driver data */
	platform_set_drvdata(pdev, private);

	return 0;
}

static int vsniff_remove(struct platform_device *pdev)
{
	struct vsniff_private_data *private;

	/* Get private data */
	private = (struct vsniff_private_data*)pdev->dev.driver_data;

	/* Release dma */
	if (private->dma)
		dma_release_channel(private->dma);

	/* Unregister chrdev */
	device_destroy(private->chrdev.cl, private->chrdev.dev);
	class_destroy(private->chrdev.cl);
	cdev_del(private->chrdev.cdev);
	unregister_chrdev_region(private->chrdev.dev, 1);

	/* Unregister v4l2 dev */
	video_unregister_device(&private->v4l2.vdev);
	v4l2_device_unregister(&private->v4l2.dev);

	return 0;
}

/* Match table for of_platform binding */
static struct of_device_id vsniff_of_match[] = {
	{ .compatible = "vsniff,video-sniffer-data", },
	{}
};

MODULE_DEVICE_TABLE(of, vsniff_of_match);

static struct platform_driver vsniff_pdev_drv = {
	.probe  = vsniff_probe,
	.remove = vsniff_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name  = "Video Sniffer",
		.of_match_table = vsniff_of_match,
	},
};


static int __init vsniff_init(void)
{
	return platform_driver_register(&vsniff_pdev_drv);
}

static void __exit vsniff_exit(void)
{
	platform_driver_unregister(&vsniff_pdev_drv);
}

module_init(vsniff_init);
module_exit(vsniff_exit);

MODULE_DESCRIPTION("Video Sniffer");
MODULE_AUTHOR("Tomasz Gorochowik");
MODULE_LICENSE("GPL v2");
