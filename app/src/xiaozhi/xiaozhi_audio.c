/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/apps/mqtt.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "xiaozhi_mqtt.h"
#include "bf0_hal.h"
#include "button.h"
#include "os_adaptor.h"
#include "opus_types.h"
#include "opus_multistream.h"
#include "os_support.h"
#include "audio_server.h"
#include "drv_audprc.h"
#include "mem_section.h"
#ifdef PKG_XIAOZHI_USING_AEC
    #include "webrtc/common_audio/vad/include/webrtc_vad.h"
    #include "sifli_resample.h"
#endif
#include "xiaozhi_client_public.h"
#include "opus.h"
#include "debug.h"
#include "opus_private.h"
#include "../weather/weather.h"
#include "gui_app_pm.h"
#include "lv_display.h"
#include "xiaozhi_ui.h"
#include "xiaozhi_websocket.h"
#include "xiaozhi_music.h"
#include "xiaozhi_music_source.h"
#include "xiaozhi_mic_dsp.h"
#include "xiaozhi_barge_ref.h"

/* Kconfig writes XZ_* into rtconfig.h; retain CONFIG_* as a direct compiler
 * override as well. Set CONFIG_XZ_MIC_LEVEL_LOG=n in proj.conf to remove it. */
#ifndef CONFIG_XZ_MIC_LEVEL_LOG
#ifdef XZ_MIC_LEVEL_LOG
#define CONFIG_XZ_MIC_LEVEL_LOG 1
#else
#define CONFIG_XZ_MIC_LEVEL_LOG 0
#endif
#endif
#ifdef XZ_MIC_DOWNMIX_MAX_ABS
#define XZ_MIC_MIX_MAX_ABS 1
#else
#define XZ_MIC_MIX_MAX_ABS 0
#endif

extern xiaozhi_ws_t g_xz_ws;
#include "app_recorder_process.h"
#include "xiaozhi_audio.h"

/* dB to trim from the codec's default +12 dB ADC gain. Lower = less sensitive:
 * the built-in mic otherwise picks up a TV on the far side of the room. */
#define XZ_MIC_GAIN_TRIM_DB 10

#define XZ_TX_STRESS 0
#if XZ_TX_STRESS
/* Voiced PCM keeps the service's 60-second silence timeout from ending the
 * transport test. The fixture and all injection code are absent in release. */
#include "../../../verification/tcp-window/stress_pcm.h"
#endif

#include "log.h"

#undef LOG_TAG
#define LOG_TAG "xz"
#define DBG_TAG "xz"
#define DBG_LVL LOG_LVL_INFO
#define XZ_THREAD_NAME "xiaozhi"
#define XZ_OPUS_STACK_SIZE (32 * 1024)
#define XZ_EVENT_MIC_RX (1 << 0)
#define XZ_EVENT_SPK_TX (1 << 1)
#define XZ_EVENT_DOWNLINK (1 << 2)
#define XZ_EVENT_EXIT (1 << 3)
#define XZ_EVENT_WAKE_BEEP (1 << 4)
#define MQTT_RECONNECT 12

#define XZ_EVENT_ALL                                                           \
    (XZ_EVENT_MIC_RX | XZ_EVENT_SPK_TX | XZ_EVENT_DOWNLINK | XZ_EVENT_EXIT | XZ_EVENT_WAKE_BEEP)

#define VOICE_STATE_IDLE                0
#define VOICE_STATE_WAIT_SPEAKING       1
#define VOICE_STATE_SPEAKING            2

#define VOICE_START_TIMES               (XZ_MIC_FRAME_LEN /320 * 2) /* 1 mic frames */
#define VOICE_STOP_TIMES                30

#define XZ_AUDIO_VERSION            "xz_audio_verson: 1.0"

#ifdef XIAOZHI_USING_MQTT
    #define XZ_DEVICE_STATE mqtt_g_state
#else
    #define XZ_DEVICE_STATE web_g_state
#endif
extern rt_mailbox_t g_bt_app_mb;
extern uint8_t vad_enable;
extern uint8_t Initiate_disconnection_flag;
extern lv_obj_t *main_container;
extern lv_obj_t *standby_screen;

struct udp_pcb *udp_pcb;
/* Software codec state: audio_write() copies PCM into the audio server ring.
 * Keep the DMA buffers in the audio driver in internal SRAM. */
xz_audio_t xz_audio L2_RET_BSS_SECT(xz_audio_state);
/* L2 retained BSS survives a UART/software reset. Keep the boot guard in
 * normal SRAM BSS so stale inited/RTOS pointers are never reused after reboot. */
static bool xz_audio_boot_initialized;
static audio_parameter_t xz_music_rx_parameters;
static volatile uint32_t xz_music_rx_frames;
static bool xz_music_rx_live;

bool xz_audio_music_parameters(audio_parameter_t *parameters)
{
    rt_base_t level = rt_hw_interrupt_disable();
    bool live = xz_music_rx_live && xz_audio.mic && xz_music_rx_parameters.read_samplerate &&
                xz_music_rx_parameters.read_channnel_num;
    if (live) *parameters = xz_music_rx_parameters;
    rt_hw_interrupt_enable(level);
    return live;
}

uint32_t xz_audio_music_rx_frames(void) { return xz_music_rx_frames; }

static void xz_music_rx_opened(const audio_parameter_t *parameters)
{
    rt_base_t level = rt_hw_interrupt_disable();
    xz_music_rx_parameters = *parameters;
    xz_music_rx_live = true;
    rt_hw_interrupt_enable(level);
}

static void xz_music_rx_unavailable(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    xz_music_rx_live = false;
    rt_hw_interrupt_enable(level);
}

static void xz_music_rx_closing(void)
{
    xz_music_audio_shutdown(xz_music_rx_unavailable);
}
static volatile bool xz_wake_beep_busy;
static bool xz_decode_inflight;
static uint32_t xz_playback_generation;
static struct rt_mutex xz_playback_mutex;
static int16_t xz_mic_mono[XZ_MIC_SAMPLES_10MS] L2_RET_BSS_SECT(xz_mic_mono);
static int16_t xz_mic_uplink[XZ_MIC_SAMPLES_10MS] L2_RET_BSS_SECT(xz_mic_uplink);
static xz_barge_detector_t xz_barge_detector;
/* One-shot reference: the SDK calls raw -> 3A -> post synchronously. */
static struct { uint16_t rms, peak[2]; uint8_t ready; } xz_mic_pre;
static xz_barge_ref_t xz_mic_ref; /* captured with raw, consumed with post */
/* Raw peaks are safety data, independent of the optional RMS diagnostics. */
static uint32_t xz_mic_raw_peak[2], xz_mic_peak_frames;
static uint32_t xz_mic_raw_blocks, xz_mic_post_blocks, xz_mic_mono_samples;
static uint64_t xz_mic_post_square;
static uint32_t xz_mic_post_peak;
static uint32_t xz_mic_bad_blocks, xz_mic_last_bytes, xz_mic_last_channels;
static rt_tick_t xz_mic_log_tick;
#if CONFIG_XZ_MIC_LEVEL_LOG
static struct
{
    uint64_t square[2];
    uint32_t peak[2], samples;
} xz_mic_levels;
#endif
static uint32_t xz_wake_beep_token;
static int xz_mic_hang;
static uint8_t xz_encode_rb_pool[XZ_MIC_FRAME_LEN * 2
#if PKG_XIAOZHI_USING_AEC
                                 + 320 * VOICE_START_TIMES
#endif
                                 ] L2_RET_BSS_SECT(xz_encode_rb);
static struct rt_ringbuffer xz_encode_rb;
#if PKG_XIAOZHI_USING_AEC
static uint8_t xz_vad_rb_pool[320 * VOICE_START_TIMES]
    L2_RET_BSS_SECT(xz_vad_rb);
static struct rt_ringbuffer xz_vad_rb;
#endif
bool g_ota_verified = true;
/* 最近一次收到服务端数据（文本或音频）的时刻，用于 speaking 状态看门狗。 */
volatile rt_tick_t g_xz_rx_tick;

#if defined(__CC_ARM) || defined(__CLANG_ARM)
L2_RET_BSS_SECT_BEGIN(g_xz_opus_stack) //6000地址
static uint32_t g_xz_opus_stack[XZ_OPUS_STACK_SIZE / sizeof(uint32_t)];
L2_RET_BSS_SECT_END
#else
static uint32_t
    g_xz_opus_stack[XZ_OPUS_STACK_SIZE / sizeof(uint32_t)] L2_RET_BSS_SECT(g_xz_opus_stack);
#endif

static void xz_opus_thread_entry(void *p);
static void audio_write_and_wait(xz_audio_t *thiz, uint8_t *data, uint32_t data_len,
                                 uint32_t generation);
static void xz_button_event_handler(int32_t pin, button_action_t action);
void xz_mic_open(xz_audio_t *thiz);
void xz_mic_close(xz_audio_t *thiz);
void xz_speaker_close(xz_audio_t *thiz);
void xz_speaker_open(xz_audio_t *thiz);

