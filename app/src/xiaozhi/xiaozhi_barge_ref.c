#include <rtthread.h>
#include "xiaozhi_mic_dsp.h"
#include "xiaozhi_barge_ref.h"

/* Store metrics for each complete TX write, never a PCM copy. */
typedef struct
{
    uint16_t ref_block_rms, ref_block_peak;
    rt_tick_t tick;
    uint32_t tx_seq, mic_seq, offset;
    uint8_t valid, tracking;
} xz_barge_ref_state_t;
static xz_barge_ref_state_t xz_ref;

void xz_barge_ref_reset(void)
{
    xz_ref = (xz_barge_ref_state_t){0};
}

void __real_audio_3a_far_put(uint8_t *fifo, uint16_t fifo_size);

void __wrap_audio_3a_far_put(uint8_t *fifo, uint16_t fifo_size)
{
    /* TX is mono 16 kHz PCM. Measure the exact write length, including short
     * final blocks; an empty or odd-sized write cannot define a PCM level. */
    uint64_t square = 0;
    uint32_t peak = 0;
    unsigned samples = fifo_size / sizeof(int16_t);
    int valid = fifo && samples && !(fifo_size & 1u);
    if (valid)
    {
        const int16_t *pcm = (const int16_t *)fifo;
        for (unsigned i = 0; i < samples; ++i)
        {
            int32_t sample = pcm[i];
            uint32_t magnitude = sample < 0 ? -sample : sample;
            square += (uint64_t)(sample * sample);
            if (magnitude > peak) peak = magnitude;
        }
    }
    uint16_t rms = valid ? (uint16_t)xz_mic_isqrt((uint32_t)(square / samples)) : 0;
    rt_enter_critical();
    if (valid)
    {
        xz_ref.ref_block_rms = rms;
        xz_ref.ref_block_peak = (uint16_t)peak;
        xz_ref.tick = rt_tick_get();
        xz_ref.valid = 1;
        ++xz_ref.tx_seq;
    }
    else xz_ref.valid = 0;
    rt_exit_critical();
    __real_audio_3a_far_put(fifo, fifo_size);
}

void xz_barge_ref_snapshot(xz_barge_ref_t *ref)
{
    *ref = (xz_barge_ref_t){0};
    ++xz_ref.mic_seq;
    if (!xz_ref.valid) return; /* missing != an observed silent TX block */
    rt_tick_t now = rt_tick_get();
    rt_tick_t age = now - xz_ref.tick;
    ref->age_ms = (uint32_t)((uint64_t)age * 1000u / RT_TICK_PER_SECOND);
    uint32_t offset = xz_ref.tx_seq - xz_ref.mic_seq;
    if (!xz_ref.tracking)
    {
        xz_ref.offset = offset;
        xz_ref.tracking = 1;
    }
    ref->skew = (int32_t)(offset - xz_ref.offset);
    ref->synced = age <= rt_tick_from_millisecond(30) &&
                  ref->skew >= -3 && ref->skew <= 3;
    if (!ref->synced)
    {
        /* Discard this pairing; a fresh pair may establish a new epoch. */
        xz_ref.tracking = 0;
        return;
    }
    ref->rms = xz_ref.ref_block_rms;
    ref->peak = xz_ref.ref_block_peak;
}
