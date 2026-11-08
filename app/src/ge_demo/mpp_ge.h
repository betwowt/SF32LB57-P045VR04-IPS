/*
 * ArtInChip MPP GE API shim for SiFli SF32LB57 EPIC.
 *
 * The 51 procedural effects were written against ArtInChip's GE API.
 * This header keeps that API surface; ge_compat.c implements it with
 * the SF32LB57 EPIC 2D engine (scaling, rotation, alpha, fill).
 */
#ifndef _MPP_GE_SHIM_H_
#define _MPP_GE_SHIM_H_

#include <stdint.h>

#define MPP_PHY_ADDR     0
#define MPP_FMT_RGB_565  1
#define MPP_FMT_RGB_888  2
#define MPP_FMT_ARGB_8888 3
#define MPP_FMT_XRGB_8888 4
#define MPP_FMT_YUV400   5

#define MPP_FLIP_H (1U << 0)
#define MPP_FLIP_V (1U << 1)

#define GE_NO_GRADIENT 0
#define GE_PD_SRC_OVER 1
#define GE_PD_ADD     2
#define GE_PD_XOR     3

struct mpp_ge
{
    int opened;
};

struct ge_point
{
    int x;
    int y;
};

struct ge_rect
{
    int x;
    int y;
    int width;
    int height;
};

struct ge_size
{
    int width;
    int height;
};

struct ge_buf
{
    int          buf_type;
    unsigned int phy_addr[3];
    int          stride[3];
    struct ge_size size;
    int          format;
    int          crop_en;
    struct ge_rect crop;
};

struct ge_ctrl
{
    int          alpha_en;
    int          alpha_rules;
    int          src_alpha_mode;
    int          src_global_alpha;
    unsigned int flags;
    int          ck_en;
    uint32_t     ck_value;
    int          dither_en;
};

struct ge_bitblt
{
    struct ge_buf  src_buf;
    struct ge_buf  dst_buf;
    struct ge_ctrl ctrl;
};

struct ge_fillrect
{
    int            type;
    uint32_t       start_color;
    struct ge_buf  dst_buf;
};

struct ge_rotation
{
    struct ge_buf  src_buf;
    struct ge_buf  dst_buf;
    int            angle_sin;
    int            angle_cos;
    struct ge_point src_rot_center;
    struct ge_point dst_rot_center;
    struct ge_ctrl ctrl;
};

struct mpp_ge *mpp_ge_open(void);
int mpp_ge_bitblt(struct mpp_ge *ge, const struct ge_bitblt *blt);
int mpp_ge_fillrect(struct mpp_ge *ge, const struct ge_fillrect *fill);
int mpp_ge_rotate(struct mpp_ge *ge, const struct ge_rotation *rot);
int mpp_ge_emit(struct mpp_ge *ge);
int mpp_ge_sync(struct mpp_ge *ge);

#endif /* _MPP_GE_SHIM_H_ */
