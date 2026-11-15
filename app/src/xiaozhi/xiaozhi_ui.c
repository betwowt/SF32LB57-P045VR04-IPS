/*
 * Round-face UI for the xiaozhi port on SF32LB57 (P045VR04 480x854 panel).
 *
 * Replaces the vendor watch UI: a black screen with one big round face, a
 * clock and a status line. Everything is drawn with LVGL primitives, so the
 * app no longer depends on the ezip emoji assets or the TTF font.
 *
 * Threading: only the xz_ui thread touches LVGL. Every other thread posts a
 * fixed-size event into a mailbox; the UI loop drains it before each
 * lv_task_handler() pass.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#include "rtthread.h"
#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "ulog.h"
#include "rtdevice.h"
#include "mem_section.h"
#include "bf0_hal.h"

#include "xiaozhi_ui.h"
#include "xiaozhi_client_public.h"
#include "xiaozhi_websocket.h"
#include "lv_tiny_ttf.h"

extern const unsigned char xiaozhi_font[];
extern const int xiaozhi_font_size;

#define TAG "xz_ui"

/* ---------------------------------------------------------------- layout */
#define SCR_W           480
#define SCR_H           854

#define FACE_X          50
#define FACE_Y          130
#define FACE_SZ         380

#define EYE_W           56
#define EYE_H           66
#define EYE_WIDE_W      60
#define EYE_WIDE_H      74
#define EYE_CLOSED_H    8

/* Stretch a true circle, rather than rounding a rectangular object's corners.
 * Speaking stays 84 px wide and 48..64 px high, centred below the cheeks. */
#define MOUTH_DIAM      56
#define MOUTH_CX        190
#define MOUTH_CY        296
static const int16_t mouth_wave[] = {48, 56, 64, 56, 50, 48};

#define COL_FACE        0xFFC94A
#define COL_INK         0x2B2118
#define COL_BLUSH       0xFF8A80
#define COL_RING        0x35D6F3
#define COL_RING_THINK  0xFFB020
#define COL_TEXT        0xE8E8E8
#define COL_DIM         0x8A8A8A

#define GLOW_PAD        36
#define BACKDROP_SZ     (FACE_SZ + 2 * GLOW_PAD)
#define DISC_TEX_SZ     56
/* One immutable RGB565 source, rendered once by the UI thread. No LVGL heap
 * allocation and no rounded-fill/mask operations during screen refresh. */
L2_RET_BSS_SECT_BEGIN(xz_face_pixels)
static uint16_t face_pixels[BACKDROP_SZ * BACKDROP_SZ]
    L2_RET_BSS_SECT(xz_face_pixels) ALIGN(64);
static uint8_t ink_pixels[DISC_TEX_SZ * DISC_TEX_SZ * 2]
    L2_RET_BSS_SECT(xz_face_pixels) ALIGN(64);
static uint8_t blush_pixels[DISC_TEX_SZ * DISC_TEX_SZ * 2]
    L2_RET_BSS_SECT(xz_face_pixels) ALIGN(64);
L2_RET_BSS_SECT_END
static const lv_image_dsc_t face_image = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
               .w = BACKDROP_SZ, .h = BACKDROP_SZ, .stride = BACKDROP_SZ * 2},
    .data_size = sizeof(face_pixels),
    .data = (const uint8_t *)face_pixels,
};
static const lv_image_dsc_t ink_image = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
               .w = DISC_TEX_SZ, .h = DISC_TEX_SZ, .stride = DISC_TEX_SZ * 2},
    .data_size = sizeof(ink_pixels), .data = ink_pixels,
};
static const lv_image_dsc_t blush_image = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
               .w = DISC_TEX_SZ, .h = DISC_TEX_SZ, .stride = DISC_TEX_SZ * 2},
    .data_size = sizeof(blush_pixels), .data = blush_pixels,
};

/* ------------------------------------------------------ small ui textures
 * EPIC's hardware path corrupts rounded fills and rounded thick lines on
 * this panel (dashes, notches, torn edges) while plain RGB565 image blits
 * render cleanly. So the mouth and both status-bar icons are rasterised in
 * software here and blitted as images. */
#define MOUTH_TW        100
#define MOUTH_TH        40
#define SIG_TW          44
#define SIG_TH          22
#define BAT_TW          60
#define BAT_TH          28
#define BAT_FILL_TW     48
#define BAT_FILL_TH     20

/* Listening/thinking ring: baked as an 8-bit alpha mask so EPIC's arc path
 * (which draws chunked, stair-stepped gaps and mallocs a mask buffer per
 * frame) is not used at all. The image is recoloured to the state colour. */
#define RING_SZ         426
#define RING_R          209     /* stroke centre radius, arc_width 8 */
#define RING_HW         4
#define RING_SPAN       70
/* The arc is drawn straight into the backdrop image: ring pixel (0,0) sits
 * RING_OFF pixels inside the backdrop, both share the face centre. */
#define RING_OFF        (GLOW_PAD - 23)

enum { MOUTH_SMILE = 0, MOUTH_FROWN = 1, MOUTH_FLAT = 2, MOUTH_STYLES = 3 };

/* One texture per visual variant, swapped with lv_image_set_src. The image
 * cache is off (LV_CACHE_DEF_SIZE=0) so this is not required for correctness,
 * but it keeps the per-state redraw to a single dsc pointer change. */
L2_RET_BSS_SECT_BEGIN(xz_ui_tex)
static uint16_t mouth_pixels[MOUTH_STYLES][MOUTH_TW * MOUTH_TH]
L2_RET_BSS_SECT(xz_ui_tex) ALIGN(64);
static uint16_t sig_pixels[2][SIG_TW * SIG_TH]
L2_RET_BSS_SECT(xz_ui_tex) ALIGN(64);
static uint16_t bat_shell_pixels[BAT_TW * BAT_TH]
L2_RET_BSS_SECT(xz_ui_tex) ALIGN(64);
static uint16_t bat_fill_pixels[2][BAT_FILL_TW * BAT_FILL_TH]
L2_RET_BSS_SECT(xz_ui_tex) ALIGN(64);
L2_RET_BSS_SECT_END

