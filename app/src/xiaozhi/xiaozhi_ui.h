#ifndef _XIAOZHI_UI_H_
#define _XIAOZHI_UI_H_

#include <stdint.h>
#include <stdbool.h>
#include "lv_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- task ---- */
void xiaozhi_ui_task(void *args);
int xiaozhi_ui_call_sync(void (*fn)(void *), void *ctx);

/* ---- state driven by websocket / audio / mqtt ---- */
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

/* Queued to the UI owner thread; text APIs never change the face state. */
void xiaozhi_ui_set_face_state(face_state_t state);
void xiaozhi_ui_chat_status(char *string);      /* 连接中/聆听中/待命中/网络连接失败 */
void xiaozhi_ui_chat_output(char *string);
void xiaozhi_ui_standby_chat_output(char *string);
void xiaozhi_ui_tts_output(char *string);       /* 正在合成/播放的文本 */
void xiaozhi_ui_update_emoji(char *string);     /* happy/sad/... */
void xiaozhi_ui_update_standby_emoji(char *string);
void xiaozhi_ui_update_ble(char *string);       /* "open"/"close" -> 网络图标 */
void ui_swith_to_standby_screen(void);
void ui_switch_to_xiaozhi_screen(void);
void xiaozhi_ui_wake(void);

/* ---- misc hooks kept for the rest of the app ---- */
void xiaozhi_ui_reinit_audio(void);
void xiaozhi_ui_config_mode(const char *ap_ssid, const char *ap_pass);
void xiaozhi_ui_update_charge_status(uint8_t is_charging);
void xiaozhi_ui_update_latest_version(char *version);
void xiaozhi_ui_update_confirm_button_event(bool is_update_button);
void xiaozhi_ui_update_volume(int volume);
void xiaozhi_ui_update_brightness(int brightness);
void xiaozhi_ui_music(const char *name, bool paused);
void ui_sleep_callback(lv_timer_t *timer);
void update_xiaozhi_ui_time(void *parameter);
void update_xiaozhi_ui_weather(void *parameter);
void show_shutdown(void);
void show_sleep_countdown_and_sleep(void);

/* ---- hardware ---- */
void xz_set_lcd_brightness(uint16_t level);
void ctrl_wakeup(bool is_wakeup);
void ctrl_interrupt(bool is_interrupt);
float get_scale_factor(void);

/* ---- voice processing switches (owned by the UI module) ---- */
extern uint8_t vad_enable;
extern uint8_t aec_enabled;

#ifdef __cplusplus
}
#endif

#endif /* _XIAOZHI_UI_H_ */
