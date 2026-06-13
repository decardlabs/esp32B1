#include "bsp_i2s.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "BSP_I2S";

static bool s_i2s_audio_initialized = false;
static i2s_chan_handle_t s_i2s_tx_handle = NULL;
static i2s_chan_handle_t s_i2s_rx_handle = NULL;

esp_err_t bsp_i2s0_audio_init(uint32_t sample_rate)
{
    esp_err_t ret;

    if (s_i2s_audio_initialized) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    ret = i2s_new_channel(&chan_cfg, &s_i2s_tx_handle, &s_i2s_rx_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "new i2s channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK_IO,
            .bclk = BSP_AUDIO_I2S_BCLK_IO,
            .ws = BSP_AUDIO_I2S_LRCK_IO,
            .dout = BSP_AUDIO_I2S_DOUT_IO,
            .din = BSP_AUDIO_I2S_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ret = i2s_channel_init_std_mode(s_i2s_tx_handle, &std_cfg);
    ESP_RETURN_ON_ERROR(ret, TAG, "init i2s tx failed");

    ret = i2s_channel_init_std_mode(s_i2s_rx_handle, &std_cfg);
    ESP_RETURN_ON_ERROR(ret, TAG, "init i2s rx failed");

    ret = i2s_channel_enable(s_i2s_tx_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "enable i2s tx failed");

    ret = i2s_channel_enable(s_i2s_rx_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "enable i2s rx failed");

    s_i2s_audio_initialized = true;
    ESP_LOGI(TAG, "i2s audio init done");
    return ESP_OK;
}

bool bsp_i2s0_audio_is_initialized(void)
{
    return s_i2s_audio_initialized;
}

i2s_chan_handle_t bsp_get_i2s0_tx_handle(void)
{
    return s_i2s_tx_handle;
}

i2s_chan_handle_t bsp_get_i2s0_rx_handle(void)
{
    return s_i2s_rx_handle;
}
