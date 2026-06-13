#include "bsp_ledc.h"

static const char *TAG = "BSP_LEDC";

void bsp_ledc_init(led_state_t ledc_mode)
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
    if (ledc_mode == LEDC_DUTY_MODE) 
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    if (ledc_mode == LEDC_FADE_MODE) 
    {
        ledc_fade_func_install(0);
    }
}


static uint32_t sg90_angle_to_pulse_us(uint8_t angle)
{
    if (angle > 180) angle = 180;
    return 500 + angle * 2000 / 180;
}

void bsp_ledc_sg90_set_angle(uint8_t angle)
{
    uint32_t pulse_us = sg90_angle_to_pulse_us(angle);

    // 14bit duty = pulse_us / 20000us * 16383
    uint32_t duty = pulse_us * 16383 / 20000 ;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    ESP_LOGI(TAG, "angle=%u -> %uus -> duty=%u...", angle, pulse_us, duty);
    
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

    // bsp_ledc_sg90_set_angle(0);
}