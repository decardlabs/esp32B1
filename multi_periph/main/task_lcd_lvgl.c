#include "task_lcd_lvgl.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp_i2c.h"
#include "lcd_st7796.h"
#include "lvgl.h"
#include "xpt2046.h"

#define LCD_LVGL_TICK_MS         2
#define LCD_LVGL_HANDLER_MIN_MS  5
#define LCD_LVGL_HANDLER_MAX_MS  20
#define LCD_LVGL_TASK_STACK      8192
#define LCD_BTN_HIT_INSET_PX     14

static const char *TAG = "TASK_LCD";

#if CONFIG_LV_FONT_MONTSERRAT_20
#define LCD_DEBUG_FONT   (&lv_font_montserrat_20)
#else
#define LCD_DEBUG_FONT   (&lv_font_montserrat_14)
#endif

static lv_disp_draw_buf_t lcd_draw_buf;
static lv_disp_drv_t lcd_disp_drv;
static lv_color_t *lcd_buf_1 = NULL;
static esp_timer_handle_t lcd_lvgl_tick_timer = NULL;
static lv_indev_drv_t lcd_touch_indev_drv;
static lv_indev_t *lcd_touch_indev = NULL;

static lv_obj_t *touch_coord_label = NULL;
static lv_obj_t *touch_raw_label = NULL;
static lv_obj_t *touch_probe_label = NULL;
static lv_obj_t *touch_click_label = NULL;
static lv_obj_t *touch_btn = NULL;

static uint16_t touch_last_x = 0;
static uint16_t touch_last_y = 0;
static uint16_t touch_last_raw_x = 0;
static uint16_t touch_last_raw_y = 0;
static uint16_t touch_last_z1 = 0;
static bool touch_last_pressed = false;
static bool touch_last_driver_touched = false;
static int touch_spi_mode = 0;
static bool touch_prev_pressed = false;
static uint32_t touch_press_edge_count = 0;
static uint32_t touch_click_count = 0;
static char touch_probe_text[64] = "TP I2C:none";

static void lcd_touch_probe_i2c(void)
{
    i2c_master_bus_handle_t bus = bsp_get_i2c0_bus_handle();
    const uint16_t candidates[] = {0x14, 0x15, 0x38, 0x5D};
    size_t found = 0;
    size_t offset = 0;

    if (bus == NULL) {
        snprintf(touch_probe_text, sizeof(touch_probe_text), "TP I2C:bus-null");
        return;
    }

    offset = (size_t)snprintf(touch_probe_text, sizeof(touch_probe_text), "TP I2C:");
    for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (i2c_master_probe(bus, candidates[i], 20) == ESP_OK) {
            int written = snprintf(touch_probe_text + offset,
                                   sizeof(touch_probe_text) - offset,
                                   "0x%02X ",
                                   (unsigned int)candidates[i]);
            if (written > 0) {
                offset += (size_t)written;
                if (offset >= sizeof(touch_probe_text)) {
                    break;
                }
            }
            found++;
        }
    }

    if (found == 0) {
        snprintf(touch_probe_text, sizeof(touch_probe_text), "TP I2C:none");
    }
}

static bool lcd_lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_disp_drv_t *disp_drv = user_ctx;
    lcd_st7796_transfer_t transfer = lcd_st7796_take_completed_transfer_from_isr();

    (void)panel_io;
    (void)edata;

    if ((transfer == LCD_ST7796_TRANSFER_LVGL) && (disp_drv != NULL)) {
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
    esp_err_t ret = lcd_st7796_draw_bitmap_owned(area->x1,
                                                 area->y1,
                                                 area->x2 + 1,
                                                 area->y2 + 1,
                                                 color_map,
                                                 LCD_ST7796_TRANSFER_LVGL);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lcd flush failed: %s", esp_err_to_name(ret));
        lv_disp_flush_ready(disp_drv);
    }
}

