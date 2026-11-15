/* Real production state/audio helpers with RTOS, network and DAC mocks.
 * No serial ports, flashing, network connections, or board access. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "cJSON.h"

#define CONFIG_IOT_PROTOCOL_MCP 1
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EBUSY 7
#define RT_ETIMEOUT 2
#define RT_WAITING_FOREVER -1
#define RT_EVENT_FLAG_OR 1
#define RT_EVENT_FLAG_CLEAR 2
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_ABRT -13
#define ERR_CONN -11
#define OPCODE_BINARY 2
#define OPCODE_TEXT 1
#define WS_CONNECT 1
#define WS_DISCONNECT 2
#define WS_TEXT 3
#define WSOCK_RESULT_ERR_CONNECT 1
#define BSP_KEY1_PIN 1
#define BSP_USING_PM 1
#define SPEAKING_THRESHOLD_MS (5 * 60 * 1000)
#define AUDIO_TYPE_LOCAL_MUSIC 1
#define AUDIO_IOCTL_BYTES_IN_CACHE 3
#define XZ_EVENT_WAKE_BEEP 16
#define MCP_RGBLED_CLOSE() ((void)0)
#define rt_snprintf snprintf

typedef int err_t;
typedef int rt_err_t;
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t rt_tick_t;
typedef uint32_t rt_uint32_t;
typedef size_t rt_size_t;
typedef enum {BUTTON_RELEASED, BUTTON_PRESSED} button_action_t;
enum ListeningMode {kListeningModeAutoStop, kListeningModeManualStop, kListeningModeAlwaysOn};
enum AbortReason {kAbortReasonNone, kAbortReasonWakeWordDetected};
enum DeviceState {kDeviceStateUnknown, kDeviceStateStarting, kDeviceStateWifiConfiguring,
    kDeviceStateIdle, kDeviceStateConnecting, kDeviceStateListening, kDeviceStateSpeaking,
    kDeviceStateUpgrading, kDeviceStateActivating, kDeviceStateFatalError, kDeviceStateThinking};
typedef enum {FS_CONNECT, FS_IDLE, FS_LISTEN, FS_SPEAK, FS_THINK, FS_ERROR, FS_SLEEP, FS_CONFIG} face_state_t;
struct rt_mutex {int depth;};
struct rt_ringbuffer {uint8_t data[8192]; size_t len;};
typedef struct {int dummy;} wsock_state_t;
static struct {wsock_state_t clnt; uint8_t is_connected; uint8_t session_id[12];
    int sem; uint32_t sample_rate, frame_duration;} g_xz_ws;
static struct rt_mutex ws_tx_mutex;
static uint32_t ws_audio_generation, ws_audio_sent, ws_audio_blocked;
static int g_xz_uplink_on = 1;
static enum DeviceState web_g_state;
static enum ListeningMode ws_listen_mode = kListeningModeAlwaysOn;
static bool ws_session_active, ws_hello_ready, ws_hello_pending, ws_wake_pending;
static bool ws_reconnect_allowed = true, ws_beep_pending, ws_beep_done;
static uint32_t ws_wake_token, ws_done_token;
static int ws_beep_result;
static rt_tick_t ws_hello_tick, last_listen_tick, g_xz_rx_tick;
static rt_tick_t g_speaking_start_tick, g_total_speaking_time;
static bool g_is_speaking;
static int ws_connecting, ws_ever_connected;
static const char *mode_str[] = {"auto", "manual", "realtime"};
static const char *hello_message = "{\"type\":\"hello\"}";
static struct rt_ringbuffer tx_rb;
static struct rt_ringbuffer *xz_tx_rb = &tx_rb;
static int xz_tx_evt;
static struct {bool inited, is_tx_enable; void *speaker; int event;
    struct rt_ringbuffer *rb_opus_encode_input; uint32_t mic_rx_count;} xz_audio;
static bool xz_wake_beep_busy;
static uint32_t xz_wake_beep_token;
static int xz_mic_hang;
static const int16_t sine1k_16[16] = {0,3127,5776,7571,8480,7571,5776,3127,
    0,-3127,-5776,-7571,-8480,-7571,-5776,-3127};

static unsigned hello_writes, listen_writes, goodbye_writes, abort_writes, binary_writes;
static unsigned downlink_calls, audio_blocks, delayed_ms, volume = 8;
static unsigned tick, mock_cache_bytes, event_count;
static int mock_write_error, mock_audio_error;
static bool close_during_listen, goodbye_on_delay;
static face_state_t face;
static jmp_buf tx_exit;
static bool run_tx;
static void parse_helLo(const u8_t *, u16_t); /* adjusted below by generated declaration */
static void ws_enter_standby(const char *);

