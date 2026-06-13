#ifndef __BSP_I2S_H__
#define __BSP_I2S_H__

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

#define BSP_AUDIO_I2S_PORT      I2S_NUM_0
#define BSP_AUDIO_I2S_MCLK_IO   GPIO_NUM_3
#define BSP_AUDIO_I2S_BCLK_IO   GPIO_NUM_46
#define BSP_AUDIO_I2S_LRCK_IO   GPIO_NUM_9
// Board net names follow the ES8388 side:
// I2S_SDIN goes into ES8388 DSDIN, so it is ESP32 I2S data out.
// I2S_SDOUT comes from ES8388 ASDOUT, so it is ESP32 I2S data in.
#define BSP_AUDIO_I2S_DOUT_IO   GPIO_NUM_10
#define BSP_AUDIO_I2S_DIN_IO    GPIO_NUM_14

esp_err_t bsp_i2s0_audio_init(uint32_t sample_rate);
bool bsp_i2s0_audio_is_initialized(void);
i2s_chan_handle_t bsp_get_i2s0_tx_handle(void);
i2s_chan_handle_t bsp_get_i2s0_rx_handle(void);

#endif
