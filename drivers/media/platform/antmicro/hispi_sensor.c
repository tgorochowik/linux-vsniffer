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

#define ENTER() printk("Entering %s @ %d \n", __func__, __LINE__)
//#define ENTER()

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

static void hispi_dma_done(void *arg)
{
	//struct vb2_buffer *vb = (struct vb2_buffer*)(arg);
	struct hispi_buffer *buf = arg;
	struct sensor_channel *channel = vb2_get_drv_priv(buf->vb.vb2_queue);
	unsigned long flags;
	
	ENTER();

	spin_lock_irqsave(&channel->spinlock, flags);
	list_del(&buf->head);
	spin_unlock_irqrestore(&channel->spinlock, flags);

	v4l2_get_timestamp(&buf->vb.v4l2_buf.timestamp);
        vb2_buffer_done(&buf->vb, VB2_BUF_STATE_DONE);
}

static void hispi_buf_queue(struct vb2_buffer *vb)
{
	unsigned long size;
	unsigned long flags;
	struct dma_async_tx_descriptor *desc;	
	struct dma_interleaved_template *xt;
	struct xilinx_vdma_config xconf;
	
	dma_addr_t addr;
	dma_cookie_t cookie;

	struct sensor_channel *channel = vb2_get_drv_priv(vb->vb2_queue);
	struct hispi_buffer *buf = vb2_buf_to_hispi_buf(vb);

	ENTER();

	addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	size = vb2_get_plane_payload(vb, 0);

	xt = kzalloc(sizeof(struct dma_async_tx_descriptor) +
                                sizeof(struct data_chunk), GFP_KERNEL);
        if (!xt) {
                vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
                return;
        }

	/* configure xilinx specific DMA */
	/*xconf.frm_dly = 0;
	xconf.park = 1;
	xconf.park_frm = 0;

	xilinx_vdma_channel_set_config(channel->dma, &xconf);*/

	xt->dst_start = addr;
	xt->src_inc = false;
	xt->dst_inc = true;
	xt->src_sgl = false;
	xt->dst_sgl = true;
	xt->frame_size = 1;
	xt->numf = channel->video_y;
	xt->sgl[0].size = channel->video_x * channel->bpp;
	xt->sgl[1].icg = 0;
	xt->dir = DMA_DEV_TO_MEM;

	printk(KERN_ERR"DMA addr is: 0x%08x, size = %ld\n", addr, size);

	desc = dmaengine_prep_interleaved_dma(channel->dma, xt, DMA_PREP_INTERRUPT);
	kfree(xt);
	if (!desc) {
		printk(KERN_ERR"desc prepare error \n");
                vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
                return;
        }

	desc->callback = hispi_dma_done;
        desc->callback_param = buf;

	cookie = dmaengine_submit(desc);
	if (cookie < 0) {
		printk(KERN_ERR"dma engine submit error \n");
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}
	
	spin_lock_irqsave(&channel->spinlock, flags);
        list_add_tail(&buf->head, &channel->queued_buffers);
        spin_unlock_irqrestore(&channel->spinlock, flags);
	
	if (vb2_is_streaming(vb->vb2_queue)) {
		printk(KERN_ERR"We're streaming ... \n");
                dma_async_issue_pending(channel->dma);
	}
	
}

static int hispi_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sensor_channel *channel = vb2_get_drv_priv(q);
	ENTER();
	dma_async_issue_pending(channel->dma);
	return 0;
}

static void hispi_stop_streaming(struct vb2_queue *q)
{
	struct sensor_channel *channel = vb2_get_drv_priv(q);
	struct hispi_buffer *buf;
	unsigned long flags;

	ENTER();
	dmaengine_terminate_all(channel->dma); 

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
		strlcpy(f->description, "GREY", sizeof(f->description));
		f->pixelformat = /*V4L2_PIX_FMT_UYVY;*/V4L2_PIX_FMT_GREY;//V4L2_PIX_FMT_SBGGR8;
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
	pix->pixelformat = /*V4L2_PIX_FMT_UYVY;*/V4L2_PIX_FMT_GREY;//V4L2_PIX_FMT_SBGGR8;
	pix->sizeimage =  pix->bytesperline * pix->height;
	pix->field = V4L2_FIELD_NONE;

	return 0;
}

#define ANTYMAKRO(a, str) do{	\
       	str[0]=a&0xff;		\
       	str[1]=(a>>8)&0xff;	\
       	str[2]=(a>>16)&0xff;	\
       	str[3]=(a>>24)&0xff;	\
	str[4]='\0';		\
	}while(0)

