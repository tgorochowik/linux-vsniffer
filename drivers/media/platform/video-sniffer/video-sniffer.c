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
	frame_size = private->image_y *
		private->image_x *
		private->image_bpp / 8;

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
		xt->numf = private->image_y;
		xt->sgl[0].size = private->image_x * private->image_bpp / 8;
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

/* File operations struct for the chrdev */
struct file_operations vsniff_chrdev_fops = {
	.open = vsniff_chrdev_open,
	.release = vsniff_chrdev_release,
	.read = vsniff_chrdev_read
};

static int vsniff_probe(struct platform_device *pdev)
{
	struct resource *resource;
	int result;

	/* Allocate memory for private data */
	private = devm_kzalloc(&pdev->dev, sizeof(*private), GFP_KERNEL);
	if (!private) {
		printk(KERN_ERR "Memory allocation failure (private data)\n");
		return -ENOMEM;
	}

	/* Get the resource */
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	private->base = devm_ioremap_resource(&pdev->dev, resource);

	if (IS_ERR(private->base)) {
		printk(KERN_ERR "IO mapping failed\n");
		return PTR_ERR(private->base);
	}

	/* Allocate memory for the buffer */
	private->buffer_virt = dmam_alloc_coherent(&pdev->dev,
						   VSNIFF_DMA_MEM_SIZE,
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

	/* Initialize image parameters */
	private->image_x = VSNIFF_RES_X;
	private->image_y = VSNIFF_RES_Y;
	private->image_bpp = VSNIFF_BPP;

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