void xz_audio_send(uint8_t *data, int len)
{
    uint32_t nonce[4];
    uint8_t *p_nonce = (uint8_t *)&(nonce[0]);

    memcpy(p_nonce, &(g_xz_context.nonce[0]), sizeof(nonce));
    *(uint16_t *)(p_nonce + 2) = htons(len);
    *(uint32_t *)(p_nonce + 12) = htonl(++g_xz_context.local_sequence);
    // Encrypt data
    HAL_AES_init((uint32_t *)&(g_xz_context.key[0]), 16,
                 (uint32_t *)&(nonce[0]), AES_MODE_CTR);
    struct pbuf *pbuf =
        pbuf_alloc(PBUF_TRANSPORT, len + sizeof(nonce) + 32, PBUF_RAM);
    if (pbuf && g_xz_context.port)
    {
        uint8_t *payload = (uint8_t *)pbuf->payload;
        memcpy(payload, nonce, sizeof(nonce));
        payload += sizeof(nonce);
        HAL_AES_run(AES_ENC, data, payload, len);
        LOCK_TCPIP_CORE();
        udp_sendto(udp_pcb, pbuf, &(g_xz_context.udp_addr), g_xz_context.port);
        UNLOCK_TCPIP_CORE();
        pbuf_free(pbuf);
    }
}

void xz_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                 const ip_addr_t *addr, u16_t port)
{
    if (memcmp(addr, &(g_xz_context.udp_addr), sizeof(ip_addr_t)) == 0 &&
        port == g_xz_context.port)
    {
        uint8_t *data = (uint8_t *)p->payload;
        uint32_t nonce[4];
        uint32_t size = 0;
        uint32_t sequence = ntohl(*(uint32_t *)&data[12]);

        if (p->len < sizeof(nonce))
        {
            rt_kprintf("Invalid audio packet size: %u\n", p->len);
            goto end;
        }
        if (data[0] != 0x01)
        {
            rt_kprintf("Invalid audio packet type: %x", data[0]);
            goto end;
        }
        if (sequence < g_xz_context.remote_sequence)
        {
            rt_kprintf(
                "Received audio packet with old sequence: %lu, expected: %lu\n",
                sequence, g_xz_context.remote_sequence);
            goto end;
        }
        if (sequence != g_xz_context.remote_sequence + 1)
        {
            rt_kprintf("Received audio packet with wrong sequence: %lu, "
                       "expected: %lu\n",
                       sequence, g_xz_context.remote_sequence + 1);
            g_xz_context.remote_sequence = sequence;
        }
        else
        {
            g_xz_context.remote_sequence = sequence;
        }
        memcpy(&(nonce[0]), data, sizeof(nonce));
        data += sizeof(nonce);
        size = p->len - sizeof(nonce);
        xz_audio_downlink(data, size, &nonce[0], 1);
    end:
        pbuf_free(p);
    }
    else
    {
        rt_kprintf("invalid udp\n");
    }
}
#ifdef XIAOZHI_USING_MQT
void simulate_button_pressed()
{
     rt_kprintf("mqtt simulate_button_pressed pressed\r\n");
    if(Initiate_disconnection_flag)//蓝牙主动断开不允许mic触发
    {
        rt_kprintf("Initiate_disconnection_flag\r\n");
        return;
    }
    xz_button_event_handler(BSP_KEY1_PIN, BUTTON_PRESSED);
}
void simulate_button_released()
{
    rt_kprintf("mqtt simulate_button_released released\r\n");
    if(Initiate_disconnection_flag)
    {
        return;
    }
    xz_button_event_handler(BSP_KEY1_PIN, BUTTON_RELEASED);
}
#else 
RT_WEAK void simulate_button_pressed()
{
}
RT_WEAK void simulate_button_released()
{
}
#endif

volatile uint32_t g_mic_cb, g_mic_bytes, g_mic_peak, g_mic_abs, g_mic_nz, g_mic_n;

static int xz_micstat(int argc, char **argv)
{
    uint32_t cb = g_mic_cb, bytes = g_mic_bytes, peak = g_mic_peak;
    uint32_t abs = g_mic_abs, nz = g_mic_nz, n = g_mic_n;
    rt_kprintf("MIC cb=%u bytes=%u peak=%u meanabs=%u nonzero=%u/%u\n",
               (unsigned)cb, (unsigned)bytes, (unsigned)peak,
               (unsigned)(n ? abs / n : 0), (unsigned)nz, (unsigned)n);
    g_mic_cb = g_mic_bytes = g_mic_peak = g_mic_abs = g_mic_nz = g_mic_n = 0;
    return 0;
}
MSH_CMD_EXPORT(xz_micstat, mic capture stats since last call);

/* ---- 隔离自测: 麦克风分块能量 / 喇叭单音 ---- */
#define XZ_REC_MAX_BLOCKS 120
volatile int xz_rec_on, xz_rec_idx, xz_rec_n;
static uint16_t xz_rec_peak[XZ_REC_MAX_BLOCKS];

static int xz_rec(int argc, char **argv)
{
    int n = (argc > 1) ? atoi(argv[1]) : 150;
    if (n > XZ_REC_MAX_BLOCKS) n = XZ_REC_MAX_BLOCKS;
    xz_rec_idx = 0; xz_rec_n = n; xz_rec_on = 1;
    rt_thread_mdelay(n * 10 + 200);          /* 每块 10 ms */
    xz_rec_on = 0;
    rt_kprintf("REC %d blocks of 10ms, peak per 50ms:\n", xz_rec_idx);
    for (int i = 0; i < xz_rec_idx; i += 5)
    {
        int pk = 0;
        for (int k = i; k < i + 5 && k < xz_rec_idx; k++)
            if (xz_rec_peak[k] > pk) pk = xz_rec_peak[k];
        rt_kprintf("%4dms:%6d%s", i * 10, pk, ((i / 5) % 8 == 7) ? "\n" : "");
    }
    rt_kprintf("\n");
    return 0;
}
MSH_CMD_EXPORT(xz_rec, capture mic block energy: xz_rec [blocks]);

/* 1 kHz @ 16 kHz 恰好 16 点一个周期 */
static const int16_t sine1k_16[16] = {
    0, 3127, 5776, 7571, 8480, 7571, 5776, 3127,
    0, -3127, -5776, -7571, -8480, -7571, -5776, -3127
};

static int xz_beep(int ms, int vol)
{
    int16_t buf[160];
    int phase = 0, ok = 0, full = 0, err = 0, first = 0;

    if (xz_audio.speaker == NULL)
    {
        rt_kprintf("speaker not open, cannot play\n");
        return -RT_ERROR;
    }
    if (vol >= 0)
    {
        audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, (uint8_t)vol);
        rt_kprintf("private volume level -> %d (now %d)\n",
                   vol, audio_server_get_private_volume(AUDIO_TYPE_LOCAL_MUSIC));
    }
    xz_speaker(1);
    rt_kprintf("play 1kHz %dms tx_en=%d speaker=%p\n", ms, xz_audio.is_tx_enable, xz_audio.speaker);

    for (int b = 0; b < ms / 10; b++)
    {
        for (int i = 0; i < 160; i++)
        {
            buf[i] = sine1k_16[phase];
            if (++phase >= 16) phase = 0;
        }
        int r = 0;
        for (int retry = 0; retry < 20; ++retry)
        {
            r = audio_write(xz_audio.speaker, (uint8_t *)buf, sizeof(buf));
            if (r != 0) break;
            rt_thread_mdelay(5);
        }
        if (b == 0) first = r;
        if (r > 0) ok++; else if (r == 0) full++; else err++;
        rt_thread_mdelay(10);          /* 按实时速率喂,约 32 kB/s */
    }
    rt_kprintf("play done: first=%d ok=%d full=%d err=%d\n", first, ok, full, err);
    return (!full && !err && ok > 0) ? RT_EOK : -RT_ERROR;
}

static int xz_play(int argc, char **argv)
{
    xz_beep((argc > 1) ? atoi(argv[1]) : 1500,
            (argc > 2) ? atoi(argv[2]) : -1);
    return 0;
}
MSH_CMD_EXPORT(xz_play, play 1kHz tone: xz_play [ms] [vol_level]);

void xz_audio_reset_uplink(void)
{
    rt_enter_critical();
    if (xz_audio.rb_opus_encode_input)
        rt_ringbuffer_reset(xz_audio.rb_opus_encode_input);
    xz_audio.mic_rx_count = 0;
    xz_mic_hang = 0;
    rt_exit_critical();
}

int xz_audio_request_wake_beep(uint32_t token)
{
    if (!xz_audio.inited || !xz_audio.event) return -RT_ERROR;
    rt_enter_critical();
    if (xz_wake_beep_busy)
    {
        rt_exit_critical();
        return -RT_EBUSY;
    }
    xz_wake_beep_busy = true;
    xz_wake_beep_token = token;
    rt_exit_critical();
    rt_err_t result = rt_event_send(xz_audio.event, XZ_EVENT_WAKE_BEEP);
    if (result != RT_EOK) xz_wake_beep_busy = false;
    return result;
}

