#ifndef TASK_VOLC_ASR_H
#define TASK_VOLC_ASR_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "config_parser.h"

BaseType_t volc_asr_task_create(const config_t *cfg);
esp_err_t volc_asr_submit(const char *wav_path);

#endif
