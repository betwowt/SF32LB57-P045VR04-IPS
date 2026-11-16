#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define AUDIO_RX_USING_PDM 1
#include "xiaozhi_mic_dsp.h"
#include "xiaozhi_barge_ref.h"

#define PKG_XIAOZHI_USING_AEC 1
#define RT_TICK_PER_SECOND 1000
#include "config-under-test.h"
#define XZ_MIC_FRAME_LEN 1920
#define XZ_EVENT_MIC_RX 1
#define XZ_TX_STRESS 0
#define VOICE_STATE_IDLE 0
#define VOICE_STATE_WAIT_SPEAKING 1
#define VOICE_STATE_SPEAKING 2
#define VOICE_START_TIMES 12
#define VOICE_STOP_TIMES 30
#define LOG_I(...) ((void)0)
#define RT_ASSERT assert
#define XZ_DEVICE_STATE web_g_state

typedef uint32_t rt_tick_t;
typedef enum {as_callback_cmd_data_coming = 7,
              as_callback_cmd_mic_stereo_before_3a = 102} audio_server_callback_cmt_t;
typedef struct {const uint8_t *data; uint32_t data_len, reserved;} audio_server_coming_data_t;
enum {kDeviceStateIdle, kDeviceStateListening, kDeviceStateThinking, kDeviceStateSpeaking};
static int web_g_state, vad_enable;
struct rb {uint8_t data[4096]; unsigned len;};
typedef struct {
    bool inited, is_rx_enable, is_websocket, vad_enabled;
    int handle, event, voice_state, voice_start_times, voice_stop_times;
    struct rb *rb_opus_encode_input, *rb_vad_cache;
    unsigned mic_rx_count;
} xz_audio_t;
static xz_audio_t xz_audio;
static struct {int is_connected;} g_xz_ws;
static bool xz_wake_beep_busy;
static uint8_t Initiate_disconnection_flag;
static int16_t xz_mic_mono[160], kws_pcm[160];
static int16_t xz_mic_uplink[160];
static int16_t vad_pcm[160];
static xz_barge_detector_t xz_barge_detector;
static struct { uint16_t rms, peak[2]; uint8_t ready; } xz_mic_pre;
static xz_barge_ref_t xz_mic_ref;
static uint32_t xz_mic_raw_peak[2], xz_mic_peak_frames;
static uint32_t xz_mic_raw_blocks, xz_mic_post_blocks, xz_mic_mono_samples;
static uint64_t xz_mic_post_square;
static uint32_t xz_mic_post_peak;
static uint32_t xz_mic_bad_blocks, xz_mic_last_bytes, xz_mic_last_channels;
static rt_tick_t xz_mic_log_tick;
#if CONFIG_XZ_MIC_LEVEL_LOG
static struct {uint64_t square[2]; uint32_t peak[2], samples;} xz_mic_levels;
#endif
static uint32_t g_mic_cb, g_mic_bytes, g_mic_peak, g_mic_abs, g_mic_nz, g_mic_n;
static unsigned xz_music_rx_frames;
static int xz_rec_on, xz_rec_idx, xz_rec_n, xz_mic_hang;
static uint16_t xz_rec_peak[120];
static rt_tick_t now, tts_start;
static uint32_t generation = 1;
static bool session_active = true;
static bool tts_stopped;
static int mock_voice = 1;
static unsigned kws_calls, vad_calls, events, requests, level_logs, pcm_logs;
static unsigned curve_logs, armed_logs;
static unsigned clip_logs, peak_logs;
static unsigned makeup_logs;
static rt_tick_t last_clip_tick;
static char last_peak[256], last_clip[256];
static uint32_t curve_gen, curve_t;
static char last_curve[256], last_armed[256];
static uint32_t request_rms, request_floor, request_pre, request_frames;
static struct rb encode_rb, vad_rb;

