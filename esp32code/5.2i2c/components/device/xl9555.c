#include "xl9555.h"

static const char *TAG = "XL9555";


i2c_master_bus_handle_t xl9555_i2c0_bus_handle = NULL;
i2c_master_dev_handle_t xl9555_dev_handle = NULL;



esp_err_t xl9555_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t tx_buf[2] = {reg, data};
    bsp_i2c0_lock();
    esp_err_t ret = i2c_master_transmit(xl9555_dev_handle, tx_buf, sizeof(tx_buf), portMAX_DELAY);
    bsp_i2c0_unlock();
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C Write Reg 0x%02x Failed! 0x%02x", reg, data);
    }
    return ret;
}

esp_err_t xl9555_read_reg(uint8_t reg, uint8_t *data)
{
    bsp_i2c0_lock();
    esp_err_t ret = i2c_master_transmit_receive(xl9555_dev_handle, &reg, 1, data, 1, portMAX_DELAY);
    bsp_i2c0_unlock();
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C Read Reg 0x%02x Failed!", reg);
    }
    return ret;
}

void xl9555_set_pin_dir(uint8_t port, uint8_t pin, bool dir)
{
    uint8_t reg_addr = 0x00, pin_value;
    
    if(port > 1 || pin > 7)
    {
        ESP_LOGE(TAG, "Invalid Port or Pin!");
        return;
    }
    
    if(port ==0)
    {
       reg_addr = XL9555_REG_CONFIG_0;
    }
    else
    {
       reg_addr = XL9555_REG_CONFIG_1;
    }
    xl9555_read_reg(reg_addr, &pin_value);
    if(dir)
    {
        pin_value |= (1 << pin);
    }
    else
    {
        pin_value &= ~(1 << pin);
    }

    xl9555_write_reg(reg_addr, pin_value);
}

void xl9555_set_pin_level(uint8_t port, uint8_t pin, bool level)
{
    uint8_t reg_addr = 0x00, pin_value;
    
    if(port > 1 || pin > 7)
    {
        ESP_LOGE(TAG, "Invalid Port or Pin!");
        return;
    }
    if(port ==0)
    {
       reg_addr = XL9555_REG_OUTPUT_0;
    }
    else
    {
       reg_addr = XL9555_REG_OUTPUT_1;
    }
    xl9555_read_reg(reg_addr, &pin_value);
    if(level)
    {
        pin_value |= (1 << pin);
    }
    else
    {
        pin_value &= ~(1 << pin);
    }

    xl9555_write_reg(reg_addr, pin_value);
}

void xl9555_get_pin_level(uint8_t port, uint8_t pin, bool *level)
{
    uint8_t reg_addr = 0x00, pin_value;
    
    if(port > 1 || pin > 7)
    {
        ESP_LOGE(TAG, "Invalid Port or Pin!");
        return;
    }
    if(port ==0)
    {
       reg_addr = XL9555_REG_INPUT_0;
    }
    else
    {
       reg_addr = XL9555_REG_INPUT_1;
    }
    xl9555_read_reg(reg_addr, &pin_value);
    *level =pin_value & (1 << pin);
}

void xl9555_init(void)
{
    
    xl9555_i2c0_bus_handle = bsp_get_i2c0_bus_handle();
    i2c_device_config_t xl9555_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_bus_add_device(xl9555_i2c0_bus_handle, &xl9555_dev_cfg, &xl9555_dev_handle);

}