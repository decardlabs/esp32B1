#include "lcd_st7796.h"
#include "bsp_spi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "qrcode_utils.h"

#define LCD_LVGL_TICK_MS         2
#define LCD_LVGL_HANDLER_MIN_MS  5
#define LCD_LVGL_HANDLER_MAX_MS  20
#define LCD_LVGL_TASK_STACK      8192

static const char *TAG = "LCD_LVGL";

static lv_disp_draw_buf_t lcd_draw_buf;
static lv_disp_drv_t lcd_disp_drv;
static lv_color_t *lcd_buf_1 = NULL;
static esp_timer_handle_t lcd_lvgl_tick_timer = NULL;

static lv_obj_t *status_label = NULL;
static lv_obj_t *light_label = NULL;

/* Forward declarations */
static void lv_ready_timer_cb(lv_timer_t *timer);
void lcd_lvgl_update_status(const char *status);
void lcd_lvgl_show_operational(void);
void lcd_lvgl_update_light_state(bool on);

/* LVGL timer callback: mark device as ready after initialization delay */
static void lv_ready_timer_cb(lv_timer_t *timer)
{
    lcd_lvgl_update_status("Ready");
    lv_timer_del(timer);
}

static bool lcd_lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lcd_st7796_transfer_t transfer = lcd_st7796_take_completed_transfer_from_isr();

    (void)panel_io;
    (void)edata;

    if (transfer == LCD_ST7796_TRANSFER_LVGL && disp_drv) {
        lv_disp_flush_ready(disp_drv);
    }

    return false;
}

static void lcd_lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LCD_LVGL_TICK_MS);
}

static void lcd_lvgl_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_err_t ret = lcd_st7796_draw_bitmap_owned(area->x1, area->y1,
                                                 area->x2 + 1, area->y2 + 1,
                                                 color_map, LCD_ST7796_TRANSFER_LVGL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "flush failed: %s", esp_err_to_name(ret));
        lv_disp_flush_ready(disp_drv);
    }
}

#define BG_COLOR        lv_color_hex(0x1565C0)

static void lcd_lvgl_matter_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, BG_COLOR, 0);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP Matter Light");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Status label at bottom */
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Initializing...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xBBDEFB), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* Forward declare */
void lcd_lvgl_show_operational(void);

void lcd_lvgl_update_status(const char *status)
{
    if (status_label) {
        lv_label_set_text(status_label, status);
    }
}

void lcd_lvgl_update_light_state(bool on)
{
    if (light_label) {
        lv_label_set_text(light_label, on ? "Light: ON" : "Light: OFF");
        lv_obj_set_style_text_color(light_label,
            on ? lv_color_hex(0x81C784) : lv_color_hex(0xFFFFFF), 0);
    }
}

