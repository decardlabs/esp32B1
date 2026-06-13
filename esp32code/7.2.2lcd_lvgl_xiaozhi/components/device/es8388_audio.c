#include "es8388_audio.h"
#include "bsp_i2c.h"
#include "bsp_i2s.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "es8388_codec.h"
#include "xl9555.h"

#define ES8388_AUDIO_DEFAULT_VOLUME    90
#define ES8388_AUDIO_DEFAULT_GAIN_DB   30.0f
#define ES8388_AUDIO_FRAME_MAX_MS      60
#define ES8388_AUDIO_MONO_FRAME_MAX_SAMPLES \
    (ES8388_AUDIO_SAMPLE_RATE * ES8388_AUDIO_FRAME_MAX_MS / 1000)

#define ES8388_DACPOWER                0x04
#define ES8388_DACCONTROL3             0x19
#define ES8388_DACCONTROL24            0x2E
#define ES8388_DACCONTROL25            0x2F
#define ES8388_DACCONTROL26            0x30
#define ES8388_DACCONTROL27            0x31
#define ES8388_OUTPUT_VOLUME_0DB       0x1E
#define ES8388_DAC_OUTPUT_ALL          0x3C
#define ES8388_DAC_MUTE_BIT            0x04

static const char *TAG = "ES8388_AUDIO";

static const audio_codec_data_if_t *s_audio_data_if = NULL;
static const audio_codec_ctrl_if_t *s_audio_ctrl_if = NULL;
static const audio_codec_gpio_if_t *s_audio_gpio_if = NULL;
static const audio_codec_if_t *s_es8388_codec_if = NULL;
static esp_codec_dev_handle_t s_es8388_dev = NULL;
static int16_t s_read_stereo_buf[ES8388_AUDIO_MONO_FRAME_MAX_SAMPLES * ES8388_AUDIO_CODEC_CHANNELS];
static int16_t s_write_stereo_buf[ES8388_AUDIO_MONO_FRAME_MAX_SAMPLES * ES8388_AUDIO_CODEC_CHANNELS];

static esp_err_t es8388_audio_check_ret(int ret, const char *msg)
{
    if (ret == ESP_CODEC_DEV_OK) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "%s failed: %d", msg, ret);
    return ESP_FAIL;
}

static esp_err_t es8388_audio_write_reg(uint8_t reg, uint8_t data)
{
    int value = data;

    ESP_RETURN_ON_FALSE(s_audio_ctrl_if != NULL, ESP_ERR_INVALID_STATE, TAG, "audio ctrl not initialized");
    return es8388_audio_check_ret(s_audio_ctrl_if->write_reg(s_audio_ctrl_if, reg, 1, &value, 1), "write codec reg");
}

static esp_err_t es8388_audio_read_reg(uint8_t reg, uint8_t *data)
{
    int value = 0;

    ESP_RETURN_ON_FALSE(s_audio_ctrl_if != NULL, ESP_ERR_INVALID_STATE, TAG, "audio ctrl not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid reg read buffer");

    ESP_RETURN_ON_ERROR(es8388_audio_check_ret(s_audio_ctrl_if->read_reg(s_audio_ctrl_if, reg, 1, &value, 1),
                                               "read codec reg"),
                        TAG,
                        "read codec reg failed");
    *data = (uint8_t)value;
    return ESP_OK;
}

static esp_err_t es8388_audio_config_speaker_route(void)
{
    uint8_t mute_reg = 0;
    esp_err_t ret;

    ret = es8388_audio_write_reg(ES8388_DACPOWER, ES8388_DAC_OUTPUT_ALL);
    ret |= es8388_audio_write_reg(ES8388_DACCONTROL24, ES8388_OUTPUT_VOLUME_0DB);
    ret |= es8388_audio_write_reg(ES8388_DACCONTROL25, ES8388_OUTPUT_VOLUME_0DB);
    ret |= es8388_audio_write_reg(ES8388_DACCONTROL26, ES8388_OUTPUT_VOLUME_0DB);
    ret |= es8388_audio_write_reg(ES8388_DACCONTROL27, ES8388_OUTPUT_VOLUME_0DB);

    if (es8388_audio_read_reg(ES8388_DACCONTROL3, &mute_reg) == ESP_OK) {
        mute_reg &= ~ES8388_DAC_MUTE_BIT;
        ret |= es8388_audio_write_reg(ES8388_DACCONTROL3, mute_reg);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "speaker route enabled");
    }
    return ret;
}

