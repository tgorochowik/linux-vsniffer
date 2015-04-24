#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/io.h>

#include <linux/fs.h>
#include <asm/uaccess.h>

#include <linux/dmaengine.h>
#include <asm/io.h>

#include "stereo_vision.h"

//#define ENTER() printk(KERN_ERR"Entering %s @ %d\n", __func__, __LINE__)
#define ENTER()

static struct stereo_vision_private *priv_data;

static uint32_t stereo_read_reg(struct stereo_vision_private *private, uint32_t offset)
{
	volatile uint32_t *reg;
	reg = (uint32_t *)((uint32_t)private->base+offset);
	return *reg;
}

static void stereo_write_reg(struct stereo_vision_private *private, uint32_t offset, uint32_t value)
{
	volatile uint32_t *reg;
	reg = (uint32_t *)((uint32_t)private->base+offset);
	*reg = value;
}

static inline void trigger_sync(struct stereo_vision_private *private)
{
	uint32_t reg;
	reg = stereo_read_reg(private, 0x00);
	reg |= TRIGGER_FSYNC;
	stereo_write_reg(private, 0x00, reg);
}

static void setup_internal_transfer(struct stereo_vision_private *private, uint32_t target)
{
        struct dma_async_tx_descriptor *desc;
        struct dma_interleaved_template *xt;
	struct dma_chan *dma;
        dma_addr_t internal_dst;
        dma_cookie_t cookie;
	void *callback;
	enum dma_transfer_direction dir;

	ENTER();
        long size = private->frame_x * private->frame_y * private->bpp;

        xt = kzalloc(sizeof(struct dma_async_tx_descriptor) +
                        sizeof(struct data_chunk), GFP_KERNEL);

	switch(target) {
		case TRANSFER_LEFT:
			callback = left_dma_done;
			dir = DMA_MEM_TO_DEV;
			dma = private->dma_internal_left.dma;
			xt->src_start = private->dma_internal_left.buffer + private->dma_internal_left.current_read_buffer*size;
			break;
		case TRANSFER_RIGHT:
			callback = right_dma_done;
			dir = DMA_MEM_TO_DEV;
			dma = private->dma_internal_right.dma;
			xt->src_start = private->dma_internal_right.buffer + private->dma_internal_right.current_read_buffer*size;
			break;
		case TRANSFER_OUT:
			callback = out_dma_done;
			dir = DMA_DEV_TO_MEM;
			dma = private->dma_internal_out.dma;
			xt->dst_start = private->dma_internal_out.buffer + private->dma_internal_out.current_write_buffer*size;
			break;
		default:
			printk(KERN_ERR"Unrecognized transfer target \n");
			kfree(xt);
			return;
	}


        xt->src_inc = false;
        xt->dst_inc = true;
        xt->src_sgl = false;
        xt->dst_sgl = true;
        xt->frame_size = 1;
        xt->numf = private->frame_y;
        xt->sgl[0].size = private->frame_x * private->bpp;
        xt->sgl[0].icg = 0;
        xt->dir = dir;

        desc = dmaengine_prep_interleaved_dma(dma, xt, DMA_PREP_INTERRUPT);
        kfree(xt);
        if (!desc) {
                printk(KERN_ERR"vdma desc prepare error \n");
                return;
        }

        desc->callback = callback;
        desc->callback_param = private;

        cookie = dmaengine_submit(desc);
        if (cookie < 0) {
                printk(KERN_ERR"vdma engine submit error \n");
                return;
        }
        /* start internal transfer */
        dma_async_issue_pending(dma);
}

static inline uint32_t get_unused_buffer(struct internal_dma *channel)
{
        uint32_t i;

        for(i=0; i<BUFFERS_COUNT; i++)
                if( (channel->current_read_buffer != i) &&
                                (channel->current_write_buffer != i))
                        break;
        return i;
}


