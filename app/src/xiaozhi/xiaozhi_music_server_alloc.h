/* Only rt_calloc/rt_free are mapped here. audio_server.c maps audio_mem_*
 * to rt_*: client/command/CPU rings go to PSRAM, while audio_mem_malloc ->
 * rt_malloc stays internal for DMA staging. libc calloc/free (audio dump)
 * are untouched. See verification/music-rx/README.md for allocation pairs. */
#ifndef XIAOZHI_MUSIC_SERVER_ALLOC_H
#define XIAOZHI_MUSIC_SERVER_ALLOC_H
#include <rtthread.h>
void *xz_music_calloc(rt_size_t count, rt_size_t size);
void xz_music_free_any(void *ptr);
#define rt_calloc xz_music_calloc
#define rt_free xz_music_free_any
#endif
