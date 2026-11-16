#ifndef XIAOZHI_MIC_DSP_H
#define XIAOZHI_MIC_DSP_H

#include <stdint.h>

#define XZ_MIC_SAMPLES_10MS 160u
#define XZ_MIC_MONO_BYTES (XZ_MIC_SAMPLES_10MS * sizeof(int16_t))
#define XZ_MIC_CLIP_PEAK 32000u
#define XZ_BARGE_REF_MIN_RMS 50u
#define XZ_BARGE_REF_HOLD_MS 300u
#define XZ_BARGE_REF_HOLD_FRAMES (XZ_BARGE_REF_HOLD_MS / 10u)

/* SDK PDM gain uses 0.5 dB units: 56 -> 44 trims 6 dB. Apply the
 * absolute setting before and after every microphone audio_open. */
#define XZ_PDM_GAIN_TRIM 44
void xz_audio_apply_pdm_gain(void);

/* KWS, VAD, recording diagnostics and uplink share +6 dB makeup PCM.
 * Raw/AEC input, post energy metrics and barge clipping stay untouched. */
static inline int16_t xz_mic_makeup_sample(int16_t sample)
{
#ifdef AUDIO_RX_USING_PDM
    int32_t value = (int32_t)sample * 2;
    return (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
#else
    return sample;
#endif
}

static inline int xz_mic_clipped(uint32_t peak_l, uint32_t peak_r)
{
    return peak_l >= XZ_MIC_CLIP_PEAK || peak_r >= XZ_MIC_CLIP_PEAK;
}

/* The audio server delivers exactly one 16 kHz / 10 ms block. Never guess
 * stereo from an even length: 320 bytes is already processed mono. */
static inline unsigned xz_mic_channels(uint32_t bytes)
{
    return bytes == XZ_MIC_MONO_BYTES * 2 ? 2 :
           bytes == XZ_MIC_MONO_BYTES ? 1 : 0;
}

static inline uint32_t xz_mic_isqrt(uint32_t value)
{
    uint32_t root = 0, bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit)
    {
        if (value >= root + bit)
        {
            value -= root + bit;
            root = (root >> 1) + bit;
        }
        else root >>= 1;
        bit >>= 2;
    }
    return root;
}

static inline int16_t xz_mic_mix(int16_t left, int16_t right, int max_abs)
{
    int32_t l = left, r = right;
    if (max_abs) return (l < 0 ? -l : l) >= (r < 0 ? -r : r) ? left : right;
    return (int16_t)((l + r) / 2); /* int32 sum also handles -32768 safely */
}

typedef struct
{
    uint32_t generation;
    uint32_t arm_sum;
    uint32_t sum_pre, sum_ref;
    uint32_t ref_age_ms;
    uint16_t echo_floor, rms, pre_rms, ref_rms, echo_gain_milli;
    uint16_t ref_hold;
    struct { uint16_t rms, age_ms; } ref_window[XZ_BARGE_REF_HOLD_FRAMES];
    uint16_t arm_frames;
    uint8_t ref_window_next, ref_seen;
    uint8_t frames, fired, phase, pre_valid, arm_reason, dtd, clipped;
} xz_barge_detector_t;

enum { XZ_BARGE_IDLE, XZ_BARGE_TRAIN, XZ_BARGE_ARMED };
enum { XZ_BARGE_ARM_NONE, XZ_BARGE_ARM_CONVERGED };

static inline void xz_barge_reset(xz_barge_detector_t *d)
{
    *d = (xz_barge_detector_t){0};
    d->ref_age_ms = XZ_BARGE_REF_HOLD_MS; /* no playback observed yet */
}

/* A bad/missing block breaks both consecutive windows, without forgetting
 * an armed baseline, held reference or the once-per-generation latch. */
static inline void xz_barge_break_streak(xz_barge_detector_t *d)
{
    d->frames = 0;
    d->arm_frames = 0;
    d->arm_sum = 0;
    d->sum_pre = d->sum_ref = 0;
    if (d->phase == XZ_BARGE_TRAIN) d->arm_reason = XZ_BARGE_ARM_NONE;
}

static inline uint32_t xz_barge_threshold(const xz_barge_detector_t *d,
    uint32_t min_rms, unsigned echo_mult)
{
    uint32_t adaptive = (uint32_t)d->echo_floor * echo_mult;
    return adaptive > min_rms ? adaptive : min_rms;
}

/* Diagnostic post/raw-mic ratio only; never used as a speech trigger. */
static inline uint32_t xz_barge_ratio(uint32_t rms, uint32_t pre_rms)
{
    return pre_rms ? rms * 1000u / pre_rms : 0;
}

