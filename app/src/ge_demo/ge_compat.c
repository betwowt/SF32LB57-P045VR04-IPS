/*
 * ge_compat.c - ArtInChip MPP GE API implemented on SF32LB57 EPIC.
 *
 * Mapping:
 *   fillrect  -> drv_epic_fill()                (EPIC colour fill)
 *   bitblt    -> drv_epic_blend()               (scale + mirror + alpha)
 *   rotate    -> drv_epic_blend() with angle    (arbitrary-angle rotation)
 *
 * EPIC has no Porter-Duff ADD/XOR rule and no colour key, so those three
 * rare cases are performed by the CPU.  Every EPIC operation is split into
 * <= EPIC_COORDINATES_MAX tiles because SF32LB57 limits a transformed
 * operation to 505x505 pixels.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "bf0_hal.h"
#include "bf0_hal_epic.h"
#include "drv_epic.h"

#include "mpp_ge.h"
#include "mpp_fb.h"
#include "mpp_mem.h"
#include "aic_core.h"

#define GE_PSRAM_HEAP_SIZE (4 * 1024 * 1024)
#define GE_TILE_MAX        EPIC_COORDINATES_MAX

/* Cacheable PSRAM arena holding all effect textures and lookup tables. */
__attribute__((section(".bss.l2_cache_non_ret_bss_ge_heap"), aligned(64)))
static uint8_t             s_ge_heap_mem[GE_PSRAM_HEAP_SIZE];
static struct rt_memheap   s_ge_heap;
static int                 s_ge_heap_ready;
static struct mpp_ge       s_ge;

/* 256-entry grey palette used to feed 8-bit YUV400 textures through L8. */
__attribute__((section(".bss.l2_cache_non_ret_bss_ge_lut"), aligned(64)))
static uint32_t            s_gray_lut[256];
static int                 s_gray_lut_ready;

static void heap_ensure(void)
{
    if (!s_ge_heap_ready)
    {
        rt_memheap_init(&s_ge_heap, "ge_psram", s_ge_heap_mem, sizeof(s_ge_heap_mem));
        s_ge_heap_ready = 1;
    }
}

static void gray_lut_ensure(void)
{
    if (!s_gray_lut_ready)
    {
        for (int i = 0; i < 256; i++)
            s_gray_lut[i] = 0xFF000000u | ((uint32_t)i << 16) | ((uint32_t)i << 8) | (uint32_t)i;
        s_gray_lut_ready = 1;
    }
}

