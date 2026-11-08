/*
 * demo_entry.c - SF32LB57 / P045VR04-IPS port of the GE demo engine.
 *
 * The ArtInChip original rendered into a double-buffered framebuffer and
 * panned the display engine.  This board uses a ramless DPI panel: the
 * LCDC's PTC state machine continuously scans the framebuffer supplied via
 * the RT-Thread graphic device.  Two PSRAM framebuffers are therefore used
 * and each finished frame is presented with a synchronous draw_rect().
 */
#include "demo_engine.h"
#include "effect_registry.h"
#include "mpp_mem.h"

#include <rtdevice.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_FB_WIDTH   DEMO_SCREEN_WIDTH
#define DEMO_FB_HEIGHT  DEMO_SCREEN_HEIGHT
#define DEMO_FB_BPP     2
#define DEMO_FB_STRIDE  (DEMO_FB_WIDTH * DEMO_FB_BPP)
#define DEMO_FB_SIZE    (DEMO_FB_STRIDE * DEMO_FB_HEIGHT)
/* The EPIC driver sizes cache maintenance as total_width * tile height
 * from a tile origin; the bottom-right tile of a full-screen output can
 * therefore sweep a little past the visible buffer.  Keep a small pad so
 * those invalidations never touch another object. */
#define DEMO_FB_PAD     2048

#define DEMO_AUTO_SWITCH_MS 10000
#define DEMO_KEY_PIN        34   /* PA34, KEY1 active-high on the devkit */

/* Two cacheable PSRAM framebuffers, 64-byte aligned for LCDC DMA. */
__attribute__((section(".bss.l2_cache_non_ret_bss_ge_fb"), aligned(64)))
static uint8_t s_fb[2][DEMO_FB_SIZE + DEMO_FB_PAD];

static struct demo_ctx g_ctx;
static volatile int    g_current_effect_idx = 0;
static volatile int    g_req_effect_idx     = -1;
static rt_thread_t     g_render_thread      = RT_NULL;
static rt_device_t     s_lcd                = RT_NULL;
static struct rt_device_graphic_info s_lcd_info;

static int get_effect_count(void)
{
    return ge_effect_count;
}

static struct effect_ops *get_effect_by_index(int index)
{
    if (index < 0 || index >= get_effect_count())
        return RT_NULL;
    return ge_effects[index];
}

static void demo_key_irq(void *args)
{
    (void)args;
    demo_next_effect();
}

static void input_init(void)
{
    rt_pin_mode(DEMO_KEY_PIN, PIN_MODE_INPUT_PULLDOWN);
    rt_pin_attach_irq(DEMO_KEY_PIN, PIN_IRQ_MODE_RISING, demo_key_irq, RT_NULL);
    rt_pin_irq_enable(DEMO_KEY_PIN, PIN_IRQ_ENABLE);
}