static void xz_audio_wake_beep(void)
{
    uint32_t token = xz_wake_beep_token;
    uint8_t old_volume = audio_server_get_private_volume(AUDIO_TYPE_LOCAL_MUSIC);
    rt_kprintf("WAKE_BEEP begin token=%u ms=80 vol=6\n", (unsigned)token);
    xz_music_tts(true);
    int result = xz_beep(80, 6);
    /* audio_write() queues PCM. Wait for actual drain, then leave 40 ms for
     * the DAC/AEC tail; neither KWS nor uplink receives the local cue. */
    uint32_t remaining = 0;
    for (int retry = 0; retry < 100; ++retry)
    {
        if (audio_ioctl(xz_audio.speaker, AUDIO_IOCTL_BYTES_IN_CACHE, &remaining) != 0)
        {
            result = -RT_ERROR;
            break;
        }
        if (!remaining) break;
        rt_thread_mdelay(5);
    }
    if (remaining) result = -RT_ETIMEOUT;
    rt_thread_mdelay(40);
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, old_volume);
    xz_audio_reset_uplink();
    xz_music_tts(false);
    xz_wake_beep_busy = false;
    rt_kprintf("WAKE_BEEP drained token=%u result=%d restore_vol=%u\n",
               (unsigned)token, result, old_volume);
    xz_ws_wake_beep_done(token, result);
}

static int xz_btnstat(int argc, char **argv)
{
    rt_kprintf("KEY pin=%d level=%d act_level=%d\n",
               BSP_KEY1_PIN, rt_pin_read(BSP_KEY1_PIN), KEY1_ACTIVE_LEVEL);
    return 0;
}
MSH_CMD_EXPORT(xz_btnstat, read KEY1 pin level);

bool xz_audio_wake_beep_active(void) { return xz_wake_beep_busy; }

void xz_audio_reset_barge_in(void)
{
    rt_enter_critical();
    xz_barge_reset(&xz_barge_detector);
    xz_barge_ref_reset();
    xz_mic_ref = (xz_barge_ref_t){0};
    xz_mic_pre.rms = 0;
    xz_mic_pre.ready = 0;
    rt_exit_critical();
}

bool xz_audio_barge_in_armed(uint32_t generation)
{
    rt_enter_critical();
    bool armed = xz_barge_detector.generation == generation &&
                 xz_barge_detector.phase == XZ_BARGE_ARMED;
    rt_exit_critical();
    return armed;
}

/* No writes to the SDK's const input, no allocation, exactly 160 output
 * samples. The same conversion is used before AEC and for stereo callbacks
 * from an SDK which can supply two processed channels. */
static void xz_mic_downmix(const int16_t *input, unsigned channels, bool pre3a)
{
    uint64_t square[2] = {0, 0};
    uint32_t peak[2] = {0, 0};
    for (unsigned i = 0; i < XZ_MIC_SAMPLES_10MS; ++i)
    {
        int16_t l = input[i * channels];
        int16_t r = channels == 2 ? input[i * channels + 1] : l;
        xz_mic_mono[i] = channels == 2 ? xz_mic_mix(l, r, XZ_MIC_MIX_MAX_ABS) : l;
        if (pre3a)
        {
            square[0] += (uint64_t)((int32_t)l * l);
            square[1] += (uint64_t)((int32_t)r * r);
            int32_t lv = l, rv = r;
            uint32_t la = lv < 0 ? -lv : lv, ra = rv < 0 ? -rv : rv;
            if (la > peak[0]) peak[0] = la;
            if (ra > peak[1]) peak[1] = ra;
        }
#if CONFIG_XZ_MIC_LEVEL_LOG
        if (channels == 2)
        {
            int32_t lv = l, rv = r;
            uint32_t la = lv < 0 ? -lv : lv, ra = rv < 0 ? -rv : rv;
            xz_mic_levels.square[0] += (uint64_t)(lv * lv);
            xz_mic_levels.square[1] += (uint64_t)(rv * rv);
            if (la > xz_mic_levels.peak[0]) xz_mic_levels.peak[0] = la;
            if (ra > xz_mic_levels.peak[1]) xz_mic_levels.peak[1] = ra;
        }
#endif
    }
    if (pre3a)
    {
        /* Use the louder raw channel, not a possibly phase-cancelled mix.
         * This reference remains available with MIC_LEVEL_LOG disabled. */
        uint64_t maximum = square[0] > square[1] ? square[0] : square[1];
        xz_mic_pre.rms = (uint16_t)xz_mic_isqrt((uint32_t)(maximum / XZ_MIC_SAMPLES_10MS));
        for (unsigned ch = 0; ch < 2; ++ch)
        {
            xz_mic_pre.peak[ch] = (uint16_t)peak[ch];
            if (peak[ch] > xz_mic_raw_peak[ch]) xz_mic_raw_peak[ch] = peak[ch];
        }
        ++xz_mic_peak_frames;
        rt_enter_critical();
        xz_barge_ref_snapshot(&xz_mic_ref);
        xz_mic_pre.ready = 1;
        rt_exit_critical();
    }
#if CONFIG_XZ_MIC_LEVEL_LOG
    if (channels == 2) xz_mic_levels.samples += XZ_MIC_SAMPLES_10MS;
#endif
}

static void xz_mic_log(void)
{
    rt_tick_t now = rt_tick_get();
    if (now - xz_mic_log_tick < rt_tick_from_millisecond(2000)) return;
    xz_mic_log_tick = now;
    rt_kprintf("MIC_PEAK L=%u R=%u clip=%u pre_valid=%u\n",
               (unsigned)xz_mic_raw_peak[0], (unsigned)xz_mic_raw_peak[1],
               (unsigned)xz_mic_clipped(xz_mic_raw_peak[0], xz_mic_raw_peak[1]),
               (unsigned)(xz_mic_peak_frames != 0));
    xz_mic_raw_peak[0] = xz_mic_raw_peak[1] = xz_mic_peak_frames = 0;
#if CONFIG_XZ_MIC_LEVEL_LOG
    if (xz_mic_levels.samples)
        rt_kprintf("MIC_L rms=%u peak=%u | MIC_R rms=%u peak=%u source=%s\n",
            (unsigned)xz_mic_isqrt((uint32_t)(xz_mic_levels.square[0] / xz_mic_levels.samples)),
            (unsigned)xz_mic_levels.peak[0],
            (unsigned)xz_mic_isqrt((uint32_t)(xz_mic_levels.square[1] / xz_mic_levels.samples)),
            (unsigned)xz_mic_levels.peak[1], xz_mic_raw_blocks ? "pre3a_LR" : "post3a_LR");
    else
        rt_kprintf("MIC_L rms=NA peak=NA | MIC_R rms=NA peak=NA source=post3a_mono\n");
    memset(&xz_mic_levels, 0, sizeof(xz_mic_levels));
#endif
    rt_kprintf("MIC_PCM raw_lr_blocks=%u post_blocks=%u mono_samples=%u "
               "mono_rms=%u mono_peak=%u post_bytes=%u post_ch=%u frame_samples=160 fs=16000 frame_ms=10 "
               "mix=%s bad_len=%u\n",
               (unsigned)xz_mic_raw_blocks, (unsigned)xz_mic_post_blocks,
               (unsigned)xz_mic_mono_samples,
               (unsigned)xz_mic_isqrt(xz_mic_mono_samples ?
                   (uint32_t)(xz_mic_post_square / xz_mic_mono_samples) : 0),
               (unsigned)xz_mic_post_peak, (unsigned)xz_mic_last_bytes,
               (unsigned)xz_mic_last_channels, XZ_MIC_MIX_MAX_ABS ? "max_abs" : "average",
               (unsigned)xz_mic_bad_blocks);
    xz_mic_raw_blocks = xz_mic_post_blocks = xz_mic_mono_samples = xz_mic_bad_blocks = 0;
    xz_mic_post_square = xz_mic_post_peak = 0;
    rt_enter_critical();
    xz_barge_detector_t detector = xz_barge_detector;
    rt_exit_critical();
    if (detector.phase != XZ_BARGE_IDLE)
        rt_kprintf("BARGE_MON generation=%u training=%u floor=%u threshold=%u "
                   "rms=%u pre=%u ratio=%u/1000 pre_valid=%u frames=%u refh=%u ref_age=%ums\n",
                   (unsigned)detector.generation, detector.phase == XZ_BARGE_TRAIN,
                   (unsigned)detector.echo_floor,
                   (unsigned)xz_barge_threshold(&detector, XZ_BARGE_IN_RMS, XZ_BARGE_IN_ECHO_MULT),
                   (unsigned)detector.rms, (unsigned)detector.pre_rms,
                   (unsigned)xz_barge_ratio(detector.rms, detector.pre_rms),
                   (unsigned)detector.pre_valid, (unsigned)detector.frames,
                   (unsigned)detector.ref_hold, (unsigned)detector.ref_age_ms);
}

