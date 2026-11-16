#ifndef XIAOZHI_H
#define XIAOZHI_H

#ifdef __cplusplus
extern "C" {
#endif

void xz_mqtt_button_init(void);
void xz_audio_init(void);
void xz_speaker_open(xz_audio_t *thiz);
void xz_speaker_close(xz_audio_t *thiz);
void xz_mic_open(xz_audio_t *thiz);
void xz_mic_close(xz_audio_t *thiz);
void reinit_audio(void);
int xz_audio_request_wake_beep(uint32_t token);
void xz_audio_reset_uplink(void);
bool xz_audio_tts_drained(void);
bool xz_audio_wake_beep_active(void);
void xz_audio_reset_barge_in(void); /* synchronous, no heap/socket I/O */
bool xz_audio_barge_in_armed(uint32_t generation); /* lifecycle diagnostic */


#ifdef __cplusplus
}
#endif


#endif