static int rt_kprintf(const char *fmt, ...) {(void)fmt; return 0;}
static void rt_enter_critical(void) {}
static void rt_exit_critical(void) {}
static void rt_mutex_take(struct rt_mutex *m, int timeout) {m->depth++;}
static void rt_mutex_release(struct rt_mutex *m) {assert(m->depth > 0); m->depth--;}
static rt_tick_t rt_tick_get(void) {return tick++;}
static rt_tick_t rt_tick_from_millisecond(int ms) {return ms;}
static void rt_thread_mdelay(int ms) {
    delayed_ms += ms;
    if (goodbye_on_delay) {goodbye_on_delay = false; ws_enter_standby("test_retry_cancel");}
}
static void rt_sem_release(int sem) {}
static int rt_event_send(int event, unsigned flags) {return RT_EOK;}
static int rt_event_recv(int event, unsigned mask, unsigned flags, int timeout, unsigned *out) {
    assert(run_tx);
    if (event_count++) longjmp(tx_exit, 1);
    *out = 1; return 0;
}
static size_t rt_ringbuffer_data_len(struct rt_ringbuffer *rb) {return rb->len;}
static size_t rt_ringbuffer_space_len(struct rt_ringbuffer *rb) {return sizeof(rb->data) - rb->len;}
static void rt_ringbuffer_reset(struct rt_ringbuffer *rb) {rb->len = 0;}
static size_t rt_ringbuffer_put(struct rt_ringbuffer *rb, const uint8_t *d, size_t n) {
    assert(n <= rt_ringbuffer_space_len(rb)); memcpy(rb->data + rb->len, d, n); rb->len += n; return n;
}
static size_t rt_ringbuffer_get(struct rt_ringbuffer *rb, uint8_t *d, size_t n) {
    assert(n <= rb->len); memcpy(d, rb->data, n); rb->len -= n;
    memmove(rb->data, rb->data+n, rb->len); return n;
}
bool aicwf_net_buf_tx_free_list_is_full(void) {return false;}
static void xiaozhi_ui_set_face_state(face_state_t s) {face = s;}
static void xiaozhi_ui_chat_status(char *s) {}
static void xiaozhi_ui_chat_output(char *s) {}
static void xiaozhi_ui_tts_output(char *s) {}
static void xiaozhi_ui_update_emoji(char *s) {}
static void xiaozhi_ui_update_standby_emoji(char *s) {}
static void xiaozhi_ui_update_confirm_button_event(int i) {}
static void xiaozhi_ui_wake(void) {}
static void xiaozhi_ui_reinit_audio(void) {}
static void ui_switch_to_xiaozhi_screen(void) {}
static void xz_speaker(int on) {xz_audio.is_tx_enable = on;}
static void xz_music_tts(bool active) {}
static void xz_music_handle_text(char *text) {}
static void xiaozhi2(int argc, char **argv) {}
static void McpServer_ParseMessage(char *json) {free(json);}
static void xz_audio_downlink(char *buf, size_t len, void *aes, int need_aes) {downlink_calls++;}
static int audio_server_get_private_volume(int type) {return volume;}
static void audio_server_set_private_volume(int type, uint8_t level) {volume = level;}
static int audio_write(void *handle, uint8_t *data, uint32_t len) {
    if (mock_audio_error) return -1;
    assert(len == 320);
    for (unsigned i=0; i<160; ++i) assert(((int16_t *)data)[i] == sine1k_16[i%16]);
    audio_blocks++; return len;
}
static int audio_ioctl(void *handle, int command, void *parameter) {
    *(uint32_t *)parameter = mock_cache_bytes; return 0;
}
static err_t wsock_write(wsock_state_t *st, const char *data, u16_t len, uint8_t op) {
    if (mock_write_error) {int e = mock_write_error; mock_write_error = 0; return e;}
    if (op == OPCODE_BINARY) {binary_writes++; return 0;}
    if (strstr(data, "\"type\":\"hello\"")) hello_writes++;
    if (strstr(data, "\"type\":\"goodbye\"")) goodbye_writes++;
    if (strstr(data, "\"type\":\"abort\"")) abort_writes++;
    if (strstr(data, "\"state\":\"start\"")) {
        listen_writes++;
        if (close_during_listen) {close_during_listen = false; ws_enter_standby("test_close_in_write");}
    }
    return ERR_OK;
}
static void wsock_close(wsock_state_t *st, int result, int err) {g_xz_ws.is_connected = 0;}

#include "production-under-test.inc"

