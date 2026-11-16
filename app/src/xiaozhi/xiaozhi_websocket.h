#ifndef XIAOZHI_WEBSOCKET_H
#define XIAOZHI_WEBSOCKET_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stdbool.h>
#include "xiaozhi_mic_dsp.h"
#include <rtthread.h>
#include "lwip/api.h"
#include "lwip/apps/websocket_client.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/apps/mqtt.h"
#include "xiaozhi_mqtt.h"
#include "bf0_hal.h"
#include <cJSON.h>
#include "button.h"
#include "audio_server.h"

void xiaozhi2(int argc, char **argv);
void xiaozhi_ws_network_status(bool failed); /* false: connecting, true: failed */
void xiaozhi_ws_configuring(void);
void xiaozhi_ws_sleep(void);
void xz_ws_button_init(void); // 对话键
void xz_ws_button_init2(void);//关机键
void xz_ws_audio_init(void);
void xz_audio_send_using_websocket(uint8_t *data, int len); // 发送音频数据
bool xz_ws_uplink_allowed(void);
bool xz_ws_playback_allowed(void);
uint32_t xz_ws_audio_epoch(void);
void xz_ws_process_wake(void); /* main event thread, nonblocking */
void xz_ws_wake_beep_done(uint32_t token, int result);
bool xz_ws_barge_in_context(uint32_t *generation, rt_tick_t *start);
bool xz_ws_request_barge_in(uint32_t generation, uint32_t rms, uint32_t floor,
                          uint32_t pre, unsigned frames,
                          bool wake_word);
bool xz_ws_request_speech_barge_in(const xz_barge_detector_t *detector);
void xz_ws_process_barge_in(void); /* main event thread; no audio/KWS-stack I/O */
void xz_ws_wake_word_detected(void);
void ws_send_speak_abort(void *ws, char *session_id, int reason);
void ws_send_listen_start(void *ws, char *session_id,enum ListeningMode mode);
void ws_send_listen_stop(void *ws, char *session_id);
    /**
     * @brief xiaozhi websocket cntext 数据结构体
     */
    typedef struct
    {
        uint32_t sample_rate;
        uint32_t frame_duration;
        uint8_t session_id[12];
        wsock_state_t clnt;
        rt_sem_t sem;
        uint8_t is_connected;
    } xiaozhi_ws_t;

    typedef enum
    {
        BUTTON_EVENT_NONE,
        BUTTON_EVENT_PRESSED,
        BUTTON_EVENT_RELEASED,
    } button_event_type_t;

#ifdef __cplusplus
}
#endif

#endif // XIAOZHI_WEBSOCKET_H
