/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <rtthread.h>
#include "lwip/api.h"
#include "lwip/tcpip.h"
#include "lwip/dns.h"
#include "lwip/apps/websocket_client.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/apps/mqtt.h"
#include "xiaozhi_mqtt.h"
#include "xiaozhi_websocket.h"
#include "bf0_hal.h"
#include <cJSON.h>
#include "button.h"
#include "audio_server.h"
#include <webclient.h>
#include "./iot/iot_c_api.h"
#include "./mcp/mcp_api.h"
#include "lv_timer.h"
#include "lv_display.h"
#include "lv_obj_pos.h"
#include "lvgl.h"
#include "lv_tiny_ttf.h"
#include "lv_obj.h"
#include "lv_label.h"
#include "lib_et_asr.h"
#include "../weather/weather.h"
#ifdef BSP_USING_PM
    #include "gui_app_pm.h"
#endif // BSP_USING_PM
#include "xiaozhi_client_public.h"
#include "xiaozhi_ui.h"
#include "xiaozhi_audio.h"
#include "xiaozhi_music.h"

/* altcp 不是线程安全的：麦克风上行在音频线程里，hello / listen / abort
 * 在 websocket 线程里，两边同时写会让 altcp 进入错误状态，
 * 表现就是 altcp_write() failed 然后整条连接被关闭。统一串行化。 */
static struct rt_mutex ws_tx_mutex;
static volatile uint32_t ws_audio_generation;
static uint32_t ws_audio_sent;
static uint32_t ws_audio_blocked;
int g_xz_uplink_on = 1;

static err_t xz_ws_write_frame(wsock_state_t *st, const char *data, u16_t len,
                              uint8_t op, uint32_t generation)
{
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    err_t e;
    /* Check at the actual write, including retries. Queue-time checks alone
     * leak queued audio after goodbye, or into the next conversation. */
    if (op == OPCODE_BINARY &&
        (!xz_ws_uplink_allowed() || generation != ws_audio_generation))
    {
        ws_audio_blocked++;
        e = ERR_ABRT;
    }
    else
    {
        e = wsock_write(st, data, len, op);
        if (op == OPCODE_BINARY && e == ERR_OK) ws_audio_sent++;
    }
    rt_mutex_release(&ws_tx_mutex);
    return e;
}

static err_t xz_ws_write_locked(wsock_state_t *st, const char *data, u16_t len,
                                uint8_t op)
{
    return xz_ws_write_frame(st, data, len, op, ws_audio_generation);
}


#define MAX_WSOCK_HDR_LEN 4096
#define WEBSOC_RECONNECT 4
#define XZ_EVT_BUTTON_PRESS   5
#define XZ_EVT_BUTTON_RELEASE 6
#define SPEAKING_THRESHOLD_MS (5 * 60 * 1000) // 小智讲话时间阈值 

extern BOOL g_pan_connected;
extern xz_audio_t *thiz;
extern rt_mailbox_t g_bt_app_mb;
extern lv_obj_t *main_container;
extern lv_obj_t *standby_screen;
extern uint8_t Initiate_disconnection_flag;
extern rt_mailbox_t g_ui_task_mb;
extern rt_tick_t last_listen_tick;
extern lv_timer_t *ui_sleep_timer;
extern volatile rt_tick_t g_xz_rx_tick;


/* 33 KB of pure software state: keep it in PSRAM so the internal heap keeps
 * room for the AIC8800 driver task (it allocates its own stack from it). */
#if defined(__CC_ARM) || defined(__CLANG_ARM)
L2_RET_BSS_SECT_BEGIN(xz_ws_state)
xiaozhi_ws_t g_xz_ws;
L2_RET_BSS_SECT_END
#else
xiaozhi_ws_t g_xz_ws L2_RET_BSS_SECT(xz_ws_state);
#endif
rt_mailbox_t g_button_event_mb;
enum DeviceState web_g_state = kDeviceStateConnecting;

static const char *mode_str[] = {"auto", "manual", "realtime"};
static const char *hello_message =
    "{"
    "\"type\":\"hello\","
    "\"version\": 3,"
#ifdef CONFIG_IOT_PROTOCOL_MCP
    "\"features\":{\"mcp\":true},"
#endif
    "\"transport\":\"websocket\","
    "\"audio_params\":{"
    "\"format\":\"opus\", \"sample_rate\":16000, \"channels\":1, "
    "\"frame_duration\":60"
    "}}";

// 倒计时动画
static lv_obj_t *countdown_screen = NULL;
static rt_thread_t countdown_thread = RT_NULL;
bool shutdown_state = true;

static rt_tick_t g_speaking_start_tick = 0;  // 讲话开始时间
static rt_tick_t g_total_speaking_time = 0;  // 累计讲话时间
static bool g_is_speaking = false;           // 是否正在讲话

/* Track the mode actually sent to the server, rather than guessing from UI text. */
static enum ListeningMode ws_listen_mode = kListeningModeAlwaysOn;
static volatile bool ws_session_active;
static volatile bool ws_hello_ready;
static volatile bool ws_hello_pending;
static volatile bool ws_wake_pending;
static volatile bool ws_reconnect_allowed = true;
static volatile bool ws_beep_pending;
static volatile bool ws_beep_done;
static volatile uint32_t ws_wake_token;
static volatile uint32_t ws_done_token;
static volatile int ws_beep_result;
/* Set when a local intent (SD music) already handled the current utterance;
 * the next server TTS reply for that turn is aborted instead of played. */
static volatile bool ws_mute_reply;
static rt_tick_t ws_hello_tick;
static struct rt_thread ws_connect_thread;
static uint8_t ws_connect_stack[6144] L2_RET_BSS_SECT(xz_ws_connect_stack);
static bool ws_connect_started;
void simulate_button_pressed(void);
static uint32_t ws_speaking_generation;
static rt_tick_t ws_barge_start_tick;
static bool ws_tts_stop_pending;
static struct
{
    uint32_t generation;
    uint32_t ref_age_ms;
    uint16_t rms, floor, pre, ref, gain, ref_hold;
    uint8_t frames;
    bool pending, wake_word, dtd, clipped;
} ws_barge_request;

bool xz_ws_uplink_allowed(void)
{
    return g_xz_uplink_on && g_xz_ws.is_connected && ws_session_active &&
           (web_g_state == kDeviceStateListening ||
            web_g_state == kDeviceStateSpeaking);
}

uint32_t xz_ws_audio_epoch(void) { return ws_audio_generation; }

bool xz_ws_playback_allowed(void) { return ws_session_active; }

/* Unknown is reserved for explicit power/sleep operations, never goodbye. */
static void ws_set_state(enum DeviceState state)
{
    face_state_t face_state;
    char *status;
    switch (state)
    {
    case kDeviceStateConnecting: face_state = FS_CONNECT; status = "连接中..."; break;
    case kDeviceStateIdle:       face_state = FS_IDLE;    status = "待命中..."; break;
    case kDeviceStateListening:  face_state = FS_LISTEN;  status = "聆听中..."; break;
    case kDeviceStateThinking:   face_state = FS_THINK;   status = "思考中..."; break;
    case kDeviceStateSpeaking:   face_state = FS_SPEAK;   status = "讲话中..."; break;
    case kDeviceStateUnknown:    face_state = FS_SLEEP;   status = "睡眠中..."; break;
    case kDeviceStateFatalError: face_state = FS_ERROR;   status = "连接失败..."; break;
    case kDeviceStateWifiConfiguring: face_state = FS_CONFIG; status = "配网中..."; break;
    default: return;
    }
    /* Serialize the state boundary against binary wsock_write(). Once IDLE
     * is published, even a TX retry cannot write another microphone frame. */
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    rt_enter_critical();
    if (state == kDeviceStateSpeaking)
    {
        ++ws_speaking_generation;
        ws_barge_start_tick = rt_tick_get();
    }
    ws_barge_request.pending = false;
    ws_tts_stop_pending = false;
    web_g_state = state;
    rt_exit_critical();
    rt_mutex_release(&ws_tx_mutex);
    xiaozhi_ui_set_face_state(face_state);
    xiaozhi_ui_chat_status(status);
}

