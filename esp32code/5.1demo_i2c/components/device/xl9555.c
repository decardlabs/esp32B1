#include "xl9555.h"

static const char *TAG = "XL9555";

// 新版I2C驱动句柄（需外部初始化或在xl9555_init中初始化）
static i2c_master_dev_handle_t xl9555_dev_handle = NULL;

// -------------------------- 私有工具函数（新版I2C读写寄存器） --------------------------
/**
 * @brief 向XL9555的某个寄存器写入1字节数据
 * @note 依赖xl9555_dev_handle已初始化（绑定到XL9555从机地址）
 */
static esp_err_t xl9555_write_reg(uint8_t reg_addr, uint8_t data) {
    if (xl9555_dev_handle == NULL) {
        ESP_LOGE(TAG, "XL9555 I2C device handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // 拼接寄存器地址+数据
    uint8_t write_buf[2] = {reg_addr, data};
    // 新版驱动：直接批量发送，无需手动拼接命令链
    esp_err_t err = i2c_master_transmit(xl9555_dev_handle, 
                                        write_buf, 
                                        sizeof(write_buf), 
                                        1000 / portTICK_PERIOD_MS);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write reg 0x%02X failed, err=0x%02X", reg_addr, err);
    }
    return err;
}

/**
 * @brief 从XL9555的某个寄存器读取1字节数据
 * @note 依赖xl9555_dev_handle已初始化
 */
static esp_err_t xl9555_read_reg(uint8_t reg_addr, uint8_t *data) {
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xl9555_dev_handle == NULL) {
        ESP_LOGE(TAG, "XL9555 I2C device handle not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // 新版驱动：先写寄存器地址，再读取数据（一键完成）
    esp_err_t err = i2c_master_transmit_receive(xl9555_dev_handle,
                                                &reg_addr, 
                                                1,          // 写入1字节（寄存器地址）
                                                data,       // 读取数据缓冲区
                                                1,          // 读取1字节
                                                1000 / portTICK_PERIOD_MS);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read reg 0x%02X failed, err=0x%02X", reg_addr, err);
    }
    return err;
}

void xl9555_set_pin_dir(uint8_t port, uint8_t pin, bool is_input) {
    if (port > 1 || pin > 7) {
        ESP_LOGE(TAG, "Invalid port(%d) or pin(%d)", port, pin);
        return;
    }

    // 1. 读取当前配置寄存器值
    uint8_t reg_addr = (port == 0) ? XL9555_REG_CONFIG_0 : XL9555_REG_CONFIG_1;
    uint8_t config_val;
    esp_err_t err = xl9555_read_reg(reg_addr, &config_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read config reg 0x%02X failed, err=0x%02X", reg_addr, err);
        return;
    }

    // 2. 修改目标引脚的方向（1=输入，0=输出）
    if (is_input) {
        config_val |= (1 << pin);  // 置1=输入
    } else {
        config_val &= ~(1 << pin); // 置0=输出
    }
    // 3. 写回配置寄存器
    err = xl9555_write_reg(reg_addr, config_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write config reg 0x%02X failed, err=0x%02X", reg_addr, err);
        return;
    }
}

void xl9555_set_pin_level(uint8_t port, uint8_t pin, bool level) {
    if (port > 1 || pin > 7) {
        ESP_LOGE(TAG, "Invalid port(%d) or pin(%d)", port, pin);
        return;
    }

    // 1. 读取当前输出寄存器值
    uint8_t reg_addr = (port == 0) ? XL9555_REG_OUTPUT_0 : XL9555_REG_OUTPUT_1;
    uint8_t output_val;
    esp_err_t err = xl9555_read_reg(reg_addr, &output_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read output reg 0x%02X failed, err=0x%02X", reg_addr, err);
        return;
    }

    // 2. 修改目标引脚的电平（1=高，0=低）
    if (level) {
        output_val |= (1 << pin);  // 置1=高电平
    } else {
        output_val &= ~(1 << pin); // 置0=低电平
    }

    // 3. 写回输出寄存器
    err = xl9555_write_reg(reg_addr, output_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write output reg 0x%02X failed, err=0x%02X", reg_addr, err);
        return;
    }
}

void xl9555_get_pin_level(uint8_t port, uint8_t pin, uint8_t *level) {
    if (port > 1 || pin > 7 || level == NULL) {
        ESP_LOGE(TAG, "Invalid port(%d) or pin(%d) or level ptr", port, pin);
        return;
    }

    // 1. 读取输入寄存器值
    uint8_t reg_addr = (port == 0) ? XL9555_REG_INPUT_0 : XL9555_REG_INPUT_1;
    uint8_t input_val;
    esp_err_t err = xl9555_read_reg(reg_addr, &input_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Read input reg 0x%02X failed, err=0x%02X", reg_addr, err);
        return;
    }

    // 2. 提取目标引脚的电平（1=高，0=低）
    *level = (input_val >> pin) & 0x01;
}

void xl9555_init(void) 
{
    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
    i2c_device_config_t dev_config = {   
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    // 3. 绑定从机设备到bsp_i2c的总线
    esp_err_t err = i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &xl9555_dev_handle);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "Add XL9555 to I2C bus failed, err=0x%02X", err);
        return;
    }
    // 4. 原有寄存器初始化逻辑（不变）
    uint8_t config_val = 0xFF;
    ESP_ERROR_CHECK(xl9555_write_reg(XL9555_REG_CONFIG_0, config_val));
    ESP_ERROR_CHECK(xl9555_write_reg(XL9555_REG_CONFIG_1, config_val));


    ESP_LOGI(TAG, "XL9555 init success (I2C addr:0x%02X)", XL9555_I2C_ADDR);


}