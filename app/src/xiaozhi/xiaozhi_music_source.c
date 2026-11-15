#include "xiaozhi_music_source.h"

static xz_music_source_stats_t source_stats;

audio_client_t xz_music_source_open(audio_type_t type, audio_rwflag_t flags,
    audio_parameter_t *parameters, audio_server_callback_func callback,
    void *userdata, audio_device_e device)
{
    audio_parameter_t owner;
    /* This adapter is compiled into audio_mp3ctrl.c only. Keep it TX-only:
     * TXRX would create a second mic owner and suspend the real microphone. */
    if (!parameters || type != AUDIO_TYPE_LOCAL_MUSIC || flags != AUDIO_TX ||
        (device != AUDIO_DEVICE_AUTO && device != AUDIO_DEVICE_SPEAKER) ||
        !xz_audio_music_parameters(&owner))
    {
        source_stats.error = -RT_EBUSY;
        rt_kprintf("[music] source reject rc=%d: live TXRX owner required\n", -RT_EBUSY);
        return RT_NULL;
    }

    audio_parameter_t source = *parameters;
    /* SDK speaker_open copies these fields even when need_init(tx,rx)=(0,0).
     * Preserve ALL of them, including 3A; preserving only RX channel count
     * avoids the assert but silently bypasses AEC in the active RX pipeline.
     * The write fields MUST stay at the decoder's original rate/channels:
     * audio_write_resample converts to the owner's existing TX mix format. */
    source.read_samplerate = owner.read_samplerate;
    source.read_channnel_num = owner.read_channnel_num;
    source.read_bits_per_sample = owner.read_bits_per_sample;
    source.read_which_mic = owner.read_which_mic;
    source.read_cache_size = owner.read_cache_size;
    source.is_need_3a = owner.is_need_3a;
    source.disable_uplink_agc = owner.disable_uplink_agc;
    rt_kprintf("[music] source TX=%lu/%u RX_KEEP=%lu/%u mic=%u 3a=%u\n",
        (unsigned long)source.write_samplerate, source.write_channnel_num,
        (unsigned long)source.read_samplerate, source.read_channnel_num,
        source.read_which_mic, source.is_need_3a);
    audio_client_t client = audio_open2(type, flags, &source, callback, userdata,
                                      AUDIO_DEVICE_SPEAKER);
    rt_base_t level = rt_hw_interrupt_disable();
    source_stats.opens++;
    source_stats.last_write = rt_tick_get();
    source_stats.error = client ? RT_EOK : -RT_ERROR;
    rt_hw_interrupt_enable(level);
    return client;
}

int xz_music_source_write(audio_client_t client, uint8_t *data, uint32_t size)
{
    int result = audio_write(client, data, size);
    rt_base_t level = rt_hw_interrupt_disable();
    if (result > 0)
    {
        source_stats.writes++;
        source_stats.bytes += result;
        source_stats.last_write = rt_tick_get();
    }
    else if (result < 0) source_stats.error = result;
    rt_hw_interrupt_enable(level);
    return result;
}

void xz_music_source_snapshot(xz_music_source_stats_t *stats)
{
    rt_base_t level = rt_hw_interrupt_disable();
    *stats = source_stats;
    rt_hw_interrupt_enable(level);
}
