#ifndef __BSP_AUDIO_H__
#define __BSP_AUDIO_H__

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BSP_AUDIO_SAMPLE_RATE        24000
#define BSP_AUDIO_BITS_PER_SAMPLE    16
#define BSP_AUDIO_CHANNELS           2
#define BSP_AUDIO_FRAME_DURATION_MS  60
#define BSP_AUDIO_FRAME_SAMPLES      ((BSP_AUDIO_SAMPLE_RATE * BSP_AUDIO_FRAME_DURATION_MS) / 1000)

esp_err_t bsp_audio_init(uint32_t sample_rate);
esp_err_t bsp_audio_read(int16_t *data, size_t sample_count, size_t *samples_read, uint32_t timeout_ms);
esp_err_t bsp_audio_write(const int16_t *data, size_t sample_count, size_t *samples_written, uint32_t timeout_ms);

#endif
