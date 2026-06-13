#include "app_buttons.h"
#include "xl9555.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "BTN";

#define SCAN_MS     20
#define DEBOUNCE_MS 60   /* 3 consecutive stable reads */
#define KEY_BEEP_MS 35
#define BTN_TASK_STACK 4096
#define ENABLE_KEY_BEEP 0

static app_btn_cb_t s_callback = NULL;
static TaskHandle_t s_task = NULL;

/* Static task buffers for FreeRTOS (§1.1) */
static StackType_t s_btn_scan_stack[BTN_TASK_STACK];
static StaticTask_t s_btn_scan_tcb;

/* Per-key state */
typedef struct {
    xl9555_key_t key_id;
    int key_num;
    bool last_raw;
    bool stable;
    int stable_ms;
    bool long_press_reported;
    int press_ms;       /* current press duration */
} key_state_t;

static key_state_t s_keys[] = {
    { .key_id = XL9555_KEY1, .key_num = 1 },
    { .key_id = XL9555_KEY2, .key_num = 2 },
    { .key_id = XL9555_KEY3, .key_num = 3 },
    { .key_id = XL9555_KEY4, .key_num = 4 },
};
static const int NUM_KEYS = sizeof(s_keys) / sizeof(s_keys[0]);

static void key_success_beep(void)
{
#if ENABLE_KEY_BEEP
    xl9555_beep_enable(true);
    vTaskDelay(pdMS_TO_TICKS(KEY_BEEP_MS));
    xl9555_beep_enable(false);
#endif
}

static void button_scan_task(void *arg)
{
    (void)arg;
    uint32_t loop_cnt = 0;

    /* Initialize filter states */
    for (int i = 0; i < NUM_KEYS; i++) {
        bool raw = false;
        esp_err_t err = xl9555_key_read(s_keys[i].key_id, &raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "KEY%d init read failed: %s", s_keys[i].key_num, esp_err_to_name(err));
        }
        s_keys[i].last_raw = raw;
        s_keys[i].stable = raw;
        s_keys[i].stable_ms = DEBOUNCE_MS;
        s_keys[i].press_ms = 0;
        s_keys[i].long_press_reported = false;
    }

    while (1) {
        loop_cnt++;
        if (loop_cnt % 500 == 0) {
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "[STACK] task=btn_scan watermark=%u words", (unsigned)watermark);
        }

        for (int i = 0; i < NUM_KEYS; i++) {
            bool raw = false;
            esp_err_t err = xl9555_key_read(s_keys[i].key_id, &raw);
            if (err != ESP_OK) {
                static int err_count = 0;
                if (++err_count <= 5) {
                    ESP_LOGE(TAG, "KEY%d read failed: %s", s_keys[i].key_num, esp_err_to_name(err));
                }
                continue;  /* skip this key on I2C error */
            }

            /* Debug: log raw key state every ~2s */
            {
                static int dbg = 0;
                if (++dbg % 100 == 0) {
                    int loop = i;
                    ESP_LOGI(TAG, "raw[%d]=%s (i=%d)", s_keys[i].key_num,
                             raw ? "1(rel)" : "0(prs)", i);
                    (void)loop;
                }
            }

            key_state_t *k = &s_keys[i];

            if (raw != k->last_raw) {
                k->last_raw = raw;
                k->stable_ms = 0;
                continue;
            }

            k->stable_ms += SCAN_MS;

            if (k->stable_ms >= DEBOUNCE_MS) {
                bool pressed = raw;

                if (pressed != k->stable) {
                    /* State changed */
                    k->stable = pressed;
                    k->press_ms = 0;

                    if (pressed) {
                        /* Just pressed - reset long press flag */
                        k->long_press_reported = false;
                    } else {
                        /* Just released - report SHORT if it wasn't already reported as LONG */
                        if (!k->long_press_reported && s_callback) {
                            key_success_beep();
                            ESP_LOGI(TAG, "KEY%d short press", k->key_num);
                            s_callback(k->key_num, BTN_EVENT_SHORT_PRESS);
                        }
                    }
                } else if (pressed) {
                    /* Still pressed - accumulate duration */
                    k->press_ms += SCAN_MS;

                    /* Check long-press threshold (for KEY1 only) */
                    if (k->key_num == 1 && !k->long_press_reported &&
                        k->press_ms >= BTN_LONG_PRESS_MS) {
                        k->long_press_reported = true;
                        ESP_LOGI(TAG, "KEY1 long press (%dms)", k->press_ms);
                        if (s_callback) {
                            s_callback(k->key_num, BTN_EVENT_LONG_PRESS);
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_MS));
    }
}

esp_err_t app_buttons_init(app_btn_cb_t callback)
{
    esp_err_t ret = xl9555_key_init();
    if (ret != ESP_OK) return ret;
    (void)xl9555_beep_enable(false);
    s_callback = callback;
    return ESP_OK;
}

void app_buttons_scan_start(void)
{
    if (s_task == NULL) {
        s_task = xTaskCreateStatic(button_scan_task, "btn_scan",
                                    BTN_TASK_STACK, NULL, 4,
                                    s_btn_scan_stack, &s_btn_scan_tcb);
        ESP_LOGI(TAG, "Button scan started (core 1, static)");
    }
}

void app_buttons_scan_stop(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
}
