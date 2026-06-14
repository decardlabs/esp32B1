#ifndef TASK_VOLC_ASR_H
#define TASK_VOLC_ASR_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "config_parser.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/** ASR queue item: either a WAV file path or an in-memory buffer */
typedef struct {
    bool     from_file;             /* true: read from wav_path; false: use data[] */
    char     wav_path[64];          /* valid when from_file == true */
    uint8_t *data;                  /* valid when from_file == false; owned by ASR after submit */
    size_t   data_len;              /* valid when from_file == false */
} asr_queue_item_t;

BaseType_t volc_asr_task_create(const config_t *cfg);
esp_err_t volc_asr_submit(const char *wav_path);
esp_err_t volc_asr_submit_data(const uint8_t *data, size_t len);

#endif
