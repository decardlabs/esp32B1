#include <stdbool.h>
#include <stdint.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io_map.h"
#include "task_key_led.h"
#include "xl9555_io.h"

#define TASK_KEY_LED_STACK_SIZE 4096
#define TASK_KEY_LED_PRIORITY   5
#define KEY_SCAN_PERIOD_MS      20
#define DEBOUNCE_MS             30
#define BEEP_ON_MS              600
#define LONG_PRESS_MS           1000
#define BLINK_HALF_PERIOD_MS    500
#define IO_ERR_LOG_INTERVAL_MS  1000

static const char *TAG = "TASK_KEY_LED";
static uint32_t s_last_io_err_log_ms = 0;

typedef struct {
    bool last_raw;
    bool stable_state;
    uint32_t stable_ms;
} key_filter_t;

typedef struct {
    key_filter_t filter;
    uint32_t hold_ms;
    bool long_active;
} key_logic_t;

static bool key_filter_update(key_filter_t *filter, bool raw_pressed)
{
    if (raw_pressed != filter->last_raw) {
        filter->last_raw = raw_pressed;
        filter->stable_ms = 0;
        return false;
    }

    filter->stable_ms += KEY_SCAN_PERIOD_MS;
    if ((filter->stable_ms >= DEBOUNCE_MS) && (raw_pressed != filter->stable_state)) {
        filter->stable_state = raw_pressed;
        if (filter->stable_state) {
            return true;
        }
    }

    return false;
}

static bool key_logic_update(key_logic_t *logic, bool raw_pressed)
{
    bool pressed_edge = key_filter_update(&logic->filter, raw_pressed);

    if (logic->filter.stable_state) {
        logic->hold_ms += KEY_SCAN_PERIOD_MS;
        if (logic->hold_ms >= LONG_PRESS_MS) {
            logic->long_active = true;
        }
    } else {
        logic->hold_ms = 0;
        logic->long_active = false;
    }

    return pressed_edge;
}

static void log_io_error_throttled(const char *op, esp_err_t err, uint32_t now_ms)
{
    if ((now_ms - s_last_io_err_log_ms) >= IO_ERR_LOG_INTERVAL_MS) {
        ESP_LOGW(TAG, "%s failed: %s", op, esp_err_to_name(err));
        s_last_io_err_log_ms = now_ms;
    }
}

static void task_key_led(void *args)
{
    key_logic_t key1_logic = {0};
    key_logic_t key2_logic = {0};
    key_logic_t key3_logic = {0};
    bool led1_on = false;
    bool led2_on = false;
    bool led3_on = false;
    bool led1_out = false;
    bool led2_out = false;
    bool led3_out = false;
    bool led1_last_out = false;
    bool led2_last_out = false;
    bool led3_last_out = false;
    bool beep_active = false;
    uint32_t beep_left_ms = 0;

    (void)args;
    esp_err_t err = xl9555_io_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "xl9555 init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "logic start: key1-led1 toggle, key2-led2 toggle+beep, key3-led3 toggle");

    while (1) {
        bool key1_pressed = false;
        bool key2_pressed = false;
        bool key3_pressed = false;
        bool read_failed = false;
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool blink_phase_on = ((now_ms / BLINK_HALF_PERIOD_MS) % 2U) == 0U;

        err = xl9555_io_key_is_pressed(&key1, &key1_pressed);
        if (err != ESP_OK) {
            log_io_error_throttled("read key1", err, now_ms);
            read_failed = true;
        }
        err = xl9555_io_key_is_pressed(&key2, &key2_pressed);
        if (err != ESP_OK) {
            log_io_error_throttled("read key2", err, now_ms);
            read_failed = true;
        }
        err = xl9555_io_key_is_pressed(&key3, &key3_pressed);
        if (err != ESP_OK) {
            log_io_error_throttled("read key3", err, now_ms);
            read_failed = true;
        }

        if (read_failed) {
            vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
            continue;
        }

        if (key_logic_update(&key1_logic, key1_pressed)) {
            led1_on = !led1_on;
            ESP_LOGI(TAG, "key1 pressed -> led1=%d", led1_on);
        }

        if (key_logic_update(&key2_logic, key2_pressed)) {
            led2_on = !led2_on;
            beep_active = true;
            beep_left_ms = BEEP_ON_MS;
            err = xl9555_io_beep_set(&beep, true);
            if (err != ESP_OK) {
                log_io_error_throttled("beep on", err, now_ms);
            }
            ESP_LOGI(TAG, "key2 pressed -> led2=%d, beep=600ms", led2_on);
        }

        if (key_logic_update(&key3_logic, key3_pressed)) {
            led3_on = !led3_on;
            ESP_LOGI(TAG, "key3 pressed -> led3=%d", led3_on);
        }

        if (beep_active) {
            if (beep_left_ms <= KEY_SCAN_PERIOD_MS) {
                beep_active = false;
                beep_left_ms = 0;
                err = xl9555_io_beep_set(&beep, false);
                if (err != ESP_OK) {
                    log_io_error_throttled("beep off", err, now_ms);
                }
            } else {
                beep_left_ms -= KEY_SCAN_PERIOD_MS;
            }
        }

        led1_out = key1_logic.long_active ? blink_phase_on : led1_on;
        led2_out = key2_logic.long_active ? blink_phase_on : led2_on;
        led3_out = key3_logic.long_active ? blink_phase_on : led3_on;

        if (led1_out != led1_last_out) {
            err = xl9555_io_led_set(&led1, led1_out);
            if (err != ESP_OK) {
                log_io_error_throttled("set led1", err, now_ms);
            } else {
                led1_last_out = led1_out;
            }
        }
        if (led2_out != led2_last_out) {
            err = xl9555_io_led_set(&led2, led2_out);
            if (err != ESP_OK) {
                log_io_error_throttled("set led2", err, now_ms);
            } else {
                led2_last_out = led2_out;
            }
        }
        if (led3_out != led3_last_out) {
            err = xl9555_io_led_set(&led3, led3_out);
            if (err != ESP_OK) {
                log_io_error_throttled("set led3", err, now_ms);
            } else {
                led3_last_out = led3_out;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
    }
}

BaseType_t task_key_led_create(void)
{
    return xTaskCreate(task_key_led,
                       "task_key_led",
                       TASK_KEY_LED_STACK_SIZE,
                       NULL,
                       TASK_KEY_LED_PRIORITY,
                       NULL);
}
