#ifndef TASK_AUDIO_CAPTURE_H
#define TASK_AUDIO_CAPTURE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "config_parser.h"

typedef enum {
    AUDIO_IDLE,
    AUDIO_RECORDING,
} audio_capture_state_t;

audio_capture_state_t audio_capture_get_state(void);
const char *audio_capture_get_last_file(void);
BaseType_t audio_capture_task_create(const config_t *cfg);

#endif
