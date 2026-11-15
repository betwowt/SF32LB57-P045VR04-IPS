#ifndef XIAOZHI_MUSIC_H
#define XIAOZHI_MUSIC_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Mount/scan/play execute on xz_music and return their result to the caller.
 * scan returns the track count; play(NULL) starts the first track, then loops. */
int xz_music_mount(void);
int xz_music_scan(void);
int xz_music_play(const char *keyword);
/* Latches a stop immediately; worker reports "stopped" after cleanup. */
void xz_music_stop(void);
/* Only for explicit RX owner teardown; waits for safe music cleanup. */
void xz_music_audio_shutdown(void (*disable_rx_snapshot)(void));
/* False while paused for TTS. */
bool xz_music_is_playing(void);
/* Called before opening TTS output, and after closing/flushing it. */
void xz_music_tts(bool speaking);
/* Nonblocking local intent dispatch; does not consume the original STT. */
bool xz_music_handle_text(const char *text);
#ifdef __cplusplus
}
#endif
#endif
