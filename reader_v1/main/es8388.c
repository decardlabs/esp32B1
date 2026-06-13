#include "es8388.h"
#include "i2c_bus.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ES8388";

static i2c_master_dev_handle_t s_dev = NULL;

/* ES8388 register map */
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

static esp_err_t reg_write(uint8_t reg, uint8_t data)
{
    uint8_t tx_buf[2] = {reg, data};
    i2c_bus_lock();
    esp_err_t ret = i2c_master_transmit(s_dev, tx_buf, sizeof(tx_buf), portMAX_DELAY);
    i2c_bus_unlock();
    return ret;
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data)
{
    i2c_bus_lock();
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, data, 1, portMAX_DELAY);
    i2c_bus_unlock();
    return ret;
}

static esp_err_t add_device(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(i2c_bus_get_handle() != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c bus not ready");

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8388_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    return i2c_master_bus_add_device(i2c_bus_get_handle(), &cfg, &s_dev);
}

esp_err_t es8388_set_volume(int vol_db)
{
    /* Map dB to attenuation register: 0x00 = 0dB, 0xC0 = -96dB */
    if (vol_db < -96) vol_db = -96;
    if (vol_db > 0) vol_db = 0;

    uint8_t dac_reg = (uint8_t)((-vol_db) * 2); /* 0.5dB per step */

    esp_err_t ret = ESP_OK;
    ret |= reg_write(ES8388_DACCONTROL4, dac_reg);
    ret |= reg_write(ES8388_DACCONTROL5, dac_reg);
    /* Line/speaker mixer output gain: 0x1E = 0dB */
    ret |= reg_write(ES8388_DACCONTROL24, 0x1E);
    ret |= reg_write(ES8388_DACCONTROL25, 0x1E);
    ret |= reg_write(ES8388_DACCONTROL26, 0x1E);
    ret |= reg_write(ES8388_DACCONTROL27, 0x1E);
    return ret;
}

esp_err_t es8388_mute(bool mute)
{
    uint8_t reg = 0;
    esp_err_t ret = reg_read(ES8388_DACCONTROL3, &reg);
    if (ret != ESP_OK) return ret;

    reg &= 0xFB;
    reg |= ((uint8_t)mute << 2);
    return reg_write(ES8388_DACCONTROL3, reg);
}

esp_err_t es8388_init(void)
{
    ESP_RETURN_ON_ERROR(add_device(), TAG, "add i2c device failed");

    esp_err_t ret = ESP_OK;

    ret |= reg_write(ES8388_DACCONTROL3, 0x04);
    ret |= reg_write(ES8388_CONTROL2, 0x50);
    ret |= reg_write(ES8388_CHIPPOWER, 0x00);

    ret |= reg_write(ES8388_CONTROL_EXT_35, 0xA0);
    ret |= reg_write(ES8388_CONTROL_EXT_37, 0xD0);
    ret |= reg_write(ES8388_CONTROL_EXT_39, 0xD0);

    ret |= reg_write(ES8388_MASTERMODE, 0x00); /* codec slave */

    ret |= reg_write(ES8388_DACPOWER, 0xC0);
    ret |= reg_write(ES8388_CONTROL1, 0x12);
    ret |= reg_write(ES8388_DACCONTROL1, 0x18); /* 16-bit I2S */
    ret |= reg_write(ES8388_DACCONTROL2, 0x02); /* single speed, ratio 256 */
    ret |= reg_write(ES8388_DACCONTROL16, 0x00);
    ret |= reg_write(ES8388_DACCONTROL17, 0x90);
    ret |= reg_write(ES8388_DACCONTROL20, 0x90);
    ret |= reg_write(ES8388_DACCONTROL21, 0x80);
    ret |= reg_write(ES8388_DACCONTROL23, 0x00);
    ret |= reg_write(ES8388_DACPOWER, 0x3C);

    ret |= reg_write(ES8388_ADCPOWER, 0xFF);
    ret |= reg_write(ES8388_ADCCONTROL1, 0x00);
    ret |= reg_write(ES8388_ADCCONTROL2, 0x00);
    ret |= reg_write(ES8388_ADCCONTROL3, 0x02);
    ret |= reg_write(ES8388_ADCCONTROL4, 0x0C);
    ret |= reg_write(ES8388_ADCCONTROL5, 0x02);
    ret |= reg_write(ES8388_ADCCONTROL8, 0x00);
    ret |= reg_write(ES8388_ADCCONTROL9, 0x00);
    ret |= reg_write(ES8388_ADCPOWER, 0x09);

    ret |= es8388_set_volume(-12);
    ret |= es8388_mute(false);

    ESP_RETURN_ON_ERROR(ret, TAG, "codec init failed");
    ESP_LOGI(TAG, "ES8388 initialized");
    return ESP_OK;
}

esp_err_t es8388_start_playback(void)
{
    return es8388_mute(false);
}

esp_err_t es8388_stop_playback(void)
{
    return es8388_mute(true);
}
