#include "bsp_ledc.h"

static const char *TAG = "BSP_LEDC";

void bsp_ledc_duty_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}


void bsp_ledc_fade_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);

    ledc_fade_func_install(0);
}


// us -> duty(0 ~ 16383)
uint32_t sg90_us_to_duty(uint32_t pulse_us)
{
    if (pulse_us < SG90_MIN_US) pulse_us = SG90_MIN_US;
    if (pulse_us > SG90_MAX_US) pulse_us = SG90_MAX_US;

    // duty = pulse_us / 20000us * 16383
    return (uint32_t)((uint64_t)pulse_us * SG90_DUTY_MAX / SG90_PERIOD_US);
}

// 1) 直接设置脉宽（us）
esp_err_t bsp_ledc_sg90_set_pulse_us(uint32_t pulse_us)
{
    uint32_t duty = sg90_us_to_duty(pulse_us);

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err != ESP_OK) return err;

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    return err;
}

// 2) 设置角度（0~180）
esp_err_t bsp_ledc_sg90_set_angle(uint8_t angle)
{
    if (angle > SG90_MAX_ANGLE) angle = SG90_MAX_ANGLE;

    uint32_t pulse_us = SG90_MIN_US +
        (uint32_t)((uint64_t)(SG90_MAX_US - SG90_MIN_US) * angle / SG90_MAX_ANGLE);

    ESP_LOGI(TAG, "angle=%u -> pulse=%uus", angle, (unsigned)pulse_us);

    return bsp_ledc_sg90_set_pulse_us(pulse_us);
}

void bsp_ledc_sg90_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);

    bsp_ledc_sg90_set_angle(90);
}