void xiaozhi_ws_network_status(bool failed)
{
    ws_set_state(failed ? kDeviceStateFatalError : kDeviceStateConnecting);
}

void xiaozhi_ws_configuring(void)
{
    ws_wake_pending = ws_beep_pending = false;
    ws_hello_ready = ws_hello_pending = false;
    ws_reconnect_allowed = false;
    ws_wake_token++;
    ws_session_active = false;
    g_is_speaking = false;
    xz_speaker(0);
    ws_set_state(kDeviceStateWifiConfiguring);
}

void xiaozhi_ws_sleep(void)
{
    ws_wake_pending = ws_beep_pending = false;
    ws_hello_ready = ws_hello_pending = false;
    ws_reconnect_allowed = false;
    ws_wake_token++;
    ws_session_active = false;
    g_is_speaking = false;
    xz_speaker(0);
    ws_set_state(kDeviceStateUnknown);
}

static void ws_enter_standby(const char *reason)
{
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    ws_session_active = false;
    ws_wake_pending = ws_beep_pending = false;
    ws_hello_ready = ws_hello_pending = false;
    ws_reconnect_allowed = false;
    ws_mute_reply = false;
    ws_wake_token++;
    ws_audio_generation++;
    rt_mutex_release(&ws_tx_mutex);
    g_is_speaking = false;
    xz_audio_reset_uplink();
    xz_speaker(0);
    xz_music_tts(false);
    ws_set_state(kDeviceStateIdle);
    rt_kprintf("WS_STANDBY reason=%s audio_tx=%u\n", reason,
               (unsigned)ws_audio_sent);
    xiaozhi_ui_chat_output("等待唤醒...");
    xiaozhi_ui_update_emoji("neutral");
}

static const char *ws_dismiss_intent(const char *text)
{
    static const char *const words[] = {"退下吧", "退下", "结束", "不用了", "关闭对话"};
    if (!text) return NULL;
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (strstr(text, words[i])) return words[i];
    return NULL;
}

static void ws_end_speaking(void)
{
    g_is_speaking = false;
    xz_speaker(0);
    xz_music_tts(false);
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    if (ws_session_active)
        ws_set_state(ws_listen_mode == kListeningModeAlwaysOn
                         ? kDeviceStateListening : kDeviceStateIdle);
    rt_mutex_release(&ws_tx_mutex);
}

static char *ws_json_string(cJSON *root, const char *key)
{
    cJSON *value = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}


void parse_helLo(const u8_t *data, u16_t len);

