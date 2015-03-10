#ifndef _HISPI_SENSOR_H_
#define _HISPI_SENSOR_H_

#define DEVICE_NAME             "HiSpi Interface"
#define HISPI_MAJOR_NUMBER      95


#define MAX_X 640
#define MAX_Y 480
#define BPP 8

struct sensor_channel {
        struct vb2_queue q;
        struct video_device vdev;
        struct mutex lock;
        spinlock_t spinlock;
	/* internal video dma */
        struct dma_chan *dma;
	/* preview dma */
	struct dma_chan *preview_dma;
    
        struct vb2_alloc_ctx *alloc_ctx;
        uint32_t video_x;
        uint32_t video_y;
        uint32_t bpp;

	struct list_head queued_buffers;

	/* internal video memory handling */
	struct mutex internal_lock;
	uint8_t flip_buffers;
	uint8_t internal_streaming;
	dma_addr_t internal_buffer_base;
	uint32_t current_write_buffer;
	uint32_t current_read_buffer;

};

struct hispi_priv_data {
        struct v4l2_device v4l2_dev;

        struct sensor_channel channel;

        struct xilinx_dma_config dma_config;
        void *buffer_virt;
        dma_addr_t video_buffer;

	void __iomem *base;
};

struct hispi_buffer {
        struct vb2_buffer vb;
        struct list_head head;
};

/* Registers */

#define STATUS_REG	0x00
#define CTRL_REG	0x04
#define MARKERS1_REG	0x0C
#define MARKERS2_REG	0x10

#define ENABLE_BIT	(1<<0)
#define SYNCED_BIT	(1<<0)
#define MARKER_MASK	0x3FF
#define MARKER_HI_SHIFT 16

#define SOF_MARKER	0x003
#define SOL_MARKER	0x001
#define EOF_MARKER	0x005
#define EOL_MARKER	0x007

/* bit reversed markers */
#define SOF_MARKER_REV	0x300
#define SOL_MARKER_REV	0x200
#define EOF_MARKER_REV	0x280
#define EOL_MARKER_REV	0x380

/* fcn defs */
static void hispi_internal_vdma_done(void *arg);

#endif