static int hispi_try_fmt_vid_cap(struct file *file, void *priv_fh,
        struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct hispi_priv_data *priv = video_drvdata(file);
	struct sensor_channel *channel = &priv->channel;
	char format[5];
	ENTER();

#if 0
	printk(KERN_ERR">>>>>>>>>>");
	ANTYMAKRO(pix->pixelformat, format);
	printk(KERN_ERR"width = %d\n", pix->width);
	printk(KERN_ERR"height = %d\n", pix->height);
	printk(KERN_ERR"colorspace = %d\n", pix->colorspace);
	printk(KERN_ERR"pixelformat = %s\n", format);
	printk(KERN_ERR"bytesperline = %d\n", pix->bytesperline);
	printk(KERN_ERR"sizeimage = %d\n", pix->sizeimage);
	printk(KERN_ERR"field = %d\n", pix->field);
	printk(KERN_ERR"priv = %d\n", pix->priv);
#endif 
	//pix->width = channel->video_x;
	//pix->height = channel->video_y;
	v4l_bound_align_image(&pix->width, 176, MAX_X, 0, &pix->height, 144,
			                MAX_Y, 0, 0);
	pix->colorspace = /*V4L2_COLORSPACE_SMPTE170M;*//*V4L2_COLORSPACE_JPEG;*/V4L2_COLORSPACE_SRGB;
	pix->pixelformat = /*V4L2_PIX_FMT_UYVY;*/V4L2_PIX_FMT_GREY;
	pix->bytesperline = /*channel->video_x*/pix->width * channel->bpp;
	pix->sizeimage =  pix->bytesperline * pix->height;
	pix->field = V4L2_FIELD_NONE;
	pix->priv = 0;
#if 0
	printk(KERN_ERR"<<<<<<<<<<<");
	ANTYMAKRO(pix->pixelformat, format);
	printk(KERN_ERR"width = %d\n", pix->width);
	printk(KERN_ERR"height = %d\n", pix->height);
	printk(KERN_ERR"colorspace = %d\n", pix->colorspace);
	printk(KERN_ERR"pixelformat = %s\n", format);
	printk(KERN_ERR"bytesperline = %d\n", pix->bytesperline);
	printk(KERN_ERR"sizeimage = %d\n", pix->sizeimage);
	printk(KERN_ERR"field = %d\n", pix->field);
	printk(KERN_ERR"priv = %d\n", pix->priv);
#endif
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
		//inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
		inp->std = V4L2_STD_UNKNOWN;
	} else return -EINVAL;
	return 0;
}

static int hispi_s_dv_timings(struct file *file, void *priv_fh,
        struct v4l2_dv_timings *timings)
{
	ENTER();
	return 0;
}

static int hispi_g_dv_timings(struct file *file, void *priv_fh,
        struct v4l2_dv_timings *timings)
{
	ENTER();
	return 0;
}

static int hispi_enum_dv_timings(struct file *file, void *priv_fh,
        struct v4l2_enum_dv_timings *timings)
{
	ENTER();
	return 0;
}

static int hispi_query_dv_timings(struct file *file, void *priv_fh,
        struct v4l2_dv_timings *timings)
{
	ENTER();
	return 0;
}

static int hispi_dv_timings_cap(struct file *file, void *priv_fh,
        struct v4l2_dv_timings_cap *cap)
{
	ENTER();
	return 0;
}

static int hispi_g_input(struct file *file, void *priv_fh, unsigned int *i)
{
	struct video_device *video = video_devdata(file);
	ENTER();
	/* set 0 input */
	//TODO: handle two inputs
	*i = 0;

	//video->tvnorms = V4L2_STD_PAL;
	return 0;
}

static int hispi_s_input(struct file *file, void *priv_fh, unsigned int i)
{
	ENTER();
	return 0;
}

static int hispi_querystd(struct file *file, void *priv, v4l2_std_id *std_id)
{
	ENTER();
	return 0;
}

static int hispi_s_std(struct file *file, void *priv, v4l2_std_id std_id)
{
	ENTER();
	return 0;
}

static int hispi_g_std(struct file *file, void *priv, v4l2_std_id *tvnorm)
{
	ENTER();
	//*tvnorm = V4L2_STD_PAL;
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

	//.vidioc_g_fmt_vid_out    	= hispi_g_fmt_vid_cap,
        //.vidioc_s_fmt_vid_out    	= hispi_s_fmt_vid_cap,
        //.vidioc_try_fmt_vid_out  	= hispi_try_fmt_vid_cap,
        //.vidioc_enum_fmt_vid_out 	= hispi_enum_fmt_vid_cap,

	//.vidioc_querystd         	= hispi_querystd,
        //.vidioc_s_std            	= hispi_s_std,
        //.vidioc_g_std            	= hispi_g_std,

        //.vidioc_s_dv_timings            = hispi_s_dv_timings,
        //.vidioc_g_dv_timings            = hispi_g_dv_timings,
        //.vidioc_query_dv_timings        = hispi_query_dv_timings,
        //.vidioc_enum_dv_timings         = hispi_enum_dv_timings,
        //.vidioc_dv_timings_cap          = hispi_dv_timings_cap,
        .vidioc_subscribe_event         = v4l2_ctrl_subscribe_event,
        .vidioc_unsubscribe_event       = v4l2_event_unsubscribe,
        .vidioc_create_bufs             = vb2_ioctl_create_bufs,
        .vidioc_prepare_buf             = vb2_ioctl_prepare_buf,
        .vidioc_reqbufs                 = vb2_ioctl_reqbufs,
        .vidioc_querybuf                = vb2_ioctl_querybuf,
        .vidioc_qbuf                    = vb2_ioctl_qbuf,
        .vidioc_dqbuf                   = vb2_ioctl_dqbuf,
};