rt_tick_t rt_tick_get(void) {return now;}
rt_tick_t rt_tick_from_millisecond(unsigned ms) {return ms;}
void rt_enter_critical(void) {}
void rt_exit_critical(void) {}
static int rt_event_send(int event, unsigned bits) {events++; return 0;}
static void rt_ringbuffer_reset(struct rb *rb) {rb->len = 0;}
static unsigned rt_ringbuffer_data_len(struct rb *rb) {return rb->len;}
static unsigned rt_ringbuffer_put(struct rb *rb, const uint8_t *data, unsigned len) {
    assert(rb->len + len <= sizeof(rb->data));
    memcpy(rb->data + rb->len, data, len); rb->len += len; return len;
}
static void rt_ringbuffer_get(struct rb *rb, uint8_t *data, unsigned len) {
    assert(rb->len >= len); memcpy(data, rb->data, len);
    rb->len -= len; memmove(rb->data, rb->data + len, rb->len);
}
static void xz_audio_reset_uplink(void) {
    encode_rb.len = 0; xz_audio.mic_rx_count = 0; xz_mic_hang = 0;
}
static bool xz_ws_uplink_allowed(void) {
    return session_active && (web_g_state == kDeviceStateListening || web_g_state == kDeviceStateSpeaking);
}
static void kws_feed_mic(const int16_t *p, uint32_t n) {
    assert(n == 160); memcpy(kws_pcm, p, 320); kws_calls++;
}
static int WebRtcVad_Process(int handle, int rate, const int16_t *p, unsigned n) {
    assert(rate == 16000 && n == 160);
    memcpy(vad_pcm, p, 320); vad_calls++; return mock_voice;
}
static bool xz_ws_barge_in_context(uint32_t *gen, rt_tick_t *start) {
    *gen = generation; *start = tts_start;
    return session_active && web_g_state == kDeviceStateSpeaking && !tts_stopped;
}
static bool xz_ws_request_speech_barge_in(const xz_barge_detector_t *d) {
    assert(d->generation == generation && d->dtd);
    requests++; request_rms = d->rms; request_floor = d->echo_floor;
    request_pre = d->pre_rms; request_frames = d->frames; return true;
}
static void simulate_button_pressed(void) {assert(!"Legacy wake must stay gated");}
static void simulate_button_released(void) {assert(!"Legacy stop must stay gated");}
static int rt_kprintf(const char *format, ...) {
    char line[256];
    va_list args; va_start(args, format);
    vsnprintf(line, sizeof(line), format, args); va_end(args);
    if (strstr(format, "MIC_MKUP")) {
        assert(strcmp(line, "MIC_MKUP uw=1 kws=1 vad=1\n") == 0);
        makeup_logs++;
        assert(makeup_logs == 1);
    }
    if (strstr(format, "MIC_PEAK")) {peak_logs++; strcpy(last_peak, line);}
    if (strstr(format, "BARGE_CLIP")) {
        if (clip_logs) assert((uint32_t)(now - last_clip_tick) >= 2000);
        last_clip_tick = now; clip_logs++; strcpy(last_clip, line);
    }
    if (strstr(format, "MIC_L rms=")) level_logs++;
    if (strstr(format, "MIC_PCM")) pcm_logs++;
    if (strstr(format, "BARGE_CURVE") || strstr(format, "phase=armed")) {
        if (strstr(format, "BARGE_CURVE")) {
            unsigned gen, t;
            assert(sscanf(line, "BARGE_CURVE gen=%u t=%ums", &gen, &t) == 2);
            assert(t < 3000 && t == (uint32_t)(now - tts_start));
            if (curve_logs && curve_gen == gen) assert(t - curve_t >= 100);
            curve_gen = gen; curve_t = t; curve_logs++;
            strcpy(last_curve, line);
        } else {
            armed_logs++;
            strcpy(last_armed, line);
        }
    }
    return 0;
}

static unsigned ref_calls;
void __real_audio_3a_far_put(uint8_t *fifo, uint16_t bytes) {ref_calls++;}
#include "xiaozhi_barge_ref.c"
#include "audio-under-test.inc"

#define XZ_PDM1_STEREO 1
#define AUDIO_RX_USING_PDM 1
#define CODEC_DATA_UNIT_LEN 320
#define PDM_NOISE_DROP_FRAMES 2
#define AUDIO_MIC_ALL 2
static unsigned mock_aec_calls;
typedef struct {
    uint8_t rx_data_tmp[640];
    bool is_need_3a, pdm1_dev, pdm2_dev;
    unsigned rx_channels, rx_drop_cnt;
} audio_device_speaker_t;
typedef struct {bool public_is_rx_mute, is_bt_3a;} audio_server_t;
struct mock_client {
    struct {unsigned read_which_mic;} parameter;
    int (*callback)(audio_server_callback_cmt_t, void *, uintptr_t);
    void *user_data;
};
static void audio_3a_uplink(uint8_t *data, unsigned bytes, bool mute, bool bt) {
    assert(bytes == 320); mock_aec_calls++;
    /* Deterministic stand-in for AEC: distinguish pre/post feeds. */
    for (unsigned i = 0; i < 160; ++i) ((int16_t *)data)[i] /= 2;
}
static void sdk_rx(audio_server_t *server, audio_device_speaker_t *my, struct mock_client *client) {
    unsigned len = 640;
#include "sdk-stereo-under-test.inc"
Exit: return;
}

static void capture(int16_t *p, unsigned bytes, bool raw) {
    audio_server_coming_data_t d = {(uint8_t *)p, bytes, 0};
    mic_callback(raw ? as_callback_cmd_mic_stereo_before_3a : as_callback_cmd_data_coming,
                 NULL, (uintptr_t)&d);
    if (raw && xz_mic_channels(bytes)) {
        assert(d.data_len == 320 && d.data == (uint8_t *)xz_mic_mono);
    }
}
static int16_t ref_level = 1000;
static bool feed_reference = true;
static void reference(int16_t amplitude) {
    int16_t pcm[160];
    for (unsigned i = 0; i < 160; ++i) pcm[i] = (i & 1) ? -amplitude : amplitude;
    unsigned before = ref_calls;
    __wrap_audio_3a_far_put((uint8_t *)pcm, sizeof(pcm));
    assert(ref_calls == before + 1 && pcm[1] == (int16_t)-amplitude);
}
static void paired_constant(int16_t amplitude, int16_t pre, bool paired) {
    int16_t mono[160], stereo[320];
    for (unsigned i = 0; i < 160; ++i) {
        stereo[2*i] = pre; stereo[2*i+1] = -pre;
    }
    if (feed_reference) reference(ref_level);
    if (paired) capture(stereo, 640, true);
    for (unsigned i = 0; i < 160; ++i) mono[i] = (i & 1) ? -amplitude : amplitude;
    capture(mono, 320, false);
    xz_audio_reset_uplink();
    now += 10;
}
static void post_constant(int16_t amplitude) {paired_constant(amplitude, 4000, true);}
/* A single clipped raw sample has low RMS and can disappear in the average
 * mix. Safety must inspect both raw peaks, independently of mix/log mode. */