/* dma callbacks */
static void left_dma_done(void *arg)
{
	struct stereo_vision_private* private = (struct stereo_vision_private*)arg;
        unsigned int flags;
        
        
	spin_lock_irqsave(&private->dma_internal_left.lock, flags);
	/* find unused buffer */
	if(private->dma_internal_left.flip_buffers) {
		private->dma_internal_left.current_read_buffer = get_unused_buffer(&private->dma_internal_left);
		private->dma_internal_left.flip_buffers = 0;
	}
	spin_unlock_irqrestore(&private->dma_internal_left.lock, flags);
	setup_internal_transfer(private, TRANSFER_LEFT);
}

static void right_dma_done(void *arg)
{
	struct stereo_vision_private* private = (struct stereo_vision_private*)arg;
        unsigned int flags;
        
	spin_lock_irqsave(&private->dma_internal_right.lock, flags);
	/* find unused buffer */
	if(private->dma_internal_right.flip_buffers) {
		private->dma_internal_right.current_read_buffer = get_unused_buffer(&private->dma_internal_right);
		private->dma_internal_right.flip_buffers = 0;
	}
	spin_unlock_irqrestore(&private->dma_internal_right.lock, flags);
	setup_internal_transfer(private, TRANSFER_RIGHT);
}

static void out_dma_done(void *arg)
{
	struct stereo_vision_private* private = (struct stereo_vision_private*)arg;
        unsigned int flags;
        
	//ENTER();
	spin_lock_irqsave(&private->dma_internal_out.lock, flags);
	/* find unused buffer */
	private->dma_internal_out.current_write_buffer = get_unused_buffer(&private->dma_internal_out);
	private->dma_internal_out.flip_buffers = 1;
	spin_unlock_irqrestore(&private->dma_internal_out.lock, flags);
	/* setup next transfer */
	setup_internal_transfer(private, TRANSFER_OUT);
}

static int stereo_open(struct inode *inode, struct file *file)
{
	unsigned minor = iminor(inode);
	ENTER();
	/* We can handle only one device (the one with minor == 0) */
	if(minor) return -ENODEV;
	file->private_data = priv_data;
	return 0;
}

static int stereo_close(struct inode *inode, struct file *file)
{
	ENTER();
	file->private_data = NULL;
	return 0;
}

static ssize_t stereo_read(struct file *file, char __user *buffer, size_t length, loff_t *offset)
{
	struct stereo_vision_private *private = file->private_data;
	uint32_t size;
	uint32_t out_buffer;
        unsigned long flags;
	ENTER();

	size = private->frame_x*private->frame_y*private->bpp;
	if( length != size ) {
		printk(KERN_ERR"whole frame should be read at once\n");
		return -EINVAL;
	}
	spin_lock_irqsave(&private->dma_internal_out.lock, flags);
	if(private->dma_internal_out.flip_buffers) {
		private->dma_internal_out.current_read_buffer = get_unused_buffer(&private->dma_internal_out);
		private->dma_internal_out.flip_buffers = 0;
	}
	spin_unlock_irqrestore(&private->dma_internal_out.lock, flags);
	out_buffer = (uint32_t)private->out_buf_base + private->dma_internal_out.current_read_buffer*size;

	if( copy_to_user((void*)buffer, (void*)(out_buffer), size) != 0 )
		return -EFAULT;
	return size;
}

