#include "es8388.h"
#include "bsp_i2c.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "xl9555.h"

#define ES8388_I2C_ADDR         0x10

#define ES8388_CONTROL1         0x00
#define ES8388_CONTROL2         0x01
#define ES8388_CHIPPOWER        0x02
#define ES8388_ADCPOWER         0x03
#define ES8388_DACPOWER         0x04
#define ES8388_MASTERMODE       0x08
#define ES8388_ADCCONTROL1      0x09
#define ES8388_ADCCONTROL2      0x0A
#define ES8388_ADCCONTROL3      0x0B
#define ES8388_ADCCONTROL4      0x0C
#define ES8388_ADCCONTROL5      0x0D
#define ES8388_ADCCONTROL8      0x10
#define ES8388_ADCCONTROL9      0x11
#define ES8388_DACCONTROL1      0x17
#define ES8388_DACCONTROL2      0x18
#define ES8388_DACCONTROL3      0x19
#define ES8388_DACCONTROL4      0x1A
#define ES8388_DACCONTROL5      0x1B
#define ES8388_DACCONTROL16     0x26
#define ES8388_DACCONTROL17     0x27
#define ES8388_DACCONTROL20     0x2A
#define ES8388_DACCONTROL21     0x2B
#define ES8388_DACCONTROL23     0x2D
#define ES8388_DACCONTROL24     0x2E
#define ES8388_DACCONTROL25     0x2F
#define ES8388_DACCONTROL26     0x30
#define ES8388_DACCONTROL27     0x31
#define ES8388_CONTROL_EXT_35   0x35
#define ES8388_CONTROL_EXT_37   0x37
#define ES8388_CONTROL_EXT_39   0x39

static const char *TAG = "ES8388";

static i2c_master_dev_handle_t es8388_dev_handle = NULL;

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t tx_buf[2] = {reg, data};
    esp_err_t ret;

    bsp_i2c0_lock();
    ret = i2c_master_transmit(es8388_dev_handle, tx_buf, sizeof(tx_buf), portMAX_DELAY);
    bsp_i2c0_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write reg 0x%02X failed", reg);
    }
    return ret;
}

static esp_err_t es8388_read_reg(uint8_t reg, uint8_t *data)
{
    esp_err_t ret;

    bsp_i2c0_lock();
    ret = i2c_master_transmit_receive(es8388_dev_handle, &reg, 1, data, 1, portMAX_DELAY);
    bsp_i2c0_unlock();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read reg 0x%02X failed", reg);
    }
    return ret;
}

static esp_err_t es8388_add_device(void)
{
    i2c_master_bus_handle_t bus_handle;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8388_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    if (es8388_dev_handle != NULL) {
        return ESP_OK;
    }

    bus_handle = bsp_get_i2c0_bus_handle();
    return i2c_master_bus_add_device(bus_handle, &dev_cfg, &es8388_dev_handle);
}

esp_err_t es8388_set_input_gain(uint8_t gain_db)
{
    uint8_t gain_reg;

    gain_db = (gain_db / 3) * 3;
    if (gain_db > 36) {
        gain_db = 36;
    }

    gain_reg = (gain_db / 3);
    gain_reg = (gain_reg << 4) | gain_reg;
    return es8388_write_reg(ES8388_ADCCONTROL1, gain_reg);
}

esp_err_t es8388_set_output_volume(uint8_t volume)
{
    uint8_t dac_reg;
    esp_err_t ret;

    if (volume > 100) {
        volume = 100;
    }

    dac_reg = (uint8_t)(((100 - volume) * 192) / 100);
    ret = es8388_write_reg(ES8388_DACCONTROL4, dac_reg);
    ret |= es8388_write_reg(ES8388_DACCONTROL5, dac_reg);
    ret |= es8388_write_reg(ES8388_DACCONTROL24, 0x1E);
    ret |= es8388_write_reg(ES8388_DACCONTROL25, 0x1E);
    ret |= es8388_write_reg(ES8388_DACCONTROL26, 0x1E);
    ret |= es8388_write_reg(ES8388_DACCONTROL27, 0x1E);
    return ret;
}

esp_err_t es8388_set_mute(bool enable)
{
    uint8_t reg = 0;
    esp_err_t ret;

    ret = es8388_read_reg(ES8388_DACCONTROL3, &reg);
    if (ret != ESP_OK) {
        return ret;
    }

    reg &= 0xFB;
    reg |= ((uint8_t)enable << 2);
    return es8388_write_reg(ES8388_DACCONTROL3, reg);
}

esp_err_t es8388_speaker_enable(bool enable)
{
    /* MD8002A SHUTDOWN is active-high: drive low to enable the speaker amp. */
    xl9555_set_pin_level(SPK_PORT, SPK_PIN, enable ? 0 : 1);
    return ESP_OK;
}

esp_err_t es8388_init(uint8_t input_gain_db, uint8_t output_volume)
{
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(es8388_add_device(), TAG, "add i2c device failed");

    ret = es8388_write_reg(ES8388_DACCONTROL3, 0x04);
    ret |= es8388_write_reg(ES8388_CONTROL2, 0x50);
    ret |= es8388_write_reg(ES8388_CHIPPOWER, 0x00);

    ret |= es8388_write_reg(ES8388_CONTROL_EXT_35, 0xA0);
    ret |= es8388_write_reg(ES8388_CONTROL_EXT_37, 0xD0);
    ret |= es8388_write_reg(ES8388_CONTROL_EXT_39, 0xD0);

    ret |= es8388_write_reg(ES8388_MASTERMODE, 0x00);

    ret |= es8388_write_reg(ES8388_DACPOWER, 0xC0);
    ret |= es8388_write_reg(ES8388_CONTROL1, 0x12);
    ret |= es8388_write_reg(ES8388_DACCONTROL1, 0x18);
    ret |= es8388_write_reg(ES8388_DACCONTROL2, 0x02);
    ret |= es8388_write_reg(ES8388_DACCONTROL16, 0x00);
    ret |= es8388_write_reg(ES8388_DACCONTROL17, 0x90);
    ret |= es8388_write_reg(ES8388_DACCONTROL20, 0x90);
    ret |= es8388_write_reg(ES8388_DACCONTROL21, 0x80);
    ret |= es8388_write_reg(ES8388_DACCONTROL23, 0x00);
    ret |= es8388_write_reg(ES8388_DACPOWER, 0x3C);

    ret |= es8388_write_reg(ES8388_ADCPOWER, 0xFF);
    ret |= es8388_set_input_gain(input_gain_db);
    ret |= es8388_write_reg(ES8388_ADCCONTROL2, 0x00);
    ret |= es8388_write_reg(ES8388_ADCCONTROL3, 0x02);
    ret |= es8388_write_reg(ES8388_ADCCONTROL4, 0x0C);
    ret |= es8388_write_reg(ES8388_ADCCONTROL5, 0x02);
    ret |= es8388_write_reg(ES8388_ADCCONTROL8, 0x00);
    ret |= es8388_write_reg(ES8388_ADCCONTROL9, 0x00);
    ret |= es8388_write_reg(ES8388_ADCPOWER, 0x09);

    ret |= es8388_set_output_volume(output_volume);
    ret |= es8388_set_mute(false);
    ret |= es8388_speaker_enable(false);

    ESP_RETURN_ON_ERROR(ret, TAG, "codec init failed");
    ESP_LOGI(TAG, "es8388 init ok");
    return ESP_OK;
}
