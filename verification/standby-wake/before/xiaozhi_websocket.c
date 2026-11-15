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
static struct rt_mutex *ws_tx_lock_obj = NULL;

static err_t xz_ws_write_locked(wsock_state_t *st, const char *data, u16_t len,
                                uint8_t op)
{
    if (ws_tx_lock_obj == NULL)
    {
        ws_tx_lock_obj = rt_mutex_create("ws_tx", RT_IPC_FLAG_PRIO);
    }
    if (ws_tx_lock_obj)
    {
        rt_mutex_take(ws_tx_lock_obj, RT_WAITING_FOREVER);
    }
    err_t e = wsock_write(st, data, len, op);
    if (ws_tx_lock_obj)
    {
        rt_mutex_release(ws_tx_lock_obj);
    }
    return e;
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

#if defined(__CC_ARM) || defined(__CLANG_ARM)
L2_RET_BSS_SECT_BEGIN(message) //6000地址
static char message[256];
L2_RET_BSS_SECT_END
#else
static char message[256] L2_RET_BSS_SECT(message);
#endif
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
static bool ws_session_active;

/* The only websocket state writer: publish its corresponding UI event together.
 * Unknown retains its existing meaning: session ended, waiting for wakeup. */
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
    web_g_state = state;
    xiaozhi_ui_set_face_state(face_state);
    xiaozhi_ui_chat_status(status);
}

void xiaozhi_ws_network_status(bool failed)
{
    ws_set_state(failed ? kDeviceStateFatalError : kDeviceStateConnecting);
}

void xiaozhi_ws_configuring(void)
{
    ws_session_active = false;
    g_is_speaking = false;
    xz_speaker(0);
    ws_set_state(kDeviceStateWifiConfiguring);
}

void xiaozhi_ws_sleep(void)
{
    ws_session_active = false;
    g_is_speaking = false;
    xz_speaker(0);
    ws_set_state(kDeviceStateUnknown);
}