#define XZ_TEX(pix, tw, th)                                            \
    {                                                                  \
        .header = {.magic = LV_IMAGE_HEADER_MAGIC,                     \
                   .cf = LV_COLOR_FORMAT_RGB565,                       \
                   .w = (tw), .h = (th), .stride = (tw) * 2},          \
        .data_size = sizeof(pix), .data = (const uint8_t *)(pix),      \
    }

static const lv_image_dsc_t mouth_images[MOUTH_STYLES] = {
    XZ_TEX(mouth_pixels[MOUTH_SMILE], MOUTH_TW, MOUTH_TH),
    XZ_TEX(mouth_pixels[MOUTH_FROWN], MOUTH_TW, MOUTH_TH),
    XZ_TEX(mouth_pixels[MOUTH_FLAT], MOUTH_TW, MOUTH_TH),
};
static const lv_image_dsc_t sig_images[2] = {
    XZ_TEX(sig_pixels[0], SIG_TW, SIG_TH),
    XZ_TEX(sig_pixels[1], SIG_TW, SIG_TH),
};
static const lv_image_dsc_t bat_shell_image =
    XZ_TEX(bat_shell_pixels, BAT_TW, BAT_TH);
static const lv_image_dsc_t bat_fill_images[2] = {
    XZ_TEX(bat_fill_pixels[0], BAT_FILL_TW, BAT_FILL_TH),
    XZ_TEX(bat_fill_pixels[1], BAT_FILL_TW, BAT_FILL_TH),
};

static uint16_t tex_rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void tex_clear(uint16_t *tex, int tw, int th, uint32_t bg)
{
    uint16_t v = tex_rgb565((bg >> 16) & 255, (bg >> 8) & 255, bg & 255);

    for (int i = 0; i < tw * th; i++)
        tex[i] = v;
}

