#include "xl9555.h"
#include "i2c_bus.h"
#include "board_pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static i2c_master_dev_handle_t s_dev = NULL;

/* Register map */
#define REG_INPUT_0     0x00
#define REG_INPUT_1     0x01
#define REG_OUTPUT_0    0x02
#define REG_OUTPUT_1    0x03
#define REG_CONFIG_0    0x06
#define REG_CONFIG_1    0x07

#define AUDIO_PORT      0

static esp_err_t reg_write(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    i2c_bus_lock();
    esp_err_t ret = i2c_master_transmit(s_dev, buf, 2, portMAX_DELAY);
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

static esp_err_t pin_set_dir_output(uint8_t port, uint8_t pin)
{
    uint8_t reg = (port == 0) ? REG_CONFIG_0 : REG_CONFIG_1;
    uint8_t val;
    esp_err_t ret = reg_read(reg, &val);
    if (ret != ESP_OK) return ret;
    val &= ~(1 << pin);
    return reg_write(reg, val);
}

static esp_err_t pin_set_level(uint8_t port, uint8_t pin, bool high)
{
    uint8_t reg = (port == 0) ? REG_OUTPUT_0 : REG_OUTPUT_1;
    uint8_t val;
    esp_err_t ret = reg_read(reg, &val);
    if (ret != ESP_OK) return ret;
    if (high) val |= (1 << pin);
    else      val &= ~(1 << pin);
    return reg_write(reg, val);
}

esp_err_t xl9555_init(void)
{
    if (s_dev != NULL) return ESP_OK;
    if (i2c_bus_get_handle() == NULL) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(i2c_bus_get_handle(), &cfg, &s_dev);
}

esp_err_t xl9555_speaker_enable(bool on)
{
    esp_err_t ret = pin_set_dir_output(AUDIO_PORT, XL9555_SPK_PIN);
    if (ret != ESP_OK) return ret;
    /* SPK_SD on XL9555 P0.0 — active LOW (low = enable speaker) */
    return pin_set_level(AUDIO_PORT, XL9555_SPK_PIN, on ? 0 : 1);
}
