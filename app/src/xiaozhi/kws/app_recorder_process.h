#ifndef __KWS_APP_RECORDER_PROCESS_H
#define __KWS_APP_RECORDER_PROCESS_H

#include <stdio.h>
#include <stdint.h>

void kws_demo();
void kws_start_shared(void);
/* Shared post-AEC PCM, with microphone makeup gain already applied. */
void kws_feed_mic(const int16_t *data, uint32_t samples);
void kws_demo_stop();

#endif
