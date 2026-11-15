/* Forced include only for three SDK software MP3 sources; no global heap
 * overrides and no changes to the audio server's DMA allocations. */
#ifndef XIAOZHI_MUSIC_ALLOC_H
#define XIAOZHI_MUSIC_ALLOC_H
#include <rtthread.h>
#include <stdlib.h>
#include <audio_mem.h>
void *xz_music_alloc(rt_size_t size);
void *xz_music_calloc(rt_size_t count, rt_size_t size);
void xz_music_free(void *ptr);
#define rt_malloc xz_music_alloc
#define rt_calloc xz_music_calloc
#define rt_free xz_music_free
#undef audio_mem_malloc
#undef audio_mem_calloc
#undef audio_mem_free
#define audio_mem_malloc xz_music_alloc
#define audio_mem_calloc xz_music_calloc
#define audio_mem_free xz_music_free
#endif