static ssize_t stereo_write(struct file *file, const char __user *buffer, size_t length, loff_t *offset)
{
	struct stereo_vision_private *private = file->private_data;
	uint32_t size;
	uint32_t in_buffer;
        unsigned long flags;
	ENTER();

	size = private->frame_x*private->frame_y*private->bpp;
	if( length != size ) {
		printk(KERN_ERR"whole frame should be written at once\n");
		return -EINVAL;
	}
	if(private->next_transfer_target == TRANSFER_LEFT) {
		spin_lock_irqsave(&private->dma_internal_left.lock, flags);
		private->dma_internal_left.current_write_buffer = get_unused_buffer(&private->dma_internal_left);
		private->dma_internal_left.flip_buffers = 1;
		spin_unlock_irqrestore(&private->dma_internal_left.lock, flags);
		in_buffer = (uint32_t)private->left_buf_base + private->dma_internal_left.current_write_buffer*size;
	} else if(private->next_transfer_target == TRANSFER_RIGHT) {
		spin_lock_irqsave(&private->dma_internal_right.lock, flags);
		private->dma_internal_right.current_write_buffer = get_unused_buffer(&private->dma_internal_right);
		private->dma_internal_right.flip_buffers = 1;
		spin_unlock_irqrestore(&private->dma_internal_right.lock, flags);
		in_buffer = (uint32_t)private->right_buf_base + private->dma_internal_right.current_write_buffer*size;
	} else {
		printk(KERN_ERR"Target buffer must be choosen before writing\n");
		return -ENOMEM;
	}

	if( copy_from_user((void*)in_buffer, buffer, size) != 0 )
		return -EFAULT;
	return size;
}