struct hispi_priv_data *priv_data;

static void hispi_stop_dma(struct hispi_priv_data *private)
{
	dmaengine_terminate_all(private->channel.dma);
}

static int hispi_start_dma(struct hispi_priv_data *private)
{
	struct dma_async_tx_descriptor *desc;
	struct sensor_channel *channel = &private->channel;

	ENTER();
	dmaengine_terminate_all(private->channel.dma);
	
	//private->dma_config.hsize = channel->video_x * channel->bpp;
	//private->dma_config.vsize = channel->video_y;
	//private->dma_config.stride = channel->video_x * channel->bpp; 

	dmaengine_device_control(private->channel.dma, DMA_SLAVE_CONFIG, (unsigned long)&private->dma_config);

	desc = dmaengine_prep_slave_single(private->channel.dma, private->video_buffer, channel->video_x * channel->bpp * channel->video_y, DMA_DEV_TO_MEM, 0);
	if (!desc) {
		pr_err("Descriptor prep error\n");
		return -ENOMEM;
	} else {
		dmaengine_submit(desc);
		dma_async_issue_pending(private->channel.dma);
	}
	return 0;	
}

static int hispi_open(struct inode *inode, struct file *file)
{
	file->private_data = (void*)priv_data;
	return 0;
}
static int hispi_close(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}
static ssize_t hispi_read(struct file *file, char __user *buffer, size_t length, loff_t *offset)
{
	struct hispi_priv_data *priv;
	int ret;
	priv = (struct hispi_priv_data *) file->private_data;

	if(copy_to_user((void*)buffer, priv->buffer_virt, length) != 0)	ret = -EFAULT;
	else ret = length;

	return length;
}
static ssize_t hispi_write(struct file *file, const char __user *buffer, size_t length, loff_t *offset)
{
	return 0;
}
static long hispi_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param)
{
	struct hispi_priv_data *priv;
	int ret;
	priv = (struct hispi_priv_data *) file->private_data;
	ENTER();
	switch (ioctl_num)
	{
		case HISPI_START_STREAM:
			if( (ret = hispi_start_dma(priv)) < 0) return ret;
			break;
		case HISPI_STOP_STREAM:
			hispi_stop_dma(priv);
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

struct file_operations fops = {
                                .read = hispi_read,
                                .write = hispi_write,
                                .unlocked_ioctl = hispi_ioctl,
                                .open = hispi_open,
                                .release = hispi_close,};

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

	if ( (ret = register_chrdev(HISPI_MAJOR_NUMBER, DEVICE_NAME, &fops)) <0) {
		printk(KERN_ERR"chrdev registration failed\n");
		return ret;		
	}

	channel->video_x = MAX_X;
	channel->video_y = MAX_Y;
	channel->bpp = BPP / 8;

	platform_set_drvdata(pdev, private);
	priv_data = private;

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

	/* set the markers */
	/*reg = (SOF_MARKER_REV << MARKER_HI_SHIFT) | SOL_MARKER_REV;
	hispi_write_reg(private, MARKERS1_REG, reg);

	reg = hispi_read_reg(private, MARKERS1_REG);
	printk(KERN_ERR"MARKERS1 = 0x%08x \n", reg);

	reg = (EOF_MARKER_REV << MARKER_HI_SHIFT) | EOL_MARKER_REV;
	hispi_write_reg(private, MARKERS2_REG, reg);

	reg = hispi_read_reg(private, MARKERS2_REG);
	printk(KERN_ERR"MARKERS2 = 0x%08x \n", reg);*/
	/* enable hardware */
	hispi_write_reg(private, CTRL_REG, ENABLE_BIT);
	return 0;
}

static int hispi_remove(struct platform_device *pdev)
{
	struct hispi_priv_data *private = (struct hispi_priv_data*)pdev->dev.driver_data;
	struct sensor_channel *channel = &private->channel;
	ENTER();
	unregister_chrdev(HISPI_MAJOR_NUMBER, DEVICE_NAME);

	/* disable hw */
	hispi_write_reg(private, CTRL_REG, 0);
	//v4l2_async_notifier_unregister(&private->notifier);
        video_unregister_device(&channel->vdev);
        v4l2_device_unregister(&private->v4l2_dev);
        //vb2_dma_contig_cleanup_ctx(hdmi_rx->alloc_ctx);
        //dma_release_channel(hdmi_rx->stream.chan);

	if(private->channel.dma)
		dma_release_channel(private->channel.dma);
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
