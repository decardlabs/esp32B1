#ifndef APP_CLOUD_TTS_H
#define APP_CLOUD_TTS_H

#include "esp_err.h"
#include <stdbool.h>

typedef void (*cloud_tts_done_cb_t)(void);

esp_err_t app_cloud_tts_init(void);
esp_err_t app_cloud_tts_speak(const char *text, cloud_tts_done_cb_t cb);
esp_err_t app_cloud_tts_prefetch(const char *text);
void app_cloud_tts_stop(void);
bool app_cloud_tts_is_busy(void);

#endif
