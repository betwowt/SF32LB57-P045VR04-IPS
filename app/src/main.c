/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL 8 on SF32LB57 + P045VR04-IPS (480x854, RGB565, ST7701S, FT6236).
 *
 * Display: the "lcd" device (drv_lcd, DPI RGB565) is driven by the SDK's
 * lv_lcd port; on a ramless DPI panel it keeps screen-sized framebuffers in
 * PSRAM (LCD_FB_USING_AUTO -> TWO_UNCOMPRESSED).
 * Touch:   the "touch" device (drv_touch) is fed by drivers/touch/ft6236.c.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "lv_ex_data.h"

#define UI_BG           lv_color_hex(0x0B1220)
#define UI_CARD         lv_color_hex(0x16213A)
#define UI_ACCENT       lv_color_hex(0x3B82F6)
#define UI_TEXT         lv_color_hex(0xE5E7EB)
#define UI_MUTED        lv_color_hex(0x94A3B8)
#define UI_DOT          lv_color_hex(0xF87171)

static lv_obj_t *slider;
static lv_obj_t *bar;
static lv_obj_t *bar_label;
static lv_obj_t *cnt_label;
static lv_obj_t *touch_label;
static lv_obj_t *touch_dot;
static lv_obj_t *touch_pad;

static int click_count;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    return label;
}

static void btn_event_cb(lv_event_t *e)
{
    if (LV_EVENT_CLICKED == lv_event_get_code(e))
    {
        click_count++;
        lv_label_set_text_fmt(cnt_label, "COUNT %d", click_count);
        rt_kprintf("[app] button clicked: %d\n", click_count);
    }
}

static void slider_event_cb(lv_event_t *e)
{
    if (LV_EVENT_VALUE_CHANGED == lv_event_get_code(e))
    {
        int32_t value = lv_slider_get_value(slider);

        lv_bar_set_value(bar, value, LV_ANIM_OFF);
        lv_label_set_text_fmt(bar_label, "VALUE %d", (int)value);
    }
}

static void touch_pad_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (LV_EVENT_PRESSED == code || LV_EVENT_PRESSING == code)
    {
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t point;
        lv_area_t pad_area;
        lv_coord_t pad_w;
        lv_coord_t pad_h;
        lv_coord_t x;
        lv_coord_t y;

        if (NULL == indev)
        {
            return;
        }

        lv_indev_get_point(indev, &point);

        /* Screen coordinates -> the pad's content area (what lv_obj_set_pos uses) */
        lv_obj_get_content_coords(touch_pad, &pad_area);
        pad_w = lv_obj_get_content_width(touch_pad);
        pad_h = lv_obj_get_content_height(touch_pad);
        x = point.x - pad_area.x1;
        y = point.y - pad_area.y1;

        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > pad_w) x = pad_w;
        if (y > pad_h) y = pad_h;

        lv_obj_set_pos(touch_dot, x - lv_obj_get_width(touch_dot) / 2,
                       y - lv_obj_get_height(touch_dot) / 2);
        lv_label_set_text_fmt(touch_label, "x=%d  y=%d", (int)point.x, (int)point.y);

        if (LV_EVENT_PRESSED == code)
        {
            rt_kprintf("[app] touch down at (%d,%d)\n", (int)point.x, (int)point.y);
        }
    }
}

static void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *btn;
    lv_obj_t *hint;
    lv_obj_t *label;

    lv_obj_set_style_bg_color(scr, UI_BG, 0);

    title = make_label(scr, "SF32LB57 + P045VR04", &lv_font_montserrat_28, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    subtitle = make_label(scr, "480x854 RGB565 | LVGL 8 | FT6236",
                          &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 62);

    /* --- button: tap counting --- */
    btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 260, 84);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_bg_color(btn, UI_ACCENT, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    cnt_label = make_label(btn, "COUNT 0", &lv_font_montserrat_24, lv_color_hex(0xFFFFFF));
    lv_obj_center(cnt_label);

    /* --- slider -> bar --- */
    label = make_label(scr, "SLIDER", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 32, 222);

    slider = lv_slider_create(scr);
    lv_obj_set_size(slider, 416, 16);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 252);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 40, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 416, 30);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 292);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 40, LV_ANIM_OFF);

    bar_label = make_label(scr, "VALUE 40", &lv_font_montserrat_16, UI_TEXT);
    lv_obj_align(bar_label, LV_ALIGN_TOP_MID, 0, 330);

    /* --- touch pad --- */
    label = make_label(scr, "TOUCH PAD", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 32, 372);

    touch_pad = lv_obj_create(scr);
    lv_obj_set_size(touch_pad, 416, 380);
    lv_obj_align(touch_pad, LV_ALIGN_TOP_MID, 0, 402);
    lv_obj_set_style_bg_color(touch_pad, UI_CARD, 0);
    lv_obj_set_style_border_color(touch_pad, UI_ACCENT, 0);
    lv_obj_set_style_border_width(touch_pad, 2, 0);
    lv_obj_set_style_radius(touch_pad, 12, 0);
    lv_obj_add_event_cb(touch_pad, touch_pad_event_cb, LV_EVENT_ALL, NULL);

    touch_dot = lv_obj_create(touch_pad);
    lv_obj_set_size(touch_dot, 22, 22);
    lv_obj_set_style_radius(touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touch_dot, UI_DOT, 0);
    lv_obj_set_style_border_width(touch_dot, 0, 0);
    lv_obj_set_pos(touch_dot, 0, 0);

    touch_label = make_label(touch_pad, "x=0  y=0", &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(touch_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    hint = make_label(scr, "tap the button / drag the slider / draw in the pad",
                      &lv_font_montserrat_16, UI_MUTED);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    rt_kprintf("[app] ui ready: %dx%d\n", lv_disp_get_hor_res(NULL),
               lv_disp_get_ver_res(NULL));
}

int main(void)
{
    rt_err_t err;

    rt_kprintf("\n[app] LVGL on SF32LB57-P045VR04-IPS\n");

    err = littlevgl2rtt_init("lcd");
    if (RT_EOK != err)
    {
        rt_kprintf("[app] littlevgl2rtt_init failed: %d\n", err);
        return err;
    }

    /* LVSF's refresh hook calls lv_ex_process_data(), so its pool/semaphore
     * must exist even for a plain-LVGL app. */
    lv_ex_data_pool_init();

    ui_init();

    /* drv_lcd turns the display on with its default brightness (50%); use the
     * full backlight for this panel. */
    {
        rt_device_t lcd = rt_device_find("lcd");
        uint8_t brightness = 100;

        if (lcd)
        {
            rt_device_control(lcd, RTGRAPHIC_CTRL_SET_BRIGHTNESS, &brightness);
        }
    }

    while (1)
    {
        uint32_t delay_ms = lv_task_handler();

        rt_thread_mdelay(delay_ms ? delay_ms : 1);
    }

    return RT_EOK;
}