static void paired_spike(int16_t post, int16_t peak, unsigned channel) {
    int16_t stereo[320], mono[160];
    for (unsigned i = 0; i < 160; ++i) {
        stereo[2*i] = 4000; stereo[2*i+1] = -4000;
        mono[i] = (i & 1) ? -post : post;
    }
    stereo[channel] = peak;
    if (feed_reference) reference(ref_level);
    capture(stereo, 640, true);
    assert(xz_mic_pre.peak[channel] == (peak < 0 ? -(int32_t)peak : peak));
    capture(mono, 320, false);
    xz_audio_reset_uplink(); now += 10;
}
static void train(int16_t amplitude) {
    for (unsigned i = 0; i < (XZ_BARGE_ARM_MS + 9) / 10; ++i) {
        assert(!xz_audio_barge_in_armed(generation));
        post_constant(amplitude);
    }
    assert(requests == 0 && xz_audio_barge_in_armed(generation));
    assert(xz_barge_detector.echo_floor == amplitude);
}
static void new_turn(void) {
    generation++; tts_start = now; requests = 0;
    xz_audio_reset_barge_in();
    tts_stopped = false;
    web_g_state = kDeviceStateSpeaking;
    ref_level = 1000; feed_reference = true;
}

static void test_ref_window(void) {
    xz_barge_detector_t d;
    xz_barge_reset(&d);
    d.generation = 1; d.phase = XZ_BARGE_ARMED;
    d.echo_gain_milli = 302; d.echo_floor = 9;
#define REF_STEP(level, valid, elapsed) \
    xz_barge_step(&d, 1, 1, 354, 449, 1, 0, level, valid, \
                  0, 150, 4, 3000, 6, 250, 250, 120, elapsed)
    REF_STEP(49, 1, 10);
    assert(!d.ref_hold && d.ref_age_ms == 300 && d.dtd);
    REF_STEP(1000, 1, 10);
    REF_STEP(59, 1, 10);
    REF_STEP(2, 1, 231); /* fix9 board failure: last block=59, age=231 ms */
    assert(d.ref_hold == 1000 && d.ref_age_ms == 231 && !d.dtd);
    assert(!d.frames && !d.fired && d.echo_gain_milli == 302 && d.echo_floor == 9);
    REF_STEP(32768, 0, 58); /* invalid reference cannot refresh the maximum */
    assert(d.ref_hold == 1000 && d.ref_age_ms == 289 && !d.dtd);
    REF_STEP(2, 1, 1); /* loud block expires independently of last-block age */
    assert(d.ref_hold == 59 && d.ref_age_ms == 290 && d.dtd);
    REF_STEP(2, 1, 9);
    assert(d.ref_hold == 59 && d.ref_age_ms == 299);
    REF_STEP(2, 1, 1);
    assert(!d.ref_hold && d.ref_age_ms == 300 && d.dtd);
    REF_STEP(0, 1, 1000);
    assert(!d.ref_hold && d.ref_age_ms == 1300);
    REF_STEP(0, 1, UINT32_MAX);
    REF_STEP(0, 1, 10);
    assert(!d.ref_hold && d.ref_age_ms == UINT32_MAX);
    REF_STEP(50, 1, 10);
    assert(d.ref_hold == 50 && !d.ref_age_ms);

    /* Compare six full ring rotations to an independent 300 ms scan. */
    uint16_t levels[180] = {0};
    xz_barge_reset(&d);
    for (unsigned i = 0; i < 180; ++i) {
        uint32_t ref = i % 37 == 0 ? 32768 :
                       i % 7 == 0 ? 49 : i % 5 == 0 ? 50 : (i * 7919u) % 16000u;
        int valid = i % 11 != 0;
        levels[i] = valid && ref >= XZ_BARGE_REF_MIN_RMS ? ref : 0;
        REF_STEP(ref, valid, 10);
        uint16_t expected = 0;
        unsigned start = i >= 29 ? i - 29 : 0;
        for (unsigned j = start; j <= i; ++j)
            if (levels[j] > expected) expected = levels[j];
        assert(d.ref_hold == expected);
    }
    xz_barge_step(&d, 2, 1, 0, 0, 1, 0, 50, 1,
                  0, 150, 4, 3000, 6, 250, 250, 120, 10);
    assert(d.ref_hold == 50 && !d.ref_age_ms);
    xz_barge_reset(&d);
    assert(!d.ref_hold && d.ref_age_ms == 300 && !d.ref_seen);
#undef REF_STEP
    puts("PASS: fix9 300ms sliding maximum, ref=59/age=231ms echo rejection, independent expiry, ring wrap, 49/50, age saturation/reset");
}

