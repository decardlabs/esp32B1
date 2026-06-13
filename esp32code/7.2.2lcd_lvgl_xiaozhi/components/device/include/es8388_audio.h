#ifndef __ES8388_AUDIO_H__
#define __ES8388_AUDIO_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ES8388_AUDIO_SAMPLE_RATE       16000
#define ES8388_AUDIO_CHANNELS          1
#define ES8388_AUDIO_CODEC_CHANNELS    2
#define ES8388_AUDIO_BITS_PER_SAMPLE   16

esp_err_t es8388_audio_init(void);
bool es8388_audio_is_initialized(void);
esp_err_t es8388_audio_read(void *data, size_t data_len);
esp_err_t es8388_audio_write(void *data, size_t data_len);
esp_err_t es8388_audio_read_mono(void *data, size_t data_len);
esp_err_t es8388_audio_write_mono(const void *data, size_t data_len);
esp_err_t es8388_audio_set_output_volume(int volume);
esp_err_t es8388_audio_set_input_gain(float gain_db);
esp_err_t es8388_audio_speaker_set(bool on);

#endif
