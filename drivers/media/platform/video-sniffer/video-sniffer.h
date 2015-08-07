#ifndef VIDEO_SNIFFER_H

#define VIDEO_SNIFFER_H

#define VSNIFF_MAX_X		800
#define VSNIFF_MAX_Y		600
#define VSNIFF_BPP		32

#define VSNIFF_DMA_MEM_SIZE	(VSNIFF_MAX_X * VSNIFF_MAX_Y * VSNIFF_BPP / 8)

struct vsniff_chrdev_private_data {
	dev_t dev;
	struct class* cl;
	struct cdev* cdev;
};

struct vsniff_private_data {
	struct dma_chan *dma;
	struct xilinx_dma_config dma_config;

	void *buffer_virt;
	dma_addr_t buffer_phys;

	uint32_t image_x;
	uint32_t image_y;
	uint32_t image_bpp;

	void __iomem *base;

	struct vsniff_chrdev_private_data chrdev;
};

#define VSNIFF_CHRDEV_NAME	"vsniff"

#endif /* VIDEO_SNIFFER_H */
