#ifndef TASK_VOLC_TTS_H
#define TASK_VOLC_TTS_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "config_parser.h"

BaseType_t volc_tts_task_create(const config_t *cfg);
esp_err_t volc_tts_speak(const char *text);

#endif
