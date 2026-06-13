/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "app_wifi_portal.h"
#include "app_wifi_store.h"
#include "bsp_i2c.h"
#include "bsp_audio.h"
#include "bsp_spi.h"
#include "es8388.h"
#include "lcd_st7796.h"
#include "task_lcd_lvgl.h"
#include "task_ws2812.h"
#include "task_sdcard.h"
#include "task_xiaozhi.h"
#include "ws2812b.h"
#include "xiaozhi_ui_status.h"
#include "xl9555.h"

static const char *TAG = "MAIN";

static esp_err_t app_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();

    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    return ret;
}

static bool app_key3_pressed_on_boot(void)
{
    bool key_level = true;
    uint8_t pressed_count = 0;

    for (int i = 0; i < 5; i++) {
        xl9555_get_pin_level(KEY_PORT, KEY3_PIN, &key_level);
        if (key_level == 0) {
            pressed_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return (pressed_count >= 4);
}

static void app_configure_log_levels(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("TASK_XIAOZHI", ESP_LOG_INFO);
    esp_log_level_set("TASK_LCD", ESP_LOG_INFO);
    esp_log_level_set("FACE_RECOG", ESP_LOG_INFO);
    esp_log_level_set("xiaozhi_activation", ESP_LOG_INFO);

    esp_log_level_set("websocket_client", ESP_LOG_ERROR);
    esp_log_level_set("transport_base", ESP_LOG_ERROR);
    esp_log_level_set("transport_ws", ESP_LOG_ERROR);
    esp_log_level_set("esp-tls", ESP_LOG_ERROR);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_ERROR);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_ERROR);
    esp_log_level_set("cam_hal", ESP_LOG_ERROR);
    esp_log_level_set("ov2640", ESP_LOG_ERROR);
    esp_log_level_set("LCD_ST7796", ESP_LOG_ERROR);
}

static esp_err_t app_start_setup_lcd(const char *status_text,
                                     const char *wifi_name,
                                     const char *ip_text,
                                     const char *binding_text)
{
    ESP_RETURN_ON_ERROR(bsp_spi2_lcd_init(), TAG, "init lcd spi failed");
    board_lcd_gpio_init();
    ESP_RETURN_ON_ERROR(lcd_st7796_init(), TAG, "init lcd panel failed");

    xiaozhi_ui_status_init();
    xiaozhi_ui_status_set_view(XIAOZHI_UI_VIEW_SETUP);
    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_CONNECTING);
    xiaozhi_ui_status_set_note(status_text);
    xiaozhi_ui_status_set_network_info(wifi_name, ip_text);
    xiaozhi_ui_status_set_binding_text(binding_text);

    ESP_RETURN_ON_ERROR(lcd_lvgl_reserve_buffer(), TAG, "reserve lcd buffer failed");
    ESP_RETURN_ON_FALSE(lcd_lvgl_task_create() == pdPASS, ESP_FAIL, TAG, "create lcd task failed");
    return ESP_OK;
}

static esp_err_t app_enter_wifi_portal_mode(const char *reason_text)
{
    char portal_note[XIAOZHI_UI_NOTE_LEN];
    char portal_url[XIAOZHI_UI_IP_LEN];
    const char *portal_ssid = NULL;
    const char *portal_ip = NULL;

    ESP_LOGW(TAG, "enter wifi portal: reason=%s", reason_text);
    ESP_RETURN_ON_ERROR(app_start_setup_lcd("正在启动 Wi-Fi 配网页面", "热点启动中", "http://192.168.4.1", "先配网"),
                        TAG,
                        "start setup lcd failed");
    ESP_RETURN_ON_ERROR(app_wifi_portal_start(), TAG, "start wifi portal failed");

    portal_ssid = app_wifi_portal_get_ap_ssid();
    portal_ip = app_wifi_portal_get_ap_ip();
    snprintf(portal_url,
             sizeof(portal_url),
             "http://%s",
             ((portal_ip != NULL) && (portal_ip[0] != '\0')) ? portal_ip : "192.168.4.1");
    snprintf(portal_note,
             sizeof(portal_note),
             "热点名: %s\n网址: %s",
             ((portal_ssid != NULL) && (portal_ssid[0] != '\0')) ? portal_ssid : "XIAOZHI_SETUP",
             portal_url);
    xiaozhi_ui_status_set_note(portal_note);
    xiaozhi_ui_status_set_network_info(portal_ssid, portal_url);
    xiaozhi_ui_status_set_binding_text("先连热点再配网");
    return ESP_OK;
}