static void tex_blend(uint16_t *tex, int tw, int th, int x, int y,
                      unsigned cov, uint32_t fg, uint32_t bg)
{
    unsigned a, r, g, b;

    if (cov == 0 || x < 0 || y < 0 || x >= tw || y >= th)
        return;
    a = cov * 255u / 16u;
    r = (((fg >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a) + 127) / 255;
    g = (((fg >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a) + 127) / 255;
    b = ((fg & 255) * a + (bg & 255) * (255 - a) + 127) / 255;
    tex[y * tw + x] = tex_rgb565(r, g, b);
}

/* Axis-aligned rounded rectangle, 4x4 supersampled edges. */
static void tex_rrect(uint16_t *tex, int tw, int th, int x0, int y0,
                      int w, int h, int rad, uint32_t fg, uint32_t bg)
{
    int r8 = rad * 8;
    int cx8 = (2 * x0 + w) * 4;
    int cy8 = (2 * y0 + h) * 4;
    int hw8 = w * 4 - r8;
    int hh8 = h * 4 - r8;

    for (int y = y0 - 1; y <= y0 + h; y++)
        for (int x = x0 - 1; x <= x0 + w; x++)
        {
            unsigned cov = 0;

            for (int sy = -3; sy <= 3; sy += 2)
                for (int sx = -3; sx <= 3; sx += 2)
                {
                    int px = 8 * x + 4 + sx;
                    int py = 8 * y + 4 + sy;
                    int qx = px - cx8, qy = py - cy8;

                    if (qx < 0)
                        qx = -qx;
                    if (qy < 0)
                        qy = -qy;
                    qx -= hw8;
                    qy -= hh8;
                    if (qx < 0)
                        qx = 0;
                    if (qy < 0)
                        qy = 0;
                    if (qx * qx + qy * qy <= r8 * r8)
                        cov++;
                }
            tex_blend(tex, tw, th, x, y, cov, fg, bg);
        }
}

static int seg_dist2(int px, int py, int x1, int y1, int x2, int y2)
{
    int dx = x2 - x1, dy = y2 - y1;
    int len2 = dx * dx + dy * dy;
    int64_t ex, ey, t;

    if (len2 == 0)
    {
        ex = px - x1;
        ey = py - y1;
        return (int)(ex * ex + ey * ey);
    }
    t = (int64_t)(px - x1) * dx + (int64_t)(py - y1) * dy;
    if (t <= 0)
    {
        ex = px - x1;
        ey = py - y1;
        return (int)(ex * ex + ey * ey);
    }
    if (t >= len2)
    {
        ex = px - x2;
        ey = py - y2;
        return (int)(ex * ex + ey * ey);
    }
    ex = px - x1;
    ey = py - y1;
    /* t*t reaches 1e10 on eighth-pixel coordinates, so it must not be int. */
    return (int)(ex * ex + ey * ey - t * t / len2);
}

/* Thick polyline with round caps, 4x4 supersampled. Points in pixels. */
static void tex_polyline(uint16_t *tex, int tw, int th, const int *pts, int n,
                         int halfw, uint32_t fg, uint32_t bg)
{
    int hw8 = halfw * 8;
    int lim = hw8 * hw8;
    int minx = pts[0], maxx = minx;
    int miny = pts[1], maxy = miny;
    int x0, x1, y0, y1;

    for (int i = 1; i < n; i++)
    {
        int x = pts[2 * i], y = pts[2 * i + 1];

        if (x < minx)
            minx = x;
        if (x > maxx)
            maxx = x;
        if (y < miny)
            miny = y;
        if (y > maxy)
            maxy = y;
    }

    x0 = minx - halfw - 1;
    x1 = maxx + halfw + 1;
    y0 = miny - halfw - 1;
    y1 = maxy + halfw + 1;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > tw - 1)
        x1 = tw - 1;
    if (y1 > th - 1)
        y1 = th - 1;

    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
        {
            unsigned cov = 0;

            for (int sy = -3; sy <= 3; sy += 2)
                for (int sx = -3; sx <= 3; sx += 2)
                {
                    int px = 8 * x + 4 + sx;
                    int py = 8 * y + 4 + sy;
                    int best = lim + 1;

                    for (int i = 0; i + 1 < n; i++)
                    {
                        int d2 = seg_dist2(px, py, 8 * pts[2 * i], 8 * pts[2 * i + 1],
                                           8 * pts[2 * i + 2], 8 * pts[2 * i + 3]);
                        if (d2 < best)
                            best = d2;
                    }
                    if (best <= lim)
                        cov++;
                }
            tex_blend(tex, tw, th, x, y, cov, fg, bg);
        }
}

static void tex_flush(const uint16_t *tex, int bytes)
{
    SCB_CleanDCache_by_Addr((uint32_t *)tex, bytes);
}

/* ------------------------------------------------- pre-rendered elements */
static void make_mouth_texture(int style)
{
    static const int p_smile[] = {6, 14, 28, 27, 50, 32, 72, 27, 94, 14};
    static const int p_flat[]  = {6, 20, 50, 20, 94, 20};
    static const int p_frown[] = {6, 28, 28, 15, 50, 10, 72, 15, 94, 28};
    const int *p;
    int n;

    if (style == MOUTH_FROWN)
    {
        p = p_frown;
        n = 5;
    }
    else if (style == MOUTH_FLAT)
    {
        p = p_flat;
        n = 3;
    }
    else
    {
        p = p_smile;
        n = 5;
    }

    uint16_t *tex = mouth_pixels[style];

    tex_clear(tex, MOUTH_TW, MOUTH_TH, COL_FACE);
    tex_polyline(tex, MOUTH_TW, MOUTH_TH, p, n, 5, COL_INK, COL_FACE);
    tex_flush(tex, MOUTH_TW * MOUTH_TH * 2);
}

static void make_sig_texture(bool on)
{
    uint16_t *tex = sig_pixels[on ? 1 : 0];
    uint32_t fg = on ? COL_TEXT : 0x3A3A3A;

    tex_clear(tex, SIG_TW, SIG_TH, 0x000000);
    for (int i = 0; i < 4; i++)
    {
        int h = 4 + i * 4;

        tex_rrect(tex, SIG_TW, SIG_TH, 2 + i * 10, 20 - h, 6, h, 2, fg, 0x000000);
    }
    tex_flush(tex, SIG_TW * SIG_TH * 2);
}

static void make_bat_textures(void)
{
    static const uint32_t fills[2] = {0xD8D8D8, 0x4CD964};
    static const int widths[2] = {14, 30};

    /* shell: outline ring plus the positive terminal nub */
    tex_clear(bat_shell_pixels, BAT_TW, BAT_TH, 0x000000);
    tex_rrect(bat_shell_pixels, BAT_TW, BAT_TH, 0, 0, 56, 28, 7, COL_TEXT, 0x000000);
    tex_rrect(bat_shell_pixels, BAT_TW, BAT_TH, 2, 3, 52, 22, 5, 0x000000, 0x000000);
    tex_rrect(bat_shell_pixels, BAT_TW, BAT_TH, 55, 9, 5, 10, 2, COL_TEXT, 0x000000);
    tex_flush(bat_shell_pixels, BAT_TW * BAT_TH * 2);

    /* fill: the black remainder is invisible against the shell interior */
    for (int i = 0; i < 2; i++)
    {
        uint16_t *tex = bat_fill_pixels[i];

        tex_clear(tex, BAT_FILL_TW, BAT_FILL_TH, 0x000000);
        tex_rrect(tex, BAT_FILL_TW, BAT_FILL_TH, 0, 0, widths[i], BAT_FILL_TH, 3,
                  fills[i], 0x000000);
        tex_flush(tex, BAT_FILL_TW * BAT_FILL_TH * 2);
    }
}

/* Every variant is baked once; the runtime only swaps dsc pointers. */
static void make_ui_textures(void)
{
    make_mouth_texture(MOUTH_SMILE);
    make_mouth_texture(MOUTH_FROWN);
    make_mouth_texture(MOUTH_FLAT);
    make_sig_texture(false);
    make_sig_texture(true);
    make_bat_textures();
}

/* --------------------------------------------------------------- events */
enum {
    EV_STATUS = 1,
    EV_EMOJI,
    EV_TEXT,
    EV_TTS,
    EV_CHARGE,
    EV_NET,
    EV_VOLUME,
    EV_BRIGHT,
    EV_SLEEP,
    EV_WAKE,
    EV_CONFIG,
    EV_UI_CALL,
    EV_VERIFY, /* temporary hardware verification; removed from release */
    EV_FACE_STATE,
    EV_DIM,
    EV_MUSIC,
};

typedef struct {
    void (*fn)(void *);
    void *ctx;
    rt_sem_t done;
} ui_call_t;

typedef struct {
    uint8_t type;
    uint8_t val;
    char text[96];
    ui_call_t *call;
} ui_evt_t;

uint8_t vad_enable = 1;   /* 1 = do not interrupt while speaking */
uint8_t aec_enabled = 1;

/* referenced by main.c / websocket / mqtt */
lv_timer_t *ui_sleep_timer = NULL;
rt_tick_t last_listen_tick = 0;
lv_obj_t *standby_screen = NULL;

static rt_mq_t ui_mq;
static rt_thread_t ui_owner;
static face_state_t g_state = FS_CONNECT;
static char g_emoji[16] = "neutral";
static char g_text[96] = {0};
static bool g_net, g_charge = true, g_dimmed;
static uint32_t g_blink_wait, g_speak_phase;
static uint16_t g_ring_ang;
static bool g_verify;
static int g_verify_height;

static lv_obj_t *scr;
static lv_obj_t *face, *eye_l, *eye_r, *blush_l, *blush_r;
static lv_obj_t *mouth_img, *mouth_open, *backdrop_obj;
static lv_obj_t *lbl_clock, *lbl_status, *lbl_text;
static char g_music[96];
static bool g_music_paused;
static lv_font_t *music_font;
static lv_obj_t *bat_shell, *bat_fill;
static lv_obj_t *wf_icon;

/* -------------------------------------------------------------- helpers */
/* Bake edge alpha and cheek opacity against the fixed face colour. Plain
 * RGB565 blits also bypass EPIC's faulty mask path on the small features. */
static void make_disc_texture(uint8_t *pixels, uint32_t color, unsigned opacity)
{
    for (int y = 0; y < DISC_TEX_SZ; y++)
        for (int x = 0; x < DISC_TEX_SZ; x++)
        {
            int dx = 8 * x - 4 * (DISC_TEX_SZ - 1);
            int dy = 8 * y - 4 * (DISC_TEX_SZ - 1);
            unsigned coverage = 0;
            for (int sy = -3; sy <= 3; sy += 2)
                for (int sx = -3; sx <= 3; sx += 2)
                    if ((dx + sx) * (dx + sx) + (dy + sy) * (dy + sy) <
                        (DISC_TEX_SZ * 4) * (DISC_TEX_SZ * 4)) coverage++;
            int index = y * DISC_TEX_SZ + x;
            unsigned alpha = (coverage * opacity + 8) / 16;
            unsigned r = (((color >> 16) & 255) * alpha + ((COL_FACE >> 16) & 255) * (255-alpha) + 127) / 255;
            unsigned g = (((color >> 8) & 255) * alpha + ((COL_FACE >> 8) & 255) * (255-alpha) + 127) / 255;
            unsigned b = ((color & 255) * alpha + (COL_FACE & 255) * (255-alpha) + 127) / 255;
            uint16_t packed = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            pixels[index * 2] = packed & 255;
            pixels[index * 2 + 1] = packed >> 8;
        }
    SCB_CleanDCache_by_Addr((uint32_t *)pixels, DISC_TEX_SZ * DISC_TEX_SZ * 2);
}

static lv_obj_t *mk_ellipse(lv_obj_t *parent, int x, int y, int w, int h,
                            const lv_image_dsc_t *source)
{
    lv_obj_t *o = lv_image_create(parent);
    lv_obj_remove_style_all(o);
    lv_image_set_src(o, source);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_image_set_inner_align(o, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* Rasterize the four concentric discs directly, including 4x4 edge coverage.
 * Coordinates use eighth-pixels so even-sized discs share the same centre.
 * This avoids the EPIC rounded-fill AND arc/border mask blend paths. */
/* Concentric glow discs, evaluated per pixel. Shared by the one-off backdrop
 * bake and by the listening ring, which restores these colours when it erases
 * its previous arc. dx8/dy8 are eighth-pixels from the face centre. */
static void backdrop_rgb8(int dx8, int dy8, unsigned *pr, unsigned *pg, unsigned *pb)
{
    static const uint32_t colors[] = {0x120F06, 0x1E1809, 0x2C230E, COL_FACE};
    int d2 = dx8 * dx8 + dy8 * dy8;
    unsigned r = 0, g = 0, b = 0;

    for (int i = 0; i < 4; i++)
    {
        int radius = (BACKDROP_SZ / 2 - i * 12) * 8;
        unsigned coverage = 16;

        if (d2 >= (radius + 6) * (radius + 6))
            continue;
        if (d2 > (radius - 6) * (radius - 6))
        {
            coverage = 0;
            for (int sy = -3; sy <= 3; sy += 2)
                for (int sx = -3; sx <= 3; sx += 2)
                {
                    int qx = dx8 + sx;
                    int qy = dy8 + sy;

                    if (qx * qx + qy * qy < radius * radius)
                        coverage++;
                }
        }
        r = (r * (16 - coverage) + (colors[i] >> 16) * coverage + 8) / 16;
        g = (g * (16 - coverage) + ((colors[i] >> 8) & 255) * coverage + 8) / 16;
        b = (b * (16 - coverage) + (colors[i] & 255) * coverage + 8) / 16;
    }
    *pr = r;
    *pg = g;
    *pb = b;
}

static uint16_t rgb565_pack(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void make_face_backdrop(void)
{
    for (int y = 0; y < BACKDROP_SZ; y++)
    {
        int dy = 8 * y - 4 * (BACKDROP_SZ - 1);

        for (int x = 0; x < BACKDROP_SZ; x++)
        {
            unsigned r, g, b;

            backdrop_rgb8(8 * x - 4 * (BACKDROP_SZ - 1), dy, &r, &g, &b);
            face_pixels[y * BACKDROP_SZ + x] = rgb565_pack(r, g, b);
        }
    }
    SCB_CleanDCache_by_Addr((uint32_t *)face_pixels, sizeof(face_pixels));

    backdrop_obj = lv_image_create(scr);
    lv_image_set_src(backdrop_obj, &face_image);
    lv_obj_set_pos(backdrop_obj, FACE_X - GLOW_PAD, FACE_Y - GLOW_PAD);
    lv_obj_clear_flag(backdrop_obj, LV_OBJ_FLAG_CLICKABLE);
}

/* Listening/thinking ring. EPIC's arc path draws it as chunked, stair-stepped
 * dashes and mallocs a mask buffer per frame, so the arc is instead composited
 * straight into the backdrop image and only the band texels are rewritten. */
static int s_ring_ang = -1;

/* Blend one arc into the backdrop; erase restores the glow colours. */
static void ring_blit(int ang, uint32_t rgb, bool erase)
{
    const int cx = RING_SZ / 2;
    const int cy = RING_SZ / 2;
    const int r_in = RING_R - RING_HW - 1;
    const int r_out = RING_R + RING_HW + 1;
    const int r_in2 = r_in * r_in;
    const int r_out2 = r_out * r_out;
    const float a0 = (float)ang * 0.0174532925f;
    const float a1 = (float)(ang + RING_SPAN) * 0.0174532925f;
    const float bx0 = cosf(a0), by0 = sinf(a0);
    const float bx1 = cosf(a1), by1 = sinf(a1);
    const float ref = (float)RING_R;
    const float half = (float)(RING_HW + 1);

    for (int y = 0; y < RING_SZ; y++)
    {
        const int dy = y - cy;
        const int dy2 = dy * dy;
        const int by = y + RING_OFF;
        int xo, xi;

        if (dy2 > r_out2 || by < 0 || by >= BACKDROP_SZ)
            continue;
        xo = (int)sqrtf((float)(r_out2 - dy2));
        xi = (dy2 < r_in2) ? (int)sqrtf((float)(r_in2 - dy2)) : 0;

        for (int side = 0; side < 2; side++)
        {
            int xs = side ? cx - xo : cx + xi;
            int xe = side ? cx - xi : cx + xo;

            for (int x = xs; x <= xe; x++)
            {
                const int dx = x - cx;
                const int bx = x + RING_OFF;
                const float rad = sqrtf((float)(dx * dx + dy2));
                float cov, cov_ang;
                unsigned r, g, b;

                if (bx < 0 || bx >= BACKDROP_SZ)
                    continue;

                cov = half - fabsf(rad - ref);
                cov_ang = bx0 * (float)dy - by0 * (float)dx;
                {
                    float edge = (float)dx * by1 - (float)dy * bx1;

                    if (edge < cov_ang)
                        cov_ang = edge;
                }
                cov_ang += 0.5f;
                if (cov_ang < cov)
                    cov = cov_ang;
                if (cov <= 0.0f)
                    continue;
                if (cov > 1.0f)
                    cov = 1.0f;

                backdrop_rgb8(8 * x - 4 * (RING_SZ - 1), 8 * y - 4 * (RING_SZ - 1),
                              &r, &g, &b);
                if (!erase)
                {
                    unsigned a = (unsigned)(cov * 255.0f);

                    r = (r * (255 - a) + ((rgb >> 16) & 255) * a) / 255;
                    g = (g * (255 - a) + ((rgb >> 8) & 255) * a) / 255;
                    b = (b * (255 - a) + (rgb & 255) * a) / 255;
                }
                face_pixels[by * BACKDROP_SZ + bx] = rgb565_pack(r, g, b);
            }
        }
    }
    SCB_CleanDCache_by_Addr((uint32_t *)face_pixels, sizeof(face_pixels));
}

static void ring_update(int ang, uint32_t rgb)
{
    if (s_ring_ang >= 0)
        ring_blit(s_ring_ang, rgb, true);
    ring_blit(ang, rgb, false);
    s_ring_ang = ang;
    lv_obj_invalidate(backdrop_obj);
}

static void ring_clear(void)
{
    if (s_ring_ang < 0)
        return;
    ring_blit(s_ring_ang, 0, true);
    s_ring_ang = -1;
    lv_obj_invalidate(backdrop_obj);
}


static bool ascii_only(const char *s)
{
    if (!s || !*s) return false;
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        /* Line breaks are fine: lv_label renders them, and the binding-code
         * hint is multi-line. Everything else must be printable ASCII. */
        if (c == '\n' || c == '\r') continue;
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static const char *state_label(face_state_t st)
{
    switch (st)
    {
    case FS_CONNECT: return "Connecting";
    case FS_IDLE:    return "Standby";
    case FS_LISTEN:  return "Listening";
    case FS_SPEAK:   return "Speaking";
    case FS_THINK:   return "Thinking";
    case FS_ERROR:   return "No network";
    case FS_SLEEP:   return "Sleeping";
    case FS_CONFIG:  return "Wi-Fi setup";
    }
    return "";
}

/* ------------------------------------------------------------ face draw */
static void mouth_set_height(int height)
{
    lv_obj_set_style_transform_scale_y(mouth_open, height * 256 / MOUTH_DIAM, 0);
}

static void mouth_set_style(int style)
{
    static int current = -1;

    if (current == style)
        return;
    current = style;
    lv_image_set_src(mouth_img, &mouth_images[style]);
}

static void apply_visual(void)
{
    bool smile = false, frown = false, open = false, wide = false, closed = false;
    const char *e = g_emoji;

    if (!strcmp(e, "happy") || !strcmp(e, "laughing") || !strcmp(e, "funny") ||
        !strcmp(e, "loving") || !strcmp(e, "kissy") || !strcmp(e, "cool") ||
        !strcmp(e, "wink"))
        smile = true;
    else if (!strcmp(e, "sad") || !strcmp(e, "crying") || !strcmp(e, "angry") ||
             !strcmp(e, "embarrassed"))
        frown = true;
    else if (!strcmp(e, "surprised") || !strcmp(e, "shocked"))
    {
        open = true;
        wide = true;
    }
    else if (!strcmp(e, "thinking"))
        open = true;
    else if (!strcmp(e, "sleepy"))
        closed = true;

    switch (g_state)
    {
    case FS_CONNECT:
        smile = false; frown = false; open = false; closed = false;
        break;
    case FS_IDLE:
        open = false;
        break;
    case FS_LISTEN:
        open = true; wide = true; closed = false;
        break;
    case FS_SPEAK:
        open = true; closed = false;
        break;
    case FS_THINK:
        open = true; closed = false;
        break;
    case FS_ERROR:
        frown = true; smile = false; open = false;
        break;
    case FS_SLEEP:
        closed = true; smile = false; frown = false; open = false;
        break;
    case FS_CONFIG:
        open = true; smile = false; frown = false; closed = false;
        break;
    }

    if (closed)
    {
        lv_obj_set_height(eye_l, EYE_CLOSED_H);
        lv_obj_set_height(eye_r, EYE_CLOSED_H);
        lv_obj_set_y(eye_l, 180);
        lv_obj_set_y(eye_r, 180);
        lv_obj_set_width(eye_l, EYE_W);
        lv_obj_set_width(eye_r, EYE_W);
    }
    else if (wide)
    {
        lv_obj_set_width(eye_l, EYE_WIDE_W);
        lv_obj_set_width(eye_r, EYE_WIDE_W);
        lv_obj_set_height(eye_l, EYE_WIDE_H);
        lv_obj_set_height(eye_r, EYE_WIDE_H);
        lv_obj_set_y(eye_l, 146);
        lv_obj_set_y(eye_r, 146);
    }
    else
    {
        lv_obj_set_width(eye_l, EYE_W);
        lv_obj_set_width(eye_r, EYE_W);
        lv_obj_set_height(eye_l, EYE_H);
        lv_obj_set_height(eye_r, EYE_H);
        lv_obj_set_y(eye_l, 150);
        lv_obj_set_y(eye_r, 150);
    }

    if (open && g_state != FS_SLEEP)
    {
        lv_obj_add_flag(mouth_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(mouth_open, LV_OBJ_FLAG_HIDDEN);
        mouth_set_height(MOUTH_DIAM);
    }
    else
    {
        lv_obj_clear_flag(mouth_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_open, LV_OBJ_FLAG_HIDDEN);
        if (frown)
            mouth_set_style(MOUTH_FROWN);
        else if (smile)
            mouth_set_style(MOUTH_SMILE);
        else
            mouth_set_style(MOUTH_FLAT);
    }

    if (g_state != FS_LISTEN && g_state != FS_THINK)
        ring_clear();

    if (g_music[0])
    {
        char status[128];
        rt_snprintf(status, sizeof(status), "%s%s", g_music_paused ? "Paused: " : "Music: ", g_music);
        lv_obj_set_style_text_font(lbl_status, music_font ? music_font : &lv_font_montserrat_24, 0);
        lv_label_set_text(lbl_status, status);
    }
    else
    {
        lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_24, 0);
        lv_label_set_text(lbl_status, state_label(g_state));
    }
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(g_state == FS_ERROR ? 0xFF6B6B : COL_TEXT), 0);

    if (g_text[0])
        lv_label_set_text(lbl_text, g_text);
    else
        lv_label_set_text(lbl_text, "");

    /* talking mouth keeps its own animation in the timer */
    if (g_state == FS_SPEAK)
        g_speak_phase = 0;
}

/* ---------------------------------------------------------------- timers */
static void blink_cb(void *var, int32_t v)
{
    (void)var;
    if (g_state == FS_SLEEP) return;
    lv_obj_set_height(eye_l, v);
    lv_obj_set_height(eye_r, v);
}

static void do_blink(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eye_l);
    lv_anim_set_values(&a, EYE_H, EYE_CLOSED_H);
    lv_anim_set_time(&a, 70);
    lv_anim_set_playback_time(&a, 70);
    lv_anim_set_repeat_count(&a, 1);
    lv_anim_set_exec_cb(&a, blink_cb);
    lv_anim_start(&a);
}

static void face_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (g_state == FS_LISTEN || g_state == FS_THINK)
    {
        g_ring_ang = (g_ring_ang + 7) % 360;
        ring_update(g_ring_ang, g_state == FS_LISTEN ? COL_RING : COL_RING_THINK);
    }

    if (g_state == FS_SPEAK && !lv_obj_has_flag(mouth_open, LV_OBJ_FLAG_HIDDEN))
    {
        g_speak_phase++;
        mouth_set_height(g_verify_height ? g_verify_height : mouth_wave[(g_speak_phase / 3) % 6]);
    }

    if (g_state == FS_IDLE || g_state == FS_CONNECT)
    {
        if (g_blink_wait == 0)
        {
            do_blink();
            g_blink_wait = 90 + (rt_tick_get() % 90); /* 2.7s .. 5.4s */
        }
        else
        {
            g_blink_wait--;
        }
    }
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char buf[8];

    if (!tm_now || tm_now->tm_year < 120)
    {
        lv_label_set_text(lbl_clock, "--:--");
        return;
    }
    rt_snprintf(buf, sizeof(buf), "%02d:%02d", tm_now->tm_hour, tm_now->tm_min);
    lv_label_set_text(lbl_clock, buf);
}

/* ------------------------------------------------------------- status bar */
static void net_icon(bool on)
{
    static bool current = true;

    if (current == on)
        return;
    current = on;
    lv_image_set_src(wf_icon, &sig_images[on ? 1 : 0]);
}

static void battery_icon(bool charging)
{
    static bool current = true;

    if (current == charging)
        return;
    current = charging;
    lv_image_set_src(bat_fill, &bat_fill_images[charging ? 1 : 0]);
}

/* ------------------------------------------------------------------ build */
static void face_build(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- status bar: pre-rendered icons (EPIC corrupts small rounded
     * fills, so bars, outline and nub come from software-rasterised
     * textures instead of lv_obj rectangles) --- */
    make_ui_textures();
    wf_icon = mk_ellipse(scr, 18, 12, SIG_TW, SIG_TH, &sig_images[g_net ? 1 : 0]);
    bat_shell = mk_ellipse(scr, SCR_W - 76, 13, BAT_TW, BAT_TH, &bat_shell_image);
    bat_fill = mk_ellipse(scr, SCR_W - 74, 17, BAT_FILL_TW, BAT_FILL_TH,
                          &bat_fill_images[g_charge ? 1 : 0]);

    lbl_clock = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_MID, 0, 10);

    /* --- face + three glow discs: one pre-rendered image blit --- */
    make_face_backdrop();
    make_disc_texture(ink_pixels, COL_INK, LV_OPA_COVER);
    make_disc_texture(blush_pixels, COL_BLUSH, LV_OPA_60);

    /* --- face --- */
    face = lv_obj_create(scr);
    lv_obj_remove_style_all(face);
    lv_obj_set_pos(face, FACE_X, FACE_Y);
    lv_obj_set_size(face, FACE_SZ, FACE_SZ);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    blush_l = mk_ellipse(face, 66, 234, 58, 28, &blush_image);
    blush_r = mk_ellipse(face, 256, 234, 58, 28, &blush_image);

    eye_l = mk_ellipse(face, 94, 150, EYE_W, EYE_H, &ink_image);
    eye_r = mk_ellipse(face, 230, 150, EYE_W, EYE_H, &ink_image);

    mouth_img = mk_ellipse(face, 140, 276, MOUTH_TW, MOUTH_TH,
                           &mouth_images[MOUTH_SMILE]);
    mouth_set_style(MOUTH_SMILE);

    mouth_open = mk_ellipse(face, MOUTH_CX - MOUTH_DIAM / 2,
                            MOUTH_CY - MOUTH_DIAM / 2, MOUTH_DIAM, MOUTH_DIAM, &ink_image);
    lv_obj_set_style_transform_pivot_x(mouth_open, MOUTH_DIAM / 2, 0);
    lv_obj_set_style_transform_pivot_y(mouth_open, MOUTH_DIAM / 2, 0);
    lv_obj_set_style_transform_scale_x(mouth_open, 384, 0); /* 1.5x => ellipse */
    lv_obj_add_flag(mouth_open, LV_OBJ_FLAG_HIDDEN);

    /* --- texts --- */
    lbl_status = lv_label_create(scr);
    lv_obj_set_width(lbl_status, SCR_W - 32);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(lbl_status, "Connecting");
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 600);

    lbl_text = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_text, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_align(lbl_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_text, SCR_W - 60);
    lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_text, "");
    lv_obj_align(lbl_text, LV_ALIGN_TOP_MID, 0, 648);

    standby_screen = scr;
    lv_screen_load(scr);

    lv_timer_create(face_timer_cb, 33, NULL);
    lv_timer_create(clock_timer_cb, 1000, NULL);
}