static rt_err_t lcd_setup(void)
{
    s_lcd = rt_device_find("lcd");
    if (!s_lcd)
    {
        rt_kprintf("[GE] lcd device not found\n");
        return -RT_ERROR;
    }

    if (rt_device_open(s_lcd, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
    {
        rt_kprintf("[GE] lcd open failed\n");
        return -RT_ERROR;
    }

    uint16_t format = RTGRAPHIC_PIXEL_FORMAT_RGB565;
    rt_device_control(s_lcd, RTGRAPHIC_CTRL_SET_BUF_FORMAT, &format);

    rt_memset(&s_lcd_info, 0, sizeof(s_lcd_info));
    rt_device_control(s_lcd, RTGRAPHIC_CTRL_GET_INFO, &s_lcd_info);

    rt_kprintf("[GE] lcd %ux%u, %u bpp\n", s_lcd_info.width, s_lcd_info.height,
               s_lcd_info.bits_per_pixel);

    return RT_EOK;
}

static void lcd_set_brightness(uint8_t percent)
{
    if (s_lcd)
        rt_device_control(s_lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &percent);
}

static void lcd_present(uint8_t *fb)
{
    struct rt_device_graphic_ops *ops = rt_graphix_ops(s_lcd);

    if (!ops || !ops->set_window || !ops->draw_rect)
        return;

    aicos_dcache_clean_range(fb, DEMO_FB_SIZE);
    ops->set_window(0, 0, DEMO_FB_WIDTH - 1, DEMO_FB_HEIGHT - 1);
    ops->draw_rect((const char *)fb, 0, 0, DEMO_FB_WIDTH - 1, DEMO_FB_HEIGHT - 1);
    /* The LCD controller reads the buffer directly; drop any CPU view of it
     * so later software fallbacks and EPIC background reads see PSRAM. */
    aicos_dcache_invalid_range(fb, DEMO_FB_SIZE);
}

static void render_thread_entry(void *parameter)
{
    int current_buf = 0;
    rt_tick_t switch_at;

    (void)parameter;

    if (lcd_setup() != RT_EOK)
        return;

    g_ctx.fb      = RT_NULL;
    g_ctx.ge      = mpp_ge_open();
    g_ctx.screen_w = DEMO_FB_WIDTH;
    g_ctx.screen_h = DEMO_FB_HEIGHT;
    g_ctx.info.framebuffer = s_fb[0];
    g_ctx.info.width       = DEMO_FB_WIDTH;
    g_ctx.info.height      = DEMO_FB_HEIGHT;
    g_ctx.info.stride      = DEMO_FB_STRIDE;
    g_ctx.info.format      = MPP_FMT_RGB_565;

    rt_memset(s_fb[0], 0, DEMO_FB_SIZE);
    rt_memset(s_fb[1], 0, DEMO_FB_SIZE);
    /* Flush the memset through the write-back cache before the GPU starts. */
    aicos_dcache_clean_range(s_fb[0], DEMO_FB_SIZE);
    aicos_dcache_clean_range(s_fb[1], DEMO_FB_SIZE);
    aicos_dcache_invalid_range(s_fb[0], DEMO_FB_SIZE);
    aicos_dcache_invalid_range(s_fb[1], DEMO_FB_SIZE);
    lcd_set_brightness(100);

    int total_effects = get_effect_count();
    rt_kprintf("[GE] %d effects registered\n", total_effects);
    if (total_effects == 0)
        return;

    struct effect_ops *curr_op = get_effect_by_index(g_current_effect_idx);
    if (curr_op && curr_op->init)
        curr_op->init(&g_ctx);

    /* Start the PTC state machine with a valid framebuffer. */
    lcd_present(s_fb[current_buf]);
    switch_at = rt_tick_get() + rt_tick_from_millisecond(DEMO_AUTO_SWITCH_MS);

    while (1)
    {
        int req = g_req_effect_idx;

        if (req != -1)
        {
            if (curr_op && curr_op->deinit)
                curr_op->deinit(&g_ctx);

            mpp_ge_sync(g_ctx.ge);

            g_current_effect_idx = req;
            g_req_effect_idx     = -1;
            curr_op              = get_effect_by_index(req);

            if (curr_op)
            {
                rt_kprintf("[GE] switch to [%d]: %s\n", req, curr_op->name);
                if (curr_op->init)
                    curr_op->init(&g_ctx);
            }

            switch_at = rt_tick_get() + rt_tick_from_millisecond(DEMO_AUTO_SWITCH_MS);
        }

        if ((rt_int32_t)(rt_tick_get() - switch_at) >= 0)
        {
            demo_next_effect();
            switch_at = rt_tick_get() + rt_tick_from_millisecond(DEMO_AUTO_SWITCH_MS);
            continue;
        }

        current_buf ^= 1;
        uint8_t *fb = s_fb[current_buf];

        g_ctx.info.framebuffer = fb;

        if (curr_op && curr_op->draw)
            curr_op->draw(&g_ctx, (unsigned long)(uintptr_t)fb);

        lcd_present(fb);

        rt_thread_mdelay(1);
    }
}

void demo_core_init(void)
{
    g_req_effect_idx = -1;
    input_init();
}

void demo_core_start(void)
{
    g_render_thread = rt_thread_create("ge_render", render_thread_entry, RT_NULL, 8192, 20, 10);
    if (g_render_thread)
    {
        rt_thread_startup(g_render_thread);
        rt_kprintf("[GE] render thread started\n");
    }
    else
    {
        rt_kprintf("[GE] render thread create failed\n");
    }
}

void demo_next_effect(void)
{
    int count = get_effect_count();
    if (count == 0)
        return;
    g_req_effect_idx = (g_current_effect_idx + 1) % count;
}

void demo_prev_effect(void)
{
    int count = get_effect_count();
    if (count == 0)
        return;
    g_req_effect_idx = (g_current_effect_idx - 1 + count) % count;
}

void demo_jump_effect(int index)
{
    if (index >= 0 && index < get_effect_count())
        g_req_effect_idx = index;
    else
        rt_kprintf("Invalid ID: %d\n", index);
}

/* --- Shell commands ----------------------------------------------------- */

static int cmd_demo_next(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    demo_next_effect();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_demo_next, demo_next, Switch to next effect);

static int cmd_demo_prev(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    demo_prev_effect();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_demo_prev, demo_prev, Switch to prev effect);

static int cmd_demo_jump(int argc, char **argv)
{
    if (argc < 2)
        return -1;
    demo_jump_effect(atoi(argv[1]));
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_demo_jump, demo_jump, Jump to effect ID);

static int cmd_demo_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int count = get_effect_count();
    rt_kprintf("--- Registered Effects (%d) ---\n", count);
    for (int i = 0; i < count; i++)
        rt_kprintf("[%02d] %s\n", i, ge_effects[i]->name);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_demo_list, demo_list, List all registered effects);
