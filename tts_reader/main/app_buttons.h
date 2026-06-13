#ifndef APP_BUTTONS_H
#define APP_BUTTONS_H

#include <stdbool.h>
#include "esp_err.h"

/* Button events */
typedef enum {
    BTN_EVENT_SHORT_PRESS = 0,
    BTN_EVENT_LONG_PRESS,
} btn_event_t;

/* Callback: key_id (1-4), event_type */
typedef void (*app_btn_cb_t)(int key_id, btn_event_t event);

esp_err_t app_buttons_init(app_btn_cb_t callback);
void app_buttons_scan_start(void);
void app_buttons_scan_stop(void);

/* Long-press threshold for mode switching (KEY1) */
#define BTN_LONG_PRESS_MS  3000

#endif
