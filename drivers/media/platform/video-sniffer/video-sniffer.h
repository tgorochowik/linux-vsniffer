#ifndef VIDEO_SNIFFER_H

#define VIDEO_SNIFFER_H

#define VSNIFF_MAX_X		1920
#define VSNIFF_MAX_Y		1080
#define VSNIFF_BPP		4

#define VSNIFF_DMA_MEM_SIZE	(VSNIFF_MAX_X * VSNIFF_MAX_Y * VSNIFF_BPP / 8)

struct vsniff_private_data {
	struct dma_chan *dma;
	struct xilinx_dma_config dma_config;

	void *buffer_virt;
	dma_addr_t buffer_base;

	void __iomem *base;
};

#define VSNIFF_CHRDEV_NAME	"vsniff"

#endif /* VIDEO_SNIFFER_H */
