#include "task_oled.h"


ssd1306_handle_t oled_handle = NULL;
void oled_init(void)
{
    ssd1306_config_t oled_cfg = {
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
    ESP_ERROR_CHECK(ssd1306_new_i2c(&oled_cfg, &oled_handle));     

    ESP_ERROR_CHECK(ssd1306_clear(oled_handle));
}

void oled_test_display(void)
{
     // ----- Draw pixels in corners of screen -----
    ESP_ERROR_CHECK(ssd1306_draw_pixel(oled_handle, 0, 0, true));
    ESP_ERROR_CHECK(ssd1306_draw_pixel(oled_handle, 127, 0, true));
    ESP_ERROR_CHECK(ssd1306_draw_pixel(oled_handle, 0, 63, true));
    ESP_ERROR_CHECK(ssd1306_draw_pixel(oled_handle, 127, 63, true));

    // ----- Draw rectangles -----
    ESP_ERROR_CHECK(ssd1306_draw_rect(oled_handle, 2, 2, 40, 20, false));
    ESP_ERROR_CHECK(ssd1306_draw_rect(oled_handle, 2, 24, 32, 16, true));

    // ----- Draw circles -----
    ESP_ERROR_CHECK(ssd1306_draw_circle(oled_handle, 32, 52, 8, true));
    ESP_ERROR_CHECK(ssd1306_draw_circle(oled_handle, 100, 52, 4, false));

    // ----- Draw lines -----
    ESP_ERROR_CHECK(ssd1306_draw_line(oled_handle, 2, 2, 40, 20, true));
    ESP_ERROR_CHECK(ssd1306_draw_line(oled_handle, 32, 52, 100, 52, true));

    // ----- Draw text -----
    ESP_ERROR_CHECK(ssd1306_draw_text(oled_handle, 48, 2, "OJBK!", true));
    ESP_ERROR_CHECK(ssd1306_draw_text_scaled(oled_handle, 48, 10, "ESP32\nwfeng", true, 2));

    bsp_i2c0_lock();
    ESP_ERROR_CHECK(ssd1306_display(oled_handle));
    bsp_i2c0_unlock();
}


void oled_task(void *pvParameters)
{
    bool flag = false;
    oled_init();
    // oled_test_display();
    while (1)
    {
        if(flag)
        {
            ESP_ERROR_CHECK(ssd1306_clear(oled_handle));
            ESP_ERROR_CHECK(ssd1306_draw_text_scaled(oled_handle, 48, 10, "ESP32\nwfeng", true, 2));
        }
        else
        {
            ESP_ERROR_CHECK(ssd1306_clear(oled_handle));
            ESP_ERROR_CHECK(ssd1306_draw_text_scaled(oled_handle, 48, 10, "I2C\nMutex", true, 2));
        }
        flag = !flag;

        bsp_i2c0_lock();
        ESP_ERROR_CHECK(ssd1306_display(oled_handle));
        bsp_i2c0_unlock();

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}




BaseType_t oled_task_create(void)
{
    return xTaskCreate(oled_task, "oled_task",4096, NULL, 10, NULL);
}