void app_main(void)
{
    app_wifi_credentials_t wifi_credentials = {0};
    bool key3_pressed = false;
    bool force_portal = false;
    esp_err_t wifi_load_ret = ESP_OK;

    app_configure_log_levels();
    ESP_LOGI(TAG, "boot start");
    ESP_ERROR_CHECK(app_init_nvs());

    /*
     * 1. Boot decision
     *    KEY3 is used as the Wi-Fi provisioning button.
     */
    bsp_i2c0_init();                                                            // I2C0 总线
    xl9555_init();                                                              // XL9555 IO 扩展器
    board_keys_gpio_init();                                                     // KEY1~KEY4 输入引脚
    key3_pressed = app_key3_pressed_on_boot();
    force_portal = app_wifi_store_take_portal_request();
    wifi_load_ret = app_wifi_store_load(&wifi_credentials);

    if (key3_pressed || force_portal || (wifi_load_ret != ESP_OK)) {
        ESP_ERROR_CHECK(app_enter_wifi_portal_mode(key3_pressed ? "KEY3 pressed at boot" :
                                                   (force_portal ? "KEY3 long press requested portal" :
                                                                   "wifi credentials missing")));
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    /*
     * 2. Peripheral / hardware init
     *    这些初始化动作现在都直接写在 main.c，打开入口就能看到。
     */
    ESP_ERROR_CHECK(bsp_spi2_lcd_init());                                       // SPI2 LCD 总线
    board_lcd_gpio_init();                                                      // LCD 复位 / 背光引脚
    board_camera_gpio_init();                                                   // 摄像头复位 / 电源引脚
    board_audio_gpio_init();                                                    // 蜂鸣器 / 功放引脚
    board_ws2812_level_shifter_init();                                          // WS2812 电平转换使能
    ESP_ERROR_CHECK(lcd_st7796_init());                                         // ST7796 LCD 面板
    ESP_ERROR_CHECK((ws2812_init() != NULL) ? ESP_OK : ESP_FAIL);               // WS2812 灯带
    ESP_ERROR_CHECK(bsp_audio_init(BSP_AUDIO_SAMPLE_RATE));                     // I2S0 音频总线
    ESP_ERROR_CHECK(es8388_init(CONFIG_XIAOZHI_INPUT_GAIN_DB,
                                CONFIG_XIAOZHI_OUTPUT_VOLUME));                 // ES8388 音频编解码器

    /*
     * 3. Device / shared state init
     */
    xiaozhi_ui_status_init();                                                   // UI 状态对象

    /*
     * 4. Thread start
     */
    ESP_ERROR_CHECK((sdcard_task_create() == pdPASS) ? ESP_OK : ESP_FAIL);      // TF 卡对话日志线程
    ESP_ERROR_CHECK((ws2812_task_create() == pdPASS) ? ESP_OK : ESP_FAIL);      // 灯带控制线程
    ESP_ERROR_CHECK((xiaozhi_task_create() == pdPASS) ? ESP_OK : ESP_FAIL);     // 小智主线程

    /*
     * 下面这个线程不是开机立刻启动：
     * - xiaozhi_audio: 在 websocket hello 后由 task_xiaozhi.c 拉起
     *
     * lcd_lvgl_task 现在会在 task_xiaozhi.c 的联网 / 激活阶段尽早拉起，
     * 这样屏幕能显示 Wi-Fi、IP 和设备绑定码。
     *
     * 下面这些线程当前没有启动，想用哪个就回 main.c 这里打开：
     * - exti_task_prco_create();   // task_exti_prco.c
     * - uart1_task_create();       // task_uart.c
     * - led_task_create();         // task_led.c
     * - timer_task_create();       // task_timer.c
     * - ledc_task_create();        // task_ledc.c
     * - oled_task_create();        // task_oled.c
     * - dht11_task_create();       // task_dht11.c
     */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
