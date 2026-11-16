#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if TEST_PDM
#define AUDIO_RX_USING_PDM 1
#endif
#include "xiaozhi_mic_dsp.h"
#define RT_ASSERT assert
#define RECORD_DATA_MAX_SIZE 1280u
#define KWS_EVT_DECODE 1
typedef struct node { struct node *next; } rt_slist_t;
typedef struct { rt_slist_t node; uint8_t *data; } kws_queue_t;
typedef struct {
    rt_slist_t recording, recorded;
    uint32_t record_offset;
    int event;
    uint8_t shared_mic;
} kws_data_t;
#define rt_container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
static unsigned gain_calls, gain_logs, events;
static int saved_gain = 56, live_gain = 56;
static void rt_enter_critical(void) {}
static void rt_exit_critical(void) {}
static rt_slist_t *rt_slist_first(rt_slist_t *list) { return list->next; }
static void rt_slist_remove(rt_slist_t *list, rt_slist_t *node) {
    assert(list->next == node); list->next = node->next; node->next = NULL;
}
static void rt_slist_append(rt_slist_t *list, rt_slist_t *node) {
    assert(!list->next); list->next = node;
}
static int rt_event_send(int event, unsigned bits) { events++; return 0; }
#if TEST_PDM
int Set_pdm_gain(int8_t value) {
    assert(value == 44); gain_calls++; saved_gain = live_gain = value; return 0;
}
static int rt_kprintf(const char *format, int gain) {
    assert(strcmp(format, "PDM_GAIN set=%d\n") == 0 && gain == 44);
    gain_logs++; return 0;
}
#endif
#include "gain-under-test.inc"

int main(void) {
    for (unsigned reopen = 0; reopen < 5; ++reopen) {
        xz_audio_apply_pdm_gain();
        if (TEST_PDM) assert(saved_gain == 44 && live_gain == 44);
        live_gain = saved_gain; /* audio_server initializes from saved gain */
        live_gain = 56; /* simulate an intervening hardware reset */
        xz_audio_apply_pdm_gain();
        if (TEST_PDM) assert(live_gain == 44);
    }
    assert(gain_calls == (TEST_PDM ? 10u : 0u));
    assert(gain_logs == (TEST_PDM ? 1u : 0u));
    /* Exhaustive signed sample range checks gain and both saturation ends. */
    for (int value = -32768; value <= 32767; ++value) {
        int expected = TEST_PDM ? value * 2 : value;
        if (expected > 32767) expected = 32767;
        if (expected < -32768) expected = -32768;
        assert(xz_mic_makeup_sample((int16_t)value) == expected);
    }
    int16_t source[160], original[160], shared_pcm[160], queue_pcm[640];
    const int16_t values[] = {321, -321, 16383, -16384, 20000, -20000, 32767, -32768};
    for (unsigned i = 0; i < 160; ++i) {
        source[i] = values[i % 8];
        shared_pcm[i] = xz_mic_makeup_sample(source[i]);
    }
    memcpy(original, source, sizeof(source));
    for (unsigned shared = 0; shared < 2; ++shared) {
        events = 0;
        kws_queue_t queue = {.data = (uint8_t *)queue_pcm};
        kws_data_t kws = {.recording = {.next = &queue.node}, .shared_mic = shared};
        const int16_t *input = shared ? shared_pcm : source;
        for (unsigned frame = 0; frame < 4; ++frame) {
            kws_push_pcm(&kws, (const uint8_t *)input, sizeof(source));
            assert(!memcmp(original, source, sizeof(source)));
            for (unsigned i = 0; i < 160; ++i)
                assert(shared_pcm[i] == xz_mic_makeup_sample(source[i]));
            assert(events == (frame == 3));
        }
        assert(kws.record_offset == 0 && kws.recorded.next == &queue.node);
        for (unsigned i = 0; i < 640; ++i)
            assert(queue_pcm[i] == xz_mic_makeup_sample(source[i % 160]));
        kws_push_pcm(&kws, (const uint8_t *)input, sizeof(source));
        assert(events == 1); /* a full KWS queue still drops safely */
    }
    printf("PASS: PDM=%d absolute gain reopens/one-time log, all int16 saturation values, standalone/shared KWS exactly one makeup\n", TEST_PDM);
    return 0;
}