static void xz_mic_barge_in(uint32_t power, int voice)
{
    static rt_tick_t last_frame;
    static rt_tick_t curve_tick;
    static uint32_t curve_generation;
    static bool curve_valid;
    static rt_tick_t clip_tick;
    static bool clip_logged;
    static rt_tick_t sync_tick;
    static bool sync_logged;
    uint32_t generation = 0;
    rt_tick_t start = 0;
    /* Lifecycle reset, context snapshot, detection and publication must not
     * race tts stop/abort on another thread. No socket I/O in this section. */
    rt_enter_critical();
    bool speaking = xz_ws_barge_in_context(&generation, &start);
    bool eligible = speaking && !xz_wake_beep_busy && !Initiate_disconnection_flag;
    rt_tick_t now = rt_tick_get();
    uint32_t frame_ms = (uint32_t)((uint64_t)(rt_tick_t)(now - last_frame) * 1000u / RT_TICK_PER_SECOND);
    if (now - last_frame > rt_tick_from_millisecond(30))
        xz_barge_break_streak(&xz_barge_detector);
    last_frame = now;
    uint8_t previous_phase = xz_barge_detector.phase;
    uint32_t pre = xz_mic_pre.rms;
    uint32_t peak_l = xz_mic_pre.peak[0], peak_r = xz_mic_pre.peak[1];
    bool clipped = xz_mic_pre.ready && xz_mic_clipped(peak_l, peak_r);
    bool ref_valid = xz_mic_pre.ready && xz_mic_ref.synced;
    bool sync_log = eligible && !ref_valid &&
                    (!sync_logged || now - sync_tick >= rt_tick_from_millisecond(2000));
    if (sync_log)
    {
        sync_logged = true;
        sync_tick = now;
    }
    bool clip_log = eligible && clipped &&
                    (!clip_logged || now - clip_tick >= rt_tick_from_millisecond(2000));
    if (clip_log)
    {
        clip_logged = true;
        clip_tick = now;
    }
    if (!speaking) xz_barge_reset(&xz_barge_detector);
    else if (xz_barge_step(&xz_barge_detector, generation, eligible,
                      xz_mic_isqrt(power), pre, xz_mic_pre.ready, clipped,
                      xz_mic_ref.rms, ref_valid,
                      voice, XZ_BARGE_IN_RMS, XZ_BARGE_IN_ECHO_MULT,
                      XZ_BARGE_DTD_MARGIN_PM, XZ_BARGE_IN_FRAMES,
                      XZ_BARGE_ARM_RATIO, XZ_BARGE_ARM_MS, XZ_BARGE_ARM_MIN_PRE, frame_ms))
    {
        if (!xz_ws_request_speech_barge_in(&xz_barge_detector))
            xz_barge_reset(&xz_barge_detector);
    }
    xz_barge_detector_t detector = xz_barge_detector;
    rt_tick_t elapsed = now - start;
    bool curve = eligible && elapsed < rt_tick_from_millisecond(3000) &&
                 (!curve_valid || curve_generation != generation ||
                  now - curve_tick >= rt_tick_from_millisecond(100));
    if (curve)
    {
        curve_valid = true;
        curve_generation = generation;
        curve_tick = now;
    }
    xz_mic_pre.ready = 0; /* never reuse a raw reference for a later post frame */
    rt_exit_critical();
    if (sync_log)
        rt_kprintf("BARGE_DTD_SYNC gen=%u age_ms=%u skew=%d pre_valid=%u\n",
                   (unsigned)generation, (unsigned)xz_mic_ref.age_ms,
                   (int)xz_mic_ref.skew, (unsigned)detector.pre_valid);
    if (clip_log)
        rt_kprintf("BARGE_CLIP pre=%u peakL=%u peakR=%u generation=%u\n",
                   (unsigned)pre, (unsigned)peak_l, (unsigned)peak_r, (unsigned)generation);
    if (detector.phase == XZ_BARGE_ARMED && previous_phase != XZ_BARGE_ARMED)
        rt_kprintf("BARGE_TRAIN generation=%u phase=armed reason=converged t=%ums window_ms=%u frames=%u "
                   "floor=%u threshold=%u "
                   "rms=%u pre=%u ratio=%u/1000 pre_valid=%u g=%u ref=%u refh=%u ref_age=%ums\n",
                   (unsigned)generation,
                   (unsigned)((uint64_t)elapsed * 1000u / RT_TICK_PER_SECOND),
                   (unsigned)detector.arm_frames * 10u, (unsigned)detector.arm_frames,
                   (unsigned)detector.echo_floor,
                   (unsigned)xz_barge_threshold(&detector, XZ_BARGE_IN_RMS, XZ_BARGE_IN_ECHO_MULT),
                   (unsigned)detector.rms, (unsigned)detector.pre_rms,
                   (unsigned)xz_barge_ratio(detector.rms, detector.pre_rms),
                   (unsigned)detector.pre_valid, (unsigned)detector.echo_gain_milli,
                   (unsigned)detector.ref_rms, (unsigned)detector.ref_hold,
                   (unsigned)detector.ref_age_ms);
    if (curve)
        rt_kprintf("BARGE_CURVE gen=%u t=%ums pre=%u post=%u ratio=%u/1000 state=%s pre_valid=%u clip=%u "
                   "ref=%u ref_peak=%u g=%u dtd=%u sync=%u refh=%u ref_age=%ums\n",
                   (unsigned)generation,
                   (unsigned)((uint64_t)elapsed * 1000u / RT_TICK_PER_SECOND),
                   (unsigned)detector.pre_rms, (unsigned)detector.rms,
                   (unsigned)xz_barge_ratio(detector.rms, detector.pre_rms),
                   detector.phase == XZ_BARGE_ARMED ? "armed" : "train",
                   (unsigned)detector.pre_valid, (unsigned)clipped,
                   (unsigned)detector.ref_rms, (unsigned)xz_mic_ref.peak,
                   (unsigned)detector.echo_gain_milli, (unsigned)detector.dtd, (unsigned)ref_valid,
                   (unsigned)detector.ref_hold, (unsigned)detector.ref_age_ms);
}

void xz_audio_apply_pdm_gain(void)
{
#ifdef AUDIO_RX_USING_PDM
    /* Already exported by drv_audprc.c, but absent from its public header.
     * Set both the saved startup value and the live L/R registers. Repeating
     * an absolute setting cannot accumulate attenuation across reopens. */
    extern int Set_pdm_gain(int8_t value);
    static bool logged;
    int result = Set_pdm_gain(XZ_PDM_GAIN_TRIM);
    RT_ASSERT(result == 0);
    if (!logged)
    {
        logged = true;
        rt_kprintf("PDM_GAIN set=%d\n", XZ_PDM_GAIN_TRIM);
    }
#endif
}