static void lcd_lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x = 0;
    uint16_t y = 0;
    bool pressed = false;

    (void)drv;

    if (xpt2046_read_point(LCD_ST7796_H_RES, LCD_ST7796_V_RES, &x, &y, &pressed) == ESP_OK && pressed) {
        uint16_t z1 = 0;
        uint16_t raw_x = 0;
        uint16_t raw_y = 0;
        bool drv_touched = false;

        (void)xpt2046_get_last_sample(&z1, &raw_x, &raw_y, &drv_touched);

        data->state = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
        touch_last_x = x;
        touch_last_y = y;
        touch_last_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_REL;
        touch_last_pressed = false;
    }

    if (touch_last_pressed && !touch_prev_pressed) {
        touch_press_edge_count++;
    }

    touch_prev_pressed = touch_last_pressed;

    (void)xpt2046_get_last_sample(&touch_last_z1,
                                  &touch_last_raw_x,
                                  &touch_last_raw_y,
                                  &touch_last_driver_touched);
}

static void lcd_lvgl_touch_init(void)
{
    if (xpt2046_init() != ESP_OK) {
        ESP_LOGW(TAG, "xpt2046 init failed, touch disabled");
        return;
    }

    lv_indev_drv_init(&lcd_touch_indev_drv);
    lcd_touch_indev_drv.type = LV_INDEV_TYPE_POINTER;
    lcd_touch_indev_drv.read_cb = lcd_lvgl_touch_read_cb;
    lcd_touch_indev = lv_indev_drv_register(&lcd_touch_indev_drv);

    if (lcd_touch_indev == NULL) {
        ESP_LOGW(TAG, "lvgl touch indev register failed");
    } else {
        ESP_LOGI(TAG, "touch indev ready");
    }
}

static void lcd_lvgl_touch_test_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_area_t btn_area;
        int32_t x1;
        int32_t y1;
        int32_t x2;
        int32_t y2;

        if (touch_btn == NULL) {
            return;
        }

        lv_obj_get_coords(touch_btn, &btn_area);
        x1 = btn_area.x1 + LCD_BTN_HIT_INSET_PX;
        y1 = btn_area.y1 + LCD_BTN_HIT_INSET_PX;
        x2 = btn_area.x2 - LCD_BTN_HIT_INSET_PX;
        y2 = btn_area.y2 - LCD_BTN_HIT_INSET_PX;

        if ((touch_last_x >= x1) && (touch_last_x <= x2) &&
            (touch_last_y >= y1) && (touch_last_y <= y2)) {
            touch_click_count++;
            if (touch_click_label != NULL) {
                lv_label_set_text_fmt(touch_click_label, "Clicks: %lu", (unsigned long)touch_click_count);
            }
        }
    }
}

static void lcd_lvgl_demo_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    (void)xpt2046_get_debug_info(&touch_spi_mode);

    if (touch_coord_label != NULL) {
        lv_label_set_text_fmt(touch_coord_label,
                              "T:%s X:%u Y:%u E:%lu",
                              touch_last_pressed ? "ON" : "OFF",
                              touch_last_x,
                              touch_last_y,
                              (unsigned long)touch_press_edge_count);
    }

    if (touch_raw_label != NULL) {
        lv_label_set_text_fmt(touch_raw_label,
                              "Z:%u RX:%u RY:%u D:%s",
                              touch_last_z1,
                              touch_last_raw_x,
                              touch_last_raw_y,
                              touch_last_driver_touched ? "ON" : "OFF");
    }

    if (touch_probe_label != NULL) {
        lv_label_set_text_fmt(touch_probe_label, "%s M:%d", touch_probe_text, touch_spi_mode);
    }
}

