#ifndef APP_TTS_H
#define APP_TTS_H

#include <stdbool.h>
#include "esp_err.h"

/* Callback invoked when TTS finishes speaking */
typedef void (*tts_done_cb_t)(void);

esp_err_t app_tts_init(void);
esp_err_t app_tts_speak(const char *text);
esp_err_t app_tts_speak_cb(const char *text, tts_done_cb_t cb);
void app_tts_stop(void);
bool app_tts_is_busy(void);

/* Volume: -48 (mute) ~ 0 (max), in dB */
esp_err_t app_tts_set_volume(int vol_db);
int app_tts_get_volume(void);

/* Speed: 0 (slowest) ~ 5 (fastest) */
esp_err_t app_tts_set_speed(int speed);
int app_tts_get_speed(void);

void app_tts_play_beep(void);

#endif /* APP_TTS_H */
