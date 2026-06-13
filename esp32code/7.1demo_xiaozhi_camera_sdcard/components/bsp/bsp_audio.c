#include "bsp_audio.h"
#include <inttypes.h>
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#define BSP_AUDIO_I2S_NUM    I2S_NUM_0
#define BSP_AUDIO_MCLK_GPIO  GPIO_NUM_3
#define BSP_AUDIO_BCLK_GPIO  GPIO_NUM_46
#define BSP_AUDIO_WS_GPIO    GPIO_NUM_9
#define BSP_AUDIO_DOUT_GPIO  GPIO_NUM_10
#define BSP_AUDIO_DIN_GPIO   GPIO_NUM_14

static const char *TAG = "BSP_AUDIO";

static i2s_chan_handle_t audio_tx_handle = NULL;
static i2s_chan_handle_t audio_rx_handle = NULL;
static uint32_t audio_sample_rate = 0;

esp_err_t bsp_audio_init(uint32_t sample_rate)
{
    if ((audio_tx_handle != NULL) && (audio_rx_handle != NULL)) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &audio_tx_handle, &audio_rx_handle), TAG, "i2s new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_MCLK_GPIO,
            .bclk = BSP_AUDIO_BCLK_GPIO,
            .ws = BSP_AUDIO_WS_GPIO,
            .dout = BSP_AUDIO_DOUT_GPIO,
            .din = BSP_AUDIO_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(audio_tx_handle, &std_cfg), TAG, "i2s tx std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(audio_rx_handle, &std_cfg), TAG, "i2s rx std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(audio_tx_handle), TAG, "i2s tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(audio_rx_handle), TAG, "i2s rx enable failed");

    audio_sample_rate = sample_rate;
    ESP_LOGI(TAG, "audio init ok, sample_rate=%" PRIu32, audio_sample_rate);
    return ESP_OK;
}

esp_err_t bsp_audio_read(int16_t *data, size_t sample_count, size_t *samples_read, uint32_t timeout_ms)
{
    size_t bytes_read = 0;
    esp_err_t ret;

    if (samples_read != NULL) {
        *samples_read = 0;
    }
    if (audio_rx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = i2s_channel_read(audio_rx_handle, data, sample_count * sizeof(int16_t), &bytes_read, timeout_ms);
    if ((ret == ESP_OK) && (samples_read != NULL)) {
        *samples_read = bytes_read / sizeof(int16_t);
    }
    return ret;
}

esp_err_t bsp_audio_write(const int16_t *data, size_t sample_count, size_t *samples_written, uint32_t timeout_ms)
{
    size_t bytes_written = 0;
    esp_err_t ret;

    if (samples_written != NULL) {
        *samples_written = 0;
    }
    if (audio_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = i2s_channel_write(audio_tx_handle, data, sample_count * sizeof(int16_t), &bytes_written, timeout_ms);
    if ((ret == ESP_OK) && (samples_written != NULL)) {
        *samples_written = bytes_written / sizeof(int16_t);
    }
    return ret;
}