esp_err_t es8388_audio_init(void)
{
    if (s_es8388_dev != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(bsp_get_i2c0_bus_handle() != NULL, ESP_ERR_INVALID_STATE, TAG, "call bsp_i2c0_init() first");
    ESP_RETURN_ON_ERROR(xl9555_audio_gpio_init(), TAG, "audio gpio init failed");
    ESP_RETURN_ON_ERROR(bsp_i2s0_audio_init(ES8388_AUDIO_SAMPLE_RATE), TAG, "i2s init failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .rx_handle = bsp_get_i2s0_rx_handle(),
        .tx_handle = bsp_get_i2s0_tx_handle(),
    };
    s_audio_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_audio_data_if != NULL, ESP_ERR_NO_MEM, TAG, "new i2s data if failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8388_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_get_i2c0_bus_handle(),
    };
    s_audio_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_audio_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "new i2c ctrl if failed");

    s_audio_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_audio_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "new gpio if failed");

    es8388_codec_cfg_t es8388_cfg = {
        .ctrl_if = s_audio_ctrl_if,
        .gpio_if = s_audio_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .pa_pin = -1,
        .pa_reverted = false,
    };
    s_es8388_codec_if = es8388_codec_new(&es8388_cfg);
    ESP_RETURN_ON_FALSE(s_es8388_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "new es8388 codec if failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = s_es8388_codec_if,
        .data_if = s_audio_data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
    };
    s_es8388_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_es8388_dev != NULL, ESP_ERR_NO_MEM, TAG, "new es8388 dev failed");

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = ES8388_AUDIO_BITS_PER_SAMPLE,
        .channel = ES8388_AUDIO_CODEC_CHANNELS,
        .sample_rate = ES8388_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_ERROR(es8388_audio_check_ret(esp_codec_dev_open(s_es8388_dev, &sample_info), "open codec"),
                        TAG, "open codec failed");
    ESP_RETURN_ON_ERROR(es8388_audio_set_output_volume(ES8388_AUDIO_DEFAULT_VOLUME), TAG, "set volume failed");
    ESP_RETURN_ON_ERROR(es8388_audio_config_speaker_route(), TAG, "speaker route config failed");
    ESP_RETURN_ON_ERROR(es8388_audio_set_input_gain(ES8388_AUDIO_DEFAULT_GAIN_DB), TAG, "set gain failed");
    ESP_RETURN_ON_ERROR(es8388_audio_speaker_set(true), TAG, "speaker enable failed");

    ESP_LOGI(TAG, "es8388 audio init done");
    return ESP_OK;
}

bool es8388_audio_is_initialized(void)
{
    return s_es8388_dev != NULL;
}

esp_err_t es8388_audio_read(void *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(s_es8388_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    ESP_RETURN_ON_FALSE((data != NULL) && (data_len > 0), ESP_ERR_INVALID_ARG, TAG, "invalid read buffer");

    return es8388_audio_check_ret(esp_codec_dev_read(s_es8388_dev, data, (int)data_len), "read pcm");
}

esp_err_t es8388_audio_write(void *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(s_es8388_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    ESP_RETURN_ON_FALSE((data != NULL) && (data_len > 0), ESP_ERR_INVALID_ARG, TAG, "invalid write buffer");

    return es8388_audio_check_ret(esp_codec_dev_write(s_es8388_dev, data, (int)data_len), "write pcm");
}

esp_err_t es8388_audio_read_mono(void *data, size_t data_len)
{
    int16_t *mono_data = (int16_t *)data;
    size_t mono_samples = data_len / sizeof(int16_t);
    size_t stereo_len = mono_samples * ES8388_AUDIO_CODEC_CHANNELS * sizeof(int16_t);

    ESP_RETURN_ON_FALSE((data != NULL) && (data_len > 0), ESP_ERR_INVALID_ARG, TAG, "invalid mono read buffer");
    ESP_RETURN_ON_FALSE((data_len % sizeof(int16_t)) == 0, ESP_ERR_INVALID_ARG, TAG, "mono read size not aligned");
    ESP_RETURN_ON_FALSE(mono_samples <= ES8388_AUDIO_MONO_FRAME_MAX_SAMPLES,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "mono read frame too large");

    ESP_RETURN_ON_ERROR(es8388_audio_read(s_read_stereo_buf, stereo_len), TAG, "read stereo pcm failed");
    for (size_t i = 0; i < mono_samples; i++) {
        mono_data[i] = s_read_stereo_buf[i * ES8388_AUDIO_CODEC_CHANNELS];
    }

    return ESP_OK;
}

esp_err_t es8388_audio_write_mono(const void *data, size_t data_len)
{
    const int16_t *mono_data = (const int16_t *)data;
    size_t mono_samples = data_len / sizeof(int16_t);
    size_t stereo_len = mono_samples * ES8388_AUDIO_CODEC_CHANNELS * sizeof(int16_t);

    ESP_RETURN_ON_FALSE((data != NULL) && (data_len > 0), ESP_ERR_INVALID_ARG, TAG, "invalid mono write buffer");
    ESP_RETURN_ON_FALSE((data_len % sizeof(int16_t)) == 0, ESP_ERR_INVALID_ARG, TAG, "mono write size not aligned");
    ESP_RETURN_ON_FALSE(mono_samples <= ES8388_AUDIO_MONO_FRAME_MAX_SAMPLES,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "mono write frame too large");

    for (size_t i = 0; i < mono_samples; i++) {
        s_write_stereo_buf[i * ES8388_AUDIO_CODEC_CHANNELS] = mono_data[i];
        s_write_stereo_buf[i * ES8388_AUDIO_CODEC_CHANNELS + 1] = mono_data[i];
    }

    return es8388_audio_write(s_write_stereo_buf, stereo_len);
}

esp_err_t es8388_audio_set_output_volume(int volume)
{
    ESP_RETURN_ON_FALSE(s_es8388_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    return es8388_audio_check_ret(esp_codec_dev_set_out_vol(s_es8388_dev, volume), "set output volume");
}

esp_err_t es8388_audio_set_input_gain(float gain_db)
{
    ESP_RETURN_ON_FALSE(s_es8388_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");

    return es8388_audio_check_ret(esp_codec_dev_set_in_gain(s_es8388_dev, gain_db), "set input gain");
}

esp_err_t es8388_audio_speaker_set(bool on)
{
    return xl9555_speaker_set(on);
}
