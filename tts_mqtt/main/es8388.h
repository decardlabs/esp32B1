#ifndef ES8388_H
#define ES8388_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t es8388_init(void);
esp_err_t es8388_set_volume(int vol_db);     /* -96 (mute) to 0 (max dB) */
esp_err_t es8388_start_playback(void);
esp_err_t es8388_stop_playback(void);
esp_err_t es8388_mute(bool mute);

#endif /* ES8388_H */