static void ui_activity(void)
{
    if (g_dimmed)
    {
        xz_set_lcd_brightness(LCD_BRIGHTNESS_DEFAULT);
        g_dimmed = false;
    }
    if (ui_sleep_timer)
        lv_timer_reset(ui_sleep_timer);
}

/* ---------------------------------------------------------------- events */
/* Only called by the UI event thread. Keep logging out of the frame timer. */
static void ui_change_state(face_state_t state)
{
    const char *name;
    if (state == g_state) return;
    switch (state)
    {
    case FS_CONNECT: name = "connect"; break;
    case FS_IDLE:    name = "idle"; break;
    case FS_LISTEN:  name = "listen"; break;
    case FS_SPEAK:   name = "speak"; break;
    case FS_THINK:   name = "think"; break;
    case FS_ERROR:   name = "error"; break;
    case FS_SLEEP:   name = "sleep"; break;
    case FS_CONFIG:  name = "config"; break;
    default: return;
    }
    g_state = state;
    rt_kprintf("UI_STATE %s\n", name);
}

static void ui_dispatch(const ui_evt_t *ev)
{
    if (ev->type == EV_FACE_STATE || ev->type == EV_SLEEP || ev->type == EV_CONFIG)
        g_verify = false;
    if (g_verify && ev->type != EV_VERIFY && ev->type != EV_UI_CALL) return;
    switch (ev->type)
    {
    case EV_MUSIC:
        rt_snprintf(g_music, sizeof(g_music), "%s", ev->text);
        g_music_paused = ev->val != 0;
        if (g_music[0] && !music_font)
            music_font = lv_tiny_ttf_create_data_ex(xiaozhi_font, xiaozhi_font_size,
                                                   24, LV_FONT_KERNING_NONE, 16);
        apply_visual();
        break;
    case EV_UI_CALL:
        ev->call->fn(ev->call->ctx);
        rt_sem_release(ev->call->done);
        break;

    case EV_VERIFY:
        g_verify = true;
        g_verify_height = ev->val >= 8 ? mouth_wave[ev->val - 8] : 0;
        ui_change_state(ev->val >= 8 ? FS_SPEAK : (face_state_t)ev->val);
        rt_snprintf(g_emoji, sizeof(g_emoji), "%s", ev->text);
        g_text[0] = 0;
        apply_visual();
        if (g_verify_height) mouth_set_height(g_verify_height);
        rt_kprintf("FACE_VERIFY state=%s emoji=%s height=%d\n", state_label(g_state), g_emoji, g_verify_height);
        break;

    case EV_FACE_STATE:
        ui_activity();
        if (g_state != (face_state_t)ev->val)
        {
            ui_change_state((face_state_t)ev->val);
            g_text[0] = 0;
            apply_visual();
        }
        break;

    case EV_STATUS:
        ui_activity();
        if (ascii_only(ev->text))
            rt_snprintf(g_text, sizeof(g_text), "%s", ev->text);
        else
            g_text[0] = 0;
        apply_visual();
        break;

    case EV_EMOJI:
        ui_activity();
        rt_snprintf(g_emoji, sizeof(g_emoji), "%s", ev->text);
        apply_visual();
        break;

    case EV_TEXT:
        if (ascii_only(ev->text))
        {
            rt_snprintf(g_text, sizeof(g_text), "%s", ev->text);
            apply_visual();
        }
        break;

    case EV_TTS:
        ui_activity();
        if (ascii_only(ev->text))
        {
            rt_snprintf(g_text, sizeof(g_text), "%s", ev->text);
            apply_visual();
        }
        break;

    case EV_CHARGE:
        g_charge = ev->val ? true : false;
        battery_icon(g_charge);
        break;

    case EV_NET:
        g_net = ev->val ? true : false;
        net_icon(g_net);
        break;

    case EV_VOLUME:
        /* the IoT speaker thing already applied it */
        break;

    case EV_BRIGHT:
        xz_set_lcd_brightness((uint16_t)ev->val);
        break;

    case EV_SLEEP:
    case EV_DIM:
        if (!g_dimmed)
        {
            xz_set_lcd_brightness(LCD_BRIGHTNESS_MIN);
            g_dimmed = true;
        }
        break;

    case EV_CONFIG:
        ui_activity();
        if (ascii_only(ev->text))
            rt_snprintf(g_text, sizeof(g_text), "%s", ev->text);
        apply_visual();
        break;

    case EV_WAKE:
        ui_activity();
        if (ui_sleep_timer)
            lv_timer_resume(ui_sleep_timer);
        lv_display_trigger_activity(NULL);
        break;
    }
}

