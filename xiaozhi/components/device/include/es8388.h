#ifndef __ES8388_H__
#define __ES8388_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t es8388_init(uint8_t input_gain_db, uint8_t output_volume);
esp_err_t es8388_set_input_gain(uint8_t gain_db);
esp_err_t es8388_set_output_volume(uint8_t volume);
esp_err_t es8388_set_mute(bool enable);
esp_err_t es8388_speaker_enable(bool enable);

#endif