static int xz_mic_mono_frame(audio_server_coming_data_t *p)
{
    xz_audio_t *thiz = &xz_audio;
    const int16_t *sm = (const int16_t *)p->data;
    uint64_t square = 0;
    uint32_t peak = 0;
    uint32_t compensated_peak = 0;
    for (unsigned i = 0; i < XZ_MIC_SAMPLES_10MS; ++i)
    {
        int32_t sample = sm[i];
        /* Share one saturated +6 dB copy across KWS, VAD and uplink.
         * All post/barge energy and clipping references remain raw. */
        int32_t compensated = xz_mic_makeup_sample(sm[i]);
        xz_mic_uplink[i] = (int16_t)compensated;
        uint32_t compensated_abs = compensated < 0 ? -compensated : compensated;
        if (compensated_abs > compensated_peak) compensated_peak = compensated_abs;
        uint32_t v = sample < 0 ? -sample : sample;
        square += (uint64_t)(sample * sample);
        g_mic_abs += v;
        if (v > g_mic_peak) g_mic_peak = v;
        if (v > peak) peak = v;
        if (v) g_mic_nz++;
        g_mic_n++;
    }
    xz_mic_post_square += square;
    if (peak > xz_mic_post_peak) xz_mic_post_peak = peak;
    if (xz_rec_on && xz_rec_idx < xz_rec_n)
        xz_rec_peak[xz_rec_idx++] = (uint16_t)compensated_peak;
#ifdef AUDIO_RX_USING_PDM
    static bool makeup_logged;
    if (!makeup_logged)
    {
        makeup_logged = true;
        rt_kprintf("MIC_MKUP uw=1 kws=1 vad=1\n");
    }
#endif
    if (!xz_wake_beep_busy)
        kws_feed_mic(xz_mic_uplink, XZ_MIC_SAMPLES_10MS); /* also during Speaking/Standby */

    int voice = -1;
#ifdef PKG_XIAOZHI_USING_AEC
    /* One VAD call per post-AEC frame, shared by uplink and local barge-in.
     * An unavailable/error VAD must never count as speech. */
    if (thiz->inited && thiz->handle && !xz_wake_beep_busy)
        voice = WebRtcVad_Process(thiz->handle, 16000, xz_mic_uplink, XZ_MIC_SAMPLES_10MS);
    if (thiz->is_websocket)
        xz_mic_barge_in((uint32_t)(square / XZ_MIC_SAMPLES_10MS), voice);
#endif
    if (thiz->inited && thiz->is_rx_enable)
    {
        /* Keep raw capture alive for local KWS, but never let Standby,
         * handshake, THINK, or the wake tone enter the encoder/VAD wake path. */
        if (thiz->is_websocket &&
            (xz_wake_beep_busy || !xz_ws_uplink_allowed()))
        {
            xz_audio_reset_uplink();
            return 0;
        }
        /* Reuse the KWS/VAD compensated copy for every uplink path,
         * including the legacy VAD pre-roll cache; never apply gain twice. */
        audio_server_coming_data_t uplink = *p;
        uplink.data = (const uint8_t *)xz_mic_uplink;
        p = &uplink;
        // data lengh is 320 bytes, which is 10ms for 16k samplerate
        /* 诊断：确认麦克风回调是否还在跑、VAD 是否放行。 */
        static uint32_t cb_cnt = 0, pass_cnt = 0;
        static rt_tick_t last_dbg = 0;
        cb_cnt++;
        /* 手动聆听（按键或唤醒词明确开启的一轮）不做 VAD 判定，直接把麦克风
         * 数据送进编码器。否则下面的 "非待命状态 return 0" 会把这期间的所有
         * 语音帧丢掉，服务端永远收不到声音。 */
        if (g_xz_ws.is_connected == 1)
        {
#if XZ_TX_STRESS
            /* The timed fixture is the only encoder input in a bench build. */
            return 0;
#endif
            /* 只上行检测到人声的帧。lwIP 只有 6 个 pbuf，播报下行和麦克风
             * 同时占用就会让 altcp_write() 失败、websocket 被关掉。 */
            pass_cnt++;
            /* 只发“说话区间”的帧：全速上行会把 AIC 发送环顶满。
             * 800 ms 拖尾，避免尾字被切。 */
#ifdef PKG_XIAOZHI_USING_AEC
            {
                int uplink_voice = thiz->vad_enabled ? voice == 1 : 1;
                if (uplink_voice)
                {
                    xz_mic_hang = 80;
                }
                else if (xz_mic_hang > 0)
                {
                    xz_mic_hang--;
                }
                if (!uplink_voice && xz_mic_hang == 0)
                {
                    return 0;
                }
            }
#endif
            rt_enter_critical();
            if (thiz->is_websocket && !xz_ws_uplink_allowed())
            {
                rt_exit_critical();
                return 0;
            }
            rt_ringbuffer_put(thiz->rb_opus_encode_input, p->data, p->data_len);
            thiz->mic_rx_count += 320;
            if (thiz->mic_rx_count >= XZ_MIC_FRAME_LEN)
            {
                thiz->mic_rx_count = 0;
                rt_event_send(thiz->event, XZ_EVENT_MIC_RX);
            }
            rt_exit_critical();
            if (rt_tick_get() - last_dbg > rt_tick_from_millisecond(10000))
            {
                last_dbg = rt_tick_get();
                rt_kprintf("mic dbg: cb=%u pass=%u rx_en=%d vad_en=%d vs=%d\n",
                           cb_cnt, pass_cnt, thiz->is_rx_enable,
                           thiz->vad_enabled, thiz->voice_state);
                cb_cnt = 0;
                pass_cnt = 0;
            }
            return 0;
        }
#ifdef PKG_XIAOZHI_USING_AEC
    if (thiz->vad_enabled) 
    {
        int ret = voice == 1;
        if(vad_enable) //如果开起了不打断功能 1是不打断
        { 
            if (XZ_DEVICE_STATE != kDeviceStateIdle) 
            {
                return 0; // 非待命状态不处理VAD
            }
        }
        if (VOICE_STATE_IDLE == thiz->voice_state)
        {
#if !ALLOW_VAD_WHEN_SPEAKING
            if ((ret == 1))
#else
            if ((ret == 1) && (XZ_DEVICE_STATE != kDeviceStateSpeaking))
#endif
            {
                LOG_I("idle --> wait speaking");
                thiz->voice_stop_times = 0;
                thiz->voice_state = VOICE_STATE_WAIT_SPEAKING;
                thiz->voice_start_times = 0;
            }
            //return 0;
        }
        else if (VOICE_STATE_WAIT_SPEAKING == thiz->voice_state)
        {
            if(vad_enable) //如果开起了不打断功能 1是不打断   
            {         
                if (XZ_DEVICE_STATE == kDeviceStateSpeaking)
                {
                    // xiaozhi is speaking, do not respond to mic input
                    LOG_I("speaking --> idle");
                    thiz->voice_start_times = 0;
                    thiz->voice_stop_times = 0;
                    thiz->voice_state = VOICE_STATE_IDLE;
                }
            }
            if (ret)
            {
                // voice
                thiz->voice_stop_times = 0;
                if (thiz->voice_start_times < VOICE_START_TIMES)
                {
                    thiz->voice_start_times++;
                    // LOG_I("wait enough voice times=%d",thiz->voice_start_times);
                    rt_ringbuffer_put(thiz->rb_vad_cache, p->data, p->data_len);
                }
                else if (thiz->voice_start_times == VOICE_START_TIMES)
                {
                        thiz->voice_start_times = 0;
                        thiz->voice_state = VOICE_STATE_SPEAKING;
                        LOG_I("call button pressed");
                        simulate_button_pressed();
                        uint8_t buf[320];
                        while (rt_ringbuffer_data_len(thiz->rb_vad_cache) >= 320)
                        {
                            rt_ringbuffer_get(thiz->rb_vad_cache, buf, 320);
                            rt_ringbuffer_put(thiz->rb_opus_encode_input, buf, 320);
                        }
                        rt_ringbuffer_put(thiz->rb_opus_encode_input, p->data, p->data_len);
                        rt_ringbuffer_reset(thiz->rb_vad_cache);
                }
            }
            else
            { // not voice
                LOG_I("wait speaking --> idle");
                thiz->voice_start_times = 0;
                thiz->voice_stop_times = 0;
                thiz->voice_state = VOICE_STATE_IDLE;
                rt_ringbuffer_reset(thiz->rb_vad_cache);
            }
            return 0;
        }
        else if (VOICE_STATE_SPEAKING == thiz->voice_state)
        {
            if (ret == 1)
            {
                LOG_I("speaking");
                thiz->voice_stop_times = 0;
            }
            else
            {
                LOG_I("not voice");
                if (thiz->voice_stop_times < VOICE_STOP_TIMES)
                {
                    thiz->voice_stop_times++;
                    LOG_I("wait no voice times=%d", thiz->voice_stop_times);
                }
                if (thiz->voice_stop_times == VOICE_STOP_TIMES)
                {
                    LOG_I("speaking --> idle");
                    thiz->voice_stop_times = 0;
                    thiz->voice_start_times = 0;
                    thiz->voice_state = VOICE_STATE_IDLE;
                    LOG_I("call button released");
                    simulate_button_released();
                    return 0;
                }
                
            }
        }
        else
        {
            RT_ASSERT(0);
        }
    }
#endif
        rt_ringbuffer_put(thiz->rb_opus_encode_input, p->data, p->data_len);
        thiz->mic_rx_count += 320;

        if (thiz->mic_rx_count >= XZ_MIC_FRAME_LEN)
        {
            thiz->mic_rx_count = 0;
            rt_event_send(thiz->event, XZ_EVENT_MIC_RX);
        }
    }
    return 0;
}

static int mic_callback(audio_server_callback_cmt_t cmd,
                        void *callback_userdata, uint32_t reserved)
{
    (void)callback_userdata;
    if (!reserved || (cmd != as_callback_cmd_data_coming &&
                     cmd != as_callback_cmd_mic_stereo_before_3a)) return 0;
    audio_server_coming_data_t *d = (audio_server_coming_data_t *)reserved;
    unsigned channels = xz_mic_channels(d->data_len);
    if (!d->data || !channels)
    {
        ++xz_mic_bad_blocks;
        rt_enter_critical();
        xz_barge_break_streak(&xz_barge_detector);
        xz_mic_pre.ready = 0;
        rt_exit_critical();
        if (xz_audio.inited) xz_audio_reset_uplink();
        if (cmd == as_callback_cmd_mic_stereo_before_3a) d->data_len = 0;
        xz_mic_log(); /* rate limited; an unknown format is dropped, never sped up */
        return 0;
    }
    if (cmd == as_callback_cmd_mic_stereo_before_3a)
    {
        xz_mic_downmix((const int16_t *)d->data, channels, true);
        d->data = (const uint8_t *)xz_mic_mono;
        d->data_len = XZ_MIC_MONO_BYTES;
        ++xz_mic_raw_blocks;
        return 0; /* raw capture must NOT reach KWS, VAD, encoder or barge-in */
    }

    g_mic_cb++;
    xz_music_rx_frames++;
    g_mic_bytes += d->data_len;
    ++xz_mic_post_blocks;
    xz_mic_mono_samples += XZ_MIC_SAMPLES_10MS;
    xz_mic_last_bytes = d->data_len;
    xz_mic_last_channels = channels;
    audio_server_coming_data_t mono = *d;
    if (channels == 2)
    {
        xz_mic_downmix((const int16_t *)d->data, channels, false);
        mono.data = (const uint8_t *)xz_mic_mono;
    }
    mono.data_len = XZ_MIC_MONO_BYTES;
    xz_mic_mono_frame(&mono);
    xz_mic_pre.ready = 0;
    xz_mic_log();
    return 0;
}

void xz_mic(int on)
{
    // Sample rate is 16K, each packet should be 60ms
    xz_audio_t *thiz = &xz_audio;
    if (on)
    {
        xz_mic_open(thiz);
    }
    else
    {
        xz_mic_close(thiz);
    }
}

void xz_speaker(int on)
{
    // Sample rate is g_xz_context.sample_rate(typical 24K),
    // each packet should be g_xz_context.frame_duration(typical 60ms)
    xz_audio_t *thiz = &xz_audio;
    if (on)
    {
        xz_speaker_open(thiz);
    }
    else
    {
        xz_speaker_close(thiz);
        /* Also release a music pause on abort, disconnect, or sleep. */
        xz_music_tts(false);
    }
}

