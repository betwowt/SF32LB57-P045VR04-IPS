#ifndef _MPP_FB_SHIM_H_
#define _MPP_FB_SHIM_H_

#include <stdint.h>

/* ArtInChip display-engine controls.  SF32LB57's LCDC has no CCM/gamma/HSBC
 * equivalent exposed by the DPI path, so these are accepted and ignored. */
#define AICFB_UPDATE_CCM_CONFIG   1
#define AICFB_UPDATE_GAMMA_CONFIG 2
#define AICFB_SET_DISP_PROP       3

struct mpp_fb
{
    int unused;
};

struct aicfb_screeninfo
{
    void *framebuffer;
    int   width;
    int   height;
    int   stride;
    int   format;
};

struct aicfb_ccm_config
{
    int     enable;
    int32_t ccm_table[12];
};

struct aicfb_gamma_config
{
    int     enable;
    uint8_t gamma_lut[3][256];
};

struct aicfb_disp_prop
{
    int bright;
    int contrast;
    int saturation;
    int hue;
};

int mpp_fb_ioctl(struct mpp_fb *fb, int command, void *arg);

#endif /* _MPP_FB_SHIM_H_ */