static void *heap_alloc_aligned(size_t size)
{
    if (size == 0)
        return NULL;

    if (size > SIZE_MAX - 96)
        return NULL;

    heap_ensure();

    void *raw = rt_memheap_alloc(&s_ge_heap, size + 64 + sizeof(void *));
    if (!raw)
        return NULL;

    uintptr_t aligned = ((uintptr_t)raw + sizeof(void *) + 63u) & ~(uintptr_t)63u;
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

static void heap_free_aligned(void *ptr)
{
    if (!ptr)
        return;

    void *raw = ((void **)ptr)[-1];
    rt_memheap_free(raw);
}

unsigned int mpp_phy_alloc(size_t size)
{
    return (unsigned int)(uintptr_t)heap_alloc_aligned(size);
}

void mpp_phy_free(unsigned int addr)
{
    heap_free_aligned((void *)(uintptr_t)addr);
}

void *mpp_alloc(size_t size)
{
    return heap_alloc_aligned(size);
}

void mpp_free(void *ptr)
{
    heap_free_aligned(ptr);
}

static int bytes_per_pixel(int format)
{
    switch (format)
    {
    case MPP_FMT_YUV400:
        return 1;
    case MPP_FMT_RGB_565:
        return 2;
    case MPP_FMT_RGB_888:
        return 3;
    case MPP_FMT_ARGB_8888:
    case MPP_FMT_XRGB_8888:
        return 4;
    default:
        return 0;
    }
}

static uint32_t epic_color_mode(int format)
{
    switch (format)
    {
    case MPP_FMT_YUV400:
        return EPIC_COLOR_L8;
    case MPP_FMT_RGB_565:
        return EPIC_COLOR_RGB565;
    case MPP_FMT_RGB_888:
        return EPIC_COLOR_RGB888;
    case MPP_FMT_ARGB_8888:
    case MPP_FMT_XRGB_8888:
        return EPIC_COLOR_ARGB8888;
    default:
        return EPIC_COLOR_RGB565;
    }
}

static inline void buf_rect(const struct ge_buf *buf, struct ge_rect *rect)
{
    if (buf->crop_en)
        *rect = buf->crop;
    else
    {
        rect->x      = 0;
        rect->y      = 0;
        rect->width  = buf->size.width;
        rect->height = buf->size.height;
    }
}

/* Clamp a rectangle to the buffer.  Negative origins are rejected by the
 * callers because they would require changing the scale mapping. */
static void clip_to_buffer(struct ge_rect *r, const struct ge_buf *buf)
{
    if (r->x < 0) { r->width += r->x; r->x = 0; }
    if (r->y < 0) { r->height += r->y; r->y = 0; }
    if (r->x + r->width > buf->size.width)  r->width  = buf->size.width - r->x;
    if (r->y + r->height > buf->size.height) r->height = buf->size.height - r->y;
    if (r->width < 0)  r->width = 0;
    if (r->height < 0) r->height = 0;
}

static int epic_wait(void)
{
    rt_err_t err = drv_gpu_check_done(DRV_EPIC_TIMEOUT_MS);
    return (err == RT_EOK) ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* CPU fallback: Porter-Duff ADD / XOR and colour key                        */
/* ------------------------------------------------------------------------- */

static inline uint16_t add565(uint16_t a, uint16_t b, unsigned alpha)
{
    unsigned ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    unsigned br = (((b >> 11) & 31) * alpha + 127) / 255;
    unsigned bg = (((b >> 5) & 63) * alpha + 127) / 255;
    unsigned bb = ((b & 31) * alpha + 127) / 255;

    unsigned rr = ar + br;
    unsigned rg = ag + bg;
    unsigned rb = ab + bb;

    if (rr > 31) rr = 31;
    if (rg > 63) rg = 63;
    if (rb > 31) rb = 31;

    return (uint16_t)((rr << 11) | (rg << 5) | rb);
}

static int software_bitblt(const struct ge_bitblt *blt)
{
    if (blt->src_buf.format != MPP_FMT_RGB_565 || blt->dst_buf.format != MPP_FMT_RGB_565)
        return -1;

    struct ge_rect src;
    struct ge_rect dst_full;
    struct ge_rect dst;

    buf_rect(&blt->src_buf, &src);
    buf_rect(&blt->dst_buf, &dst_full);
    clip_to_buffer(&src, &blt->src_buf);
    dst = dst_full;
    clip_to_buffer(&dst, &blt->dst_buf);

    if (src.width <= 0 || src.height <= 0 || dst.width <= 0 || dst.height <= 0)
        return -1;
    if (src.x < 0 || src.y < 0 || dst.x < 0 || dst.y < 0)
        return -1;
    if (dst_full.width <= 0 || dst_full.height <= 0)
        return -1;

    uint8_t *src_base = (uint8_t *)(uintptr_t)blt->src_buf.phy_addr[0];
    uint8_t *dst_base = (uint8_t *)(uintptr_t)blt->dst_buf.phy_addr[0];
    unsigned alpha    = blt->ctrl.src_global_alpha ? (unsigned)blt->ctrl.src_global_alpha : 255u;

    for (int y = 0; y < dst.height; y++)
    {
        int dyi = dst.y + y;
        int sy  = src.y + (int)((int64_t)(dyi - dst_full.y) * src.height / dst_full.height);
        if (blt->ctrl.flags & MPP_FLIP_V)
            sy = src.y + src.height - 1 - (sy - src.y);

        uint16_t *d = (uint16_t *)(dst_base + (size_t)dyi * blt->dst_buf.stride[0]) + dst.x;

        for (int x = 0; x < dst.width; x++)
        {
            int dxi = dst.x + x;
            int sx  = src.x + (int)((int64_t)(dxi - dst_full.x) * src.width / dst_full.width);
            if (blt->ctrl.flags & MPP_FLIP_H)
                sx = src.x + src.width - 1 - (sx - src.x);

            uint16_t s = *(uint16_t *)(src_base + (size_t)sy * blt->src_buf.stride[0] + (size_t)sx * 2);

            if (blt->ctrl.ck_en && s == (uint16_t)blt->ctrl.ck_value)
                continue;

            if (blt->ctrl.alpha_rules == GE_PD_ADD)
                d[x] = add565(d[x], s, alpha);
            else if (blt->ctrl.alpha_rules == GE_PD_XOR)
                d[x] ^= s;
            else if (alpha < 255 && blt->ctrl.alpha_rules == GE_PD_SRC_OVER)
            {
                uint16_t inv = add565(0, d[x], 255 - alpha);
                d[x]         = add565(inv, s, alpha);
            }
            else
                d[x] = s;
        }
    }

    aicos_dcache_clean_range(dst_base + (size_t)dst.y * blt->dst_buf.stride[0],
                            (size_t)dst.height * blt->dst_buf.stride[0]);
    return 0;
}

static int software_rotate(const struct ge_rotation *rot)
{
    if (rot->src_buf.format != MPP_FMT_RGB_565 || rot->dst_buf.format != MPP_FMT_RGB_565)
        return -1;

    struct ge_rect dst;
    buf_rect(&rot->dst_buf, &dst);
    clip_to_buffer(&dst, &rot->dst_buf);

    if (dst.width <= 0 || dst.height <= 0)
        return -1;

    uint8_t *src = (uint8_t *)(uintptr_t)rot->src_buf.phy_addr[0];
    uint8_t *dstp = (uint8_t *)(uintptr_t)rot->dst_buf.phy_addr[0];
    int32_t  sinv = rot->angle_sin;
    int32_t  cosv = rot->angle_cos;
    unsigned alpha = rot->ctrl.src_global_alpha ? (unsigned)rot->ctrl.src_global_alpha : 255u;

    for (int y = dst.y; y < dst.y + dst.height; y++)
    {
        uint16_t *d = (uint16_t *)(dstp + (size_t)y * rot->dst_buf.stride[0]);

        for (int x = dst.x; x < dst.x + dst.width; x++)
        {
            int dx = x - rot->dst_rot_center.x;
            int dy = y - rot->dst_rot_center.y;
            int sx = rot->src_rot_center.x + (int)((cosv * dx + sinv * dy) >> 12);
            int sy = rot->src_rot_center.y + (int)((-sinv * dx + cosv * dy) >> 12);

            if ((unsigned)sx >= (unsigned)rot->src_buf.size.width ||
                (unsigned)sy >= (unsigned)rot->src_buf.size.height)
                continue;

            uint16_t s = *(uint16_t *)(src + (size_t)sy * rot->src_buf.stride[0] + (size_t)sx * 2);

            if (rot->ctrl.alpha_rules == GE_PD_ADD)
                d[x] = add565(d[x], s, alpha);
            else if (rot->ctrl.alpha_rules == GE_PD_XOR)
                d[x] ^= s;
            else if (alpha < 255 && rot->ctrl.alpha_rules == GE_PD_SRC_OVER)
            {
                uint16_t inv = add565(0, d[x], 255 - alpha);
                d[x]         = add565(inv, s, alpha);
            }
            else
                d[x] = s;
        }
    }

    aicos_dcache_clean_range(dstp + (size_t)dst.y * rot->dst_buf.stride[0],
                            (size_t)dst.height * rot->dst_buf.stride[0]);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

struct mpp_ge *mpp_ge_open(void)
{
    if (!s_ge.opened)
    {
        drv_gpu_open();
        s_ge.opened = 1;
    }
    return &s_ge;
}

int mpp_ge_fillrect(struct mpp_ge *ge, const struct ge_fillrect *fill)
{
    (void)ge;

    struct ge_rect r;
    buf_rect(&fill->dst_buf, &r);
    clip_to_buffer(&r, &fill->dst_buf);

    if (r.width <= 0 || r.height <= 0 || r.x < 0 || r.y < 0)
        return -1;

    int bpp = bytes_per_pixel(fill->dst_buf.format);
    if (!bpp)
        return -1;


    EPIC_AreaTypeDef dst_area;
    EPIC_AreaTypeDef fill_area;

    dst_area.x0  = 0;
    dst_area.y0  = 0;
    dst_area.x1  = (int16_t)(fill->dst_buf.size.width - 1);
    dst_area.y1  = (int16_t)(fill->dst_buf.size.height - 1);
    fill_area.x0 = (int16_t)r.x;
    fill_area.y0 = (int16_t)r.y;
    fill_area.x1 = (int16_t)(r.x + r.width - 1);
    fill_area.y1 = (int16_t)(r.y + r.height - 1);

    /* GE fills are opaque; the ESP/ArtInChip shim also forces alpha 0xFF. */
    uint32_t argb = fill->start_color | 0xFF000000u;

    rt_err_t err = drv_epic_fill(epic_color_mode(fill->dst_buf.format),
                                 (uint8_t *)(uintptr_t)fill->dst_buf.phy_addr[0],
                                 &dst_area, &fill_area, argb,
                                 0, NULL, NULL, NULL);
    if (err != RT_EOK)
        return -1;

    if (epic_wait() != 0)
        return -1;

    return 0;
}

static void epic_config_common(EPIC_LayerConfigTypeDef *layer, const struct ge_buf *buf,
                               const struct ge_rect *r, int x, int y, int w, int h)
{
    HAL_EPIC_LayerConfigInit(layer);
    layer->data        = (uint8_t *)(uintptr_t)buf->phy_addr[0];
    layer->color_mode  = epic_color_mode(buf->format);
    layer->total_width = (uint16_t)buf->size.width;
    layer->x_offset    = (int16_t)x;
    layer->y_offset    = (int16_t)y;
    layer->width       = (uint16_t)w;
    layer->height      = (uint16_t)h;
    layer->color_en    = false;

    if (buf->format == MPP_FMT_YUV400)
    {
        gray_lut_ensure();
        layer->lookup_table = (uint8_t *)s_gray_lut;
    }

    (void)r;
}

static void epic_config_fg(EPIC_LayerConfigTypeDef *fg, const struct ge_buf *src,
                           const struct ge_rect *src_rect, const struct ge_rect *dst_rect,
                           const struct ge_ctrl *ctrl, unsigned alpha)
{
    int src_bpp = bytes_per_pixel(src->format);

    epic_config_common(fg, src, src_rect, dst_rect->x, dst_rect->y, src_rect->width,
                       src_rect->height);

    if (src_bpp > 0)
    {
        fg->data = (uint8_t *)(uintptr_t)src->phy_addr[0] +
                   (size_t)src_rect->y * src->stride[0] + (size_t)src_rect->x * src_bpp;
    }

    fg->alpha = (uint8_t)alpha;

    /* Scale/mirror around the source centre and centre the transformed image
     * on the destination rectangle.  With no transform this reduces to
     * x_offset/y_offset == dst_rect->x/y. */
    fg->transform_cfg.pivot_x = (int16_t)(src_rect->width / 2);
    fg->transform_cfg.pivot_y = (int16_t)(src_rect->height / 2);
    fg->x_offset = (int16_t)(dst_rect->x + (dst_rect->width - src_rect->width) / 2);
    fg->y_offset = (int16_t)(dst_rect->y + (dst_rect->height - src_rect->height) / 2);

    if (dst_rect->width > 0 && dst_rect->height > 0)
    {
        fg->transform_cfg.scale_x =
            (uint32_t)((EPIC_INPUT_SCALE_NONE * (uint64_t)src_rect->width + dst_rect->width / 2) /
                       dst_rect->width);
        fg->transform_cfg.scale_y =
            (uint32_t)((EPIC_INPUT_SCALE_NONE * (uint64_t)src_rect->height + dst_rect->height / 2) /
                       dst_rect->height);
    }

    if (ctrl->flags & MPP_FLIP_H)
        fg->transform_cfg.h_mirror = 1;
    if (ctrl->flags & MPP_FLIP_V)
        fg->transform_cfg.v_mirror = 1;

    if (ctrl->dither_en)
        fg->dither_level = EPIC_DITHER_LEVEL_MIDDLE;
}

int mpp_ge_bitblt(struct mpp_ge *ge, const struct ge_bitblt *blt)
{
    (void)ge;

    struct ge_rect src;
    struct ge_rect dst_full;
    struct ge_rect dst;

    buf_rect(&blt->src_buf, &src);
    buf_rect(&blt->dst_buf, &dst_full);
    clip_to_buffer(&src, &blt->src_buf);
    dst = dst_full;
    clip_to_buffer(&dst, &blt->dst_buf);

    if (src.width <= 0 || src.height <= 0 || dst.width <= 0 || dst.height <= 0)
        return -1;
    if (src.x < 0 || src.y < 0 || dst.x < 0 || dst.y < 0)
        return -1;

    if (blt->ctrl.alpha_rules == GE_PD_ADD || blt->ctrl.alpha_rules == GE_PD_XOR || blt->ctrl.ck_en)
        return software_bitblt(blt);

    unsigned alpha = blt->ctrl.alpha_en ? 255u : (unsigned)blt->ctrl.src_global_alpha;


    int dst_bpp = bytes_per_pixel(blt->dst_buf.format);
    if (!dst_bpp)
        return -1;

    for (int ty = dst.y; ty < dst.y + dst.height; ty += GE_TILE_MAX)
    {
        int tile_h = dst.y + dst.height - ty;
        if (tile_h > GE_TILE_MAX)
            tile_h = GE_TILE_MAX;

        for (int tx = dst.x; tx < dst.x + dst.width; tx += GE_TILE_MAX)
        {
            int tile_w = dst.x + dst.width - tx;
            if (tile_w > GE_TILE_MAX)
                tile_w = GE_TILE_MAX;

            struct ge_rect tile = {tx, ty, tile_w, tile_h};

            /* drv_epic_blend's lock path assumes layers[0]=background and
             * layers[1]=foreground, so always submit both. */
            EPIC_LayerConfigTypeDef layers[2];

            epic_config_common(&layers[0], &blt->dst_buf, &tile, tile.x, tile.y, tile.width,
                               tile.height);
            layers[0].data = (uint8_t *)(uintptr_t)blt->dst_buf.phy_addr[0] +
                             (size_t)tile.y * blt->dst_buf.stride[0] + (size_t)tile.x * dst_bpp;

            epic_config_fg(&layers[1], &blt->src_buf, &src, &dst_full, &blt->ctrl, alpha);

            EPIC_LayerConfigTypeDef out;
            epic_config_common(&out, &blt->dst_buf, &tile, tile.x, tile.y, tile.width, tile.height);
            out.data = (uint8_t *)(uintptr_t)blt->dst_buf.phy_addr[0] +
                       (size_t)tile.y * blt->dst_buf.stride[0] + (size_t)tile.x * dst_bpp;

            rt_err_t err = drv_epic_blend(layers, 2, &out, NULL);
            if (err != RT_EOK)
                return -1;

            if (epic_wait() != 0)
                return -1;
        }
    }

    return 0;
}

int mpp_ge_rotate(struct mpp_ge *ge, const struct ge_rotation *rot)
{
    (void)ge;

    struct ge_rect src;
    struct ge_rect dst;

    buf_rect(&rot->src_buf, &src);
    buf_rect(&rot->dst_buf, &dst);
    clip_to_buffer(&src, &rot->src_buf);
    clip_to_buffer(&dst, &rot->dst_buf);

    if (src.width <= 0 || src.height <= 0 || dst.width <= 0 || dst.height <= 0)
        return -1;
    if (src.x < 0 || src.y < 0 || dst.x < 0 || dst.y < 0)
        return -1;

    if (rot->ctrl.alpha_rules == GE_PD_ADD || rot->ctrl.alpha_rules == GE_PD_XOR)
        return software_rotate(rot);

    unsigned alpha = rot->ctrl.alpha_en ? 255u : (unsigned)rot->ctrl.src_global_alpha;

    int angle = (int)lroundf(atan2f((float)rot->angle_sin, (float)rot->angle_cos) *
                             (1800.0f / 3.1415926535f));
    angle = (angle + 3600) % 3600;

    /* The source centre must land on the destination centre. */
    int fg_x = rot->dst_rot_center.x - rot->src_rot_center.x;
    int fg_y = rot->dst_rot_center.y - rot->src_rot_center.y;


    int dst_bpp = bytes_per_pixel(rot->dst_buf.format);
    if (!dst_bpp)
        return -1;

    for (int ty = dst.y; ty < dst.y + dst.height; ty += GE_TILE_MAX)
    {
        int tile_h = dst.y + dst.height - ty;
        if (tile_h > GE_TILE_MAX)
            tile_h = GE_TILE_MAX;

        for (int tx = dst.x; tx < dst.x + dst.width; tx += GE_TILE_MAX)
        {
            int tile_w = dst.x + dst.width - tx;
            if (tile_w > GE_TILE_MAX)
                tile_w = GE_TILE_MAX;

            struct ge_rect tile = {tx, ty, tile_w, tile_h};

            EPIC_LayerConfigTypeDef bg;
            epic_config_common(&bg, &rot->dst_buf, &tile, tile.x, tile.y, tile.width, tile.height);
            bg.data = (uint8_t *)(uintptr_t)rot->dst_buf.phy_addr[0] +
                      (size_t)tile.y * rot->dst_buf.stride[0] + (size_t)tile.x * dst_bpp;

            EPIC_LayerConfigTypeDef fg;
            epic_config_common(&fg, &rot->src_buf, &src, fg_x, fg_y, src.width, src.height);
            fg.data = (uint8_t *)(uintptr_t)rot->src_buf.phy_addr[0] +
                      (size_t)src.y * rot->src_buf.stride[0] +
                      (size_t)src.x * bytes_per_pixel(rot->src_buf.format);
            fg.alpha                     = (uint8_t)alpha;
            fg.transform_cfg.angle       = (int16_t)angle;
            fg.transform_cfg.pivot_x     = (int16_t)rot->src_rot_center.x;
            fg.transform_cfg.pivot_y     = (int16_t)rot->src_rot_center.y;
            fg.transform_cfg.scale_x     = EPIC_INPUT_SCALE_NONE;
            fg.transform_cfg.scale_y     = EPIC_INPUT_SCALE_NONE;

            EPIC_LayerConfigTypeDef layers[2] = {bg, fg};

            EPIC_LayerConfigTypeDef out;
            epic_config_common(&out, &rot->dst_buf, &tile, tile.x, tile.y, tile.width, tile.height);
            out.data = (uint8_t *)(uintptr_t)rot->dst_buf.phy_addr[0] +
                       (size_t)tile.y * rot->dst_buf.stride[0] + (size_t)tile.x * dst_bpp;

            rt_err_t err = drv_epic_blend(layers, 2, &out, NULL);
            if (err != RT_EOK)
                return -1;

            if (epic_wait() != 0)
                return -1;
        }
    }

    return 0;
}

int mpp_ge_emit(struct mpp_ge *ge)
{
    (void)ge;
    return 0;
}

int mpp_ge_sync(struct mpp_ge *ge)
{
    (void)ge;
    return epic_wait();
}

int mpp_fb_ioctl(struct mpp_fb *fb, int command, void *arg)
{
    (void)fb;
    (void)command;
    (void)arg;
    return 0;
}