#ifdef XIAOZHI_USING_MQTT
static void xz_button_event_handler(int32_t pin, button_action_t action)
{
    lv_display_trigger_activity(NULL);
    xiaozhi_ui_wake(); // 唤醒设备
    rt_kprintf("in mqtt button handle2\n");
    // 如果当前处于KWS模式，则退出KWS模式
    if (g_kws_running) 
    {  
        rt_kprintf("KWS exit\n");
        g_kws_force_exit = 1;
    }
    static button_action_t last_action = BUTTON_RELEASED;
    rt_kprintf("button(%d) %d:", pin, action);
    if (last_action == action)
    {
        return;
    }
    last_action = action;

        if (action == BUTTON_PRESSED)
        {
            
            lv_obj_t *now_screen = lv_screen_active();
            rt_kprintf("pressed\r\n");
            rt_kprintf("mqtt按键->对话\n");
            if (now_screen == standby_screen)
            {
                ui_switch_to_xiaozhi_screen();
            }
            if (mqtt_g_state == kDeviceStateUnknown) // goodby唤醒
            {
                mqtt_hello(&g_xz_context);
            }
            if (mqtt_g_state == kDeviceStateSpeaking)
            {
                mqtt_g_state = kDeviceStateListening;
            }

            xiaozhi_ui_chat_status("聆听中...");
        }
        else if (action == BUTTON_RELEASED)
        {
            rt_kprintf("released\r\n");
            xiaozhi_ui_chat_status("待命中...");
        }
    
}

void xz_mqtt_button_init(void)
{
    static int initialized = 0;

    if (initialized == 0)
    {
        button_cfg_t cfg;
        cfg.pin = BSP_KEY1_PIN;

        cfg.active_state = BSP_KEY1_ACTIVE_HIGH;
        cfg.mode = PIN_MODE_INPUT;
        cfg.button_handler = xz_button_event_handler;
        int32_t id = button_init(&cfg);
        RT_ASSERT(id >= 0);
        RT_ASSERT(SF_EOK == button_enable(id));
        initialized = 1;
    }
}

void xz_audio_init()
{
    rt_kprintf("xz_audio_init\n");
    rt_kprintf("exit sniff mode\n");
    // bt_interface_exit_sniff_mode(
    //     (unsigned char *)&g_bt_app_env.bd_addr); // exit sniff mode
    // bt_interface_wr_link_policy_setting(
    //     (unsigned char *)&g_bt_app_env.bd_addr,
    //     BT_NOTIFY_LINK_POLICY_ROLE_SWITCH); // close role switch
    if (udp_pcb)
    {
        udp_remove(udp_pcb);
        udp_pcb = NULL;
    }
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, VOL_DEFAULE_LEVEL);
    xz_audio_decoder_encoder_open(0);

    xz_mqtt_button_init();
    udp_pcb = udp_new();
    g_xz_context.local_sequence = 0;
    g_xz_context.remote_sequence = 0;
    udp_recv(udp_pcb, xz_udp_recv, NULL);
}
#endif
static void audio_write_and_wait(xz_audio_t *thiz, uint8_t *data,
                                 uint32_t data_len, uint32_t generation)
{
    int ret;
#if PKG_XIAOZHI_USING_AEC
    uint32_t bytes;
    bytes =
        sifli_resample_process(thiz->resample, (int16_t *)data, data_len, 0);
#endif
    int try_times = 0;
    while (!thiz->is_exit)
    {
        if (thiz->is_websocket && !xz_ws_playback_allowed()) break;
        rt_mutex_take(&xz_playback_mutex, RT_WAITING_FOREVER);
        if (!thiz->is_tx_enable || generation != xz_playback_generation)
        {
            rt_mutex_release(&xz_playback_mutex);
            break;
        }
#if PKG_XIAOZHI_USING_AEC
        ret = audio_write(thiz->speaker,
                          (uint8_t *)sifli_resample_get_output(thiz->resample),
                          bytes);
#else
        ret = audio_write(thiz->speaker, data, data_len);
#endif
        rt_mutex_release(&xz_playback_mutex);
        if (ret)
        {
            break;
        }
        rt_thread_mdelay(10);
        try_times++;
        if (try_times > 10)
        {
            LOG_I("speaker write failed len=%d\n", data_len);
            LOG_I("speaker busy, tx=%d\r\n", thiz->is_tx_enable);
            break;
        }
    }
}
static void xz_opus_thread_entry(void *p)
{
    int err;
    xz_audio_t *thiz = &xz_audio;
    thiz->decoder = opus_decoder_create(24000, 1, &err);
    RT_ASSERT(thiz->decoder);

    thiz->encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    RT_ASSERT(thiz->encoder);
    opus_encoder_ctl(thiz->encoder,
                     OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_60_MS));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_VBR_CONSTRAINT(1));

    opus_encoder_ctl(thiz->encoder, OPUS_SET_BITRATE(8000));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_LSB_DEPTH(16));

    opus_encoder_ctl(thiz->encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_PACKET_LOSS_PERC(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_PREDICTION_DISABLED(0));

    opus_encoder_ctl(thiz->encoder,
                     OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_BANDWIDTH(OPUS_AUTO));
    
    opus_encoder_ctl(thiz->encoder, OPUS_SET_FORCE_MODE(MODE_SILK_ONLY));
#if XZ_TX_STRESS
    rt_tick_t last_stress = rt_tick_get();
    const rt_tick_t stress_period = rt_tick_from_millisecond(60);
    size_t stress_sample = 0;
#endif
    while (!thiz->is_exit)
    {
        rt_uint32_t evt = 0;
#if XZ_TX_STRESS
        rt_tick_t stress_wait = stress_period;
        if (g_xz_ws.is_connected && thiz->is_rx_enable)
        {
            rt_tick_t elapsed = rt_tick_get() - last_stress;
            stress_wait = elapsed < stress_period ? stress_period - elapsed : 0;
        }
#endif
        rt_event_recv(thiz->event, XZ_EVENT_ALL,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
#if XZ_TX_STRESS
                      stress_wait, &evt);
#else
                      RT_WAITING_FOREVER, &evt);
#endif
        if (evt & XZ_EVENT_EXIT)
        {
            break;
        }
        if (evt & XZ_EVENT_WAKE_BEEP)
        {
            xz_audio_wake_beep();
            evt &= ~XZ_EVENT_MIC_RX;
        }
#if XZ_TX_STRESS
        /* Feed one voiced 60 ms frame at real-time speed, without a speaker
         * or a person. Silence-only input makes the server close after 60 s. */
        if (thiz->is_rx_enable &&
            rt_ringbuffer_data_len(thiz->rb_opus_encode_input) < XZ_MIC_FRAME_LEN)
        {
            if (g_xz_ws.is_connected &&
                rt_tick_get() - last_stress >= stress_period)
            {
                last_stress = rt_tick_get();
                rt_ringbuffer_put(thiz->rb_opus_encode_input,
                                  (const uint8_t *)&xz_stress_pcm[stress_sample],
                                  XZ_MIC_FRAME_LEN);
                stress_sample += XZ_MIC_FRAME_LEN / sizeof(int16_t);
                if (stress_sample >= sizeof(xz_stress_pcm) / sizeof(int16_t))
                    stress_sample = 0;
                evt |= XZ_EVENT_MIC_RX;
            }
        }
#endif
        if ((evt & XZ_EVENT_MIC_RX) && thiz->is_rx_enable &&
            (!thiz->is_websocket || xz_ws_uplink_allowed()))
        {
            /* 麦克风回调每 10 ms 就发一次事件，但一帧是 60 ms 数据。
             * 不检查可用长度就编码，会让上行跑在 6 倍实时速率，
             * 瞬间打满 Wi-Fi 发送队列（altcp_write 失败 -> websocket 被关）。 */
            rt_enter_critical();
            uint32_t generation = xz_ws_audio_epoch();
            if (rt_ringbuffer_data_len(thiz->rb_opus_encode_input) <
                XZ_MIC_FRAME_LEN)
            {
                rt_exit_critical();
                continue;
            }
            rt_ringbuffer_get(thiz->rb_opus_encode_input,
                              (uint8_t *)&thiz->encode_in[0], XZ_MIC_FRAME_LEN);
            rt_exit_critical();

            opus_int32 len = opus_encode(
                thiz->encoder, (const opus_int16 *)thiz->encode_in,
                XZ_MIC_FRAME_LEN / 2, thiz->encode_out, XZ_MIC_FRAME_LEN);
            if (len < 0 || len > XZ_MIC_FRAME_LEN)
            {
                LOG_I("opus_encode len=%d samrate=%d", len,
                      g_xz_context.sample_rate);
                RT_ASSERT(0);
            }
#ifdef XIAOZHI_USING_MQTT
            xz_audio_send(thiz->encode_out, len);
#else
            rt_enter_critical();
            if (generation == xz_ws_audio_epoch())
                xz_audio_send_using_websocket(thiz->encode_out, len);
            rt_exit_critical();
#endif
            if (rt_ringbuffer_data_len(thiz->rb_opus_encode_input) >=
                XZ_MIC_FRAME_LEN)
            {
                rt_event_send(thiz->event, XZ_EVENT_MIC_RX);
            }
        }

        if (evt & XZ_EVENT_SPK_TX)
        {
        }

        if ((evt & XZ_EVENT_DOWNLINK) && thiz->is_tx_enable)
        {
            rt_slist_t *decode;
            rt_enter_critical();
            decode = rt_slist_first(&thiz->downlink_decode_busy);
            if (decode) rt_slist_remove(&thiz->downlink_decode_busy, decode);
            xz_decode_inflight = decode != NULL;
            uint32_t playback_generation = xz_playback_generation;
            rt_exit_critical();
            if (!decode) continue; /* an abort may have drained the event's queue */
            xz_decode_queue_t *queue =
                rt_container_of(decode, xz_decode_queue_t, node);
            opus_int32 res = opus_decode(
                thiz->decoder, (const uint8_t *)queue->data, queue->data_len,
                (opus_int16 *)&thiz->downlink_decode_out[0], XZ_SPK_FRAME_LEN,
                0);

            if (res != XZ_SPK_FRAME_LEN / 2)
            {
                LOG_I("decode out samples=%d\n", res);
                //RT_ASSERT(0);
            }
            if(res > 0)
            {
                audio_write_and_wait(thiz, (uint8_t *)thiz->downlink_decode_out,
                                 res * 2, playback_generation);
            }

            uint8_t need_decode_gain = 0;
            rt_enter_critical();
            rt_slist_append(&thiz->downlink_decode_idle, decode);
            xz_decode_inflight = false;
            if (rt_slist_first(&thiz->downlink_decode_busy))
            {
                need_decode_gain = true;
            }
            rt_exit_critical();

            if (need_decode_gain)
                rt_event_send(thiz->event, XZ_EVENT_DOWNLINK);
        }
    }
    if (thiz->encoder)
        opus_encoder_destroy(thiz->encoder);
    if (thiz->decoder)
        opus_decoder_destroy(thiz->decoder);

    rt_kprintf("---xz thread exit---\r\n");
}