int main(void) {
    test_ref_window();
    xz_audio = (xz_audio_t){.inited=true, .is_rx_enable=true, .is_websocket=true,
        .vad_enabled=true, .handle=1, .rb_opus_encode_input=&encode_rb, .rb_vad_cache=&vad_rb};
    g_xz_ws.is_connected = 1;
    int16_t stereo[320], mono[160];
    for (unsigned i = 0; i < 160; ++i) {stereo[2*i] = 1000; stereo[2*i+1] = -2000; mono[i] = 321;}
    assert(xz_mic_mix(32767,32767,0) == 32767);
    assert(xz_mic_mix(-32768,-32768,0) == -32768);
    assert(xz_mic_mix(-32768,32767,0) == 0);
    assert(xz_mic_mix(-32768,32767,1) == -32768);
    for (uint32_t v = 0; v < 32769; ++v) assert(xz_mic_isqrt(v*v) == v);

    web_g_state = kDeviceStateIdle;
    capture(stereo, 640, true); /* raw callback only mixes; no KWS/uplink */
    assert(kws_calls == 0 && vad_calls == 0 && encode_rb.len == 0);
    assert(xz_mic_mono[0] == (XZ_MIC_MIX_MAX_ABS ? -2000 : -500));
#if CONFIG_XZ_MIC_LEVEL_LOG
    assert(xz_mic_levels.samples == 160 && xz_mic_levels.peak[0] == 1000 && xz_mic_levels.peak[1] == 2000);
    assert(xz_mic_isqrt(xz_mic_levels.square[0]/160) == 1000);
    assert(xz_mic_isqrt(xz_mic_levels.square[1]/160) == 2000);
#endif
    capture(xz_mic_mono, 320, false); /* normal post-AEC shape */
    assert(kws_calls == 1 && vad_calls == 1 && encode_rb.len == 0);
    capture(stereo, 640, false); /* alternate processed stereo SDK */
    assert(kws_calls == 2 && g_mic_n == 320 && encode_rb.len == 0);
    xz_rec_on = 1; xz_rec_n = 2;
    capture(mono, 320, false); /* pre-mixed mono gets exactly one makeup */
    assert(kws_calls == 3 && kws_pcm[0] == 642 && vad_pcm[0] == 642);
    assert(xz_rec_idx == 1 && xz_rec_peak[0] == 642 && makeup_logs == 1);
    xz_rec_on = 0;
    capture(stereo, 480, false); capture(stereo, 641, false);
    assert(kws_calls == 3 && xz_mic_bad_blocks == 2);
    web_g_state = kDeviceStateListening;
    unsigned before = events;
    for (unsigned i = 0; i < 6; ++i) capture(mono, 320, false);
    assert(encode_rb.len == 1920 && xz_audio.mic_rx_count == 0 && events == before+1);
    assert(((int16_t *)encode_rb.data)[0] == 642 && mono[0] == 321);
    assert(!memcmp(encode_rb.data, kws_pcm, 320) && !memcmp(kws_pcm, vad_pcm, 320));
    assert(xz_mic_post_peak == (XZ_MIC_MIX_MAX_ABS ? 2000u : 500u));
    xz_audio_reset_uplink();
    mono[0] = 32767; mono[1] = -32768; mono[2] = 16383; mono[3] = -16384;
    xz_rec_on = 1;
    capture(mono, 320, false);
    assert(((int16_t *)encode_rb.data)[0] == 32767);
    assert(((int16_t *)encode_rb.data)[1] == -32768);
    assert(((int16_t *)encode_rb.data)[2] == 32766);
    assert(((int16_t *)encode_rb.data)[3] == -32768);
    assert(kws_pcm[2] == 32766 && mono[2] == 16383); /* source is untouched */
    assert(!memcmp(encode_rb.data, kws_pcm, 320) && !memcmp(kws_pcm, vad_pcm, 320));
    assert(xz_rec_idx == 2 && xz_rec_peak[1] == 32768);
    xz_rec_on = 0;
    for (unsigned i = 0; i < 160; ++i) mono[i] = 321;
    xz_audio_reset_uplink();
    web_g_state = kDeviceStateThinking;
    capture(stereo, 640, false);
    assert(encode_rb.len == 0 && requests == 0);
    xz_wake_beep_busy = true; before = kws_calls;
    capture(mono, 320, false); assert(kws_calls == before && encode_rb.len == 0);
    xz_wake_beep_busy = false;

    new_turn();
    reference(-32768);
    xz_barge_ref_t measured;
    xz_barge_ref_snapshot(&measured);
    assert(measured.synced && measured.rms == 32768 && measured.peak == 32768);
    /* Digital saturation must not trip raw clipping or rescale post energy. */
    paired_constant(20000, 20000, true);
    assert(kws_pcm[0] == 32767 && vad_pcm[0] == 32767);
    assert(xz_barge_detector.rms == 20000 && !xz_barge_detector.clipped);
    puts("PASS: fix10 shared KWS/VAD/uplink makeup once, compensated recording, one-time log, raw energy/clipping preserved");

    /* Calibrate g=4000 with 25 consecutive raw=4000/ref=1000/post=100.
     * Bad startup leakage and quiet input must never arm by elapsed time. */
    new_turn();
    for (unsigned i = 0; i < 400; ++i) post_constant(1001);
    assert(!requests && !xz_audio_barge_in_armed(generation));
    for (unsigned i = 0; i < 400; ++i) paired_constant(0, 0, true);
    assert(!requests && !xz_audio_barge_in_armed(generation));
    train(100);
    assert(xz_barge_detector.echo_gain_milli == 4000);
    assert(strstr(last_armed, "window_ms=250 frames=25 floor=100 threshold=400"));
    assert(strstr(last_armed, "g=4000 ref=1000"));
    /* The supplied failure mode: post/pre can be 0.5 or 1.0, but
     * calibrated echo-level mic input still fails DTD. No ratio gate. */
    for (unsigned i = 0; i < 200; ++i) paired_constant(2000 + (i & 1)*2000, 4000, true);
    assert(!requests && !xz_barge_detector.frames && !xz_barge_detector.dtd);
    for (unsigned i = 0; i < 5; ++i) paired_constant(400, 12000, true);
    assert(!requests);
    paired_constant(400, 12000, true); /* inclusive DTD and floor boundary */
    assert(requests == 1 && request_rms == 400 && request_floor == 100 && request_pre == 12000);
    assert(request_frames == 6 && xz_barge_detector.dtd);
    for (unsigned i = 0; i < 12; ++i) paired_constant(5000, 10000, true);
    assert(requests == 1);

    /* Three simulated boots with a realistic, unclamped g=500.
     * Loud playback scales pre with ref and must never self-interrupt. */
    for (unsigned reboot = 0; reboot < 3; ++reboot) {
        new_turn();
        unsigned curves_before = curve_logs;
        for (unsigned i = 0; i < 25; ++i) paired_constant(13, 500, true);
        assert(xz_audio_barge_in_armed(generation) && xz_barge_detector.echo_gain_milli == 500);
        ref_level = 4000;
        for (unsigned i = 0; i < 275; ++i) paired_constant(1900, 2000, true);
        assert(!requests && xz_barge_detector.echo_floor == 13);
        assert(curve_logs == curves_before + 30); /* full 0..2900 ms */
        assert(strstr(last_curve, "g=500 dtd=0 sync=1"));
        for (unsigned i = 0; i < 5; ++i) paired_constant(150, 5999, true);
        assert(!requests); /* just below 3x calibrated echo amplitude */
        for (unsigned i = 0; i < 6; ++i) paired_constant(150, 6000, true);
        assert(requests == 1 && xz_barge_detector.dtd);
    }

    /* Ref=50 cannot lower a recent loud block; only its expiry lowers DTD. */
    new_turn(); train(25); ref_level = 50;
    for (unsigned i = 0; i < 29; ++i) paired_constant(150, 600, true);
    assert(!requests && xz_barge_detector.ref_hold == 1000);
    for (unsigned i = 0; i < 3; ++i) paired_constant(150, 599, true);
    assert(!requests && !xz_barge_detector.frames);
    for (unsigned i = 0; i < 6; ++i) paired_constant(150, 600, true);
    assert(requests == 1);
    for (unsigned silent = 0; silent < 2; ++silent) {
        new_turn(); train(25); ref_level = silent ? 49 : 0;
        for (unsigned i = 0; i < 30; ++i) paired_constant(200, 150, true);
        assert(xz_barge_detector.ref_age_ms == 300);
        assert(!requests && xz_barge_detector.frames == 1);
        for (unsigned i = 0; i < 12; ++i) paired_constant(149, 150, true);
        assert(!requests && xz_barge_detector.ref_rms == ref_level);
        mock_voice = -1;
        for (unsigned i = 0; i < 12; ++i) paired_constant(200, 150, true);
        assert(!requests); mock_voice = 1;
        for (unsigned i = 0; i < 6; ++i) paired_constant(150, 150, true);
        assert(requests == 1 && xz_barge_detector.dtd);
    }

    /* Missing TX cannot masquerade as silent playback or calibrate a gain. */
    new_turn(); feed_reference = false;
    for (unsigned i = 0; i < 100; ++i) paired_constant(25, 4000, true);
    assert(!xz_audio_barge_in_armed(generation));
    feed_reference = true; ref_level = 0;
    for (unsigned i = 0; i < 100; ++i) paired_constant(25, 4000, true);
    assert(!xz_audio_barge_in_armed(generation));
    ref_level = 1000; train(25);

    /* Every bad frame restarts the 25-frame convergence window and gain sums. */
    for (unsigned failure = 0; failure < 7; ++failure) {
        new_turn();
        for (unsigned i = 0; i < 24; ++i) paired_constant(30, 120, true);
        assert(xz_barge_detector.arm_frames == 24 && xz_barge_detector.sum_ref == 24000);
        switch (failure) {
        case 0: paired_constant(25, 119, true); break;
        case 1: paired_constant(31, 120, true); break;
        case 2: paired_constant(30, 120, false); break;
        case 3: capture(stereo, 480, false); break;
        case 4: now += 31; paired_constant(31, 120, true); break;
        case 5: xz_wake_beep_busy = true; post_constant(100); xz_wake_beep_busy = false; break;
        case 6: Initiate_disconnection_flag = 1; post_constant(100); Initiate_disconnection_flag = 0; break;
        }
        assert(!xz_barge_detector.arm_frames && !xz_barge_detector.arm_sum);
        assert(!xz_barge_detector.sum_ref && !xz_barge_detector.sum_pre);
        for (unsigned i = 0; i < 24; ++i) paired_constant(30, 120, true);
        assert(!xz_audio_barge_in_armed(generation));
        paired_constant(30, 120, true);
        assert(xz_audio_barge_in_armed(generation) && xz_barge_detector.echo_gain_milli == 120);
    }

    /* Clipping on either channel/sign freezes the floor/gain and resets
     * both windows. It must not clear the fired latch. */
    const int16_t clips[] = {32000, -32000, 32767, -32768};
    for (unsigned channel = 0; channel < 2; ++channel) {
        for (unsigned v = 0; v < sizeof(clips)/sizeof(clips[0]); ++v) {
            new_turn();
            for (unsigned i = 0; i < 24; ++i) post_constant(100);
            paired_spike(100, clips[v], channel);
            assert(!xz_audio_barge_in_armed(generation) && !xz_barge_detector.sum_ref);
            train(100); ref_level = 0;
            for (unsigned i = 0; i < 31; ++i) post_constant(0); /* expire echo tail hold */
            for (unsigned i = 0; i < 5; ++i) post_constant(1200);
            paired_spike(1200, clips[v], channel);
            assert(!requests && !xz_barge_detector.frames);
            assert(xz_barge_detector.echo_floor == 100 && xz_barge_detector.echo_gain_milli == 4000);
            for (unsigned i = 0; i < 6; ++i) post_constant(1200);
            assert(requests == 1);
            paired_spike(5000, clips[v], channel);
            for (unsigned i = 0; i < 12; ++i) post_constant(1200);
            assert(requests == 1);
        }
    }
    assert(clip_logs && strstr(last_clip, "peakL=") && strstr(last_clip, "peakR="));
    new_turn();
    for (unsigned i = 0; i < 25; ++i) paired_spike(100, 31999, i % 2);
    assert(xz_audio_barge_in_armed(generation));
    unsigned clips_before = clip_logs;
    now += 2000;
    for (unsigned i = 0; i < 401; ++i) paired_spike(100, 32767, 0);
    assert(clip_logs == clips_before + 3);
    assert(strstr(last_peak, "L=32767 R=4000 clip=1 pre_valid=1"));

    /* One-frame order/delay is tolerated. Stale TX (>3 frames), sequence
     * drift, malformed reference, and missing raw data break speech streaks. */
    new_turn(); train(100);
    feed_reference = false;
    now -= 10; /* latest reference was one frame ago */
    for (unsigned i = 0; i < 3; ++i) paired_constant(400, 12000, true);
    assert(xz_barge_detector.frames == 3);
    paired_constant(400, 12000, true); /* 40 ms stale */
    assert(!requests && !xz_barge_detector.frames && !xz_mic_ref.synced);
    feed_reference = true;
    for (unsigned i = 0; i < 6; ++i) paired_constant(400, 12000, true);
    assert(requests == 1);
    new_turn(); train(100);
    for (unsigned i = 0; i < 5; ++i) paired_constant(400, 12000, true);
    for (unsigned i = 0; i < 4; ++i) reference(1000); /* >3 extra TX events */
    paired_constant(400, 12000, true);
    assert(!requests && !xz_barge_detector.frames && xz_mic_ref.skew == 4);
    for (unsigned i = 0; i < 6; ++i) paired_constant(400, 12000, true);
    assert(requests == 1);
    new_turn(); train(100);
    for (unsigned i = 0; i < 5; ++i) paired_constant(400, 12000, true);
    for (unsigned i = 0; i < 160; ++i) mono[i] = 1000;
    __wrap_audio_3a_far_put((uint8_t *)mono, 318);
    assert(xz_ref.valid && xz_ref.ref_block_rms == 1000); /* 159-point short block is measured */
    feed_reference = false; paired_constant(400, 12000, true);
    assert(requests == 1); /* the held short-block metric can complete pairing */
    new_turn(); train(100); feed_reference = true;
    for (unsigned i = 0; i < 5; ++i) paired_constant(400, 12000, true);
    paired_constant(400, 8000, false);
    assert(!requests && !xz_barge_detector.frames && !xz_barge_detector.pre_valid);

    /* The current TX block is used directly; held DTD remains active for
     * 300 ms after its level falls below the reference minimum. */
    new_turn(); train(25); ref_level = 0;
    paired_constant(150, 150, true);
    assert(xz_barge_detector.ref_rms == 0 && !xz_barge_detector.dtd);
    paired_constant(150, 150, true);
    assert(!xz_barge_detector.ref_rms && !xz_barge_detector.dtd);
    assert(xz_barge_detector.ref_hold == 1000);

    /* fix7 board failure: g=345, floor=30, tail pre=385/post=305.
     * Both zero and sub-50 reference must reject it through age 299 ms. */
    for (unsigned silent = 0; silent < 2; ++silent) {
        new_turn();
        for (unsigned i = 0; i < 25; ++i) paired_constant(30, 345, true);
        assert(xz_audio_barge_in_armed(generation) && xz_barge_detector.echo_gain_milli == 345);
        assert(strstr(last_armed, "g=345 ref=1000 refh=1000 ref_age=0ms"));
        ref_level = silent ? 49 : 0;
        for (unsigned i = 0; i < 29; ++i) {
            paired_constant(305, 385, true);
            assert(!requests && !xz_barge_detector.dtd && !xz_barge_detector.frames);
            assert(xz_barge_detector.ref_hold == 1000 && xz_barge_detector.ref_age_ms == (i + 1) * 10);
        }
        now -= 1; paired_constant(305, 385, true); /* age 299, not frame-count based */
        assert(xz_barge_detector.ref_age_ms == 299 && !xz_barge_detector.dtd);
        now -= 9; paired_constant(150, 150, true); /* exactly age 300: absolute gate */
        assert(xz_barge_detector.ref_age_ms == 300 && xz_barge_detector.dtd);
        assert(!requests && xz_barge_detector.frames == 1);
        for (unsigned i = 0; i < 5; ++i) paired_constant(150, 150, true);
        assert(requests == 1 && !xz_barge_detector.ref_hold);
    }

    /* Real near-end speech still passes the held DTD at its inclusive
     * pre=1035 boundary for g=345/refh=1000, with six frames and one latch. */
    new_turn();
    for (unsigned i = 0; i < 25; ++i) paired_constant(30, 345, true);
    ref_level = 0;
    for (unsigned i = 0; i < 6; ++i) paired_constant(305, 1034, true);
    assert(!requests && !xz_barge_detector.frames);
    for (unsigned i = 0; i < 6; ++i) paired_constant(305, 1035, true);
    assert(requests == 1 && xz_barge_detector.dtd && xz_barge_detector.ref_age_ms < 300);
    for (unsigned i = 0; i < 12; ++i) paired_constant(305, 1035, true);
    assert(requests == 1);

    /* A >3-frame pairing event resets the speech streak, retaining both the
     * hold and its age. Quiet re-pairing cannot bypass the tail DTD. */
    new_turn(); train(25); ref_level = 0;
    for (unsigned i = 0; i < 5; ++i) paired_constant(150, 12000, true);
    assert(xz_barge_detector.frames == 5);
    for (unsigned i = 0; i < 4; ++i) reference(0);
    paired_constant(305, 385, true);
    assert(xz_mic_ref.skew == 4 && !xz_mic_ref.synced);
    assert(!requests && !xz_barge_detector.frames && xz_barge_detector.ref_hold == 1000);
    assert(xz_barge_detector.ref_age_ms == 60);
    paired_constant(305, 385, true);
    assert(xz_mic_ref.synced && !xz_barge_detector.dtd && !requests);
    assert(xz_barge_detector.ref_hold == 1000 && xz_barge_detector.ref_age_ms == 70);

    /* Capture gaps age the window by real time and still break the streak.
     * Later ref>=50 refreshes age, while the older maximum expires at 300ms. */
    now += 1000; paired_constant(150, 150, true);
    assert(xz_barge_detector.dtd && xz_barge_detector.frames == 1);
    assert(xz_barge_detector.ref_age_ms == 1080);
    for (unsigned i = 0; i < 5; ++i) paired_constant(150, 150, true);
    assert(requests == 1);
    new_turn(); train(25); ref_level = 50;
    for (unsigned i = 0; i < 29; ++i) paired_constant(150, 599, true);
    assert(xz_barge_detector.ref_hold == 1000 && !xz_barge_detector.ref_age_ms);
    paired_constant(150, 599, true);
    assert(xz_barge_detector.ref_hold == 50 && !xz_barge_detector.ref_age_ms);
    assert(!xz_barge_detector.dtd && !requests);
    ref_level = 0;
    for (unsigned i = 0; i < 6; ++i) paired_constant(150, 600, true);
    assert(requests == 1 && xz_barge_detector.ref_hold == 50 && xz_barge_detector.ref_age_ms == 60);
    new_turn();
    paired_constant(0, 0, true);
    assert(xz_barge_detector.phase == XZ_BARGE_TRAIN && !xz_barge_detector.ref_age_ms);
    assert(xz_barge_detector.ref_hold == 1000); /* new generation observes only new TX */
    xz_audio_reset_barge_in();
    assert(!xz_barge_detector.ref_hold && xz_barge_detector.ref_age_ms == 300);

    /* Gain clamps, window rounding, 64-bit products and full-scale sums. */
    xz_barge_detector_t tuned = {0};
    for (unsigned i = 0; i < 100; ++i)
        xz_barge_step(&tuned, 1, 1, 0, 120, 1, 0, 32768, 1, 1, 150, 4, 2000, 6, 250, 1000, 120, 10);
    assert(tuned.echo_gain_milli == 50);
    for (unsigned i = 0; i < 11; ++i) {
        assert(!xz_barge_step(&tuned, 2, 1, 4000, 32768, 1, 0, 50, 1, 1, 600, 4, 4000, 6, 125, 101, 800, 10));
        assert((tuned.phase == XZ_BARGE_ARMED) == (i == 10));
    }
    assert(tuned.echo_gain_milli == 4000);
    for (unsigned i = 0; i < 100; ++i)
        assert(!xz_barge_step(&tuned, 3, 1, 8192, 32768, 1, 0, 32768, 1, 1, 600, 4, 4000, 6, 250, 1000, 8000, 10));
    assert(tuned.echo_floor == 8192 && tuned.echo_gain_milli == 1000);
    xz_barge_step(&tuned, 3, 1, 32768, 32768, 1, 0, 32768, 1, 1, 600, 4, 4000, 6, 250, 1000, 8000, 10);
    assert(!tuned.dtd && !tuned.frames);
    tuned.echo_floor = 32768;
    assert(xz_barge_threshold(&tuned, 600, 4) == 131072);
    now = UINT32_MAX - 100; new_turn(); train(100);
    for (unsigned i = 0; i < 6; ++i) paired_constant(400, 12000, true);
    assert(requests == 1);
    now = UINT32_MAX - 300; new_turn(); train(25); ref_level = 0;
    for (unsigned i = 0; i < 29; ++i) paired_constant(305, 385, true);
    assert(!requests && xz_barge_detector.ref_age_ms == 290 && !xz_barge_detector.dtd);
    paired_constant(150, 150, true);
    assert(xz_barge_detector.ref_age_ms == 300 && xz_barge_detector.dtd);
    for (unsigned i = 0; i < 5; ++i) paired_constant(150, 150, true);
    assert(requests == 1);
    xz_audio_reset_barge_in();
    web_g_state = kDeviceStateIdle; session_active = false;

    now = 10000; capture(mono, 320, false);
    unsigned level_before = level_logs, pcm_before = pcm_logs, peak_before = peak_logs;
    for (unsigned i = 0; i < 199; ++i) {now += 10; capture(mono, 320, false);}
    assert(level_logs == level_before && pcm_logs == pcm_before);
    now += 10; capture(mono, 320, false);
    assert(pcm_logs == pcm_before+1 && peak_logs == peak_before+1);
    assert(strstr(last_peak, "L=0 R=0 clip=0 pre_valid=0"));
    assert(level_logs == level_before + (CONFIG_XZ_MIC_LEVEL_LOG ? 1u : 0u));

    audio_device_speaker_t device = {.is_need_3a=true, .pdm1_dev=true, .rx_channels=2};
    audio_server_t server = {0};
    struct mock_client client = {.parameter={AUDIO_MIC_ALL}, .callback=mic_callback};
    before = kws_calls;
    for (unsigned i = 0; i < 3; ++i) {
        memcpy(device.rx_data_tmp, stereo, 640);
        sdk_rx(&server, &device, &client);
        if (i < 2) assert(kws_pcm[0] == 0); /* preserve PDM startup suppression */
    }
    assert(mock_aec_calls == 3 && kws_calls == before+3);
    assert(kws_pcm[0] == (XZ_MIC_MIX_MAX_ABS ? -2000 : -500));
    client.callback = NULL;
    memcpy(device.rx_data_tmp, stereo, 640);
    sdk_rx(&server, &device, &client);
    assert(((int16_t *)device.rx_data_tmp)[0] == -250 && mock_aec_calls == 4);
    puts("PASS: actual SDK adapter: raw LR -> app downmix -> 320-byte AEC -> mono KWS, startup drop and fallback");
    printf("PASS: audio log=%d mix=%d: raw/post stereo, mono fallback, malformed drop, "
           "16k/10ms -> 60ms Opus; unclipped/converged/mean/250ms arming, adaptive/reference-DTD/6 frames, "
           "L/R clip boundaries/silence rejection/2s peak+clip logs, 100ms/3s curve, stop/missing-pre/tick-wrap/beep/state/VAD guards\n",
           CONFIG_XZ_MIC_LEVEL_LOG, XZ_MIC_MIX_MAX_ABS);
    puts("PASS: fix7 ref=0/49 echo-tail rejection, 299/300ms boundary, held-DTD speech, sync retention, real-time gaps, refresh/reset, tick wrap");
    return 0;
}
