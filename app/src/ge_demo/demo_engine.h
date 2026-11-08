/*
 * demo_engine.h - SF32LB57/EPIC port of the ArtInChip GE demo engine.
 */
#ifndef _DEMO_ENGINE_H_
#define _DEMO_ENGINE_H_

#include <rtthread.h>
#include "aic_core.h"
#include "mpp_fb.h"
#include "mpp_ge.h"
#include "artinchip_fb.h"
#include "aic_drv_ge.h"
#include "demo_utils.h"

/* P045VR04-IPS: 480x854 RGB565, portrait native scan. */
#define DEMO_SCREEN_WIDTH  480
#define DEMO_SCREEN_HEIGHT 854

/* Low-resolution procedural canvas.  It keeps the panel aspect ratio
 * (480/854 == 240/427) so the 2x EPIC upscale is uniform. */
#define DEMO_QVGA_W 240
#define DEMO_QVGA_H 427

struct demo_ctx
{
    struct mpp_fb          *fb;
    struct mpp_ge          *ge;
    struct aicfb_screeninfo info;
    int                     screen_w;
    int                     screen_h;
};

struct effect_ops
{
    const char *name;
    int  (*init)(struct demo_ctx *ctx);
    void (*draw)(struct demo_ctx *ctx, unsigned long phy_addr);
    void (*deinit)(struct demo_ctx *ctx);
    bool is_vi_isolated;
};

/* The SF32 port registers effects explicitly in effect_registry.c, so the
 * linker-section based auto registration is compiled out. */
#define REGISTER_EFFECT(ops_struct)

void demo_core_init(void);
void demo_core_start(void);
void demo_next_effect(void);
void demo_prev_effect(void);
void demo_jump_effect(int index);

#endif /* _DEMO_ENGINE_H_ */