static void ui_post(uint8_t type, uint8_t val, const char *text)
{
    ui_evt_t ev;
    rt_memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.val = val;
    if (text)
    {
        rt_snprintf(ev.text, sizeof(ev.text), "%s", text);
        /* A long UTF-8 filename may end at the event buffer boundary. */
        if (type == EV_MUSIC && strlen(text) >= sizeof(ev.text))
        {
            size_t n = sizeof(ev.text) - 1;
            while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
            ev.text[n] = 0;
        }
    }

    if (!ui_mq)
        return;
    /* State transitions must survive a full queue. The owner dispatches them
     * directly so it can never block waiting on its own queue. */
    if (type == EV_FACE_STATE)
    {
        if (rt_thread_self() == ui_owner)
            ui_dispatch(&ev);
        else
            while (rt_mq_send(ui_mq, &ev, sizeof(ev)) == -RT_EFULL)
                rt_thread_mdelay(1);
    }
    else
        rt_mq_send(ui_mq, &ev, sizeof(ev));
}

/* ------------------------------------------------------------ public API */
/* Run a diagnostic LVGL operation on its owner thread. The caller keeps ctx
 * alive until completion; no LVGL access is made from the requesting thread. */
int xiaozhi_ui_call_sync(void (*fn)(void *), void *ctx)
{
    if (!ui_mq || !fn) return -RT_ERROR;
    ui_call_t call = {fn, ctx, rt_sem_create("ui_call", 0, RT_IPC_FLAG_FIFO)};
    if (!call.done) return -RT_ENOMEM;
    ui_evt_t ev = {0};
    ev.type = EV_UI_CALL;
    ev.call = &call;
    rt_err_t err = rt_mq_send(ui_mq, &ev, sizeof(ev));
    if (err == RT_EOK) err = rt_sem_take(call.done, RT_WAITING_FOREVER);
    rt_sem_delete(call.done);
    return err;
}