void xz_aec_mic_open(xz_audio_t *thiz)
{
    if (!thiz->mic)
    {
        LOG_I("mic on,thiz->inited=%d", thiz->inited);
        // if (thiz->inited != 2)
        // {
        //     return;
        // }
        while (1)
        {
            uint8_t buf[128];
            rt_base_t level = rt_hw_interrupt_disable();  // 添加临界区保护
            int len = rt_ringbuffer_get(thiz->rb_opus_encode_input,
                                        (uint8_t *)&buf[0], sizeof(buf));
            rt_hw_interrupt_enable(level);  // 恢复中断
            if (len == 0)
            {
                break;
            }
        }
        audio_parameter_t pa = {0};
        pa.write_bits_per_sample = 16;
        pa.write_channnel_num = 1;
        pa.write_samplerate = 16000;
        pa.read_bits_per_sample = 16;
        pa.read_channnel_num = 2;
        pa.read_which_mic = AUDIO_MIC_ALL;
        pa.read_samplerate = 16000;
        pa.read_cache_size = 0;
        pa.write_cache_size = 32000;
        pa.is_need_3a = 1;
        pa.disable_uplink_agc = 1;
        xz_audio_apply_pdm_gain();
        thiz->mic = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TXRX, &pa,
                               mic_callback, NULL);
        RT_ASSERT(thiz->mic);
        xz_audio_apply_pdm_gain();
        xz_music_rx_opened(&pa);
        thiz->speaker = thiz->mic;
        thiz->is_rx_enable = 1;   //麦克风常开
        thiz->is_tx_enable = 1;

    }
}
void xz_mic_open(xz_audio_t *thiz)
{
#if !PKG_XIAOZHI_USING_AEC
    if (!thiz->mic)
    {
        LOG_I("mic on");
        while (1)
        {
            uint8_t buf[128];
            int len = rt_ringbuffer_get(thiz->rb_opus_encode_input,
                                        (uint8_t *)&buf[0], sizeof(buf));
            if (len == 0)
            {
                break;
            }
        }

        audio_parameter_t pa = {0};
        pa.write_bits_per_sample = 16;
        pa.write_channnel_num = 1;
        pa.write_samplerate = 24000;
        pa.read_bits_per_sample = 16;
        pa.read_channnel_num = 1;
        pa.read_samplerate = 16000;
        pa.read_cache_size = 0;
        pa.write_cache_size = 0;
        xz_audio_apply_pdm_gain();
        thiz->mic = audio_open(AUDIO_TYPE_LOCAL_RECORD, AUDIO_RX, &pa,
                               mic_callback, NULL);
        RT_ASSERT(thiz->mic);
        xz_audio_apply_pdm_gain();
        thiz->is_rx_enable = 1;
    }
#endif
}

void xz_aec_mic_close(xz_audio_t *thiz)
{
    if (thiz->mic)
    {
        LOG_I("mic off");
        xz_music_rx_closing();
        audio_close(thiz->mic);
        thiz->mic = NULL;
        thiz->is_rx_enable = 0;
    }
}
void xz_mic_close(xz_audio_t *thiz)
{
#if !PKG_XIAOZHI_USING_AEC
    if (thiz->mic)
    {
        LOG_I("mic off");
        audio_close(thiz->mic);
        thiz->mic = NULL;
        thiz->is_rx_enable = 0;
    }
#endif
}

void xz_speaker_open(xz_audio_t *thiz)
{
#if PKG_XIAOZHI_USING_AEC
        LOG_I("speaker on");
        xiaozhi_ui_chat_status("\u8bb2\u8bdd\u4e2d...");
        thiz->is_tx_enable = 1;
#else
    if (!thiz->speaker)
    {
        LOG_I("speaker on,thiz->inited=%d", thiz->inited);
        // if (thiz->inited != 2)
        // {
        //     return;
        // }
        xiaozhi_ui_chat_status("\u8bb2\u8bdd\u4e2d...");
        audio_parameter_t pa = {0};
        pa.write_bits_per_sample = 16;
        pa.write_channnel_num = 1;
        pa.write_samplerate = 24000;
        pa.read_bits_per_sample = 16;
        pa.read_channnel_num = 1;
        pa.read_samplerate = 16000;
        pa.read_cache_size = 0;
        pa.write_cache_size = 32000;
        thiz->speaker =
            audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &pa, NULL, NULL);
        RT_ASSERT(thiz->speaker);
        thiz->is_tx_enable = 1;
    }
#endif
}
void xz_speaker_close(xz_audio_t *thiz)
{
#if PKG_XIAOZHI_USING_AEC
    if (!thiz->inited) return;
    LOG_I("speaker off");
    xiaozhi_ui_chat_status("\u5f85\u547d\u4e2d...");
        rt_slist_t *idle;
        rt_mutex_take(&xz_playback_mutex, RT_WAITING_FOREVER);
        rt_enter_critical();
        thiz->is_tx_enable = 0;
        ++xz_playback_generation;
        while (1)
        {
            idle = rt_slist_first(&thiz->downlink_decode_busy);
            if (idle)
            {
                rt_slist_remove(&thiz->downlink_decode_busy, idle);
                rt_slist_append(&thiz->downlink_decode_idle, idle);
            }
            else
            {
                break;
            }
        }
        rt_exit_critical();
        if (thiz->speaker) audio_ioctl(thiz->speaker, AUDIO_IOCTL_DROP_TX_CACHE, NULL);
        rt_mutex_release(&xz_playback_mutex);
#else

    if (thiz->speaker)
    {
        for (int i = 0; i < 1000; i++)
        {
            if (!rt_slist_first(&thiz->downlink_decode_busy))
            {
                break;
            }
            rt_thread_mdelay(10);
        }
        uint32_t cache_time_ms = 150;
        audio_ioctl(thiz->speaker, 1, &cache_time_ms);
        rt_thread_mdelay(cache_time_ms + 20);
        audio_close(thiz->speaker);
        thiz->speaker = NULL;
        thiz->is_tx_enable = 0;
        rt_slist_t *idle;
        rt_enter_critical();
        while (1)
        {
            idle = rt_slist_first(&thiz->downlink_decode_busy);
            if (idle)
            {
                rt_slist_remove(&thiz->downlink_decode_busy, idle);
                rt_slist_append(&thiz->downlink_decode_idle, idle);
            }
            else
            {
                break;
            }
        }
        rt_exit_critical();
    }
#endif
}

bool xz_audio_tts_drained(void)
{
    if (!xz_audio.inited) return true;
    rt_enter_critical();
    bool empty = !xz_decode_inflight && !rt_slist_first(&xz_audio.downlink_decode_busy);
    rt_exit_critical();
    if (!empty || !xz_audio.speaker) return empty;
    uint32_t remaining = 0;
    return audio_ioctl(xz_audio.speaker, AUDIO_IOCTL_BYTES_IN_CACHE, &remaining) == 0 &&
           remaining == 0;
}

/**
 * @brief 打开音频解码器和编码器
 *
 * 该函数负责初始化音频处理模块，包括事件创建、队列初始化、线程创建等
 * 如果模块尚未初始化，则根据参数决定是否使用WebSocket，并准备音频处理线程
 *
 * @param is_websocket 指示是否使用WebSocket的标志
 */
