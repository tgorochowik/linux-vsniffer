#ifndef _STEREO_VISION_H_
#define _STEREO_VISION_H_

#define DEVICE_NAME "Stereo Vision"
#define INTERFACE_MAJOR_NUMBER 95

//#define FRAME_X 640
//#define FRAME_Y 480
#define FRAME_X 320
#define FRAME_Y 240
#define BPP 1

#define BUFFERS_COUNT 3

struct internal_dma {
	struct dma_chan *dma;
	//struct mutex lock;
        spinlock_t lock;
	uint8_t flip_buffers;
	uint8_t current_write_buffer;
	uint8_t current_read_buffer;
	dma_addr_t buffer;
};

struct stereo_vision_private {
	struct dma_chan *dma_input;
	struct dma_chan *dma_output;
	/* internal video memory dmas*/
	struct internal_dma dma_internal_left;
	struct internal_dma dma_internal_right;
	struct internal_dma dma_internal_out;
#if 0
	struct dma_chan *dma_internal_left;
	struct dma_chan *dma_internal_right;
	struct dma_chan *dma_internal_out;
	/* internal video memory buffers locations*/
	dma_addr_t left_buffer;
	dma_addr_t right_buffer;
	dma_addr_t out_buffer;
	/* markers for double buffering */
	uint8_t current_out_buffer;
	uint8_t current_left_buffer;
	uint8_t current_right_buffer;
#endif
	/* target of the next write */
	uint32_t next_transfer_target;

	uint32_t frame_x;
	uint32_t frame_y;
	uint32_t bpp;

	void __iomem *base;
	void __iomem *left_buf_base;
	void __iomem *right_buf_base;
	void __iomem *out_buf_base;
};

/* misc */
#define TRANSFER_LEFT		0x10000000
#define TRANSFER_RIGHT		0x10000001
#define TRANSFER_OUT		0x10000002

/* ioctls */
#define STEREO_IOCTL_MAGIC	'S'

#define SET_LEFT_AS_TARGET		_IO(STEREO_IOCTL_MAGIC, 1)
#define SET_RIGHT_AS_TARGET		_IO(STEREO_IOCTL_MAGIC, 2)
#define START_INTERNAL_TRANSFERS	_IO(STEREO_IOCTL_MAGIC, 3)
#define STOP_INTERNAL_TRANSFERS		_IO(STEREO_IOCTL_MAGIC, 4)
#define TOGGLE_SOURCE			_IO(STEREO_IOCTL_MAGIC, 5)

/* regs */
#define LEFT_BUFFER_READY	(1<<0)
#define RIGHT_BUFFER_READY	(1<<1)
#define OUT_BUFFER_READY	(1<<2)
#define SWITCH_SOURCE		(1<<3)
#define TRIGGER_FSYNC		(1<<4)

/* fcn headers */
static void left_dma_done(void *arg);
static void right_dma_done(void *arg);
static void out_dma_done(void *arg);

#endif
