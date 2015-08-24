#ifndef VIDEO_SNIFFER_H

#define VIDEO_SNIFFER_H

#define VSNIFF_RES_X		1920
#define VSNIFF_RES_Y		1080

/* Blank values used only to know how much encoded data to read */
#define VSNIFF_HBLANK		50
#define VSNIFF_VBLANK		20

#define VSNIFF_BPP		32

/* Number of encoded frames to copy */
#define VSNIFF_NFRAMES		2

#define VSNIFF_DMA_X		(VSNIFF_RES_X + VSNIFF_HBLANK)
#define VSNIFF_DMA_Y		(VSNIFF_RES_Y + VSNIFF_VBLANK)

#define VSNIFF_DMA_FSIZE	VSNIFF_DMA_X * VSNIFF_DMA_Y * (VSNIFF_BPP / 8)
#define VSNIFF_DMA_MEM_SIZE	VSNIFF_DMA_FSIZE * VSNIFF_NFRAMES

/* Video sniffer register values */
#define VSNIFF_REG_MODE_RGB	0x0000
#define VSNIFF_REG_MODE_TMDS	0x0001

/* ioctl related defines */
#define VSNIFF_IOC_MAGIC	'i'
#define VSNIFF_SETMODE_RGB	_IO(VSNIFF_IOC_MAGIC, 0x40)
#define VSNIFF_SETMODE_TMDS	_IO(VSNIFF_IOC_MAGIC, 0x41)

struct vsniff_chrdev_private_data {
	dev_t dev;
	struct class* cl;
	struct cdev* cdev;
};

struct vsniff_ctrl_regs {
	uint32_t mode;
};

struct vsniff_private_data {
	struct dma_chan *dma;
	struct xilinx_dma_config dma_config;

	void *buffer_virt;
	dma_addr_t buffer_phys;

	uint32_t image_x;
	uint32_t image_y;
	uint32_t image_bpp;

	struct vsniff_ctrl_regs __iomem *regs;

	struct vsniff_chrdev_private_data chrdev;
};

#define VSNIFF_CHRDEV_NAME	"vsniff"

#endif /* VIDEO_SNIFFER_H */
