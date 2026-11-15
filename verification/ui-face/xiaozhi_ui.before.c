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
#include <time.h>

#include "rtthread.h"
#include "littlevgl2rtt.h"
#include "lvgl.h"
#include "ulog.h"
#include "rtdevice.h"

#include "xiaozhi_ui.h"
#include "xiaozhi_client_public.h"

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

#define COL_FACE        0xFFC94A
#define COL_INK         0x2B2118
#define COL_BLUSH       0xFF8A80
#define COL_RING        0x35D6F3
#define COL_RING_THINK  0xFFB020
#define COL_TEXT        0xE8E8E8
#define COL_DIM         0x8A8A8A

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
};

typedef struct {
    uint8_t type;
    uint8_t val;
    char text[96];
} ui_evt_t;

typedef enum {
    FS_CONNECT = 0,
    FS_IDLE,
    FS_LISTEN,
    FS_SPEAK,
    FS_THINK,
    FS_ERROR,
    FS_SLEEP,
    FS_CONFIG,
} face_state_t;

uint8_t vad_enable = 1;   /* 1 = do not interrupt while speaking */
uint8_t aec_enabled = 1;

/* referenced by main.c / websocket / mqtt */
lv_timer_t *ui_sleep_timer = NULL;
rt_tick_t last_listen_tick = 0;
lv_obj_t *standby_screen = NULL;

static rt_mq_t ui_mq;
static face_state_t g_state = FS_CONNECT;
static char g_emoji[16] = "neutral";
static char g_text[96] = {0};
static bool g_net, g_charge = true, g_dimmed;
static uint32_t g_blink_wait, g_speak_phase;
static uint16_t g_ring_ang;

static lv_obj_t *scr;
static lv_obj_t *face, *eye_l, *eye_r, *blush_l, *blush_r;
static lv_obj_t *mouth_line, *mouth_open, *ring;
static lv_obj_t *lbl_clock, *lbl_status, *lbl_text;
static lv_obj_t *bat_body, *bat_fill;
static lv_obj_t *wf_bars[4];

