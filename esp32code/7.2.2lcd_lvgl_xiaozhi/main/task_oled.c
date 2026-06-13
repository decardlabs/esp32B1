#include "task_oled.h"

static const char *TAG = "TASK_OLED";

ssd1306_handle_t oled_handle = NULL;
QueueHandle_t queue_oled_handle = NULL;

void oled_send_msg(const oled_msg_t *msg, TickType_t timeout)
{
    if (queue_oled_handle) {
        xQueueSend(queue_oled_handle, msg, timeout);
    }
}

void oled_queue_init(void)
{
    if (!queue_oled_handle) {
        queue_oled_handle = xQueueCreate(10, sizeof(oled_msg_t)); // 深度10自己按需改
        if (!queue_oled_handle) {
            ESP_LOGE(TAG, "xQueueCreate failed");
        }
    }
}

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

    oled_queue_init();
    oled_init();
    // oled_test_display();
    while (1)
    {

        oled_msg_t msg;
        if (xQueueReceive(queue_oled_handle, &msg, portMAX_DELAY) == pdTRUE) {
            switch (msg.type) {
            case OLED_MSG_TYPE_DHT11:
                ESP_ERROR_CHECK(ssd1306_clear(oled_handle));
                char oled_display_buf[40] = {0};
                snprintf(oled_display_buf, sizeof(oled_display_buf), "T: %u\nH: %u", msg.u_data.dht11.temp, msg.u_data.dht11.humi);
                ESP_ERROR_CHECK(ssd1306_draw_text_scaled(oled_handle, 0, 10, oled_display_buf, true, 2));
                break;

            default:
                break;
            }
        }

        bsp_i2c0_lock();
        ESP_ERROR_CHECK(ssd1306_display(oled_handle));
        bsp_i2c0_unlock();

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}




BaseType_t oled_task_create(void)
{
    return xTaskCreate(oled_task, "oled_task",4096, NULL, 10, NULL);
}

