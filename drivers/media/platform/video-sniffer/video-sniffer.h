#ifndef VIDEO_SNIFFER_H

#define VIDEO_SNIFFER_H

#define VSNIFF_RES_X		1920
#define VSNIFF_RES_Y		1080

/* Blank values used only to know how much encoded data to read. These have to
 * be set to the same values as in the video-sniffer ipcore in the fpga */
#define VSNIFF_HBLANK		50
#define VSNIFF_VBLANK		20

#define VSNIFF_BPP		  (32/8)

/* Number of encoded frames to copy */
#define VSNIFF_NFRAMES		3

#define VSNIFF_DMA_X		(VSNIFF_RES_X + VSNIFF_HBLANK)
#define VSNIFF_DMA_Y		(VSNIFF_RES_Y + VSNIFF_VBLANK)

#define VSNIFF_DMA_FSIZE	VSNIFF_DMA_X * VSNIFF_DMA_Y * VSNIFF_BPP
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
	uint32_t res_x;
	uint32_t res_y;
};

struct vsniff_v4l2_private_data {
	struct vb2_alloc_ctx *alloc_ctx;
	struct dma_chan *dma;
	struct v4l2_device dev;
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex lock;
	struct list_head queued_buffers;
	spinlock_t spinlock;
};

struct vsniff_private_data {
	struct dma_chan *dma;
	struct xilinx_dma_config dma_config;

	void *buffer_virt;
	dma_addr_t buffer_phys;

	struct vsniff_ctrl_regs __iomem *regs;

	struct vsniff_chrdev_private_data chrdev;
	struct vsniff_v4l2_private_data v4l2;
};

struct vsniff_v4l2_buffer {
	struct vb2_buffer vb;
	struct list_head head;
};

#define VSNIFF_CHRDEV_NAME	"vsniff"

#endif /* VIDEO_SNIFFER_H */
