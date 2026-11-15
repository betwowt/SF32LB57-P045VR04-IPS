/* Force-include ONLY in audio_mp3ctrl.c; SDK files stay unmodified. Include
 * declarations before mapping names so the real audio API remains available
 * to the application adapter and to every other audio client. */
#ifndef XIAOZHI_MUSIC_MP3_PORT_H
#define XIAOZHI_MUSIC_MP3_PORT_H
#include "xiaozhi_music_alloc.h"
#include "xiaozhi_music_source.h"
#define audio_open2 xz_music_source_open
#define audio_write xz_music_source_write
#endif