static void ws_end_speaking(void)
{
    g_is_speaking = false;
    xz_speaker(0);
    if (ws_session_active)
        ws_set_state(ws_listen_mode == kListeningModeAlwaysOn
                         ? kDeviceStateListening : kDeviceStateIdle);
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

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,"
             "\"descriptors\":%s}",
             g_xz_ws.session_id, desc);

    rt_kprintf("Sending IoT descriptors:\n");
    rt_kprintf(msg);
    rt_kprintf("\n");
    if (g_xz_ws.is_connected == 1)
    {
        xz_ws_write_locked(&g_xz_ws.clnt, msg, strlen(msg), OPCODE_TEXT);
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

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,"
             "\"states\":%s}",
             g_xz_ws.session_id, state);

    rt_kprintf("Sending IoT states:\n");
    rt_kprintf(msg);
    rt_kprintf("\n");
    if (g_xz_ws.is_connected == 1)
    {
        xz_ws_write_locked(&g_xz_ws.clnt, msg, strlen(msg), OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

void ws_send_speak_abort(void *ws, char *session_id, int reason)
{
    rt_kprintf("speak abort\n");
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

void ws_send_listen_start(void *ws, char *session_id, enum ListeningMode mode)
{
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
            ws_listen_mode = mode;
            ws_session_active = true;
            ws_set_state(kDeviceStateListening);
        }
        else
            ws_set_state(kDeviceStateFatalError);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
}

void ws_send_listen_stop(void *ws, char *session_id)
{
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
    if (g_xz_ws.is_connected == 1)
    {
        xz_ws_write_locked((wsock_state_t *)ws, hello_message, strlen(hello_message),
                    OPCODE_TEXT);
    }
    else
    {
        rt_kprintf("websocket is not connected\n");
    }
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
    uint8_t hdr[3];
    static uint8_t frame[512];
    (void)p;
    for (;;)
    {
        rt_uint32_t evt = 0;
        rt_event_recv(xz_tx_evt, 1, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &evt);
        while (rt_ringbuffer_data_len(xz_tx_rb) >= 3)
        {
            rt_ringbuffer_get(xz_tx_rb, hdr, 3);
            uint16_t len = (uint16_t)(hdr[0] | (hdr[1] << 8));
            uint8_t op = hdr[2];
            if (len == 0 || len > sizeof(frame))
            {
                continue;
            }
            rt_ringbuffer_get(xz_tx_rb, frame, len);
            if (g_xz_ws.is_connected)
            {
                /* AIC 驱动每发一个包都要从内部堆申请 ~3 KB 大块内存。堆里只有
                 * 几十 KB 且已碎片化，TX 环积压时申请必然失败（sdio buf alloc
                 * fail -> 包发不出去 -> TCP 窗口永久卡死）。环快满时先等一等。 */
                extern bool aicwf_net_buf_tx_free_list_is_full(void);
                int guard = 0;
                while (aicwf_net_buf_tx_free_list_is_full() &&
                       g_xz_ws.is_connected && guard++ < 200)
                {
                    rt_thread_mdelay(5);
                }
                /* ERR_MEM = 发送窗口暂时满（对端未确认）。此时不能丢帧也不能关连接，
                 * 原地退避重试，最多 100 x 20 ms = 2 s，之后丢弃这一帧并记账。 */
                err_t e = xz_ws_write_locked(&g_xz_ws.clnt, (const char *)frame,
                                             len, op);
                int spins = 0;
                while (e == ERR_MEM && g_xz_ws.is_connected && spins < 100)
                {
                    rt_thread_mdelay(20);
                    e = xz_ws_write_locked(&g_xz_ws.clnt, (const char *)frame,
                                           len, op);
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

/* 对照实验开关：0 = 完全不上行音频，只保留握手/下行 TTS。
 * 用 xz_uplink on/off 在串口切换。 */
int g_xz_uplink_on = 1;

void xz_audio_send_using_websocket(uint8_t *data, int len)
{
    if (!g_xz_uplink_on)
    {
        return;
    }
    /* 支持打断：播报期间也允许上行。真正决定发不发的是 mic 回调里的
     * VAD（双麦 + AEC 已经把喇叭回声消掉），静音帧不会进来。 */
    if (g_xz_ws.is_connected == 1)
    {
        if (xz_tx_rb && len > 0 && len <= 400 &&
            rt_ringbuffer_space_len(xz_tx_rb) >= (rt_size_t)(len + 3))
        {
            uint8_t hdr[3] = {(uint8_t)(len & 0xff),
                              (uint8_t)((len >> 8) & 0xff), OPCODE_BINARY};
            rt_ringbuffer_put(xz_tx_rb, hdr, 3);
            rt_ringbuffer_put(xz_tx_rb, data, len);
            rt_event_send(xz_tx_evt, 1);
        }
        err_t err = ERR_OK;
        /* 每 50 帧打一行，确认上行链路真的有数据出去。 */
        static uint32_t frames = 0, errors = 0;
        if (err != ERR_OK)
            errors++;
        if ((++frames % 50) == 0)
            rt_kprintf("uplink %u frames, %u errors, last len=%d err=%d\n",
                       frames, errors, len, err);
    }
    // else
    //     rt_kprintf("Websocket disconnected\n");
}

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

            xiaozhi_ui_chat_output("小智已断开，正在重连...");
            xiaozhi_ui_update_emoji("neutral");
        }
        rt_kprintf("WebSocket closed\n");
        g_xz_ws.is_connected = 0;
        ws_session_active = false;
        g_is_speaking = false;
        xz_speaker(0);
        ws_set_state(kDeviceStateConnecting);
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
        xz_audio_downlink(buf, len, NULL, 0);
    }
    return 0;
}
void xiaozhi2(int argc, char **argv);

static void xz_button_event_handler(int32_t pin, button_action_t action)
{
    rt_kprintf("in ws button handle\n");
    xiaozhi_ui_wake(); // 唤醒设备
     rt_kprintf("in ws button handle2\n");
    // 如果当前处于KWS模式，则退出KWS模式
        if (g_kws_running) 
        {  
            rt_kprintf("KWS exit\n");
            g_kws_force_exit = 1;
        }
    static button_action_t last_action = BUTTON_RELEASED;
    if (last_action == action)
        return;
    last_action = action;

    if (action == BUTTON_PRESSED)
    {
        rt_kprintf("pressed\r\n");
        rt_kprintf("按键->对话");

        // 检查是否弹窗显示中

        xiaozhi_ui_update_confirm_button_event(1); // 模拟点击更新按钮

        // 1. 检查是否处于睡眠状态（WebSocket未连接）
        if (!g_xz_ws.is_connected)
        {
            rt_mb_send(g_bt_app_mb, WEBSOC_RECONNECT); // 发送重连消息
            ws_set_state(kDeviceStateConnecting);
        }
        else
        {
            /* Always-on keys only interrupt; a sleeping/manual session starts
             * listening explicitly. Releasing an always-on key never stops it. */
            if (web_g_state == kDeviceStateSpeaking)
            {
                /* 正在播报时按键 = 打断。realtime 模式下不必重发 listen。 */
                ws_send_speak_abort(&g_xz_ws.clnt, g_xz_ws.session_id,
                                    kAbortReasonNone);
                xz_speaker(0);
                g_is_speaking = false;
            }
            if (!ws_session_active)
            {
                /* goodbye ended the old session: obtain a new hello before
                 * sending listen, rather than reusing the expired session id. */
                ws_set_state(kDeviceStateConnecting);
                ws_send_hello(&g_xz_ws.clnt);
            }
            else if (ws_listen_mode != kListeningModeAlwaysOn)
                ws_send_listen_start(&g_xz_ws.clnt, g_xz_ws.session_id,
                                     ws_listen_mode);
            else
                ws_set_state(kDeviceStateListening);
        }
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
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, 8); // 设置音量
    xz_audio_decoder_encoder_open(1); // 打开音频解码器和编码器

}
void parse_helLo(const u8_t *data, u16_t len)
{
    cJSON *item = NULL;
    cJSON *root = NULL;
    rt_kprintf(data);
    rt_kprintf("--\r\n");
    root = cJSON_Parse(data); /*json_data 为MQTT的原始数据*/
    if (!root)
    {
        rt_kprintf("Error before: [%s]\n", cJSON_GetErrorPtr());
        return;
    }

    char *type = cJSON_GetObjectItem(root, "type")->valuestring;
    rt_kprintf("type = %s\n", type);
    if (strcmp(type, "hello") == 0)
    {
        char *session_id = cJSON_GetObjectItem(root, "session_id")->valuestring;
        rt_kprintf("session_id = %s\n", session_id);
        cJSON *audio_param = cJSON_GetObjectItem(root, "audio_params");
        char *sample_rate =
            cJSON_GetObjectItem(audio_param, "sample_rate")->valuestring;
        char *duration =
            cJSON_GetObjectItem(audio_param, "duration")->valuestring;
        g_xz_ws.sample_rate = atoi(sample_rate);
        g_xz_ws.frame_duration = atoi(duration);
        strncpy(g_xz_ws.session_id, session_id, 9);
        
#ifndef CONFIG_IOT_PROTOCOL_MCP
        send_iot_descriptors(); // 发送iot描述
        send_iot_states();      // 发送iot状态
#endif// CONFIG_IOT_PROTOCOL_MCP
        xiaozhi_ui_chat_output("小智已连接!");
        xiaozhi_ui_update_emoji("neutral");
        xiaozhi_ui_update_standby_emoji("funny");
        rt_kprintf("hello->对话\n");
        ui_switch_to_xiaozhi_screen();//切换到小智对话界面
        /* realtime 模式：hello 之后开一轮持续聆听，由服务端做 VAD。
         * 之后麦克风数据持续上行，按键只负责打断/提示，不再重复发 listen。 */
        ws_send_listen_start(&g_xz_ws.clnt, g_xz_ws.session_id,
                             kListeningModeAlwaysOn);
    }
    else if (strcmp(type, "goodbye") == 0)
    {
        xiaozhi_ws_sleep();
        rt_kprintf("session ended\n");
        xiaozhi_ui_chat_output("等待唤醒...");
        xiaozhi_ui_update_emoji("sleep");
    }
    else if (strcmp(type, "stt") == 0)
    {
        if (!ws_session_active) goto done;
        ws_set_state(kDeviceStateThinking);
        char *txt = ws_json_string(root, "text");
        xz_music_handle_text(txt);
        xiaozhi_ui_chat_output(txt);
        last_listen_tick = rt_tick_get();
    }
    else if (strcmp(type, "tts") == 0)
    {
        if (!ws_session_active) goto done;
        char *state = ws_json_string(root, "state");
        if (!state) goto done;

        if (strcmp(state, "start") == 0)
        {
            xz_music_tts(true);
            ws_set_state(kDeviceStateSpeaking);
            xz_speaker(1); // 打开扬声器
            if (!g_is_speaking)
            {
                // 开始累计讲话时间
                g_is_speaking = true;
                g_speaking_start_tick = rt_tick_get();
            }
        }
        else if (strcmp(state, "stop") == 0)
        {           
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
            
            ws_end_speaking();
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
                    err = xz_ws_write_locked(&g_xz_ws.clnt, hello_message,
                                      strlen(hello_message), OPCODE_TEXT);
                    rt_kprintf("Web socket write %d\r\n", err);
                    if (err != ERR_OK)
                    {
                        g_xz_ws.is_connected = 0;
                        ws_set_state(kDeviceStateFatalError);
                    }
                    while (g_xz_ws.is_connected)
                    {
                        rt_thread_mdelay(500);
                        /* Keep the existing 4s recovery, but decide its state
                         * here instead of writing web_g_state in the mic callback. */
                        if (web_g_state == kDeviceStateSpeaking &&
                            (rt_tick_get() - g_xz_rx_tick) >
                                rt_tick_from_millisecond(4000))
                            ws_end_speaking();
                    }
                    rt_kprintf("ws lost -> reconnect\n");
                    if (g_xz_ws.clnt.pcb)
                    {
                        wsock_close(&g_xz_ws.clnt, WSOCK_RESULT_ERR_CONNECT,
                                    ERR_OK);
                        g_xz_ws.clnt.pcb = NULL;
                    }
                    /* UI changes are posted to its owner thread. */
                    ws_session_active = false;
                    g_is_speaking = false;
                    xz_speaker(0);
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

    xiaozhi_ws_connect();
}
MSH_CMD_EXPORT(xiaozhi2, Get Xiaozhi)

/* Bench helper: drive the same wake path as KEY1 / the KWS engine so the
 * button -> listen -> uplink chain can be exercised over the console. */
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