/* -------------------------------------------------------------- helpers */
static lv_obj_t *mk_shape(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
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
static const lv_point_precise_t pts_smile[] = {{0, 0}, {30, 17}, {60, 22}, {90, 17}, {120, 0}};
static const lv_point_precise_t pts_flat[]  = {{0, 6}, {60, 6}, {120, 6}};
static const lv_point_precise_t pts_frown[] = {{0, 22}, {30, 5}, {60, 0}, {90, 5}, {120, 22}};

static void mouth_line_points(const lv_point_precise_t *p, uint32_t n)
{
    lv_line_set_points(mouth_line, p, n);
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
        lv_obj_add_flag(mouth_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(mouth_open, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(mouth_open, 96, 40);
        lv_obj_set_pos(mouth_open, 142, 258);
    }
    else
    {
        lv_obj_clear_flag(mouth_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_open, LV_OBJ_FLAG_HIDDEN);
        if (frown)
            mouth_line_points(pts_frown, 5);
        else if (smile)
            mouth_line_points(pts_smile, 5);
        else
            mouth_line_points(pts_flat, 3);
    }

    if (g_state == FS_LISTEN || g_state == FS_THINK)
    {
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_color(ring, lv_color_hex(g_state == FS_LISTEN ? COL_RING : COL_RING_THINK), LV_PART_MAIN);
    }
    else
    {
        lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(lbl_status, state_label(g_state));
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
        lv_arc_set_bg_angles(ring, g_ring_ang, (g_ring_ang + 70) % 360);
    }

    if (g_state == FS_SPEAK && !lv_obj_has_flag(mouth_open, LV_OBJ_FLAG_HIDDEN))
    {
        static const int16_t wave[] = {22, 40, 56, 40, 24, 18};
        g_speak_phase++;
        lv_obj_set_height(mouth_open, wave[(g_speak_phase / 3) % 6]);
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
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_color(wf_bars[i], lv_color_hex(on ? COL_TEXT : 0x3A3A3A), 0);
}

static void battery_icon(bool charging)
{
    lv_obj_set_style_bg_color(bat_fill, lv_color_hex(charging ? 0x4CD964 : 0xD8D8D8), 0);
    lv_obj_set_size(bat_fill, charging ? 30 : 14, 12);
}

/* ------------------------------------------------------------------ build */
static void face_build(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- status bar --- */
    for (int i = 0; i < 4; i++)
    {
        wf_bars[i] = mk_shape(scr, 20 + i * 10, 34 - (4 + i * 4), 6, 4 + i * 4, COL_TEXT);
    }
    net_icon(false);

    /* Battery icon. LVGL 9.4 draws rounded *borders* with holes, so the 2 px
     * white outline is a white rounded rect with a black one inset on top. */
    bat_body = mk_shape(scr, SCR_W - 76, 14, 56, 26, COL_TEXT);
    lv_obj_set_style_radius(bat_body, 7, 0);
    lv_obj_t *bat_inner = mk_shape(scr, SCR_W - 74, 16, 52, 22, 0x000000);
    lv_obj_set_style_radius(bat_inner, 5, 0);
    bat_fill = mk_shape(scr, SCR_W - 72, 18, 24, 12, 0xD8D8D8);
    lv_obj_set_style_radius(bat_fill, 3, 0);

    lbl_clock = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_MID, 0, 10);

    /* --- face glow: concentric discs, not a shadow (LVGL 9.4 shadow+radius
     *        renders the fill with holes, see xz_shot bisect) --- */
    {
        static const uint32_t glow_col[3] = {0x120F06, 0x1E1809, 0x2C230E};
        for (int i = 0; i < 3; i++)
        {
            int pad = 12 * (3 - i);
            lv_obj_t *g = mk_shape(scr, FACE_X - pad, FACE_Y - pad,
                                   FACE_SZ + pad * 2, FACE_SZ + pad * 2, glow_col[i]);
            lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* --- listening ring behind the face --- */
    ring = lv_arc_create(scr);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, FACE_SZ + 46, FACE_SZ + 46);
    lv_obj_set_pos(ring, FACE_X - 23, FACE_Y - 23);
    lv_obj_set_style_arc_width(ring, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(ring, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(COL_RING), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 0, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_arc_set_bg_angles(ring, 0, 70);

    /* --- face --- */
    face = mk_shape(scr, FACE_X, FACE_Y, FACE_SZ, FACE_SZ, COL_FACE);

    blush_l = mk_shape(face, 66, 244, 58, 32, COL_BLUSH);
    lv_obj_set_style_bg_opa(blush_l, LV_OPA_60, 0);
    blush_r = mk_shape(face, 256, 244, 58, 32, COL_BLUSH);
    lv_obj_set_style_bg_opa(blush_r, LV_OPA_60, 0);

    eye_l = mk_shape(face, 94, 150, EYE_W, EYE_H, COL_INK);
    eye_r = mk_shape(face, 230, 150, EYE_W, EYE_H, COL_INK);

    mouth_line = lv_line_create(face);
    lv_obj_remove_style_all(mouth_line);
    lv_obj_set_pos(mouth_line, 130, 262);
    lv_obj_set_size(mouth_line, 120, 30);
    lv_obj_set_style_line_width(mouth_line, 10, 0);
    lv_obj_set_style_line_rounded(mouth_line, true, 0);
    lv_obj_set_style_line_color(mouth_line, lv_color_hex(COL_INK), 0);
    mouth_line_points(pts_smile, 5);

    mouth_open = mk_shape(face, 142, 258, 96, 40, COL_INK);
    lv_obj_add_flag(mouth_open, LV_OBJ_FLAG_HIDDEN);

    /* --- texts --- */
    lbl_status = lv_label_create(scr);
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
static void ui_dispatch(const ui_evt_t *ev)
{
    switch (ev->type)
    {
    case EV_STATUS:
        ui_activity();
        if (strstr(ev->text, "聆听")) g_state = FS_LISTEN;
        else if (strstr(ev->text, "待命")) g_state = FS_IDLE;
        else if (strstr(ev->text, "说话")) g_state = FS_SPEAK;
        else if (strstr(ev->text, "失败") || strstr(ev->text, "错误")) g_state = FS_ERROR;
        else if (strstr(ev->text, "连接") || strstr(ev->text, "初始化")) g_state = FS_CONNECT;
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
        if (g_state != FS_SPEAK)
        {
            g_state = FS_SPEAK;
            g_text[0] = 0;
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
        g_state = FS_SLEEP;
        apply_visual();
        if (!g_dimmed)
        {
            xz_set_lcd_brightness(LCD_BRIGHTNESS_MIN);
            g_dimmed = true;
        }
        break;

    case EV_CONFIG:
        ui_activity();
        g_state = FS_CONFIG;
        if (ascii_only(ev->text))
            rt_snprintf(g_text, sizeof(g_text), "%s", ev->text);
        apply_visual();
        break;

    case EV_WAKE:
        if (g_dimmed)
        {
            xz_set_lcd_brightness(LCD_BRIGHTNESS_DEFAULT);
            g_dimmed = false;
        }
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
        rt_snprintf(ev.text, sizeof(ev.text), "%s", text);

    if (!ui_mq)
        return;
    /* never block the caller: drop the oldest is overkill, just fail */
    rt_mq_send(ui_mq, &ev, sizeof(ev));
}

/* ------------------------------------------------------------ public API */
void xiaozhi_ui_chat_status(char *string) { ui_post(EV_STATUS, 0, string); }
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
    ui_post(EV_STATUS, 0, "说话中");
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
    ui_post(EV_SLEEP, 0, NULL);
}

void xiaozhi_ui_config_mode(const char *ap_ssid, const char *ap_pass)
{
    char buf[96];
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
    ui_post(EV_SLEEP, 0, NULL);
}

void show_sleep_countdown_and_sleep(void)
{
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