void send_iot_descriptors(void)
{
    const char *desc = iot_get_descriptors_json();
    if (desc == NULL)
    {
        rt_kprintf("Failed to get IoT descriptors\n");
        return;
    }

    /* One static buffer: the descriptor array grew past 1 KB once the Light
     * thing was added (and past 1.8 KB with Music). A truncated snprintf
     * produced invalid JSON that the server rejected with a bare "error". */
    static char msg[3072];
    int n = snprintf(msg, sizeof(msg),
                     "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,"
                     "\"descriptors\":%s}",
                     g_xz_ws.session_id, desc);
    if (n < 0 || n >= (int)sizeof(msg))
    {
        rt_kprintf("WS_IOT_DESC overflow n=%d buf=%d desc=%d\n", n,
                   (int)sizeof(msg), (int)strlen(desc));
        return;
    }
    rt_kprintf("WS_IOT_DESC len=%d desc=%d\n", n, (int)strlen(desc));
    if (g_xz_ws.is_connected == 1)
    {
        xz_ws_write_locked(&g_xz_ws.clnt, msg, n, OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

void send_iot_states(void)
{
    const char *state = iot_get_states_json();
    if (state == NULL)
    {
        rt_kprintf("Failed to get IoT states\n");
        return;
    }

    static char msg[1024];
    int n = snprintf(msg, sizeof(msg),
                     "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,"
                     "\"states\":%s}",
                     g_xz_ws.session_id, state);
    if (n < 0 || n >= (int)sizeof(msg))
    {
        rt_kprintf("WS_IOT_STATE overflow n=%d buf=%d state=%d\n", n,
                   (int)sizeof(msg), (int)strlen(state));
        return;
    }
    rt_kprintf("WS_IOT_STATE len=%d state=%d\n", n, (int)strlen(state));
    if (g_xz_ws.is_connected == 1)
    {
        xz_ws_write_locked(&g_xz_ws.clnt, msg, n, OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

void ws_send_speak_abort(void *ws, char *session_id, int reason)
{
    char message[256];
    rt_enter_critical();
    xz_audio_reset_barge_in();
    ws_barge_request.pending = false;
    rt_exit_critical();
    rt_kprintf("SPEAK_ABORT reason=%s(%d) state=%d\n",
               reason ? "wake_word_detected" : "none", reason, web_g_state);
    rt_snprintf(message, 256, "{\"session_id\":\"%s\",\"type\":\"abort\"",
                session_id);
    if (reason)
        strcat(message, ",\"reason\":\"wake_word_detected\"}");
    else
        strcat(message, "}");

    if (g_xz_ws.is_connected == 1)
    {
        xz_ws_write_locked((wsock_state_t *)ws, message, strlen(message), OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

bool xz_ws_barge_in_context(uint32_t *generation, rt_tick_t *start)
{
    rt_enter_critical();
    *generation = ws_speaking_generation;
    *start = ws_barge_start_tick;
    bool active = g_xz_ws.is_connected && ws_session_active &&
                  web_g_state == kDeviceStateSpeaking && !ws_tts_stop_pending;
    rt_exit_critical();
    return active;
}

bool xz_ws_request_barge_in(uint32_t generation, uint32_t rms, uint32_t floor,
                          uint32_t pre, unsigned frames,
                          bool wake_word)
{
    rt_enter_critical();
    bool active = g_xz_ws.is_connected && ws_session_active &&
                  web_g_state == kDeviceStateSpeaking &&
                  (wake_word || !ws_tts_stop_pending) &&
                  generation == ws_speaking_generation;
    if (active && !ws_barge_request.pending)
    {
        ws_barge_request.generation = generation;
        ws_barge_request.rms = rms;
        ws_barge_request.floor = floor;
        ws_barge_request.pre = pre;
        ws_barge_request.ref = ws_barge_request.gain = 0;
        ws_barge_request.ref_hold = 0;
        ws_barge_request.ref_age_ms = XZ_BARGE_REF_HOLD_MS;
        ws_barge_request.dtd = false;
        ws_barge_request.clipped = false;
        ws_barge_request.frames = frames;
        ws_barge_request.wake_word = wake_word;
        ws_barge_request.pending = true; /* publish last, no mailbox/heap allocation */
    }
    rt_exit_critical();
    return active;
}

bool xz_ws_request_speech_barge_in(const xz_barge_detector_t *detector)
{
    rt_enter_critical();
    bool accepted = false;
    if (!ws_barge_request.pending)
    {
        accepted = xz_ws_request_barge_in(detector->generation, detector->rms,
            detector->echo_floor, detector->pre_rms, detector->frames, false);
        if (accepted)
        {
            ws_barge_request.ref = detector->ref_rms;
            ws_barge_request.ref_hold = detector->ref_hold;
            ws_barge_request.ref_age_ms = detector->ref_age_ms;
            ws_barge_request.gain = detector->echo_gain_milli;
            ws_barge_request.dtd = detector->dtd;
            ws_barge_request.clipped = detector->clipped;
        }
    }
    rt_exit_critical();
    return accepted;
}

static void ws_abort_speaking(int reason, const char *source)
{
    /* Caller holds ws_tx_mutex. No reconnect/listen_start/beep and no music
     * stop command: xz_speaker(0) releases the existing TTS music pause. */
    if (!g_xz_ws.is_connected || !ws_session_active ||
        web_g_state != kDeviceStateSpeaking) return;
    rt_kprintf("SPEAK_INTERRUPT source=%s generation=%u\n", source,
               (unsigned)ws_speaking_generation);
    ws_send_speak_abort(&g_xz_ws.clnt, g_xz_ws.session_id, reason);
    xz_speaker(0);
    g_is_speaking = false;
    ws_set_state(kDeviceStateListening);
}

void xz_ws_process_barge_in(void)
{
    /* Called every ~20 ms from the main event thread. The audio callback and
     * KWS thread only publish a small request, never take the socket lock. */
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    rt_enter_critical();
    bool pending = ws_barge_request.pending;
    uint32_t generation = ws_barge_request.generation;
    uint32_t rms = ws_barge_request.rms;
    uint32_t floor = ws_barge_request.floor;
    uint32_t pre = ws_barge_request.pre;
    uint32_t ref = ws_barge_request.ref;
    uint32_t ref_hold = ws_barge_request.ref_hold;
    uint32_t ref_age_ms = ws_barge_request.ref_age_ms;
    uint32_t gain = ws_barge_request.gain;
    bool dtd = ws_barge_request.dtd;
    bool clipped = ws_barge_request.clipped;
    unsigned frames = ws_barge_request.frames;
    bool wake_word = ws_barge_request.wake_word;
    ws_barge_request.pending = false;
    rt_exit_critical();
    if (pending && ws_session_active && g_xz_ws.is_connected &&
        web_g_state == kDeviceStateSpeaking && generation == ws_speaking_generation &&
        (wake_word || !ws_tts_stop_pending) &&
        !xz_audio_wake_beep_active() && !Initiate_disconnection_flag)
    {
        if (!wake_word)
            rt_kprintf("BARGE_IN gen=%u source=speech pre=%u post=%u ref=%u g=%u dtd=%u rms=%u floor=%u frames=%u refh=%u ref_age=%ums clip=%u ref_blk=%u\n",
                       (unsigned)generation, (unsigned)pre, (unsigned)rms,
                       (unsigned)ref, (unsigned)gain, (unsigned)dtd,
                       (unsigned)rms, (unsigned)floor, frames,
                       (unsigned)ref_hold, (unsigned)ref_age_ms,
                       (unsigned)clipped, (unsigned)ref);
        ws_abort_speaking(kAbortReasonWakeWordDetected, wake_word ? "kws" : "speech");
    }
    /* Normal tts stop drains queued PCM before closing the TX gate. An abort
     * instead discards it immediately, including a partially decoded block. */
    if (ws_tts_stop_pending && web_g_state == kDeviceStateSpeaking &&
        xz_audio_tts_drained()) ws_end_speaking();
    rt_mutex_release(&ws_tx_mutex);
}

void xz_ws_wake_word_detected(void)
{
    if (xz_audio_wake_beep_active() || Initiate_disconnection_flag) return;
    uint32_t generation;
    rt_tick_t start;
    rt_kprintf("KWS_WAKE state=%d\n", web_g_state);
    xz_ws_barge_in_context(&generation, &start);
    /* A wake word may still explicitly interrupt PCM draining after stop;
     * only the automatic speech detector is disabled by tts stop. */
    if (!xz_ws_request_barge_in(generation, 0, 0, 0, 0, true))
        simulate_button_pressed(); /* retain the existing Standby/session wake */
}

void ws_send_listen_start(void *ws, char *session_id, enum ListeningMode mode)
{
    char message[256];
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    if (!ws_wake_pending || !ws_beep_done || ws_beep_result != RT_EOK ||
        ws_done_token != ws_wake_token || !ws_hello_ready)
    {
        rt_mutex_release(&ws_tx_mutex);
        return; /* Every new session must complete its local wake cue first. */
    }
    uint32_t token = ws_wake_token;
    rt_kprintf("listen start,mode=%d\n",mode);
    rt_snprintf(message, 256,
                "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":"
                "\"start\",\"mode\":\"%s\"}",
                session_id, mode_str[mode]);
    // rt_kputs("\r\n");
    // rt_kputs(message);
    // rt_kputs("\r\n");
    if (g_xz_ws.is_connected == 1)
    {
        if (xz_ws_write_locked((wsock_state_t *)ws, message, strlen(message), OPCODE_TEXT) == ERR_OK)
        {
            if (ws_wake_pending && token == ws_wake_token && ws_hello_ready &&
                g_xz_ws.is_connected)
            {
                ws_listen_mode = mode;
                ws_session_active = true;
                ws_set_state(kDeviceStateListening);
            }
        }
        else
        {
            ws_session_active = false;
            ws_set_state(kDeviceStateFatalError);
        }
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
    rt_mutex_release(&ws_tx_mutex);
}

void ws_send_listen_stop(void *ws, char *session_id)
{
    char message[256];
    rt_kprintf("listen stop\n");
    rt_snprintf(
        message, 256,
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
        session_id);
    if (g_xz_ws.is_connected == 1)
    {
        xz_ws_write_locked((wsock_state_t *)ws, message, strlen(message), OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}
void ws_send_hello(void *ws)
{
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    if (g_xz_ws.is_connected == 1 && !ws_hello_pending && !ws_hello_ready)
    {
        ws_hello_pending = true;
        ws_hello_tick = rt_tick_get();
        if (xz_ws_write_locked((wsock_state_t *)ws, hello_message,
                               strlen(hello_message), OPCODE_TEXT) != ERR_OK)
        {
            ws_hello_pending = false;
            ws_wake_pending = false;
            ws_set_state(kDeviceStateFatalError);
            wsock_close(&g_xz_ws.clnt, WSOCK_RESULT_ERR_CONNECT, ERR_CONN);
            g_xz_ws.is_connected = 0;
        }
    }
    rt_mutex_release(&ws_tx_mutex);
}

void xz_ws_wake_beep_done(uint32_t token, int result)
{
    /* Audio thread reports completion; only the main event thread may start
     * listening. A stale completion can never resurrect a cancelled wake. */
    rt_enter_critical();
    ws_done_token = token;
    ws_beep_result = result;
    ws_beep_done = true;
    rt_exit_critical();
}

void xz_ws_process_wake(void)
{
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    if (!ws_wake_pending || !g_xz_ws.is_connected) goto out;
    if (!ws_hello_ready)
    {
        ws_send_hello(&g_xz_ws.clnt);
        goto out;
    }
    if (!ws_beep_pending)
    {
        ws_beep_done = false;
        ws_beep_pending = true;
        uint32_t token = ++ws_wake_token;
        int result = xz_audio_request_wake_beep(token);
        if (result == -RT_EBUSY)
        {
            ws_beep_pending = false; /* a cancelled cue is still draining */
            goto out;
        }
        if (result != RT_EOK)
            xz_ws_wake_beep_done(token, result);
        goto out;
    }
    if (!ws_beep_done || ws_done_token != ws_wake_token) goto out;
    if (ws_beep_result == RT_EOK)
    {
        xz_audio_reset_uplink();
        rt_kprintf("WAKE_BEEP complete token=%u -> listen_start\n",
                   (unsigned)ws_wake_token);
        ws_send_listen_start(&g_xz_ws.clnt, g_xz_ws.session_id,
                             kListeningModeAlwaysOn);
    }
    else
    {
        rt_kprintf("WAKE_BEEP failed token=%u err=%d; stay Standby\n",
                   (unsigned)ws_wake_token, ws_beep_result);
        ws_set_state(kDeviceStateIdle);
    }
    ws_wake_pending = ws_beep_pending = false;
out:
    rt_mutex_release(&ws_tx_mutex);
}
/* ---- 上行音频发送线程 ----------------------------------------------------
 * 这个 lwIP 端口没开 LWIP_TCPIP_CORE_LOCKING，在音频线程里直接调
 * wsock_write() 会和 lwIP 线程的收包处理打架，结果是 altcp_write() 失败、
 * 整条连接被关闭。上行音频改成投递到环形队列，由专属线程统一写 socket。 */
#define XZ_TX_RB_SIZE 8192
static uint8_t xz_tx_rb_pool[XZ_TX_RB_SIZE] L2_RET_BSS_SECT(xz_tx_rb);
static struct rt_ringbuffer xz_tx_rb_state;
static uint8_t xz_tx_stack[3072] L2_RET_BSS_SECT(xz_tx_stack);
static struct rt_thread xz_tx_thread;
static struct rt_ringbuffer *xz_tx_rb = NULL;
static struct rt_event *xz_tx_evt = NULL;

static void xz_ws_tx_entry(void *p)
{
    uint8_t hdr[7]; /* length, opcode, conversation generation (little endian) */
    static uint8_t frame[512];
    (void)p;
    for (;;)
    {
        rt_uint32_t evt = 0;
        rt_event_recv(xz_tx_evt, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &evt);
        for (;;)
        {
            rt_enter_critical();
            if (rt_ringbuffer_data_len(xz_tx_rb) < sizeof(hdr))
            {
                rt_exit_critical();
                break;
            }
            rt_ringbuffer_get(xz_tx_rb, hdr, sizeof(hdr));
            uint16_t len = (uint16_t)(hdr[0] | (hdr[1] << 8));
            uint8_t op = hdr[2];
            if (len == 0 || len > sizeof(frame))
            {
                rt_ringbuffer_reset(xz_tx_rb);
                rt_exit_critical();
                continue;
            }
            rt_ringbuffer_get(xz_tx_rb, frame, len);
            rt_exit_critical();
            uint32_t generation = (uint32_t)hdr[3] | ((uint32_t)hdr[4] << 8) |
                                  ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 24);
            if (xz_ws_uplink_allowed() && generation == ws_audio_generation)
            {
                /* AIC 驱动每发一个包都要从内部堆申请 ~3 KB 大块内存。堆里只有
                 * 几十 KB 且已碎片化，TX 环积压时申请必然失败（sdio buf alloc
                 * fail -> 包发不出去 -> TCP 窗口永久卡死）。环快满时先等一等。 */
                extern bool aicwf_net_buf_tx_free_list_is_full(void);
                int guard = 0;
                while (aicwf_net_buf_tx_free_list_is_full() &&
                       xz_ws_uplink_allowed() &&
                       generation == ws_audio_generation && guard++ < 200)
                {
                    rt_thread_mdelay(5);
                }
                /* ERR_MEM = 发送窗口暂时满（对端未确认）。此时不能丢帧也不能关连接，
                 * 原地退避重试，最多 100 x 20 ms = 2 s，之后丢弃这一帧并记账。 */
                err_t e = xz_ws_write_frame(&g_xz_ws.clnt, (const char *)frame,
                                            len, op, generation);
                int spins = 0;
                while (e == ERR_MEM && xz_ws_uplink_allowed() &&
                       generation == ws_audio_generation && spins < 100)
                {
                    rt_thread_mdelay(20);
                    e = xz_ws_write_frame(&g_xz_ws.clnt, (const char *)frame,
                                          len, op, generation);
                    spins++;
                }
                if (e == ERR_OK)
                {
                    static uint32_t sent;
                    if ((++sent % 50) == 0)
                        rt_kprintf("ws tx sent %u frames\n", (unsigned)sent);
                }
                if (e == ERR_MEM)
                {
                    static rt_uint32_t drop_cnt;
                    if ((++drop_cnt % 30) == 1)
                    {
                        rt_kprintf("ws tx backpressure: dropped %u frames\n",
                                   (unsigned)drop_cnt);
                    }
                }
            }
        }
    }
}

static void xz_ws_tx_init(void)
{
    if (xz_tx_rb != NULL)
    {
        return;
    }
    rt_ringbuffer_init(&xz_tx_rb_state, xz_tx_rb_pool, sizeof(xz_tx_rb_pool));
    xz_tx_rb = &xz_tx_rb_state;
    xz_tx_evt = rt_event_create("xz_tx", RT_IPC_FLAG_FIFO);
    if (xz_tx_evt == NULL)
    {
        xz_tx_rb = NULL;
        rt_kprintf("xz tx queue alloc failed\n");
        return;
    }
    rt_err_t err = rt_thread_init(&xz_tx_thread, "xz_ws_tx", xz_ws_tx_entry,
                                  RT_NULL, xz_tx_stack, sizeof(xz_tx_stack),
                                  20, 10);
    if (err == RT_EOK)
    {
        rt_thread_startup(&xz_tx_thread);
    }
    else
    {
        rt_event_delete(xz_tx_evt);
        xz_tx_evt = NULL;
        xz_tx_rb = NULL;
        rt_kprintf("xz tx thread init failed: %d\n", err);
    }
}

void xz_audio_send_using_websocket(uint8_t *data, int len)
{
    if (!xz_tx_rb || !data || len <= 0 || len > 400) return;
    rt_enter_critical();
    uint32_t generation = ws_audio_generation;
    bool queued = false;
    if (xz_ws_uplink_allowed() &&
        rt_ringbuffer_space_len(xz_tx_rb) >= (rt_size_t)(len + 7))
    {
        uint8_t hdr[7] = {(uint8_t)len, (uint8_t)(len >> 8), OPCODE_BINARY,
                         (uint8_t)generation, (uint8_t)(generation >> 8),
                         (uint8_t)(generation >> 16), (uint8_t)(generation >> 24)};
        rt_ringbuffer_put(xz_tx_rb, hdr, sizeof(hdr));
        rt_ringbuffer_put(xz_tx_rb, data, len);
        queued = true;
    }
    rt_exit_critical();
    if (queued) rt_event_send(xz_tx_evt, 1);
}

static void xz_wsstat(int argc, char **argv)
{
    (void)argc; (void)argv;
    rt_kprintf("WS_AUDIO connected=%d ready=%d session=%d state=%d wake=%d "
               "uplink=%d audio_tx=%u blocked=%u epoch=%u\n",
               g_xz_ws.is_connected, ws_hello_ready, ws_session_active,
               web_g_state, ws_wake_pending, xz_ws_uplink_allowed(),
               (unsigned)ws_audio_sent, (unsigned)ws_audio_blocked,
               (unsigned)ws_audio_generation);
}
MSH_CMD_EXPORT(xz_wsstat, show websocket session and actual audio writes);

/* 正在发起连接（还没拿到 101）：此期间的 DISCONNECT 都是残留事件。 */
static volatile int ws_connecting = 0;
static volatile int ws_ever_connected = 0;

err_t my_wsapp_fn(int code, char *buf, size_t len)
{
    if (code == WS_CONNECT)
    {
        int status = (uint16_t)(uint32_t)buf;
        rt_kprintf("ws on_connect status=%d\n", status);
        if (status == 101) // wss setup success
        {
            ws_ever_connected = 1;
            g_xz_ws.is_connected = 1;
            rt_sem_release(g_xz_ws.sem);
        }
    }
    else if (code == WS_DISCONNECT)
    {
        if (!g_xz_ws.is_connected)
        {
            /* 旧 socket 的 poll 线程在重建期间还会甩出断开事件，
             * 直接放行会把刚发起的新连接立刻判死。 */
            if (ws_connecting)
            {
                return ERR_OK;
            }
            rt_sem_release(g_xz_ws.sem);
            return ERR_OK; /* A late close must not overwrite a failed attempt. */
        }
        else
        {
            //  #ifdef BSP_USING_PM
            //             // 关闭 VAD
            //             if(thiz->vad_enabled)
            //             {
            //                 thiz->vad_enabled = false;
            //                 rt_kprintf("web_cloae,so vad_close\n");
            //             }
            //  #endif
            MCP_RGBLED_CLOSE();

            xiaozhi_ui_chat_output(ws_reconnect_allowed
                                      ? "小智已断开，正在重连..." : "等待唤醒...");
            xiaozhi_ui_update_emoji("neutral");
        }
        rt_kprintf("WebSocket closed\n");
        g_xz_ws.is_connected = 0;
        ws_session_active = false;
        ws_hello_ready = ws_hello_pending = false;
        ws_beep_pending = false;
        ws_wake_token++;
        ws_audio_generation++;
        xz_audio_reset_uplink();
        g_is_speaking = false;
        xz_speaker(0);
        if (ws_reconnect_allowed)
            ws_set_state(kDeviceStateConnecting);
        else if (web_g_state != kDeviceStateUnknown &&
                 web_g_state != kDeviceStateWifiConfiguring)
            ws_set_state(kDeviceStateIdle);
    }
    else if (code == WS_TEXT)
    {
        g_xz_rx_tick = rt_tick_get();
        // 打印原始数据
        rt_kprintf("web send to me:\n");
        rt_kprintf("%.*s\n", (int)len, buf); // 打印接收到的文本数据
        parse_helLo(buf, len);
    }
    else
    {
        // Receive Audio Data
        if (ws_session_active && web_g_state == kDeviceStateSpeaking)
            xz_audio_downlink(buf, len, NULL, 0);
    }
    return 0;
}
void xiaozhi2(int argc, char **argv);

static void xz_button_event_handler(int32_t pin, button_action_t action)
{
    (void)pin;
    xiaozhi_ui_wake(); // 唤醒设备
    rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
    /* KWS emits PRESS only. Session/pending guards debounce it without
     * requiring a RELEASE, and the shared KWS stream stays available. */

    if (action == BUTTON_PRESSED)
    {
        rt_kprintf("pressed\r\n");
        rt_kprintf("按键->对话");

        /* Music is a standalone function. Standby gates the mic, so a wake is
         * the only way to reach the device: let it silence playback first,
         * then continue into the normal wake flow ("播放音乐" restarts it). */
        if (xz_music_is_playing())
        {
            xz_music_stop();
            rt_kprintf("BTN_MUSIC_STOP\n");
            xiaozhi_ui_chat_output("音乐已停止");
        }

        // 检查是否弹窗显示中

        xiaozhi_ui_update_confirm_button_event(1); // 模拟点击更新按钮

        if (ws_wake_pending) goto out;
        if (ws_session_active)
        {
            if (web_g_state == kDeviceStateSpeaking)
            {
                /* 正在播报时按键 = 打断。realtime 模式下不必重发 listen。 */
                ws_abort_speaking(kAbortReasonNone, "button");
            }
            ws_set_state(kDeviceStateListening);
            goto out;
        }
        ws_wake_pending = true;
        ws_beep_pending = ws_beep_done = false;
        ws_reconnect_allowed = true;
        rt_kprintf("WAKE_REQUEST connected=%d ready=%d\n",
                   g_xz_ws.is_connected, ws_hello_ready);
        if (!g_xz_ws.is_connected) ws_set_state(kDeviceStateConnecting);
        xiaozhi2(0, NULL); /* starts the monitor once, then returns */
        xz_ws_process_wake();
    }
    else if (action == BUTTON_RELEASED)
    {
#ifdef BSP_USING_PM
        xiaozhi_ui_wake();
#endif
        rt_kprintf("released\r\n");
        // 手动模式松键结束上行；always-on 继续聆听。
        if (g_xz_ws.is_connected && ws_session_active &&
            ws_listen_mode != kListeningModeAlwaysOn &&
            web_g_state == kDeviceStateListening)
        {
            ws_send_listen_stop(&g_xz_ws.clnt, g_xz_ws.session_id);
            ws_set_state(kDeviceStateThinking);
        }
    }
out:
    rt_mutex_release(&ws_tx_mutex);
}
#ifndef XIAOZHI_USING_MQTT
/* 唤醒词/VAD 在 KWS 线程里触发；那里直接跑按钮处理会连带调用 LVGL 和
 * websocket，把 KWS 线程的栈压爆（崩溃点落在 kiss_fftr2 的 FFT 里）。
 * 统一投递到主事件线程执行。 */
void simulate_button_pressed()
{
    if (Initiate_disconnection_flag) //蓝牙主动断开不允许mic触发
        return;
    rt_mb_send(g_bt_app_mb, XZ_EVT_BUTTON_PRESS);
}

/* 主事件线程入口 */
void xz_do_button_press(void)
{
    xz_button_event_handler(BSP_KEY1_PIN, BUTTON_PRESSED);
}
void simulate_button_released()
{
    if (Initiate_disconnection_flag)
        return;
    rt_mb_send(g_bt_app_mb, XZ_EVT_BUTTON_RELEASE);
}

void xz_do_button_release(void)
{
    xz_button_event_handler(BSP_KEY1_PIN, BUTTON_RELEASED);
}
#endif

static void xz_button2_event_handler(int32_t pin, button_action_t action)
{
    if (action == BUTTON_PRESSED)
    {

        rt_kprintf("xz_button2_event_handler - pressed\n");

        xiaozhi_ui_update_confirm_button_event(0); // 模拟点击取消按钮

    }
    else if (action == BUTTON_LONG_PRESSED)
    {
        // 按下超过3秒，触发关机
        rt_kprintf("xz_button2_event_handler - long pressed\n");
        //检查设备是否已绑定设备码
        // if (g_activation_context.is_activated)
        // {
        //     rt_sem_release(g_activation_context.sem);
        // }

            // 长按3秒，直接发送关机消息到ui_task
        shutdown_state = false;
        xiaozhi_ui_wake(); // 唤醒设备
        rt_thread_mdelay(100);
        rt_mb_send(g_ui_task_mb, UI_EVENT_SHUTDOWN);
    }

    else if (action == BUTTON_RELEASED)
    {
        rt_kprintf("xz_button2_event_handler - released\n");
    }
}

void xz_ws_button_init(void) // Session key
{
    static int initialized = 0;
    rt_kprintf("xz_ws_button_init\n");
    if (initialized == 0)
    {
        // 按键1（对话+唤醒）
        button_cfg_t cfg1;
        cfg1.pin = BSP_KEY1_PIN;
        cfg1.active_state = KEY1_ACTIVE_LEVEL;
        cfg1.mode = PIN_MODE_INPUT;
        cfg1.button_handler = xz_button_event_handler; // Session key
        int32_t id1 = button_init(&cfg1);
        RT_ASSERT(id1 >= 0);
        RT_ASSERT(SF_EOK == button_enable(id1));
        initialized = 1;
    }
}
void xz_ws_button_init2(void)
{
    static int initialized = 0;
    rt_kprintf("xz_ws_button2_init\n");
    /* This board wires a single user key (BSP_KEY1_PIN); the power key is
     * handled by the PMIC, so there is no second GPIO button to register. */
    (void)xz_button2_event_handler;
    initialized = 1;
}
void xz_ws_audio_init()
{
    rt_mutex_init(&ws_tx_mutex, "ws_tx", RT_IPC_FLAG_PRIO);
    /* g_xz_ws 位于 PSRAM 段，软复位后不会像普通 .bss 那样被清零。上次运行的
     * pcb/sem 指针会原样留在内存里：pcb 非空会让 wsock_init() 直接失败，sem
     * 变成野指针则让 rt_sem_take() 崩在 rt_object_get_type()。只清状态字段，
     * 不对整个 33 KB 结构做 memset（它与 KWS 线程栈紧邻）。 */
    g_xz_ws.sem = NULL;
    g_xz_ws.is_connected = 0;
    g_xz_ws.clnt.pcb = NULL;
    g_xz_ws.clnt.state0 = 0;
    g_xz_ws.clnt.state1 = 0;
    g_xz_ws.session_id[0] = '\0';
    g_xz_ws.sample_rate = 0;
    g_xz_ws.frame_duration = 0;

    rt_kprintf("xz_audio_init\n");
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, VOL_DEFAULE_LEVEL);
    xz_audio_decoder_encoder_open(1); // 打开音频解码器和编码器

}
/* Local intent handling for a recognised utterance. Kept as one function so the
 * console (xz_say) can exercise it exactly like a server "stt" message. */
static void ws_handle_stt_text(char *txt)
{
    if (!ws_session_active || txt == NULL)
        return;

    const char *intent = ws_dismiss_intent(txt);
    if (intent)
    {
        rt_kprintf("DISMISS_INTENT keyword=%s text=%s\n", intent, txt);
        ws_enter_standby("local_dismiss"); /* shut the gate before sending */
        ws_send_speak_abort(&g_xz_ws.clnt, g_xz_ws.session_id, kAbortReasonNone);
        char goodbye[128];
        rt_snprintf(goodbye, sizeof(goodbye),
                    "{\"session_id\":\"%s\",\"type\":\"goodbye\"}",
                    g_xz_ws.session_id);
        if (g_xz_ws.is_connected)
            xz_ws_write_locked(&g_xz_ws.clnt, goodbye, strlen(goodbye), OPCODE_TEXT);
        return;
    }
    ws_set_state(kDeviceStateThinking);
    /* A local music command is complete on its own: remember to drop the
     * cloud reply (its music/search plugin would talk over the SD player). */
    ws_mute_reply = xz_music_handle_text(txt);
    xiaozhi_ui_chat_output(txt);
    last_listen_tick = rt_tick_get();
}

void parse_helLo(const u8_t *data, u16_t len)
{
    cJSON *root = NULL;
    rt_kprintf("%.*s", (int)len, data);
    rt_kprintf("--\r\n");
    root = cJSON_ParseWithLength((const char *)data, len);
    if (!root)
    {
        rt_kprintf("Error before: [%s]\n", cJSON_GetErrorPtr());
        return;
    }

    char *type = ws_json_string(root, "type");
    if (!type) goto done;
    rt_kprintf("type = %s\n", type);
    if (strcmp(type, "hello") == 0)
    {
        /* A delayed hello after goodbye must not reopen the old session. */
        if (!ws_hello_pending) goto done;
        char *session_id = ws_json_string(root, "session_id");
        if (!session_id) goto done;
        rt_kprintf("session_id = %s\n", session_id);
        cJSON *audio_param = cJSON_GetObjectItem(root, "audio_params");
        cJSON *sample_rate = cJSON_GetObjectItem(audio_param, "sample_rate");
        cJSON *duration = cJSON_GetObjectItem(audio_param, "frame_duration");
        if (!duration) duration = cJSON_GetObjectItem(audio_param, "duration");
        g_xz_ws.sample_rate = cJSON_IsNumber(sample_rate) ? sample_rate->valueint :
                             (cJSON_IsString(sample_rate) ? atoi(sample_rate->valuestring) : 16000);
        g_xz_ws.frame_duration = cJSON_IsNumber(duration) ? duration->valueint :
                                (cJSON_IsString(duration) ? atoi(duration->valuestring) : 60);
        rt_snprintf((char *)g_xz_ws.session_id, sizeof(g_xz_ws.session_id), "%s", session_id);
        ws_hello_pending = false;
        ws_session_active = false;
        
#ifndef CONFIG_IOT_PROTOCOL_MCP
        send_iot_descriptors(); // 发送iot描述
        send_iot_states();      // 发送iot状态
#endif// CONFIG_IOT_PROTOCOL_MCP
        xiaozhi_ui_chat_output("小智已连接!");
        xiaozhi_ui_update_emoji("neutral");
        xiaozhi_ui_update_standby_emoji("funny");
        rt_kprintf("WS_HELLO ready -> Standby wake_pending=%d\n", ws_wake_pending);
        ui_switch_to_xiaozhi_screen();//切换到小智对话界面
        ws_set_state(kDeviceStateIdle);
        ws_hello_ready = true; /* publish only after all hello UI work completes */
    }
    else if (strcmp(type, "goodbye") == 0)
    {
        ws_enter_standby("server_goodbye");
    }
    else if (strcmp(type, "stt") == 0)
    {
        if (!ws_session_active) goto done;
        ws_handle_stt_text(ws_json_string(root, "text"));
    }
    else if (strcmp(type, "tts") == 0)
    {
        if (!ws_session_active) goto done;
        char *state = ws_json_string(root, "state");
        if (!state) goto done;

        if (strcmp(state, "start") == 0)
        {
            if (ws_mute_reply)
            {
                /* The SD player already handled this utterance, or the reply
                 * comes from the cloud music/search plugin. Drop the spoken
                 * answer and stay in Listening; music is not paused at all. */
                ws_mute_reply = false;
                ws_send_speak_abort(&g_xz_ws.clnt, g_xz_ws.session_id,
                                    kAbortReasonNone);
                rt_kprintf("MUSIC_ABORT_REPLY\n");
                ws_set_state(kDeviceStateListening);
                goto done;
            }
            rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
            if (!ws_session_active)
            {
                rt_mutex_release(&ws_tx_mutex);
                goto done;
            }
            xz_music_tts(true);
            xz_audio_reset_barge_in();
            ws_set_state(kDeviceStateSpeaking);
            rt_kprintf("BARGE_TRAIN generation=%u phase=start arm_ms=%u arm_ratio=%u/1000 "
                       "arm_min_pre=%u echo_mult=%u dtd_margin_pm=%u\n",
                       (unsigned)ws_speaking_generation, XZ_BARGE_ARM_MS, XZ_BARGE_ARM_RATIO,
                       XZ_BARGE_ARM_MIN_PRE, XZ_BARGE_IN_ECHO_MULT, XZ_BARGE_DTD_MARGIN_PM);
            xz_speaker(1); // 打开扬声器
            if (!g_is_speaking)
            {
                // 开始累计讲话时间
                g_is_speaking = true;
                g_speaking_start_tick = rt_tick_get();
            }
            rt_mutex_release(&ws_tx_mutex);
        }
        else if (strcmp(state, "stop") == 0)
        {
            rt_mutex_take(&ws_tx_mutex, RT_WAITING_FOREVER);
            rt_enter_critical();
            bool no_convergence = web_g_state == kDeviceStateSpeaking && !ws_tts_stop_pending &&
                                  !xz_audio_barge_in_armed(ws_speaking_generation);
            if (web_g_state == kDeviceStateSpeaking) ws_tts_stop_pending = true;
            ws_barge_request.pending = false;
            xz_audio_reset_barge_in();
            rt_exit_critical();
            if (no_convergence)
                rt_kprintf("BARGE_TRAIN generation=%u phase=skip reason=no_convergence\n",
                           (unsigned)ws_speaking_generation);
            // 计算本次讲话时间并累加
            if (g_is_speaking) 
            {
                rt_tick_t current_tick = rt_tick_get();
                rt_tick_t speaking_duration = current_tick - g_speaking_start_tick;
                g_total_speaking_time += speaking_duration;
                g_is_speaking = false;

                rt_kprintf("xiaozhi total_speaking_time: %d ticks\n", g_total_speaking_time);
                // 检查是否达到5分钟阈值
                if (g_total_speaking_time >= rt_tick_from_millisecond(SPEAKING_THRESHOLD_MS)) 
                {
                    rt_kprintf("Speaking time reached 5 minutes, reinitializing audio\n");
                    g_total_speaking_time = 0; // 重置累计时间
                    xiaozhi_ui_reinit_audio();
                }
            }
            
            /* A late stop for an aborted turn must not leave Thinking or
             * Listening. Finish natural playback only after its PCM drains. */
            if (web_g_state == kDeviceStateSpeaking)
            {
                if (xz_audio_tts_drained()) ws_end_speaking();
            }
            rt_mutex_release(&ws_tx_mutex);
        }
        else if (strncmp(state, "sentence_", 9) == 0)
        {
            /* Sentence events never resurrect a stopped/aborted reply. */
            if (web_g_state == kDeviceStateSpeaking &&
                strcmp(state, "sentence_start") == 0)
                xiaozhi_ui_tts_output(ws_json_string(root, "text"));
        }
        else
        {
            rt_kprintf("Unkown test: %s\n", state);
        }
    }
    else if (strcmp(type, "llm") == 0)
    {
        if (ws_session_active)
            xiaozhi_ui_update_emoji(ws_json_string(root, "emotion"));
        /* Emotion updates leave THINK (and all other states) unchanged. */
    }
    else if (strcmp(type, "iot") == 0)
    {
#ifndef CONFIG_IOT_PROTOCOL_MCP
        rt_kprintf("iot command\n");
        cJSON *commands = cJSON_GetObjectItem(root, "commands");
        // rt_kprintf("commands: %s\n", cJSON_Print(commands));
        for (int i = 0; i < cJSON_GetArraySize(commands); i++)
        {
            // rt_kprintf("command %d: %s\n", i,
            // cJSON_Print(cJSON_GetArrayItem(commands, i)));
            cJSON *cmd = cJSON_GetArrayItem(commands, i);
            // rt_kprintf("cmd: %s\n", cJSON_Print(cmd));
            char *cmd_str = cJSON_PrintUnformatted(cmd);
            // rt_kprintf("cmd_str: %s\n", cmd_str);
            if (cmd_str)
            {
                iot_invoke((uint8_t *)cmd_str, strlen(cmd_str));
                send_iot_states(); // 发送 IoT 状态
                rt_free(cmd_str);
            }
        }
#endif // 定义了MCP就不走IOT
    }
    else if (strcmp(type, "mcp") == 0)
    {
        rt_kprintf("mcp command\n");
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload && cJSON_IsObject(payload))
        {
            McpServer_ParseMessage(cJSON_PrintUnformatted(payload));
        }
    }
    else
    {
        rt_kprintf("Unkown type: %s\n", type);
    }

done:
    cJSON_Delete(root); /*每次调用cJSON_Parse函数后，都要释放内存*/
}

static void svr_found_callback(const char *name, const ip_addr_t *ipaddr,
                               void *callback_arg)
{
    if (ipaddr != NULL)
    {
        rt_kprintf("DNS lookup succeeded, IP: %s\n", ipaddr_ntoa(ipaddr));
    }
}

void xiaozhi_ws_connect(void)
{
    if (!g_pan_connected)
    {
        ws_set_state(kDeviceStateConnecting);
        xiaozhi_ui_chat_status("请开启网络共享");
        xiaozhi_ui_chat_output("请在手机上开启网络共享后重新发起连接");
        xiaozhi_ui_update_emoji("embarrassed");
        return;
    }

    /* wsock_init() 拒绝任何非 NULL 的 pcb，所以每次重连前必须把它清掉，
     * 无论上一次是 CLOSED 还是会话中。只有已经连上且仍活跃时才跳过。 */
    if (g_xz_ws.clnt.pcb != NULL)
    {
        if (g_xz_ws.clnt.pcb->state != CLOSED)
        {
            if (g_xz_ws.is_connected)
            {
                rt_kprintf("WebSocket connected, reconnect skipped\n");
                return;
            }
            rt_kprintf("ws stale pcb state=%d -> force close\n",
                       g_xz_ws.clnt.pcb->state);
            wsock_close(&g_xz_ws.clnt, WSOCK_RESULT_OK, ERR_OK);
        }
        else
        {
            rt_kprintf("ws closed pcb leftover -> clear\n");
        }
        g_xz_ws.clnt.pcb = NULL;
    }

    err_t err;
    xz_ws_tx_init();
    uint32_t retry = 10;
    /* 连接建立后必须一直守着：断线就清理重连。原来这里 break 掉直接返回，
     * 之后再断线就没人发起连接，界面永远停在“连接中”。 */
    for (;;)
    {
        /* Goodbye may be followed by TCP close. Only a new wake may reconnect. */
        while (!ws_reconnect_allowed) rt_thread_mdelay(100);
        ws_set_state(kDeviceStateConnecting);
        /* 内部堆紧张时 rt_sem_create() 会返回 NULL，后面直接 rt_sem_take(NULL)
         * 会在 rt_object_get_type() 里触发 mem manage fault。必须显式检查。 */
        if (g_xz_ws.sem == NULL)
        {
            g_xz_ws.sem = rt_sem_create("xz_ws", 0, RT_IPC_FLAG_FIFO);
            if (g_xz_ws.sem == NULL)
            {
                rt_size_t total = 0, used = 0, max_used = 0;
                rt_memory_info(&total, &used, &max_used);
                rt_kprintf("ws sem alloc failed, heap %u/%u used (max %u)\n",
                           (unsigned)used, (unsigned)total, (unsigned)max_used);
                ws_set_state(kDeviceStateFatalError);
                rt_thread_mdelay(1000);
                continue;
            }
        }

        /* 每次尝试前都要重新初始化：close 之后 wsock 内部状态是 invalid，
         * 直接再 connect 会返回 -6（wsock_connect: invalid state）。 */
        wsock_init(&g_xz_ws.clnt, 1, 1,
                   my_wsapp_fn); // 初始化websocket,注册回调函数
        char *Client_Id = get_client_id();
        rt_kprintf("ws connecting host=%s path=%s\n", XIAOZHI_HOST,
                   XIAOZHI_WSPATH);
        ws_connecting = 1;
        err = wsock_connect(
            &g_xz_ws.clnt, MAX_WSOCK_HDR_LEN, XIAOZHI_HOST, XIAOZHI_WSPATH,
            LWIP_IANA_PORT_HTTPS, XIAOZHI_TOKEN, NULL,
            "Protocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
            xz_get_mac_address(), Client_Id);
        /* ws_connecting 要一直保持到 sem 等待结束：旧 socket 的 poll 线程
         * 会在 connect 返回之后再甩一条 DISCONNECT，提前清零就会把刚发起的
         * 连接立刻判死，重连永远成功不了。 */
        rt_kprintf("Web socket connection %d\r\n", err);
        if (err == 0)
        {
            rt_kprintf("err = 0\n");
            err_t sem_ret = rt_sem_take(g_xz_ws.sem, 50000);
            ws_connecting = 0;
            if (RT_EOK == sem_ret)
            {
                rt_kprintf("g_xz_ws.is_connected = %d\n", g_xz_ws.is_connected);
                if (g_xz_ws.is_connected)
                {
                    ws_send_hello(&g_xz_ws.clnt);
                    while (g_xz_ws.is_connected)
                    {
                        rt_thread_mdelay(500);
                        if (ws_hello_pending &&
                            rt_tick_get() - ws_hello_tick > rt_tick_from_millisecond(10000))
                        {
                            rt_kprintf("WS_HELLO timeout; cancel wake, reconnect to Standby\n");
                            ws_wake_pending = ws_hello_pending = false;
                            wsock_close(&g_xz_ws.clnt, WSOCK_RESULT_ERR_TIMEOUT, ERR_TIMEOUT);
                            g_xz_ws.is_connected = 0;
                        }
                        /* Keep the existing 4s recovery, but decide its state
                         * here instead of writing web_g_state in the mic callback. */
                        if (web_g_state == kDeviceStateSpeaking &&
                            (rt_tick_get() - g_xz_rx_tick) >
                                rt_tick_from_millisecond(4000))
                            ws_end_speaking();
                    }
                    rt_kprintf("ws lost reconnect_allowed=%d\n", ws_reconnect_allowed);
                    if (g_xz_ws.clnt.pcb)
                    {
                        wsock_close(&g_xz_ws.clnt, WSOCK_RESULT_ERR_CONNECT,
                                    ERR_OK);
                        g_xz_ws.clnt.pcb = NULL;
                    }
                    /* UI changes are posted to its owner thread. */
                    ws_session_active = false;
                    ws_hello_ready = ws_hello_pending = false;
                    g_is_speaking = false;
                    xz_speaker(0);
                    if (!ws_reconnect_allowed) continue;
                    ws_set_state(err == ERR_OK ? kDeviceStateConnecting
                                              : kDeviceStateFatalError);
                    rt_kprintf("ws reconnect attempt\n");
                    rt_thread_mdelay(1500);
                    continue;
                }
                else
                {
                    ws_set_state(kDeviceStateFatalError);
                    rt_kprintf("err = wsock_write_Web socket disconnected\r\n");
                }
            }
            else
            {
                rt_kprintf("Web socket connected timeout\r\n");
                if (g_xz_ws.clnt.pcb)
                {
                    wsock_close(&g_xz_ws.clnt, WSOCK_RESULT_ERR_CONNECT, ERR_OK);
                    g_xz_ws.clnt.pcb = NULL;
                }
                ws_set_state(kDeviceStateFatalError);
                rt_thread_mdelay(5000);
            }
        }
        else
        {
            ws_connecting = 0;
            rt_kprintf("ws connect attempt failed, retry\n");
            if (g_xz_ws.clnt.pcb)
            {
                wsock_close(&g_xz_ws.clnt, WSOCK_RESULT_ERR_CONNECT, ERR_OK);
                g_xz_ws.clnt.pcb = NULL;
            }
            ws_set_state(kDeviceStateFatalError);
            /* 旧 socket 的 poll 线程还会甩出几条断开事件，紧接着重连会被
             * 这些残留事件立刻打断，形成 1 秒一次的空转。给足退避时间。 */
            rt_thread_mdelay(5000);
            retry++;
            /* 不再自动重启：用户要求不能莫名重启。这里只做退避重试。 */
        }
    }
}

static void xz_ws_connect_entry(void *parameter)
{
    (void)parameter;
    xiaozhi_ws_connect();
}

void xiaozhi2(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!g_pan_connected)
    {
        ws_set_state(kDeviceStateConnecting);
        xiaozhi_ui_chat_status("网络连接中...");
        xiaozhi_ui_chat_output("正在等待 Wi-Fi 连接...");
        xiaozhi_ui_update_emoji("embarrassed");
        return;
    }

    xiaozhi_ui_wake();

    /* Keep main() free to consume later KWS/key events. Stack stays in PSRAM. */
    if (!ws_connect_started)
    {
        ws_reconnect_allowed = true;
        rt_err_t result = rt_thread_init(&ws_connect_thread, "xz_ws",
                                         xz_ws_connect_entry, NULL,
                                         ws_connect_stack, sizeof(ws_connect_stack), 20, 10);
        if (result == RT_EOK)
        {
            ws_connect_started = true;
            rt_thread_startup(&ws_connect_thread);
        }
        else
        {
            rt_kprintf("WS monitor init failed: %d\n", result);
            ws_set_state(kDeviceStateFatalError);
        }
    }
}
MSH_CMD_EXPORT(xiaozhi2, Get Xiaozhi)

/* Bench helper: drive the same wake path as KEY1 / the KWS engine so the
 * button -> listen -> uplink chain can be exercised over the console. */
/* Bench helper: feed a sentence into the same local intent path as a server
 * "stt" message, so wake/dismiss can be exercised from the console. */
static void xz_say(int argc, char **argv)
{
    static char text[256];
    int used = 0;

    if (argc < 2)
    {
        rt_kprintf("usage: xz_say <sentence>\n");
        return;
    }
    text[0] = '\0';
    for (int i = 1; i < argc && used < (int)sizeof(text) - 1; i++)
    {
        int n = rt_snprintf(text + used, sizeof(text) - used, "%s%s",
                            i == 1 ? "" : " ", argv[i]);
        if (n < 0)
            break;
        used += (used + n < (int)sizeof(text)) ? n : (int)sizeof(text) - 1 - used;
    }
    rt_kprintf("xz_say: session=%d text=%s\n", ws_session_active, text);
    ws_handle_stt_text(text);
}
MSH_CMD_EXPORT(xz_say, feed a sentence through the local stt intent path);

/* Bench helper: hand the server a text utterance as if it had recognised
 * speech, so the cloud reply (llm/tts) can be exercised without a speaker. */
static void xz_listen_text(int argc, char **argv)
{
    static char text[192];
    static char msg[320];
    int used = 0;

    if (argc < 2)
    {
        rt_kprintf("usage: xz_listen_text <sentence>\n");
        return;
    }
    if (!g_xz_ws.is_connected || !ws_session_active)
    {
        rt_kprintf("xz_listen_text: no active session\n");
        return;
    }
    text[0] = '\0';
    for (int i = 1; i < argc && used < (int)sizeof(text) - 1; i++)
    {
        int n = rt_snprintf(text + used, sizeof(text) - used, "%s%s",
                            i == 1 ? "" : " ", argv[i]);
        if (n < 0)
            break;
        used += (used + n < (int)sizeof(text)) ? n : (int)sizeof(text) - 1 - used;
    }
    int n = rt_snprintf(msg, sizeof(msg),
                        "{\"session_id\":\"%s\",\"type\":\"listen\","
                        "\"state\":\"detect\",\"text\":\"%s\"}",
                        g_xz_ws.session_id, text);
    if (n < 0 || n >= (int)sizeof(msg))
    {
        rt_kprintf("xz_listen_text: message too long\n");
        return;
    }
    rt_kprintf("XZ_TEXT %s\n", msg);
    xz_ws_write_locked(&g_xz_ws.clnt, msg, n, OPCODE_TEXT);
}
MSH_CMD_EXPORT(xz_listen_text, send a text utterance to the server);

/* Bench helper: arm/disarm the reply-mute flag without speaking, so the
 * tts-abort branch can be exercised together with xz_listen_text. */
static void xz_mute(int argc, char **argv)
{
    if (argc == 2)
    {
        ws_mute_reply = (atoi(argv[1]) != 0);
    }
    rt_kprintf("xz_mute: mute_reply=%d\n", ws_mute_reply ? 1 : 0);
}
MSH_CMD_EXPORT(xz_mute, arm the next-reply mute flag: xz_mute 0|1);

static void xz_btn_sim(int argc, char **argv)
{
    int hold_ms = (argc > 1) ? atoi(argv[1]) : 8000;
    rt_kprintf("xz_btn_sim: press, hold %d ms\n", hold_ms);
    simulate_button_pressed();
    rt_thread_mdelay(hold_ms);
    simulate_button_released();
}
MSH_CMD_EXPORT(xz_btn_sim, simulate KEY1 press/release)

/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/