static int xz_face(int argc, char **argv)
{
    static const char *names[] = {"connect", "idle", "listen", "speak", "think", "error", "sleep", "config",
                                  "speak0", "speak1", "speak2", "speak3", "speak4", "speak5"};
    for (unsigned i = 0; argc > 1 && i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (!strcmp(argv[1], names[i]))
        {
            ui_post(EV_VERIFY, i, argc > 2 ? argv[2] : "neutral");
            return 0;
        }
    }
    rt_kprintf("xz_face connect|idle|listen|speak|think|error|sleep|config|speak0..5 [emoji]\n");
    return -1;
}
MSH_CMD_EXPORT(xz_face, TEMPORARY face verification);

void xiaozhi_ui_set_face_state(face_state_t state)
{
    if (state >= FS_CONNECT && state <= FS_CONFIG)
        ui_post(EV_FACE_STATE, (uint8_t)state, NULL);
}
void xiaozhi_ui_chat_status(char *string) { ui_post(EV_STATUS, 0, string); }
void xiaozhi_ui_music(const char *name, bool paused) { ui_post(EV_MUSIC, paused, name); }
void xiaozhi_ui_chat_output(char *string) { ui_post(EV_TEXT, 0, string); }
void xiaozhi_ui_standby_chat_output(char *string) { ui_post(EV_TEXT, 0, string); }
void xiaozhi_ui_tts_output(char *string) { ui_post(EV_TTS, 0, string); }
void xiaozhi_ui_update_emoji(char *string) { ui_post(EV_EMOJI, 0, string); }
void xiaozhi_ui_update_standby_emoji(char *string) { ui_post(EV_EMOJI, 0, string); }
void xiaozhi_ui_update_ble(char *string) { ui_post(EV_NET, string && strstr(string, "open") ? 1 : 0, NULL); }
void xiaozhi_ui_update_charge_status(uint8_t is_charging) { ui_post(EV_CHARGE, is_charging, NULL); }
void xiaozhi_ui_update_volume(int volume) { ui_post(EV_VOLUME, (uint8_t)volume, NULL); }
void xiaozhi_ui_update_brightness(int brightness) { ui_post(EV_BRIGHT, (uint8_t)brightness, NULL); }