/* No wall-clock deadline can arm the detector. Only consecutive 10 ms
 * unclipped blocks with real echo (pre >= arm_min_pre) and a converged residual
 * may do so. Silence never proves convergence.
 * Freeze their mean, excluding all earlier startup transients. Missing
 * pre-3A or TX data fails closed. Gain uses only ref >= 50 frames of this
 * same window; it and the residual floor freeze together when armed.
 * elapsed_ms is the actual time since the previous callback, including gaps.
 * Caller serializes resets against the callback. */
static inline int xz_barge_step(xz_barge_detector_t *d, uint32_t generation,
    int eligible, uint32_t rms, uint32_t pre_rms, int pre_valid, int clipped,
    uint32_t ref_rms, int ref_valid,
    int voice, uint32_t min_rms, unsigned echo_mult, unsigned dtd_margin_pm,
    unsigned min_frames, unsigned arm_ratio, unsigned arm_ms, unsigned arm_min_pre,
    uint32_t elapsed_ms)
{
    if (d->generation != generation || d->phase == XZ_BARGE_IDLE)
    {
        xz_barge_reset(d);
        d->generation = generation;
        d->phase = XZ_BARGE_TRAIN;
    }
    d->rms = (uint16_t)rms;
    d->pre_rms = pre_valid ? (uint16_t)pre_rms : 0;
    d->pre_valid = !!pre_valid;
    d->clipped = !!clipped;
    d->ref_rms = (uint16_t)ref_rms;
    /* One slot per nonempty 10 ms observation. Expire by actual elapsed
     * time, including missing/sync/clipped frames and capture gaps. */
    for (unsigned i = 0; i < XZ_BARGE_REF_HOLD_FRAMES; ++i)
    {
        if (!d->ref_window[i].rms) continue;
        if (elapsed_ms >= XZ_BARGE_REF_HOLD_MS - d->ref_window[i].age_ms)
            d->ref_window[i].rms = 0;
        else d->ref_window[i].age_ms += elapsed_ms;
    }
    if (ref_valid && ref_rms >= XZ_BARGE_REF_MIN_RMS)
    {
        d->ref_window[d->ref_window_next].rms = (uint16_t)ref_rms;
        d->ref_window[d->ref_window_next].age_ms = 0;
        d->ref_window_next = (d->ref_window_next + 1u) % XZ_BARGE_REF_HOLD_FRAMES;
        d->ref_seen = 1;
        d->ref_age_ms = 0;
    }
    else if (d->ref_seen)
    {
        /* Keep the original age since the last nonempty reference, even
         * after the maximum expires. Saturate during long silence. */
        uint32_t room = UINT32_MAX - d->ref_age_ms;
        d->ref_age_ms += elapsed_ms < room ? elapsed_ms : room;
    }
    d->ref_hold = 0;
    for (unsigned i = 0; i < XZ_BARGE_REF_HOLD_FRAMES; ++i)
        if (d->ref_window[i].rms > d->ref_hold) d->ref_hold = d->ref_window[i].rms;
    uint32_t dtd_ref = d->ref_hold;
    /* Cross-multiplied DTD: pre*1000 >= g*ref*margin/1000. */
    d->dtd = ref_valid && (dtd_ref < XZ_BARGE_REF_MIN_RMS || (d->echo_gain_milli &&
        (uint64_t)pre_rms * 1000000u >=
        (uint64_t)d->echo_gain_milli * dtd_ref * dtd_margin_pm));
    if (!eligible || clipped || !ref_valid)
    {
        xz_barge_break_streak(d);
        return 0;
    }
    if (d->phase == XZ_BARGE_TRAIN)
    {
        d->frames = 0;
        if (!pre_valid || pre_rms < arm_min_pre || rms * 1000u > pre_rms * arm_ratio)
        {
            xz_barge_break_streak(d);
            return 0;
        }
        d->arm_reason = XZ_BARGE_ARM_CONVERGED;
        d->arm_sum += rms;
        if (ref_rms >= XZ_BARGE_REF_MIN_RMS)
        {
            d->sum_pre += pre_rms;
            d->sum_ref += ref_rms;
        }
        if (++d->arm_frames >= (arm_ms + 9u) / 10u)
        {
            if (!d->sum_ref)
            {
                xz_barge_break_streak(d);
                return 0; /* no arbitrary gain when calibration has no TX */
            }
            uint32_t gain = (uint32_t)((uint64_t)d->sum_pre * 1000u / d->sum_ref);
            d->echo_gain_milli = (uint16_t)(gain < 50 ? 50 : gain > 4000 ? 4000 : gain);
            d->echo_floor = (uint16_t)(d->arm_sum / d->arm_frames);
            d->phase = XZ_BARGE_ARMED;
        }
        /* The arming window itself is never counted as near-end speech. */
        return 0;
    }
    if (d->fired || voice != 1 || !pre_valid || !pre_rms ||
        rms < xz_barge_threshold(d, min_rms, echo_mult) ||
        !d->dtd)
    {
        d->frames = 0;
        return 0;
    }
    if (++d->frames < min_frames) return 0;
    d->fired = 1;
    return 1;
}

#endif
