#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PKG_XIAOZHI_USING_AEC 1
#define RT_WAITING_FOREVER -1
#define AUDIO_IOCTL_DROP_TX_CACHE 7
#define AUDIO_IOCTL_BYTES_IN_CACHE 3
#define LOG_I(...) ((void)0)
typedef struct node {struct node *next;} rt_slist_t;
typedef struct {
    bool inited, is_tx_enable, is_websocket, is_exit;
    int speaker, resample;
    rt_slist_t downlink_decode_busy, downlink_decode_idle;
} xz_audio_t;
static xz_audio_t xz_audio;
static bool xz_decode_inflight, abort_on_delay;
static uint32_t xz_playback_generation;
static struct mutex {unsigned depth;} xz_playback_mutex;
static unsigned writes, drops, cache_bytes;
static int write_result = 320;
static uint8_t resampled[320];
static void xz_speaker_close(xz_audio_t *thiz);
static void rt_enter_critical(void) {}
static void rt_exit_critical(void) {}
static void rt_mutex_take(struct mutex *m, int timeout) {assert(m->depth == 0); m->depth++;}
static void rt_mutex_release(struct mutex *m) {assert(m->depth == 1); m->depth--;}
static rt_slist_t *rt_slist_first(rt_slist_t *head) {return head->next;}
static void rt_slist_remove(rt_slist_t *head, rt_slist_t *node) {
    while (head->next && head->next != node) head = head->next;
    assert(head->next == node); head->next = node->next; node->next = NULL;
}
static void rt_slist_append(rt_slist_t *head, rt_slist_t *node) {
    while (head->next) head = head->next;
    assert(head != node); head->next = node; node->next = NULL;
}
static void xiaozhi_ui_chat_status(const char *s) {}
static bool xz_ws_playback_allowed(void) {return true;}
static uint32_t sifli_resample_process(int resample, int16_t *p, unsigned len, int flag) {
    assert(len == 320); memcpy(resampled, p, len); return len;
}
static uint8_t *sifli_resample_get_output(int handle) {return resampled;}
static int audio_write(int handle, uint8_t *p, unsigned len) {
    assert(handle == xz_audio.speaker && len == 320 && xz_playback_mutex.depth == 1);
    writes++; return write_result;
}
static int audio_ioctl(int handle, int command, void *arg) {
    assert(handle == xz_audio.speaker);
    if (command == AUDIO_IOCTL_DROP_TX_CACHE) {assert(xz_playback_mutex.depth == 1); drops++; cache_bytes = 0;}
    else {assert(command == AUDIO_IOCTL_BYTES_IN_CACHE); *(uint32_t *)arg = cache_bytes;}
    return 0;
}
static void rt_thread_mdelay(int ms) {
    if (abort_on_delay) {abort_on_delay = false; xz_speaker_close(&xz_audio);}
}

#include "playback-under-test.inc"

int main(void) {
    xz_audio = (xz_audio_t){.inited=true, .is_tx_enable=true, .is_websocket=true, .speaker=1};
    rt_slist_t queued1 = {0}, queued2 = {0}, inflight = {0};
    rt_slist_append(&xz_audio.downlink_decode_busy, &queued1);
    rt_slist_append(&xz_audio.downlink_decode_busy, &queued2);
    xz_decode_inflight = true;
    cache_bytes = 3200;
    uint32_t old_generation = xz_playback_generation;
    xz_speaker_close(&xz_audio);
    assert(!xz_audio.is_tx_enable && !xz_audio.downlink_decode_busy.next && drops == 1 && cache_bytes == 0);
    assert(xz_audio.downlink_decode_idle.next == &queued1 && queued1.next == &queued2);
    assert(!queued2.next && !inflight.next && xz_playback_generation != old_generation);
    assert(!xz_audio_tts_drained()); /* in-flight decode remains owned by codec */
    xz_decode_inflight = false;
    assert(xz_audio_tts_drained());
    cache_bytes = 320; assert(!xz_audio_tts_drained()); cache_bytes = 0;
    uint8_t frame[320] = {0};
    audio_write_and_wait(&xz_audio, frame, 320, old_generation);
    assert(writes == 0);
    xz_audio.is_tx_enable = true; /* next TTS, old decoded block still rejected */
    audio_write_and_wait(&xz_audio, frame, 320, old_generation);
    assert(writes == 0);
    audio_write_and_wait(&xz_audio, frame, 320, xz_playback_generation);
    assert(writes == 1);
    write_result = 0; abort_on_delay = true;
    audio_write_and_wait(&xz_audio, frame, 320, xz_playback_generation);
    assert(writes == 2 && drops == 2 && !xz_audio.is_tx_enable && xz_playback_mutex.depth == 0);
    puts("PASS: actual playback stop: queued/cache discard, in-flight ownership, stale write rejection, abort during retry");
    return 0;
}
