/* Compile the production adapter against the REAL SDK audio_server.h.
 * Model its unconditional RX copies with a live PDM/3A owner. */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "xiaozhi_music_source.h"

static audio_parameter_t owner, submitted;
static bool live = true;
static unsigned opens;
static int write_result;
static rt_tick_t ticks = 42;
static const audio_client_t client = (audio_client_t)(uintptr_t)0x1234;
static void *const token = (void *)(uintptr_t)0x5678;
static audio_server_callback_func saved_callback;
static void *saved_userdata;

rt_base_t rt_hw_interrupt_disable(void) { return 0; }
void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
rt_tick_t rt_tick_get(void) { return ticks; }
void rt_kprintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
bool xz_audio_music_parameters(audio_parameter_t *parameters)
{
    if (live) *parameters = owner;
    return live;
}
static bool corrupts_live_rx(const audio_parameter_t *parameters)
{
    return parameters->read_channnel_num != owner.read_channnel_num ||
        parameters->read_samplerate != owner.read_samplerate ||
        parameters->read_which_mic != owner.read_which_mic ||
        parameters->is_need_3a != owner.is_need_3a;
}
static int callback(audio_server_callback_cmt_t cmd, void *userdata, uint32_t reserved)
{
    (void)cmd; (void)userdata; (void)reserved;
    return 0;
}
audio_client_t audio_open2(audio_type_t type, audio_rwflag_t flags,
    audio_parameter_t *parameters, audio_server_callback_func cb,
    void *userdata, audio_device_e device)
{
    assert(type == AUDIO_TYPE_LOCAL_MUSIC);
    assert(flags == AUDIO_TX); /* A second RX owner would suspend the real mic. */
    assert(device == AUDIO_DEVICE_SPEAKER);
    assert(!corrupts_live_rx(parameters));
    assert(parameters->read_bits_per_sample == owner.read_bits_per_sample);
    assert(parameters->read_cache_size == owner.read_cache_size);
    assert(parameters->disable_uplink_agc == owner.disable_uplink_agc);
    submitted = *parameters;
    saved_callback = cb;
    saved_userdata = userdata;
    opens++;
    return client;
}
int audio_write(audio_client_t target, uint8_t *data, uint32_t size)
{
    assert(target == client && data && size == 4608);
    return write_result;
}

int main(void)
{
    owner.write_samplerate = owner.read_samplerate = 16000;
    owner.write_channnel_num = owner.read_channnel_num = 1;
    owner.read_bits_per_sample = 16;
    owner.is_need_3a = owner.disable_uplink_agc = 1;
    owner.read_which_mic = AUDIO_MIC0_ONLY;
    audio_parameter_t mp3 = {0};
    mp3.write_samplerate = 44100;
    mp3.write_channnel_num = 2;
    mp3.write_bits_per_sample = 16;
    mp3.write_cache_size = 13824;
    audio_parameter_t original = mp3;
    assert(corrupts_live_rx(&mp3)); /* Reproduces the old zero-RX/3A bug. */

    /* Standby and Listening use the same live owner. Repeated opens also
     * cover sources rejoining after pause and on next-track format changes. */
    for (unsigned state = 0; state < 2; state++)
        for (unsigned track = 0; track < 8; track++)
        {
            mp3.write_samplerate = track & 1 ? 22050 : 44100;
            mp3.write_channnel_num = track & 1 ? 1 : 2;
            audio_parameter_t before = mp3;
            assert(xz_music_source_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX,
                &mp3, callback, token, AUDIO_DEVICE_AUTO) == client);
            assert(!memcmp(&mp3, &before, sizeof(mp3)));
            assert(submitted.write_samplerate == before.write_samplerate);
            assert(submitted.write_channnel_num == before.write_channnel_num);
            assert(submitted.write_cache_size == before.write_cache_size);
            assert(submitted.write_bits_per_sample == before.write_bits_per_sample);
            assert(saved_callback == callback && saved_userdata == token);
        }
    /* Copy a changed live mic configuration rather than hardcoding mono. */
    owner.read_channnel_num = 2;
    owner.read_which_mic = AUDIO_MIC_ALL;
    owner.read_samplerate = 8000;
    assert(xz_music_source_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX,
        &original, callback, token, AUDIO_DEVICE_SPEAKER) == client);
    assert(opens == 17);

    uint8_t pcm[4608] = {0};
    write_result = 0;
    assert(xz_music_source_write(client, pcm, sizeof(pcm)) == 0);
    write_result = -1;
    assert(xz_music_source_write(client, pcm, sizeof(pcm)) == -1);
    ticks = 123;
    write_result = sizeof(pcm);
    assert(xz_music_source_write(client, pcm, sizeof(pcm)) == sizeof(pcm));
    xz_music_source_stats_t stats;
    xz_music_source_snapshot(&stats);
    assert(stats.writes == 1 && stats.bytes == sizeof(pcm) && stats.last_write == ticks);
    assert(stats.error == -1); /* Retains last failure for diagnosis. */

    live = false;
    assert(!xz_music_source_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX,
        &original, callback, token, AUDIO_DEVICE_AUTO));
    live = true;
    assert(!xz_music_source_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TXRX,
        &original, callback, token, AUDIO_DEVICE_AUTO));
    assert(!xz_music_source_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX,
        &original, callback, token, AUDIO_DEVICE_HFP));
    assert(opens == 17);
    puts("PASS: live RX/3A preserved, TX format retained, TX-only, repeated opens, progress and rejection");
    return 0;
}
