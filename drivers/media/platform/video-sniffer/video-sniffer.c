/*
 * Video Sniffer
 *
 * Copyright (C) 2015 Antmicro Ltd.
 *
 * Author(s): *  Tomasz Gorochowik <tgorochowik@antmicro.com>
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

#include "video-sniffer.h"

/* Global variables for chrdev */
static int vsniff_chrdev_is_open;
static struct mutex vsniff_chrdev_lock;
static int vsniff_chrdev_major;

static int vsniff_chrdev_open(struct inode *inode, struct file *file)
{
	if (vsniff_chrdev_is_open)
		return -EBUSY;

	mutex_lock(&vsniff_chrdev_lock);
	vsniff_chrdev_is_open++;
	mutex_unlock(&vsniff_chrdev_lock);

	try_module_get(THIS_MODULE);

	return 0;
}

static int vsniff_chrdev_release(struct inode *inode, struct file *file)
{
	mutex_lock(&vsniff_chrdev_lock);
	vsniff_chrdev_is_open--;
	mutex_unlock(&vsniff_chrdev_lock);

	module_put(THIS_MODULE);

	return 0;
}

static ssize_t vsniff_chrdev_read(struct file *file, char *buffer,
				  size_t length, loff_t *offset)
{

	return 0;
}

/* File operations struct for the chrdev */
struct file_operations vsniff_chrdev_fops = {
	.open = vsniff_chrdev_open,
	.release = vsniff_chrdev_release,
	.read = vsniff_chrdev_read
};

static int vsniff_probe(struct platform_device *pdev) {
	struct vsniff_private_data *private;
	struct resource *resource;
	int err;

	/* Allocate memory for private data */
	private = devm_kzalloc(&pdev->dev, sizeof(*private), GFP_KERNEL);
	if (!private) {
		printk(KERN_ERR "Memory allocation failure\n");
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
	private->buffer_virt = devm_kzalloc(&pdev->dev,
					    VSNIFF_DMA_MEM_SIZE,
					    GFP_DMA);

	if (!private->buffer_virt) {
		printk(KERN_ERR "Memory allocation failure\n");
		return -ENOMEM;
	}

	/* Translate addresses */
	private->buffer_base = (dma_addr_t)virt_to_phys(private->buffer_virt);
	printk(KERN_INFO "virt = 0x%p, phys = 0x%08x \n",
	       private->buffer_virt, private->buffer_base);

	/* Request DMA channel */
	private->dma = dma_request_slave_channel(&pdev->dev, "frame");

	/* Xilinx DMA driver might be not initialized yet - defer if failed */
	if (private->dma == NULL)
		return -EPROBE_DEFER;

	/* Attempt to get memory base from devicetree */
	err = of_property_read_u32(pdev->dev.of_node, "vsniff,frame-mem-base",
				   (u32*)&(private->buffer_base));

	/* Exit if failed */
	if (err)
		return err;

	/* Set driver data */
	platform_set_drvdata(pdev, private);

	/* Initialize chrdev driver */
	vsniff_chrdev_is_open = 0;
	mutex_init(&vsniff_chrdev_lock);

	vsniff_chrdev_major = register_chrdev(0, VSNIFF_CHRDEV_NAME,
					      &vsniff_chrdev_fops);

	if (vsniff_chrdev_major < 0) {
		printk(KERN_ALERT "Registering vsniff_chrdev failed: %d\n.",
		       vsniff_chrdev_major);
		return vsniff_chrdev_major;
	}

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
	unregister_chrdev(vsniff_chrdev_major, VSNIFF_CHRDEV_NAME);

	return 0;
}

/* Match table for of_platform binding */
static struct of_device_id vsniff_of_match[] = {
	{ .compatible = "vsniff,video-sniffer", },
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