/* Wake the main screen without touching the status text (used while the
 * device is waiting to be bound). */
void xiaozhi_ui_wake(void)
{
    ui_post(EV_WAKE, 0, NULL);
}

void ui_switch_to_xiaozhi_screen(void)
{
    ui_post(EV_WAKE, 0, NULL);
}

void ui_swith_to_standby_screen(void)
{
    ui_post(EV_STATUS, 0, "待命中");
}

void ui_sleep_callback(lv_timer_t *timer)
{
    (void)timer;
    extern volatile int g_kws_running;
    /* KWS 监听期间禁止自动睡眠:一旦进入待机,PDM/识别线程停摆,
     * 唤醒词就永久失效,设备表现为对说话完全无反应。 */
    if (g_kws_running)
    {
        return;
    }
    /* Inactivity dims the panel; protocol state remains owned by websocket. */
    ui_post(EV_DIM, 0, NULL);
}

void xiaozhi_ui_config_mode(const char *ap_ssid, const char *ap_pass)
{
    char buf[96];
    xiaozhi_ws_configuring();
    rt_snprintf(buf, sizeof(buf), "AP %s  pass %s\nopen http://192.168.4.1",
                ap_ssid ? ap_ssid : "", ap_pass ? ap_pass : "");
    ui_post(EV_CONFIG, 0, buf);
}