static long stereo_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param)
{
	struct stereo_vision_private *private = file->private_data;
	uint32_t reg;
	ENTER();
	switch(ioctl_num) {
		case SET_LEFT_AS_TARGET:
			private->next_transfer_target = TRANSFER_LEFT;
			break;
		case SET_RIGHT_AS_TARGET:
			private->next_transfer_target = TRANSFER_RIGHT;
			break;
		case START_INTERNAL_TRANSFERS:
			setup_internal_transfer(private, TRANSFER_LEFT);
			setup_internal_transfer(private, TRANSFER_RIGHT);
			setup_internal_transfer(private, TRANSFER_OUT);
			trigger_sync(private);
			break;
		case STOP_INTERNAL_TRANSFERS:
			dmaengine_terminate_all(private->dma_internal_left.dma);
			dmaengine_terminate_all(private->dma_internal_right.dma);
			dmaengine_terminate_all(private->dma_internal_out.dma);
			break;
		case TOGGLE_SOURCE:
			reg = stereo_read_reg(private, 0x00);
			if(reg & SWITCH_SOURCE) reg &= ~(SWITCH_SOURCE);
			else reg |= SWITCH_SOURCE;
			stereo_write_reg(private, 0x00, reg);
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

struct file_operations fops = {
				.read = stereo_read,
				.write = stereo_write,
				.unlocked_ioctl = stereo_ioctl,
				.open = stereo_open,
				.release = stereo_close,};

static int stereo_probe(struct platform_device *pdev)
{
	struct stereo_vision_private *private;
	struct resource *res;
	resource_size_t frame_size;
	int err;

	private = devm_kzalloc(&pdev->dev, sizeof(*private), GFP_KERNEL);
        if (!private) {
                printk(KERN_ERR"Mem alloc for private data failed \n");
                return -ENOMEM;
        }

	/* do the mappings */
        res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        private->base = devm_ioremap_resource(&pdev->dev, res);
        if (IS_ERR(private->base)) {
                printk(KERN_ERR"IO mapping failed \n");
                return PTR_ERR(private->base);
        }

	/* get DMAs */
	private->dma_input = dma_request_slave_channel(&pdev->dev, "input");
	if(private->dma_input == NULL) return -EPROBE_DEFER;

	private->dma_output = dma_request_slave_channel(&pdev->dev, "output");
	if(private->dma_output == NULL) return -EPROBE_DEFER;

	private->dma_internal_left.dma = dma_request_slave_channel(&pdev->dev, "buffer-left");
	if(private->dma_internal_left.dma == NULL) return -EPROBE_DEFER;

	private->dma_internal_right.dma = dma_request_slave_channel(&pdev->dev, "buffer-right");
	if(private->dma_internal_right.dma == NULL) return -EPROBE_DEFER;

	private->dma_internal_out.dma = dma_request_slave_channel(&pdev->dev, "buffer-out");
	if(private->dma_internal_out.dma == NULL) return -EPROBE_DEFER;

	err = of_property_read_u32(pdev->dev.of_node, "ant,left-buffer", (u32*)&(private->dma_internal_left.buffer));
	if(err) return err;

	err = of_property_read_u32(pdev->dev.of_node, "ant,right-buffer", (u32*)&(private->dma_internal_right.buffer));
	if(err) return err;

	err = of_property_read_u32(pdev->dev.of_node, "ant,out-buffer", (u32*)&(private->dma_internal_out.buffer));
	if(err) return err;

	frame_size = FRAME_X * FRAME_Y * BPP;
	private->left_buf_base = devm_ioremap_nocache(&pdev->dev, (resource_size_t)private->dma_internal_left.buffer, frame_size*BUFFERS_COUNT);
	private->right_buf_base = devm_ioremap_nocache(&pdev->dev, (resource_size_t)private->dma_internal_right.buffer, frame_size*BUFFERS_COUNT);
	private->out_buf_base = devm_ioremap_nocache(&pdev->dev, (resource_size_t)private->dma_internal_out.buffer, frame_size*BUFFERS_COUNT);

	if( IS_ERR(private->left_buf_base) || IS_ERR(private->right_buf_base) || IS_ERR(private->out_buf_base) ){
		printk(KERN_ERR"Internal buffers mappings failed \n");
		return -ENOMEM;
	}
	/* init mutexes */
	/*mutex_init(&private->dma_internal_left.lock);
	mutex_init(&private->dma_internal_right.lock);
	mutex_init(&private->dma_internal_out.lock);
	*/
        #define SPIN_LOCK_UNLOCKED      (spinlock_t) { 0, 0 }
        private->dma_internal_left.lock = SPIN_LOCK_UNLOCKED;
        private->dma_internal_right.lock = SPIN_LOCK_UNLOCKED;
        private->dma_internal_out.lock = SPIN_LOCK_UNLOCKED;
	/* set some defaults */
	private->frame_x = FRAME_X;
	private->frame_y = FRAME_Y;
	private->bpp = BPP;

	platform_set_drvdata(pdev, private);
	/* set global private pointer */
	priv_data = private;

	err = register_chrdev(INTERFACE_MAJOR_NUMBER, DEVICE_NAME, &fops);
	if (err < 0)
	{
		printk(KERN_ERR"%s: Char device registration failed\n", DEVICE_NAME);
		return -ENODEV;
	}
	ENTER();
	return 0;
}

static int stereo_remove(struct platform_device *pdev)
{
	struct stereo_vision_private *private = (struct stereo_vision_private*)pdev->dev.driver_data;

	unregister_chrdev(INTERFACE_MAJOR_NUMBER, DEVICE_NAME);
	ENTER();
	if(private->dma_input) dma_release_channel(private->dma_input);
	if(private->dma_output) dma_release_channel(private->dma_output);
	if(private->dma_internal_left.dma) dma_release_channel(private->dma_internal_left.dma);
	if(private->dma_internal_right.dma) dma_release_channel(private->dma_internal_right.dma);
	if(private->dma_internal_out.dma) dma_release_channel(private->dma_internal_out.dma);
	return 0;
}

/* match table for of_platform binding */
static struct of_device_id stereo_of_match[] = {
         { .compatible = "ant,stereo-vision", },
         {}
};

MODULE_DEVICE_TABLE(of, stereo_of_match);

static struct platform_driver stereo_platform_driver = {
         .probe   = stereo_probe,               /* Probe method */
         .remove  = stereo_remove,              /* Detach method */
         .driver  = {
                 .owner = THIS_MODULE,
                 .name = "Stereo Vision",           /* Driver name */
                 .of_match_table = stereo_of_match,
                 },
};

static int __init stereo_init(void)
{
        ENTER();
        return platform_driver_register(&stereo_platform_driver);
}

static void __exit stereo_exit(void)
{
        ENTER();
        platform_driver_unregister(&stereo_platform_driver);
}

module_init(stereo_init);
module_exit(stereo_exit);

MODULE_DESCRIPTION("Antmicro Stereo Vision IPCore");
MODULE_AUTHOR("Karol Gugala");
MODULE_LICENSE("GPL v2");

