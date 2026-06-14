#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "bsp_spi.h"
#include "xl9555.h"
#include "lcd_st7796.h"
#include "es8388.h"
#include "ws2812b.h"
#include "tf_sdcard.h"
#include "config_parser.h"
#include "app_wifi.h"
#include "qa_state_machine.h"
#include "task_qa_lvgl.h"
#include "task_audio_capture.h"
#include "task_volc_asr.h"
#include "task_volc_llm.h"
#include "task_ws2812.h"
#include "esp_heap_caps.h"

static const char *TAG = "QA_MAIN";

/* Padding to prevent .bss overflow from corrupting log mutex */
static uint8_t s_bss_pad[4096] __attribute__((unused));

/* Config storage — large struct that must NOT be on the stack (main_task stack ~4KB) */
static config_t s_config;


static void log_mem_snapshot(const char *stage) {
    ESP_LOGI("MEM", "stage=%s heap_free=%d heap_min=%d"
             " int_free=%d int_largest=%d"
             " ps_free=%d ps_largest=%d",
             stage,
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void my_wifi_cb(bool connected, const char *ip) {
    if (connected) {
        qa_ui_add_log("[WIFI] 已连接: %s", ip);
        qa_ui_set_status("按住KEY3说话");
        log_mem_snapshot("wifi_connected");
    } else {
        ESP_LOGW(TAG, "Wi-Fi disconnected");
    }
}

static void log_stack_watermarks(void) {
    const char *names[] = {"qa_lvgl_task","volc_asr","volc_llm",
                           "audio_capture","sdcard_task","ws2812_task","timer_task"};
    for (int i = 0; i < 7; i++) {
        TaskHandle_t h = xTaskGetHandle(names[i]);
        if (h) {
            UBaseType_t w = uxTaskGetStackHighWaterMark(h);
            ESP_LOGI("STACK", "task=%-12s watermark=%u bytes",
                     names[i], w * sizeof(StackType_t));
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "===== QA Volc Boot =====");
    ESP_LOGI(TAG, "Log mutex check 1");
    ESP_LOGI(TAG, "Log mutex check 2");

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS init done");

    // 2. BSP init
    bsp_i2c0_init();
    xl9555_init();
    board_keys_gpio_init();
    board_audio_gpio_init();
    board_lcd_gpio_init();
    board_ws2812_level_shifter_init();
    // board_camera_gpio_init();  // Camera not needed for MVP
    bsp_spi2_lcd_init();
    ESP_ERROR_CHECK(lcd_st7796_init());
    ESP_ERROR_CHECK(es8388_init(24, 60));  // input_gain=24, output_vol=60
    ESP_ERROR_CHECK(bsp_audio_init(BSP_AUDIO_SAMPLE_RATE));
    ws2812_init();

    ESP_LOGI(TAG, "BSP init OK");
    log_mem_snapshot("boot_done");

    // 3. Mount TF card and read config
    memset(&s_config, 0, sizeof(s_config));
    if (tf_sdcard_mount() == ESP_OK) {
        // Check KEY4 at boot: rewrite config.ini with test LLM endpoint
        bool key4_boot;
        xl9555_get_pin_level(KEY_PORT, KEY4_PIN, &key4_boot);
        if (key4_boot == 0) {
            ESP_LOGI(TAG, "KEY4 pressed at boot — rewriting config.ini");
            FILE *f = fopen("/sdcard/config.ini", "w");
            if (f) {
                fprintf(f,
                    "WIFI_SSID=XL206-2\n"
                    "WIFI_PASS=13510602814\n"
                    "ASR_API_KEY=423199ee-f156-411b-84d6-ff2469c54a34\n"
                    "ASR_RESOURCE_ID=volc.seedasr.auc\n"
                    "LLM_API_KEY=sk-XaugTmX1euOEE6hM019eA650997d75899dBeA7874aA21429\n"
                    "LLM_ENDPOINT=https://api.decard.cc/v1/chat/completions\n"
                    "LLM_MODEL=deepseek-v4-flash\n");
                fclose(f);
                ESP_LOGI(TAG, "config.ini rewritten, rebooting...");
                qa_ui_add_log("[SYS] 配置已更新，重启中");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }

        ESP_LOGI(TAG, "TF card mounted, reading config.ini");
        if (config_parse("/sdcard/config.ini", &s_config) == ESP_OK) {
            log_mem_snapshot("config_loaded");
        } else {
            ESP_LOGW(TAG, "config.ini not found, using defaults");
        }
    } else {
        ESP_LOGW(TAG, "TF card mount failed, using defaults");
    }

    // Read Wi-Fi credentials (from config or fallback)
    const char *ssid = config_get_string(&s_config, "WIFI_SSID", "XL206-2");
    const char *pass = config_get_string(&s_config, "WIFI_PASS", "");
    ESP_LOGI(TAG, "Connecting to Wi-Fi: %s", ssid);

    app_wifi_register_cb(my_wifi_cb);
    ESP_ERROR_CHECK(app_wifi_start(ssid, pass));
    log_mem_snapshot("wifi_started");

    // 4. Start LVGL UI task
    ESP_LOGI(TAG, "Starting LVGL UI...");
    lcd_lvgl_reserve_buffer();
    lcd_lvgl_task_create();
    ESP_LOGI(TAG, "LVGL UI started");

    // 5. Create worker tasks
    audio_capture_task_create(&s_config);
    volc_asr_task_create(&s_config);
    volc_llm_task_create(&s_config);
    ws2812_task_create();
    ESP_LOGI(TAG, "All worker tasks created");

    // 6. Enter main loop
    ESP_LOGI(TAG, "Boot complete, entering main loop");
    qa_ui_add_log("[SYS] 系统就绪，等待输入");
    qa_ui_set_status("按住KEY3说话");

    vTaskDelay(pdMS_TO_TICKS(3000)); // wait for all tasks to start
    log_stack_watermarks();
    log_mem_snapshot("stack_check");

    bool key4_was_pressed = false;
    TickType_t key4_press_tick = 0;

    while (1) {
        // KEY1 — scroll UP
        bool key1_level;
        xl9555_get_pin_level(KEY_PORT, KEY1_PIN, &key1_level);
        if (key1_level == 0) {
            qa_ui_scroll(-1);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        // KEY4 — short press: scroll DOWN; long press (>1.5s): clear dialog
        bool key4_level;
        xl9555_get_pin_level(KEY_PORT, KEY4_PIN, &key4_level);
        if (key4_level == 0 && !key4_was_pressed) {
            key4_was_pressed = true;
            key4_press_tick = xTaskGetTickCount();
        } else if (key4_level != 0 && key4_was_pressed) {
            // Released: if less than 1.5s, scroll down
            if ((xTaskGetTickCount() - key4_press_tick) < pdMS_TO_TICKS(1500)) {
                qa_ui_scroll(1);
            }
            key4_was_pressed = false;
        } else if (key4_level == 0 && key4_was_pressed) {
            // Still pressed: check for long press
            if ((xTaskGetTickCount() - key4_press_tick) >= pdMS_TO_TICKS(1500)) {
                qa_ui_clear_all();
                qa_ui_add_log("[CLR] 对话已清除");
                key4_was_pressed = false;  // prevent repeat
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }

        // Periodic Wi-Fi status update
        static int tick = 0;
        if (++tick >= 20) {
            tick = 0;
            if (!app_wifi_is_connected()) {
                qa_ui_set_status("Wi-Fi连接中...");
            }
        }

        // OOM check every ~5 seconds
        static int oom_tick = 0;
        if (++oom_tick >= 100) {
            oom_tick = 0;
            size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            if (int_free < 30 * 1024 || int_largest < 24 * 1024) {
                ESP_LOGW(TAG, "OOM detected: int_free=%d int_largest=%d", int_free, int_largest);
                if (qa_degrade_step_up() != ESP_OK) {
                    ESP_DRAM_LOGE(TAG, "OOM critical: cannot degrade further");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
