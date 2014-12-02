#ifndef _ANT_DISPCTRL_DRV_H_
#define _ANT_DISPCTRL_DRV_H_

#include <drm/drm.h>
#include <drm/drm_fb_cma_helper.h>
#include <linux/of.h>
#include <linux/clk.h>

struct axi_dispctrl_private { 
        struct drm_device *drm_dev;
        struct drm_fbdev_cma *fbdev;
        struct drm_crtc *crtc;

	void __iomem *base;
	
        struct dma_chan *dma;

};

#define pr_dev_info(fmt, ...) \
         printk(KERN_ERR"%s [%s:%d] "fmt,__FILE__, __func__, __LINE__, ##__VA_ARGS__)

#endif
