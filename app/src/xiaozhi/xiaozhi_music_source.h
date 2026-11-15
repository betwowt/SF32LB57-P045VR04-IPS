#ifndef XIAOZHI_MUSIC_SOURCE_H
#define XIAOZHI_MUSIC_SOURCE_H
#include <audio_server.h>

/* Snapshot the live TXRX owner's parameters, never infer RX from MP3 PCM. */
bool xz_audio_music_parameters(audio_parameter_t *parameters);
uint32_t xz_audio_music_rx_frames(void);

typedef struct {
    uint32_t opens, writes, bytes;
    rt_tick_t last_write;
    int error;
} xz_music_source_stats_t;

audio_client_t xz_music_source_open(audio_type_t type, audio_rwflag_t flags,
    audio_parameter_t *parameters, audio_server_callback_func callback,
    void *userdata, audio_device_e device);
int xz_music_source_write(audio_client_t client, uint8_t *data, uint32_t size);
void xz_music_source_snapshot(xz_music_source_stats_t *stats);
#endif