static void lcd_lvgl_demo_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *title;
    lv_obj_t *btn_label;
    lv_obj_t *touch_hint_label;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0F1720), 0);

    title = lv_label_create(scr);
    lv_label_set_text(title, "Touch Debug");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF2F6FF), 0);
    lv_obj_set_style_text_font(title, LCD_DEBUG_FONT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    touch_coord_label = lv_label_create(scr);
    lv_label_set_text(touch_coord_label, "T:OFF X:0 Y:0 E:0");
    lv_obj_set_style_text_color(touch_coord_label, lv_color_hex(0xD9E2EC), 0);
    lv_obj_set_style_text_font(touch_coord_label, LCD_DEBUG_FONT, 0);
    lv_obj_align(touch_coord_label, LV_ALIGN_TOP_LEFT, 10, 42);

    touch_raw_label = lv_label_create(scr);
    lv_label_set_text(touch_raw_label, "Z:0 RX:0 RY:0 D:OFF");
    lv_obj_set_style_text_color(touch_raw_label, lv_color_hex(0x8FE3CF), 0);
    lv_obj_set_style_text_font(touch_raw_label, LCD_DEBUG_FONT, 0);
    lv_obj_align_to(touch_raw_label, touch_coord_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    touch_probe_label = lv_label_create(scr);
    lv_label_set_text_fmt(touch_probe_label, "%s M:%d", touch_probe_text, touch_spi_mode);
    lv_obj_set_style_text_color(touch_probe_label, lv_color_hex(0xFCA5A5), 0);
    lv_obj_set_style_text_font(touch_probe_label, LCD_DEBUG_FONT, 0);
    lv_obj_align_to(touch_probe_label, touch_raw_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    touch_click_label = lv_label_create(scr);
    lv_label_set_text(touch_click_label, "Clicks: 0");
    lv_obj_set_style_text_color(touch_click_label, lv_color_hex(0xFCD34D), 0);
    lv_obj_set_style_text_font(touch_click_label, LCD_DEBUG_FONT, 0);
    lv_obj_align_to(touch_click_label, touch_probe_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    touch_hint_label = lv_label_create(scr);
    lv_label_set_text(touch_hint_label, "Touch Area");
    lv_obj_set_style_text_color(touch_hint_label, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_text_font(touch_hint_label, LCD_DEBUG_FONT, 0);
    lv_obj_align(touch_hint_label, LV_ALIGN_TOP_MID, 0, 178);

    touch_btn = lv_btn_create(scr);
    lv_obj_set_size(touch_btn, 300, 250);
    lv_obj_align(touch_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(touch_btn, lcd_lvgl_touch_test_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(touch_btn, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_color(touch_btn, lv_color_hex(0x60A5FA), 0);
    lv_obj_set_style_border_width(touch_btn, 2, 0);

    btn_label = lv_label_create(touch_btn);
    lv_label_set_text(btn_label, "Tap Me");
    lv_obj_set_style_text_font(btn_label, LCD_DEBUG_FONT, 0);
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xE5E7EB), 0);
    lv_obj_center(btn_label);

    lv_timer_create(lcd_lvgl_demo_timer_cb, 90, NULL);
}

esp_err_t lcd_lvgl_reserve_buffer(void)
{
    if (lcd_buf_1 != NULL) {
        return ESP_OK;
    }

    lcd_buf_1 = heap_caps_malloc(LCD_ST7796_H_RES * LCD_ST7796_BUF_LINES * sizeof(lv_color_t),
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (lcd_buf_1 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void lcd_lvgl_task(void *pvParameters)
{
    lv_disp_t *disp;
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lcd_lvgl_tick_cb,
        .name = "lvgl_tick",
    };

    (void)pvParameters;

    ESP_ERROR_CHECK(lcd_lvgl_reserve_buffer());

    lv_init();
    ESP_ERROR_CHECK(lcd_st7796_register_event_callbacks(&(esp_lcd_panel_io_callbacks_t) {
        .on_color_trans_done = lcd_lvgl_flush_ready_cb,
    }, &lcd_disp_drv));

    lv_disp_draw_buf_init(&lcd_draw_buf, lcd_buf_1, NULL, LCD_ST7796_H_RES * LCD_ST7796_BUF_LINES);

    lv_disp_drv_init(&lcd_disp_drv);
    lcd_disp_drv.hor_res = LCD_ST7796_H_RES;
    lcd_disp_drv.ver_res = LCD_ST7796_V_RES;
    lcd_disp_drv.flush_cb = lcd_lvgl_flush_cb;
    lcd_disp_drv.draw_buf = &lcd_draw_buf;
    disp = lv_disp_drv_register(&lcd_disp_drv);
    lv_disp_set_default(disp);

    lcd_lvgl_touch_init();
    (void)xpt2046_get_debug_info(&touch_spi_mode);
    lcd_touch_probe_i2c();

    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &lcd_lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lcd_lvgl_tick_timer, LCD_LVGL_TICK_MS * 1000));

    lcd_lvgl_demo_create();
    ESP_LOGI(TAG, "lvgl demo start");

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
    return xTaskCreate(lcd_lvgl_task, "lcd_lvgl_task", LCD_LVGL_TASK_STACK, NULL, 6, NULL);
}
