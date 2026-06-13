#include "task_oled.h"

ssd1306_handle_t d = NULL;

void oled_init(void)
{
    ssd1306_config_t cfg = {
    .width  = 128,
    .height = 64,
    .fb     = NULL, // let driver allocate internally
    .fb_len = 0,
    .iface.i2c =
        {
            .port     = I2C_NUM_0,
            .addr     = 0x3C,        // typical SSD1306 I2C address
            .rst_gpio = GPIO_NUM_NC, // no reset pin
        },
    };
    ESP_ERROR_CHECK(ssd1306_new_i2c(&cfg, &d));
}

void oled_test_display(void)
{
    // ----- Clear screen -----
    ESP_ERROR_CHECK(ssd1306_clear(d));
    // ----- Draw pixels in corners of screen -----
    ESP_ERROR_CHECK(ssd1306_draw_pixel(d, 0, 0, true));
    ESP_ERROR_CHECK(ssd1306_draw_pixel(d, 128 - 1, 0, true));
    ESP_ERROR_CHECK(ssd1306_draw_pixel(d, 0, 64 - 1, true));
    ESP_ERROR_CHECK(ssd1306_draw_pixel(d, 128 - 1, 64 - 1, true));
    // ----- Draw rectangles -----
    ESP_ERROR_CHECK(ssd1306_draw_rect(d, 2, 2, 40, 20, false));
    ESP_ERROR_CHECK(ssd1306_draw_rect(d, 2, 24, 32, 16, true));
    // ----- Draw circles -----
    ESP_ERROR_CHECK(ssd1306_draw_circle(d, 32, 52, 8, true));
    ESP_ERROR_CHECK(ssd1306_draw_circle(d, 100, 52, 4, false));
    // ----- Draw lines -----
    ESP_ERROR_CHECK(ssd1306_draw_line(d, 2, 2, 40, 20, true));
    ESP_ERROR_CHECK(ssd1306_draw_line(d, 32, 52, 100, 52, true));
    // ----- Draw text -----
    ESP_ERROR_CHECK(ssd1306_draw_text(d, 48, 2, "OK!", true));
    ESP_ERROR_CHECK(
    ssd1306_draw_text_scaled(d, 48, 10, "ESP32\nwfeng", true, 2));

    ESP_ERROR_CHECK(ssd1306_display(d));
}

void oled_task(void *pvParameters)
{
    oled_init();
    oled_test_display();
    while(1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

BaseType_t oled_task_create(void)
{
   return xTaskCreate(oled_task, "oled_task", 4096, NULL, 10, NULL);
}