void xz_audio_decoder_encoder_open(uint8_t is_websocket)
{
    // 获取音频处理模块的实例
    xz_audio_t *thiz = &xz_audio;

    LOG_I("%s", XZ_AUDIO_VERSION);
    if (!xz_audio_boot_initialized)
    {
        memset(thiz, 0, sizeof(*thiz));
        rt_mutex_init(&xz_playback_mutex, "xz_play", RT_IPC_FLAG_PRIO);
        xz_audio_boot_initialized = true;
    }
    // 检查模块是否已经初始化，避免重复初始化
    if (!thiz->inited)
    {
        memset(thiz, 0, sizeof(xz_audio_t));
        thiz->is_websocket = is_websocket; /* gate callbacks before audio_open */
        //thiz->inited = 1; 
#if PKG_XIAOZHI_USING_AEC
        int ret;
        thiz->vad_enabled = true;
        audio_parameter_t pa = {0};
        pa.write_bits_per_sample = 16;
        pa.write_channnel_num = 1;
        pa.write_samplerate = 16000;
        pa.read_bits_per_sample = 16;
        pa.read_channnel_num = 2;
        pa.read_which_mic = AUDIO_MIC_ALL;
        pa.read_samplerate = 16000;
        pa.read_cache_size = 0;
        pa.write_cache_size = 32000;
        pa.is_need_3a = 1;
        pa.disable_uplink_agc = 1;
        xz_audio_apply_pdm_gain();
        thiz->mic = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TXRX, &pa,
                               mic_callback, NULL);
        RT_ASSERT(thiz->mic);
        xz_audio_apply_pdm_gain();
        xz_music_rx_opened(&pa);

        /* Analog ADC gain defaults to +12 dB. At +8 dB the on-board mic picks
         * up a TV across the room well enough for the server VAD to answer it,
         * so trim 10 dB (+2 dB effective). Live-tune with the shell command
         * `mic_gain <-60..30>`; `mic_gain` alone prints the current value. */
        mic_gain_decrease(XZ_MIC_GAIN_TRIM_DB);

        thiz->speaker = thiz->mic;
        thiz->is_rx_enable = 1;   //麦克风常开
        thiz->is_tx_enable = 1;
        thiz->resample = sifli_resample_open(1, 24000, 16000);
        RT_ASSERT(thiz->resample);
        ret = WebRtcVad_Create(&thiz->handle);
        RT_ASSERT(!ret);
        ret = WebRtcVad_Init(thiz->handle);
        RT_ASSERT(!ret);
        ret = WebRtcVad_set_mode(thiz->handle, 3); // 0 ~ 3
        RT_ASSERT(!ret);

#endif
        // 根据参数设置是否使用WebSocket
        thiz->is_websocket = is_websocket;

        // 创建事件，用于音频处理中的同步
        thiz->event = rt_event_create("xzaudio", RT_IPC_FLAG_FIFO);
        RT_ASSERT(thiz->event);

        // 初始化空闲和忙状态的解码队列
        rt_slist_init(&thiz->downlink_decode_idle);
        rt_slist_init(&thiz->downlink_decode_busy);

        // 为下行链路解码队列分配内存
        for (int i = 0; i < XZ_DOWNLINK_QUEUE_NUM; i++)
        {
            thiz->downlink_queue[i].size = 256;
            thiz->downlink_queue[i].data =
                opus_heap_malloc(thiz->downlink_queue[i].size);
            RT_ASSERT(thiz->downlink_queue[i].data);

            // 将新分配的队列节点添加到空闲队列中
            rt_slist_append(&thiz->downlink_decode_idle,
                            &thiz->downlink_queue[i].node);
        }

#if PKG_XIAOZHI_USING_AEC
        rt_ringbuffer_init(&xz_vad_rb, xz_vad_rb_pool, sizeof(xz_vad_rb_pool));
        thiz->rb_vad_cache = &xz_vad_rb;
#endif
        rt_ringbuffer_init(&xz_encode_rb, xz_encode_rb_pool,
                           sizeof(xz_encode_rb_pool));
        thiz->rb_opus_encode_input = &xz_encode_rb;
        // 初始化音频处理线程
        rt_err_t err;
        err = rt_thread_init(&thiz->thread, XZ_THREAD_NAME,
                             xz_opus_thread_entry, // 音频处理线程入口函数
                             NULL, g_xz_opus_stack, sizeof(g_xz_opus_stack),
                             RT_THREAD_PRIORITY_MIDDLE +
                                 RT_THREAD_PRIORITY_HIGHER,
                             RT_THREAD_TICK_DEFAULT);

        // 启动音频处理线程
        rt_thread_startup(&thiz->thread);

        // 标记模块已初始化
        thiz->inited = 1;

        /* 关键词唤醒(“小智小智”):复用上面的麦克风数据流 */
        kws_start_shared();

        rt_kprintf("xz_audio_decoder_encoder open ok\n");
    }
        rt_kprintf("xz_audio_decoder_encoder open2 ok\n");
}

void xz_audio_decoder_encoder_close(void)
{
    xz_audio_t *thiz = &xz_audio;

    LOG_I("xz_audio_decoder_encoder close in  %d", thiz->inited);
    
    thiz->is_exit = 1;
    rt_event_send(thiz->event, XZ_EVENT_EXIT);
    while (rt_thread_find(XZ_THREAD_NAME))
    {
        LOG_I("wait thread %s exit", XZ_THREAD_NAME);
        os_delay(100);
    }

    rt_ringbuffer_reset(thiz->rb_opus_encode_input);
    rt_event_delete(thiz->event);

    for (int i = 0; i < XZ_DOWNLINK_QUEUE_NUM; i++)
    {
        if (thiz->downlink_queue[i].data)
        {
            opus_heap_free(thiz->downlink_queue[i].data);
            thiz->downlink_queue[i].data = NULL;
        }
    }
#if PKG_XIAOZHI_USING_AEC
    rt_ringbuffer_reset(thiz->rb_vad_cache);
    sifli_resample_close(thiz->resample);
    xz_music_rx_closing();
    audio_close(thiz->mic);
    thiz->mic = NULL;
    thiz->speaker = NULL;
    thiz->rb_vad_cache = NULL;
    thiz->resample = NULL;
    if (thiz->handle)
    {
        WebRtcVad_Free(thiz->handle);
        thiz->handle = NULL;
    }
#else
    if (thiz->mic)
    {
        audio_close(thiz->mic);
        thiz->mic = NULL;
    }

    if (thiz->speaker)
    {
        audio_close(thiz->speaker);
        thiz->speaker = NULL;
    }
#endif
    thiz->inited = 0;

    rt_kprintf("xz_audio_decoder_encoder close out ok\n");
}

void reinit_audio()
{
    xz_audio_t *thiz = &xz_audio;
    xz_speaker_close(thiz);
    xz_aec_mic_close(thiz);
    thiz->speaker = NULL; /* TXRX alias was closed with mic */
    xz_audio_decoder_encoder_close();
    // 重新打开音频解码器和编码器
    xz_audio_decoder_encoder_open(1); 
    xz_aec_mic_open(thiz);
    xz_speaker_open(thiz);
}

void xz_audio_downlink(uint8_t *data, uint32_t size, uint32_t *aes_value,
                       uint8_t need_aes)
{
    int try_times = 0;
    g_xz_rx_tick = rt_tick_get();
    xz_audio_t *thiz = &xz_audio;
    rt_slist_t *idle;
    if (!thiz->inited)
    {
        LOG_I("%s invalid\r\n", __FUNCTION__);
        return;
    }
    // LOG_I("%s tx=%d inited=%d\r\n", __FUNCTION__, thiz->is_tx_enable,
    // thiz->inited);
    rt_enter_critical();
    idle = rt_slist_first(&thiz->downlink_decode_idle);
    rt_exit_critical();
    if (idle)
    {
        xz_decode_queue_t *queue =
            rt_container_of(idle, xz_decode_queue_t, node);
        if (queue->size < size + 16)
        {
            opus_heap_free(queue->data);
            queue->size = size + 16;
            queue->data = opus_heap_malloc(queue->size);
            RT_ASSERT(queue->data);
        }
        queue->data_len = size;
        if (need_aes)
        {
            HAL_AES_init((uint32_t *)&(g_xz_context.key[0]), 16, aes_value,
                         AES_MODE_CTR);
            HAL_AES_run(AES_DEC, data, queue->data, size);
        }
        else
        {
            memcpy(queue->data, data, size);
        }
        rt_enter_critical();
        rt_slist_remove(&thiz->downlink_decode_idle, idle);
        rt_slist_append(&thiz->downlink_decode_busy, idle);
        rt_exit_critical();

        rt_event_send(thiz->event, XZ_EVENT_DOWNLINK);
    }
    else
    {
        /* 这个函数跑在 websocket 接收回调里，绝不能阻塞：一阻塞就停收，
         * 服务端窗口填满后 altcp_write() 失败，整个 websocket 被关掉。
         * 队列满时直接丢这一帧，只统计次数。 */
        static uint32_t drop_cnt = 0;
        (void)try_times;
        if ((++drop_cnt % 50) == 1)
            rt_kprintf("downlink queue full, dropped=%u\n", drop_cnt);
    }
}

/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/