void lcd_lvgl_show_operational(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, BG_COLOR, 0);

    /* Title at top */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP Matter Light");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Light state - centered, large */
    light_label = lv_label_create(scr);
    lv_label_set_text(light_label, "Light: OFF");
    lv_obj_set_style_text_color(light_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(light_label, &lv_font_montserrat_20, 0);
    lv_obj_center(light_label);

    /* Network info - below center */
    lv_obj_t *wifi_label = lv_label_create(scr);
    lv_label_set_text(wifi_label, "Wi-Fi Connected");
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(0xBBDEFB), 0);
    lv_obj_set_style_text_font(wifi_label, &lv_font_montserrat_16, 0);
    lv_obj_align(wifi_label, LV_ALIGN_BOTTOM_MID, 0, -36);

    /* Device readiness status - bottom */
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Initializing...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xBBDEFB), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* One-shot LVGL timer: mark "Ready" after 10s (allow subscriptions to establish) */
    lv_timer_t *t = lv_timer_create(lv_ready_timer_cb, 10000, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void lcd_lvgl_show_pairing_info(const char *qr_code_str, const char *manual_code)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, BG_COLOR, 0);

    /* Title at top */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP Matter Light");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* Generate QR code image - scale=5 for ~125x125 px */
    lv_img_dsc_t *qr_img = qrcode_get_lvgl_image(qr_code_str, 5);
    if (qr_img) {
        lv_obj_t *qr = lv_img_create(scr);
        lv_img_set_src(qr, qr_img);
        /* Center QR with slight upward offset for manual code below */
        lv_obj_align(qr, LV_ALIGN_CENTER, 0, -20);

        /* Compact manual code right below QR */
        lv_obj_t *code_label = lv_label_create(scr);
        lv_label_set_text_fmt(code_label, "Code: %s", manual_code);
        lv_obj_set_style_text_color(code_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(code_label, &lv_font_montserrat_16, 0);
        lv_obj_align_to(code_label, qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    } else {
        /* No QR - show text */
        lv_obj_t *fallback = lv_label_create(scr);
        lv_label_set_text(fallback, "Use manual code to pair");
        lv_obj_set_style_text_color(fallback, lv_color_hex(0xFFCDD2), 0);
        lv_obj_set_style_text_font(fallback, &lv_font_montserrat_16, 0);
        lv_obj_align(fallback, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t *code_label = lv_label_create(scr);
        lv_label_set_text_fmt(code_label, "Code: %s", manual_code);
        lv_obj_set_style_text_color(code_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(code_label, &lv_font_montserrat_16, 0);
        lv_obj_align_to(code_label, fallback, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    }

    /* Status at bottom */
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Waiting for pairing...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xBBDEFB), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void lcd_lvgl_task(void *pvParameters)
{
    lv_disp_t *disp;
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lcd_lvgl_tick_cb,
        .name = "lvgl_tick",
    };

    (void)pvParameters;

    /* Allocate DMA-capable LVGL draw buffer */
    lcd_buf_1 = (lv_color_t *)heap_caps_malloc(
        LCD_ST7796_H_RES * LCD_ST7796_BUF_LINES * sizeof(lv_color_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!lcd_buf_1) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer");
        vTaskDelete(NULL);
        return;
    }

    lv_init();

    /* Register LCD flush ISR callback */
    ESP_ERROR_CHECK(lcd_st7796_register_event_callbacks(&(esp_lcd_panel_io_callbacks_t){
        .on_color_trans_done = lcd_lvgl_flush_ready_cb,
    }, &lcd_disp_drv));

    lv_disp_draw_buf_init(&lcd_draw_buf, lcd_buf_1, NULL,
                          LCD_ST7796_H_RES * LCD_ST7796_BUF_LINES);

    lv_disp_drv_init(&lcd_disp_drv);
    lcd_disp_drv.hor_res = LCD_ST7796_H_RES;
    lcd_disp_drv.ver_res = LCD_ST7796_V_RES;
    lcd_disp_drv.flush_cb = lcd_lvgl_flush_cb;
    lcd_disp_drv.draw_buf = &lcd_draw_buf;
    disp = lv_disp_drv_register(&lcd_disp_drv);
    lv_disp_set_default(disp);

    /* Start LVGL tick timer */
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &lcd_lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lcd_lvgl_tick_timer, LCD_LVGL_TICK_MS * 1000));

    /* Create initial UI (show Matter info when available) */
    lcd_lvgl_matter_ui_create();
    ESP_LOGI(TAG, "LVGL display ready");

    while (1) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms < LCD_LVGL_HANDLER_MIN_MS) {
            wait_ms = LCD_LVGL_HANDLER_MIN_MS;
        } else if (wait_ms > LCD_LVGL_HANDLER_MAX_MS) {
            wait_ms = LCD_LVGL_HANDLER_MAX_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

BaseType_t lcd_lvgl_task_create(void)
{
    return xTaskCreate(lcd_lvgl_task, "lcd_lvgl", LCD_LVGL_TASK_STACK, NULL, 6, NULL);
}
