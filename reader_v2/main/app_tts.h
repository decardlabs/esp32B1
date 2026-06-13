#ifndef APP_TTS_H
#define APP_TTS_H

#include <stdbool.h>
#include "esp_err.h"

/* Callback invoked when TTS finishes speaking */
typedef void (*tts_done_cb_t)(void);

/* TTS channel selection */
typedef enum {
    TTS_CHANNEL_LOCAL = 0,   /* esp_tts engine (always available) */
    TTS_CHANNEL_CLOUD,        /* Doubao cloud TTS via WiFi */
    TTS_CHANNEL_AUTO,         /* Cloud if WiFi+config OK, else local */
} tts_channel_t;

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

esp_err_t app_tts_set_channel(tts_channel_t ch);
tts_channel_t app_tts_get_channel(void);

/* I2S handle for shared use by cloud TTS */
void *app_tts_get_i2s_handle(void);

#endif /* APP_TTS_H */
