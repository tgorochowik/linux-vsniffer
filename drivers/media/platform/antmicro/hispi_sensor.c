/*
 * Antmicro HiSpi sensor intefrace
 *
 * Copyright (C) 2015 Antmicro Ltd.
 *
 * Author(s): 
 *	Karol Gugala <kgugala@antmicro.com>
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
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/delay.h>

#include <linux/fs.h>
#include <asm/uaccess.h>

#include <linux/amba/xilinx_dma.h>
#include <linux/dmaengine.h>

#include <asm/io.h>

#include <uapi/video/antmicro/hispi_sensor.h>

/* V4L */
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-event.h>
#include <media/v4l2-of.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

#include "hispi_sensor.h"

//#define ENTER() printk("Entering %s @ %d \n", __func__, __LINE__)
#define ENTER()

static unsigned int hispi_read_reg(struct hispi_priv_data *priv, unsigned int offset)
{
	volatile unsigned int *reg = (unsigned int *)(priv->base + offset);
	return *reg;
}

static void hispi_write_reg(struct hispi_priv_data *priv, unsigned int offset, unsigned int value)
{
	volatile unsigned int *reg = (unsigned int *)(priv->base + offset);
	*reg = value;
}

static struct hispi_buffer *vb2_buf_to_hispi_buf(struct vb2_buffer *vb)
{
        return container_of(vb, struct hispi_buffer, vb);
}

static const struct v4l2_file_operations hispi_fops = { 
        .owner = THIS_MODULE,
        .open = v4l2_fh_open,
        .release = vb2_fop_release,
        .unlocked_ioctl = video_ioctl2,
        .read = vb2_fop_read,
        .poll = vb2_fop_poll,
        .mmap = vb2_fop_mmap,
};

static int hispi_queue_setup(struct vb2_queue *q,
        const struct v4l2_format *fmt, unsigned int *num_buffers,
        unsigned int *num_planes, unsigned int sizes[], void *alloc_ctxs[])

{
	struct sensor_channel *channel = vb2_get_drv_priv(q);

	ENTER();
	if (*num_buffers < 1)
                *num_buffers = 1;
        *num_planes = 1;

        if (fmt) {
		printk(KERN_ERR"We have format and we're going to use it \n");
                sizes[0] = fmt->fmt.pix.sizeimage;
	}
        else {
		printk(KERN_ERR"Format is taken from channel settings \n");
                sizes[0] = channel->video_x * channel->video_y * channel->bpp;
	}
        if (sizes[0] == 0)
                return -EINVAL;

        alloc_ctxs[0] = channel->alloc_ctx;
	return 0;
}