void xiaozhi_ui_reinit_audio(void)
{
}

void xiaozhi_ui_update_latest_version(char *version)
{
    (void)version;
}

void xiaozhi_ui_update_confirm_button_event(bool is_update_button)
{
    (void)is_update_button;
}

void update_xiaozhi_ui_time(void *parameter)
{
    (void)parameter;
}

void update_xiaozhi_ui_weather(void *parameter)
{
    (void)parameter;
}

void show_shutdown(void)
{
    xiaozhi_ws_sleep();
    ui_post(EV_SLEEP, 0, NULL);
}

void show_sleep_countdown_and_sleep(void)
{
    xiaozhi_ws_sleep();
    ui_post(EV_SLEEP, 0, NULL);
}

void ctrl_wakeup(bool is_wakeup)
{
    aec_enabled = is_wakeup ? 1 : 0;
}

void ctrl_interrupt(bool is_interrupt)
{
    vad_enable = is_interrupt ? 0 : 1;
}

float get_scale_factor(void)
{
    return 1.0f;
}

void xz_set_lcd_brightness(uint16_t level)
{
    rt_device_t bl_device = rt_device_find("lcd");
    if (!bl_device)
        return;

    int ret = rt_device_open(bl_device, RT_DEVICE_OFLAG_RDWR);
    if (ret == RT_EOK || ret == -RT_EBUSY)
    {
        rt_device_control(bl_device, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &level);
    }
    if (ret == RT_EOK)
        rt_device_close(bl_device);
}

void xiaozhi_ui_task(void *args)
{
    (void)args;
    ui_owner = rt_thread_self();

    extern const char xz_rom2_banner[];
    extern const char xz_rom3_banner[];
    rt_kprintf("[xz_ui] face ui start: %s | %s\n", xz_rom2_banner, xz_rom3_banner);

    ui_mq = rt_mq_create("xz_ui_mq", sizeof(ui_evt_t), 32, RT_IPC_FLAG_FIFO);
    if (!ui_mq)
    {
        LOG_E("[xz_ui] mq create failed");
        return;
    }
    if (littlevgl2rtt_init("lcd") != RT_EOK)
    {
        LOG_E("[xz_ui] lvgl init failed");
        return;
    }
    face_build();
    battery_icon(g_charge);
    net_icon(g_net);
    apply_visual();

    while (1)
    {
        ui_evt_t ev;
        while (rt_mq_recv(ui_mq, &ev, sizeof(ev), 0) == RT_EOK)
        {
            ui_dispatch(&ev);
        }

        uint32_t ms = lv_task_handler();
        rt_thread_mdelay(ms ? ms : 10);
    }
}