static void json(const char *s) {parse_helLo((const u8_t *)s, (u16_t)strlen(s));}
static void drain_tx(void) {
    run_tx = true; event_count = 0;
    if (!setjmp(tx_exit)) xz_ws_tx_entry(NULL);
    run_tx = false;
    assert(ws_tx_mutex.depth == 0);
}
static void hello(void) {
    ws_send_hello(&g_xz_ws.clnt);
    json("{\"type\":\"hello\",\"session_id\":\"test1234\",\"audio_params\":{\"sample_rate\":16000,\"frame_duration\":60}}");
}
static void finish_wake(void) {
    xz_ws_process_wake();
    assert(ws_beep_pending && xz_wake_beep_busy && !ws_session_active);
    unsigned writes = listen_writes;
    xz_ws_process_wake(); assert(listen_writes == writes);
    xz_audio_wake_beep();
    assert(!xz_wake_beep_busy && volume == 8);
    xz_ws_process_wake();
    assert(ws_session_active && web_g_state == kDeviceStateListening && face == FS_LISTEN);
    assert(listen_writes == writes + 1);
}

int main(void) {
    static int fake_speaker;
    static struct rt_ringbuffer pcm_rb;
    uint8_t frame_data[60] = {1};
    xz_audio.inited = true; xz_audio.event = 1; xz_audio.speaker = &fake_speaker;
    xz_audio.rb_opus_encode_input = &pcm_rb;
    g_xz_ws.is_connected = 1;
    hello();
    assert(web_g_state == kDeviceStateIdle && face == FS_IDLE);
    assert(!ws_session_active && !xz_ws_uplink_allowed() && listen_writes == 0);
    for (int i=0; i<100; ++i) xz_audio_send_using_websocket(frame_data, sizeof(frame_data));
    drain_tx(); assert(binary_writes == 0 && tx_rb.len == 0);
    assert(xz_ws_write_frame(&g_xz_ws.clnt, "x", 1, OPCODE_BINARY, 0) == ERR_ABRT);
    puts("PASS: boot hello -> Standby, microphone writes = 0");

    xz_button_event_handler(1, BUTTON_PRESSED);
    assert(!ws_session_active && xz_wake_beep_busy);
    unsigned blocks = audio_blocks;
    finish_wake(); assert(audio_blocks == blocks+8 && delayed_ms >= 120);
    assert(hello_writes == 1); /* use the ready boot handshake */
    xz_audio_send_using_websocket(frame_data, sizeof(frame_data));
    drain_tx(); assert(binary_writes == 1);
    puts("PASS: wake -> 80ms cue + drain/tail -> realtime listen_start");

    json("{\"type\":\"stt\",\"text\":\"你好\"}");
    assert(web_g_state == kDeviceStateThinking && !xz_ws_uplink_allowed());
    json("{\"type\":\"tts\",\"state\":\"start\"}");
    assert(web_g_state == kDeviceStateSpeaking && xz_ws_uplink_allowed());
    json("{\"type\":\"tts\",\"state\":\"stop\"}");
    assert(web_g_state == kDeviceStateListening && ws_session_active);
    ws_set_state(kDeviceStateSpeaking); ws_end_speaking();
    assert(web_g_state == kDeviceStateListening);
    puts("PASS: normal reply / speaking watchdog -> Listening, session retained");

    xz_audio_send_using_websocket(frame_data, sizeof(frame_data));
    uint32_t old_generation = ws_audio_generation;
    unsigned before = binary_writes;
    json("{\"type\":\"goodbye\"}");
    assert(web_g_state == kDeviceStateIdle && !ws_session_active && !ws_reconnect_allowed);
    drain_tx(); assert(binary_writes == before);
    unsigned listen_before = listen_writes, hello_before = hello_writes;
    xz_ws_process_wake();
    assert(listen_writes == listen_before && hello_writes == hello_before);
    my_wsapp_fn(WS_DISCONNECT, NULL, 0);
    assert(web_g_state == kDeviceStateIdle && !ws_reconnect_allowed);
    json("{\"type\":\"tts\",\"state\":\"start\"}");
    my_wsapp_fn(4, "x", 1);
    assert(web_g_state == kDeviceStateIdle && downlink_calls == 0);
    puts("PASS: goodbye + TCP close + late TTS -> Standby, no auto wake/reconnect");

    /* KWS emits a second PRESS without RELEASE; this must still wake. */
    xz_button_event_handler(1, BUTTON_PRESSED);
    assert(ws_wake_pending && ws_reconnect_allowed && web_g_state == kDeviceStateConnecting);
    my_wsapp_fn(WS_CONNECT, (char *)(uintptr_t)101, 0);
    xz_ws_process_wake(); hello(); finish_wake();
    assert(xz_ws_write_frame(&g_xz_ws.clnt, "x", 1, OPCODE_BINARY, old_generation) == ERR_ABRT);
    assert(binary_writes == before);
    puts("PASS: repeated KWS PRESS -> reconnect + hello + cue; old generation rejected");

    static const char *words[] = {"退下吧", "退下", "结束", "不用了", "关闭对话"};
    for (unsigned i=0; i<5; ++i) {
        char text[128];
        snprintf(text, sizeof(text), "{\"type\":\"stt\",\"text\":\"%s\"}", words[i]);
        unsigned goodbyes = goodbye_writes;
        json(text);
        assert(web_g_state == kDeviceStateIdle && !ws_session_active && !ws_hello_ready);
        assert(goodbye_writes == goodbyes+1 && !ws_reconnect_allowed);
        xz_button_event_handler(1, BUTTON_PRESSED);
        xz_ws_process_wake(); hello(); finish_wake();
    }
    puts("PASS: all five dismiss intents -> immediate Standby + abort/goodbye; wake again");

    char *off_args[] = {"xz_uplink", "off"};
    char *on_args[] = {"xz_uplink", "on"};
    xz_uplink(2, off_args); assert(g_xz_uplink_on == 0);
    xz_audio_send_using_websocket(frame_data, sizeof(frame_data));
    drain_tx(); assert(binary_writes == before);
    assert(xz_ws_write_frame(&g_xz_ws.clnt, "x", 1, OPCODE_BINARY, ws_audio_generation) == ERR_ABRT);
    xz_uplink(2, on_args); assert(g_xz_uplink_on == 1);
    xz_audio_send_using_websocket(frame_data, sizeof(frame_data));
    mock_write_error = ERR_MEM; goodbye_on_delay = true;
    drain_tx(); assert(binary_writes == before && web_g_state == kDeviceStateIdle);
    puts("PASS: manual uplink gate and goodbye during ERR_MEM retry block every write");

    xz_button_event_handler(1, BUTTON_PRESSED); hello(); xz_ws_process_wake();
    uint32_t stale_token = ws_wake_token;
    json("{\"type\":\"goodbye\"}");
    xz_audio_wake_beep(); xz_ws_process_wake();
    assert(!ws_session_active && web_g_state == kDeviceStateIdle);
    xz_button_event_handler(1, BUTTON_PRESSED); hello(); xz_ws_process_wake();
    xz_ws_wake_beep_done(stale_token, RT_EOK); xz_ws_process_wake();
    assert(!ws_session_active);
    xz_audio_wake_beep(); close_during_listen = true; xz_ws_process_wake();
    assert(!ws_session_active && web_g_state == kDeviceStateIdle);
    puts("PASS: cancelled/stale cue and goodbye inside listen write cannot revive session");

    xz_button_event_handler(1, BUTTON_PRESSED); hello(); xz_ws_process_wake();
    my_wsapp_fn(WS_DISCONNECT, NULL, 0); /* transport drops while the cue drains */
    my_wsapp_fn(WS_CONNECT, (char *)(uintptr_t)101, 0); hello();
    xz_ws_process_wake();
    assert(ws_wake_pending && !ws_beep_pending && xz_wake_beep_busy);
    xz_audio_wake_beep(); /* old cue cannot finish the new wake */
    xz_ws_process_wake(); finish_wake();
    json("{\"type\":\"goodbye\"}");
    puts("PASS: disconnect during cue -> wait for old drain -> fresh cue -> Listening");

    xz_button_event_handler(1, BUTTON_PRESSED); hello(); xz_ws_process_wake();
    mock_audio_error = 1; xz_audio_wake_beep(); mock_audio_error = 0;
    xz_ws_process_wake(); assert(!ws_session_active && web_g_state == kDeviceStateIdle);
    assert(volume == 8);
    puts("PASS: cue failure restores volume and stays Standby");

    xz_button_event_handler(1, BUTTON_PRESSED); finish_wake();
    my_wsapp_fn(WS_DISCONNECT, NULL, 0);
    assert(web_g_state == kDeviceStateConnecting && !ws_session_active && !ws_wake_pending);
    my_wsapp_fn(WS_CONNECT, (char *)(uintptr_t)101, 0); hello();
    assert(web_g_state == kDeviceStateIdle && !ws_session_active);
    assert(ws_tx_mutex.depth == 0);
    puts("PASS: unexpected disconnect -> Connect -> hello -> Standby (no auto listen)");
    return 0;
}
