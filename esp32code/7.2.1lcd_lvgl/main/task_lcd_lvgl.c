#include "task_lcd_lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lcd_st7796.h"
#include "lvgl.h"

#define LCD_LVGL_TICK_MS         2
#define LCD_LVGL_HANDLER_MIN_MS  5
#define LCD_LVGL_HANDLER_MAX_MS  20
#define LCD_LVGL_TASK_STACK      8192

static const char *TAG = "TASK_LCD";

static lv_disp_draw_buf_t lcd_draw_buf;
static lv_disp_drv_t lcd_disp_drv;
static lv_color_t *lcd_buf_1 = NULL;
static esp_timer_handle_t lcd_lvgl_tick_timer = NULL;
static lv_obj_t *demo_arc = NULL;
static lv_obj_t *demo_bar = NULL;
static lv_obj_t *demo_value_label = NULL;
static int demo_value = 0;
static int demo_dir = 1;

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

static void lcd_lvgl_demo_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    demo_value += demo_dir;
    if (demo_value >= 100) {
        demo_value = 100;
        demo_dir = -1;
    } else if (demo_value <= 0) {
        demo_value = 0;
        demo_dir = 1;
    }

    lv_arc_set_value(demo_arc, demo_value);
    lv_bar_set_value(demo_bar, demo_value, LV_ANIM_ON);
    lv_label_set_text_fmt(demo_value_label, "%d%%", demo_value);
}

static void lcd_lvgl_demo_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *panel;
    lv_obj_t *label;
    lv_obj_t *spinner;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

    title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32S3 ST7796");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF2F6FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "LVGL 8.3 Demo");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x8FE3CF), 0);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 260, 300);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x182635), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2B4358), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -30);

    demo_arc = lv_arc_create(panel);
    lv_obj_set_size(demo_arc, 150, 150);
    lv_arc_set_rotation(demo_arc, 135);
    lv_arc_set_bg_angles(demo_arc, 0, 270);
    lv_arc_set_range(demo_arc, 0, 100);
    lv_arc_set_value(demo_arc, demo_value);
    lv_obj_remove_style(demo_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(demo_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(demo_arc, LV_ALIGN_TOP_MID, 0, 6);

    demo_value_label = lv_label_create(panel);
    lv_label_set_text(demo_value_label, "0%");
    lv_obj_set_style_text_color(demo_value_label, lv_color_hex(0xF2F6FF), 0);
    lv_obj_align_to(demo_value_label, demo_arc, LV_ALIGN_CENTER, 0, 0);

    demo_bar = lv_bar_create(panel);
    lv_obj_set_size(demo_bar, 210, 14);
    lv_bar_set_range(demo_bar, 0, 100);
    lv_bar_set_value(demo_bar, demo_value, LV_ANIM_OFF);
    lv_obj_align_to(demo_bar, demo_arc, LV_ALIGN_OUT_BOTTOM_MID, 0, 22);

    spinner = lv_spinner_create(panel, 1200, 80);
    lv_obj_set_size(spinner, 48, 48);
    lv_obj_align(spinner, LV_ALIGN_BOTTOM_LEFT, 12, -10);

    label = lv_label_create(panel);
    lv_label_set_text(label, "Display OK");
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6EEF8), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_RIGHT, -12, -20);

    lv_timer_create(lcd_lvgl_demo_timer_cb, 35, NULL);
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