static int hispi_buf_prepare(struct vb2_buffer *vb)
{
	struct sensor_channel *channel = vb2_get_drv_priv(vb->vb2_queue);
	unsigned size;

	ENTER();
	size = channel->video_x * channel->video_y * channel->bpp;
	if (vb2_plane_size(vb, 0) < size) {
		printk(KERN_ERR"data will not fit the plane (%lu < %u)\n",
						vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}
	vb2_set_plane_payload(vb, 0, size);
        return 0;
}

static void setup_internal_transfer(struct sensor_channel *channel)
{
	struct dma_async_tx_descriptor *desc;
	struct dma_interleaved_template *xt;
	dma_addr_t internal_dst;
	dma_cookie_t cookie;

	long size = channel->video_x * channel->video_y * channel->bpp;

	internal_dst = channel->internal_buffer_base + channel->current_write_buffer*size;

	xt = kzalloc(sizeof(struct dma_async_tx_descriptor) +
			sizeof(struct data_chunk), GFP_KERNEL);

	xt->dst_start = internal_dst;
	xt->src_inc = false;
	xt->dst_inc = true;
	xt->src_sgl = false;
	xt->dst_sgl = true;
	xt->frame_size = 1;
	xt->numf = channel->video_y;
	xt->sgl[0].size = channel->video_x * channel->bpp;
	xt->sgl[0].icg = 0;
	xt->dir = DMA_DEV_TO_MEM;

	//printk(KERN_ERR"Internal VDMA addr is: 0x%08x, size = %ld\n", internal_dst, size);

	desc = dmaengine_prep_interleaved_dma(channel->dma, xt, DMA_PREP_INTERRUPT);
	kfree(xt);
	if (!desc) {
		printk(KERN_ERR"vdma desc prepare error \n");
		//vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	desc->callback = hispi_internal_vdma_done;
	desc->callback_param = channel;

	cookie = dmaengine_submit(desc);
	if (cookie < 0) {
		printk(KERN_ERR"vdma engine submit error \n");
		//vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}
	/* start internal transfer */
	dma_async_issue_pending(channel->dma);
}

static inline uint32_t get_unused_buffer(struct sensor_channel *channel)
{
	uint32_t i;

	for(i=0; i<3; i++)
		if( (channel->current_read_buffer != i) &&
				(channel->current_write_buffer != i))
			break;
	return i;
}

static void hispi_internal_vdma_done(void *arg)
{
	struct sensor_channel *channel = arg;

	mutex_lock(&channel->internal_lock);
	/* find unused buffer */
	channel->current_write_buffer = get_unused_buffer(channel);
	channel->flip_buffers = 1;
	mutex_unlock(&channel->internal_lock);
	/* setup next transfer */
	setup_internal_transfer(channel);
	ENTER();
}

static void hispi_dma_done(void *arg)
{
	//struct vb2_buffer *vb = (struct vb2_buffer*)(arg);
	struct hispi_buffer *buf = arg;
	struct sensor_channel *channel = vb2_get_drv_priv(buf->vb.vb2_queue);
	unsigned long flags;

	ENTER();
	/* switch the read bufferd buffers*/
	mutex_lock(&channel->internal_lock);
	if(channel->flip_buffers) {
		channel->current_read_buffer = get_unused_buffer(channel);
		channel->flip_buffers = 0;
	}
	mutex_unlock(&channel->internal_lock);

	spin_lock_irqsave(&channel->spinlock, flags);
	list_del(&buf->head);
	spin_unlock_irqrestore(&channel->spinlock, flags);

	v4l2_get_timestamp(&buf->vb.v4l2_buf.timestamp);
        vb2_buffer_done(&buf->vb, VB2_BUF_STATE_DONE);

	//printk(KERN_ERR"%s: new read buffer is %d \n", __func__, channel->current_read_buffer);
}

static void hispi_buf_queue(struct vb2_buffer *vb)
{
	unsigned long size;
	unsigned long flags;
	struct dma_async_tx_descriptor *desc;
	struct dma_interleaved_template *xt;
	//struct xilinx_vdma_config xconf;

	dma_addr_t addr;
	dma_addr_t internal_dst;
	dma_addr_t internal_src;
	dma_cookie_t cookie;

	struct sensor_channel *channel = vb2_get_drv_priv(vb->vb2_queue);
	struct hispi_buffer *buf = vb2_buf_to_hispi_buf(vb);

	ENTER();

	addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	size = vb2_get_plane_payload(vb, 0);

	/*xt = kzalloc(sizeof(struct dma_async_tx_descriptor) +
                                sizeof(struct data_chunk), GFP_KERNEL);*/
        /*if (!xt) {
                vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
                return;
        }*/

	/* If internal transfers are not running */
	if(!channel->internal_streaming) {
		/* set internal video transfers */
		channel->internal_streaming = 1;
		internal_dst = channel->internal_buffer_base + channel->current_write_buffer*size;

		xt = kzalloc(sizeof(struct dma_async_tx_descriptor) +
                                sizeof(struct data_chunk), GFP_KERNEL);

		xt->dst_start = internal_dst;
		xt->src_inc = false;
		xt->dst_inc = true;
		xt->src_sgl = false;
		xt->dst_sgl = true;
		xt->frame_size = 1;
		xt->numf = channel->video_y;
		xt->sgl[0].size = channel->video_x * channel->bpp;
		xt->sgl[0].icg = 0;
		xt->dir = DMA_DEV_TO_MEM;

		printk(KERN_ERR"Internal VDMA addr is: 0x%08x, size = %ld\n", internal_dst, size);

		desc = dmaengine_prep_interleaved_dma(channel->dma, xt, DMA_PREP_INTERRUPT);
		kfree(xt);
		if (!desc) {
			printk(KERN_ERR"vdma desc prepare error \n");
                	//vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
	                return;
	        }

		desc->callback = hispi_internal_vdma_done;
	        desc->callback_param = channel;

		cookie = dmaengine_submit(desc);
		if (cookie < 0) {
			printk(KERN_ERR"vdma engine submit error \n");
			//vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
			return;
		}
		/* start internal transfer */
		dma_async_issue_pending(channel->dma);
	}

	/* prepare DMA transfer from video memory to RAM */
	internal_src = channel->internal_buffer_base + channel->current_read_buffer*size;
	desc=channel->preview_dma->device->device_prep_dma_memcpy(channel->preview_dma, addr, internal_src, size, DMA_CTRL_ACK | DMA_PREP_INTERRUPT);

	if(!desc) {

		printk(KERN_ERR"dma desc prepare error \n");
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	desc->callback = hispi_dma_done;
	desc->callback_param = buf;

	cookie = desc->tx_submit(desc);
	if(cookie < 0) {
		printk(KERN_ERR"dma engine submit error \n");
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	spin_lock_irqsave(&channel->spinlock, flags);
        list_add_tail(&buf->head, &channel->queued_buffers);
        spin_unlock_irqrestore(&channel->spinlock, flags);

	if (vb2_is_streaming(vb->vb2_queue)) {
		//printk(KERN_ERR"We're streaming ... \n");
                dma_async_issue_pending(channel->preview_dma);
	}

}

static int hispi_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sensor_channel *channel = vb2_get_drv_priv(q);
	ENTER();
	dma_async_issue_pending(channel->preview_dma);
	return 0;
}

static void hispi_stop_streaming(struct vb2_queue *q)
{
	struct sensor_channel *channel = vb2_get_drv_priv(q);
	struct hispi_buffer *buf;
	unsigned long flags;

	ENTER();
	dmaengine_terminate_all(channel->preview_dma);
	dmaengine_terminate_all(channel->dma);
	channel->internal_streaming = 0;

	spin_lock_irqsave(&channel->spinlock, flags);

        list_for_each_entry(buf, &channel->queued_buffers, head)
                vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
        INIT_LIST_HEAD(&channel->queued_buffers);

        spin_unlock_irqrestore(&channel->spinlock, flags);

	vb2_wait_for_all_buffers(q);
}

static const struct vb2_ops hispi_qops = {
        .queue_setup = hispi_queue_setup,
        .wait_prepare = vb2_ops_wait_prepare,
        .wait_finish = vb2_ops_wait_finish,

        .buf_prepare = hispi_buf_prepare,
        .buf_queue = hispi_buf_queue,
        .start_streaming = hispi_start_streaming,
        .stop_streaming = hispi_stop_streaming,
};

static int hispi_log_status(struct file *file, void *priv)
{
	ENTER();
	return 0;
}

static int hispi_querycap(struct file *file, void *priv_fh,
        struct v4l2_capability *vcap)
{
	ENTER();
	strlcpy(vcap->driver, "hispi_streamer", sizeof(vcap->driver));
        strlcpy(vcap->card, "hispi_streamer", sizeof(vcap->card));
        //snprintf(vcap->bus_info, sizeof(vcap->bus_info), "platform:hispi_streamer");
        //vcap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        vcap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;//cap->device_caps;// | V4L2_CAP_DEVICE_CAPS;
	vcap->version = KERNEL_VERSION(0, 0, 5);
	return 0;
}

static int hispi_streamon(struct file *file, void *priv_fh,
        enum v4l2_buf_type buffer_type)
{
	struct hispi_priv_data *priv = video_drvdata(file);
	//struct v4l2_pix_format *pix = &f->fmt.pix;
	struct sensor_channel *channel = &priv->channel;

	ENTER();
	if (buffer_type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return vb2_streamon(&channel->q, buffer_type);
}

static int hispi_streamoff(struct file *file, void *priv_fh,
        enum v4l2_buf_type buffer_type)
{
	ENTER();
	return 0;
}

static int hispi_enum_fmt_vid_cap(struct file *file, void *priv_fh,
        struct v4l2_fmtdesc *f)
{
	ENTER();
	printk(KERN_ERR"index[%d] = %s \n", f->index, f->description);

	if(f->index == 0) {
		strlcpy(f->description, "BA81", sizeof(f->description));
		f->pixelformat = /*V4L2_PIX_FMT_UYVY;*/V4L2_PIX_FMT_GREY;/*V4L2_PIX_FMT_SBGGR8;*/
	}
	else return -EINVAL;
	return 0;
}

static int hispi_g_fmt_vid_cap(struct file *file, void *priv_fh,
        struct v4l2_format *f)
{
	struct hispi_priv_data *priv = video_drvdata(file);
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct sensor_channel *channel = &priv->channel;

	ENTER();
	pix->width = channel->video_x;
	pix->height = channel->video_y;
	pix->bytesperline = channel->video_x * channel->bpp;
	pix->colorspace = /*V4L2_COLORSPACE_SMPTE170M;*/V4L2_COLORSPACE_SRGB;//V4L2_COLORSPACE_REC709;
	pix->pixelformat = /*V4L2_PIX_FMT_UYVY;*/V4L2_PIX_FMT_GREY;/*V4L2_PIX_FMT_SBGGR8;*/
	pix->sizeimage =  pix->bytesperline * pix->height;
	pix->field = V4L2_FIELD_NONE;

	return 0;
}

static int hispi_try_fmt_vid_cap(struct file *file, void *priv_fh,
        struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct hispi_priv_data *priv = video_drvdata(file);
	struct sensor_channel *channel = &priv->channel;
	ENTER();

	v4l_bound_align_image(&pix->width, 176, MAX_X, 0, &pix->height, 144,
			                MAX_Y, 0, 0);
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->pixelformat = V4L2_PIX_FMT_GREY;
	pix->bytesperline = pix->width * channel->bpp;
	pix->sizeimage =  pix->bytesperline * pix->height;
	pix->field = V4L2_FIELD_NONE;
	pix->priv = 0;
	return 0;
}

static int hispi_s_fmt_vid_cap(struct file *file, void *priv_fh,
        struct v4l2_format *f)
{
	ENTER();
	return 0;
}

static int hispi_enum_input(struct file *file, void *priv_fh,
        struct v4l2_input *inp)
{
	ENTER();

	if (inp->index == 0) {
		snprintf(inp->name, sizeof(inp->name), "HiSpi sensor");
		inp->type = V4L2_INPUT_TYPE_CAMERA;
		inp->std = V4L2_STD_UNKNOWN;
	} else return -EINVAL;
	return 0;
}

static int hispi_g_input(struct file *file, void *priv_fh, unsigned int *i)
{
	ENTER();
	/* set 0 input */
	//TODO: handle two inputs
	*i = 0;
	return 0;
}

static int hispi_s_input(struct file *file, void *priv_fh, unsigned int i)
{
	ENTER();
	return 0;
}

static const struct v4l2_ioctl_ops hispi_ioctl_ops = {
        .vidioc_querycap                = hispi_querycap,
        .vidioc_log_status              = hispi_log_status,
        .vidioc_streamon                = hispi_streamon,
        .vidioc_streamoff               = hispi_streamoff,
        .vidioc_enum_input              = hispi_enum_input,
        .vidioc_g_input                 = hispi_g_input,
        .vidioc_s_input                 = hispi_s_input,
        .vidioc_enum_fmt_vid_cap        = hispi_enum_fmt_vid_cap,
        .vidioc_g_fmt_vid_cap           = hispi_g_fmt_vid_cap,
        .vidioc_s_fmt_vid_cap           = hispi_s_fmt_vid_cap,
        .vidioc_try_fmt_vid_cap         = hispi_try_fmt_vid_cap,

        .vidioc_subscribe_event         = v4l2_ctrl_subscribe_event,
        .vidioc_unsubscribe_event       = v4l2_event_unsubscribe,
        .vidioc_create_bufs             = vb2_ioctl_create_bufs,
        .vidioc_prepare_buf             = vb2_ioctl_prepare_buf,
        .vidioc_reqbufs                 = vb2_ioctl_reqbufs,
        .vidioc_querybuf                = vb2_ioctl_querybuf,
        .vidioc_qbuf                    = vb2_ioctl_qbuf,
        .vidioc_dqbuf                   = vb2_ioctl_dqbuf,
};

static int hispi_register_video_dev(struct hispi_priv_data *private)
{
	struct sensor_channel *channel = &private->channel;
	struct video_device *vdev = &channel->vdev;
	int ret;

	mutex_init(&channel->lock);

	snprintf(vdev->name, sizeof(vdev->name),
                 "%s", private->v4l2_dev.name);

        vdev->v4l2_dev = &private->v4l2_dev;
        vdev->fops = &hispi_fops;
        vdev->release = video_device_release_empty;
	vdev->ctrl_handler = NULL;
	vdev->lock = &channel->lock;
	vdev->queue = &channel->q;
	vdev->queue->lock =  &channel->lock;

	INIT_LIST_HEAD(&channel->queued_buffers);

	vdev->queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vdev->queue->io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	vdev->queue->drv_priv = channel;
	vdev->queue->buf_struct_size = sizeof(struct sensor_channel);
	vdev->queue->ops = &hispi_qops;
	vdev->queue->mem_ops = &vb2_dma_contig_memops;
	vdev->queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

	ret = vb2_queue_init(vdev->queue);
	if (ret)
		return ret;

	vdev->ioctl_ops = &hispi_ioctl_ops;

	return video_register_device(vdev, VFL_TYPE_GRABBER, -1);
}

static int hispi_probe(struct platform_device *pdev)
{
	struct hispi_priv_data *private;
	struct sensor_channel *channel;
	struct resource *res;
	unsigned int reg;
	int ret;
	int err;

	ENTER();
	private = devm_kzalloc(&pdev->dev, sizeof(*private), GFP_KERNEL);
        if (!private) {
                printk(KERN_ERR"Mem alloc for private data failed \n");
                return -ENOMEM;
        }

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	private->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(private->base)) {
		printk(KERN_ERR"IO mapping failed \n");
		return PTR_ERR(private->base);
	}

	/* disable and reset hardware */
	hispi_write_reg(private, CTRL_REG, 0);

	channel = &private->channel;
	private->buffer_virt = devm_kzalloc(&pdev->dev, MAX_X * MAX_Y * (BPP/8), GFP_DMA);

	if (!private->buffer_virt) {
		printk(KERN_ERR"Could not allocate buffer\n");
		return -ENOMEM;
	}
	private->video_buffer = (dma_addr_t)virt_to_phys(private->buffer_virt);
	printk(KERN_ERR"virt = 0x%p, phys = 0x%08x \n", private->buffer_virt, private->video_buffer);

	printk(KERN_ERR"Going to request channel \n");
	private->channel.dma = dma_request_slave_channel(&pdev->dev, "video");
        if (private->channel.dma == NULL)
                return -EPROBE_DEFER;

	printk(KERN_ERR"Going to request preview channel \n");
	private->channel.preview_dma = dma_request_slave_channel(&pdev->dev, "preview");
	if (private->channel.dma == NULL) {
		dma_release_channel(private->channel.dma);
		return -EPROBE_DEFER;
	}

	if( (err = of_property_read_u32(pdev->dev.of_node, "ant,video-mem-base", (u32*)&(channel->internal_buffer_base))) ) return err;
	/* set default values */
	channel->current_write_buffer = 0;
	channel->current_read_buffer = 1;
	channel->internal_streaming = 0;

	channel->video_x = MAX_X;
	channel->video_y = MAX_Y;
	channel->bpp = BPP / 8;

	mutex_init(&channel->internal_lock);

	platform_set_drvdata(pdev, private);

	channel->alloc_ctx = vb2_dma_contig_init_ctx(&pdev->dev);
	if (IS_ERR(channel->alloc_ctx)) {
		ret = PTR_ERR(channel->alloc_ctx);
		printk(KERN_ERR"Failed to init ctx \n");
		return -ret;
	}
	video_set_drvdata(&private->channel.vdev, private);

	ret = v4l2_device_register(&pdev->dev, &private->v4l2_dev);
        if (ret) {
                printk(KERN_ERR"Failed to register card: %d\n", ret);
                return -ret;
        }

	ret = hispi_register_video_dev(private);
	if (ret) {
		printk(KERN_ERR"Failed to register video dev: %d\n", ret);
		return -ret;
	}

	/* enable hardware */
	hispi_write_reg(private, CTRL_REG, ENABLE_BIT);

	msleep(100);
	reg = hispi_read_reg(private, STATUS_REG);
	printk(KERN_ERR"Sensor is %ssynced\n", reg?"":"not ");
	return 0;
}

static int hispi_remove(struct platform_device *pdev)
{
	struct hispi_priv_data *private = (struct hispi_priv_data*)pdev->dev.driver_data;
	struct sensor_channel *channel = &private->channel;
	ENTER();
	/* disable hw */
	hispi_write_reg(private, CTRL_REG, 0);
	//v4l2_async_notifier_unregister(&private->notifier);
        video_unregister_device(&channel->vdev);
        v4l2_device_unregister(&private->v4l2_dev);
        //vb2_dma_contig_cleanup_ctx(hdmi_rx->alloc_ctx);
        //dma_release_channel(hdmi_rx->stream.chan);

	if(private->channel.dma)
		dma_release_channel(private->channel.dma);
	if(private->channel.preview_dma)
		dma_release_channel(private->channel.preview_dma);
	return 0;
}

/* match table for of_platform binding */
static struct of_device_id hispi_of_match[] = {
         { .compatible = "ant,hispi_interface", },
         {}
};

MODULE_DEVICE_TABLE(of, hispi_of_match);

static struct platform_driver hispi_platform_driver = {
         .probe   = hispi_probe,               /* Probe method */
         .remove  = hispi_remove,              /* Detach method */
         .driver  = {
                 .owner = THIS_MODULE,
                 .name = "HiSpi Interface",           /* Driver name */
                 .of_match_table = hispi_of_match,
                 },
};

static int __init hispi_init(void)
{
	ENTER();
        return platform_driver_register(&hispi_platform_driver);
}

static void __exit hispi_exit(void)
{
	ENTER();
        platform_driver_unregister(&hispi_platform_driver);
}

module_init(hispi_init);
module_exit(hispi_exit);

MODULE_DESCRIPTION("Antmicro HiSpi sensor interface");
MODULE_AUTHOR("Karol Gugala");
MODULE_LICENSE("GPL v2");
