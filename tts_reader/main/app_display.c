#include "app_display.h"
#include "bsp_spi.h"
#include "lcd_st7796.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lvgl.h"

/* 16px Chinese font */
LV_FONT_DECLARE(lv_font_xiaozhi_cn_16);

static const char *TAG = "DISPLAY";
#define APP_VERSION "V1.1"
static const int LCD_W = LCD_ST7796_H_RES;
static const int LCD_H = LCD_ST7796_V_RES;

#define TXT_TOP_Y 35
#define TXT_SIDE_MARGIN 12
#define TXT_LINE_SPACE 4
#define TXT_SCROLL_STEP_PX 1
#define TXT_SCROLL_TICK_MS 35
#define TXT_SCROLL_PAUSE_TICKS 35

/* ── State ─────────────────────────────────────── */
static disp_mode_t s_mode = DISP_MODE_FILE_SELECT;
static char s_filename[64] = {0};
static char s_text[2048] = {0};
static int  s_file_idx  = 0;
static int  s_file_cnt  = 0;
static char s_file_names[64][64];
static int  s_prog_cur  = 0;
static int  s_prog_total = 0;
static int  s_vol_level = 2;
static int  s_spd_level = 2;
static int  s_vol_max = 5;
static int  s_spd_max = 5;
static volatile bool s_dirty = false;
static SemaphoreHandle_t s_ui_lock = NULL;

/* ── LVGL objects ──────────────────────────────── */
static lv_obj_t *s_scr = NULL;
/* File-select list */
#define MAX_VISIBLE_FILES 12
static lv_obj_t *s_sel_title = NULL;
static lv_obj_t *s_file_labels[MAX_VISIBLE_FILES];
static lv_obj_t *s_sel_hint = NULL;
/* Ready */
static lv_obj_t *s_rdy_file = NULL, *s_rdy_preview = NULL, *s_rdy_hint = NULL;
/* Playing */
static lv_obj_t *s_play_file = NULL, *s_play_text = NULL;
static lv_obj_t *s_play_prog = NULL, *s_play_prog_label = NULL;
static lv_obj_t *s_play_hint = NULL;
/* Paused */
static lv_obj_t *s_pause_file = NULL, *s_pause_text = NULL, *s_pause_hint = NULL;
/* Shared */
static lv_obj_t *s_vol_label = NULL, *s_spd_label = NULL;
static lv_obj_t *s_ver_label = NULL;

typedef struct {
    lv_obj_t *view;
    lv_obj_t *label;
    int32_t y;
    int32_t max_y;
    int32_t pause_ticks;
    uint32_t next_tick_ms;
} text_scroll_state_t;

static text_scroll_state_t s_rdy_scroll = {0};
static text_scroll_state_t s_play_scroll = {0};
static text_scroll_state_t s_pause_scroll = {0};

static int fit_text_height(int desired_h)
{
    int line_h = lv_font_xiaozhi_cn_16.line_height + TXT_LINE_SPACE;
    if (line_h <= 0) return desired_h;
    int lines = desired_h / line_h;
    if (lines < 1) lines = 1;
    return lines * line_h;
}

static void scroll_state_init(text_scroll_state_t *st, lv_obj_t *view, lv_obj_t *label)
{
    if (!st) return;
    st->view = view;
    st->label = label;
    st->y = 0;
    st->max_y = 0;
    st->pause_ticks = TXT_SCROLL_PAUSE_TICKS;
    st->next_tick_ms = 0;
}

static void scroll_reset(text_scroll_state_t *st)
{
    if (!st || !st->view || !st->label) return;

    lv_obj_update_layout(st->label);
    lv_obj_update_layout(st->view);

    lv_coord_t label_h = lv_obj_get_height(st->label);
    lv_coord_t view_h = lv_obj_get_content_height(st->view);
    if (view_h < 1) {
        view_h = lv_obj_get_height(st->view);
    }

    st->y = 0;
    st->pause_ticks = TXT_SCROLL_PAUSE_TICKS;
    st->next_tick_ms = (uint32_t)esp_timer_get_time() / 1000U + TXT_SCROLL_TICK_MS;
    st->max_y = (label_h > view_h) ? (label_h - view_h) : 0;
    lv_obj_scroll_to_y(st->view, 0, LV_ANIM_OFF);
}

static void scroll_step(text_scroll_state_t *st, uint32_t now_ms)
{
    if (!st || !st->view || !st->label) return;
    if (st->max_y <= 0) return;
    if (now_ms < st->next_tick_ms) return;

    st->next_tick_ms = now_ms + TXT_SCROLL_TICK_MS;
    if (st->pause_ticks > 0) {
        st->pause_ticks--;
        return;
    }

    st->y += TXT_SCROLL_STEP_PX;
    if (st->y >= st->max_y) {
        st->y = 0;
        st->pause_ticks = TXT_SCROLL_PAUSE_TICKS;
    }
    lv_obj_scroll_to_y(st->view, st->y, LV_ANIM_OFF);
}

static text_scroll_state_t *current_scroll_state(void)
{
    switch (s_mode) {
    case DISP_MODE_READY:
        return &s_rdy_scroll;
    case DISP_MODE_PLAYING:
        return &s_play_scroll;
    case DISP_MODE_PAUSED:
        return &s_pause_scroll;
    default:
        return NULL;
    }
}

/* ── LVGL callbacks ────────────────────────────── */
static bool lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    lcd_st7796_transfer_t t = lcd_st7796_take_completed_transfer_from_isr();
    if (t == LCD_ST7796_TRANSFER_LVGL && drv) lv_disp_flush_ready(drv);
    (void)panel_io; (void)edata;
    return false;
}

static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *map)
{
    esp_err_t ret = lcd_st7796_draw_bitmap_owned(
        area->x1, area->y1, area->x2+1, area->y2+1, map, LCD_ST7796_TRANSFER_LVGL);
    if (ret != ESP_OK) lv_disp_flush_ready(drv);
}

static void lvgl_tick_cb(void *arg) { (void)arg; lv_tick_inc(2); }


/* ── Screen switching ──────────────────────────── */
static void hide_all(void)
{
    lv_obj_add_flag(s_sel_title, LV_OBJ_FLAG_HIDDEN);
    for (int i=0;i<MAX_VISIBLE_FILES;i++) lv_obj_add_flag(s_file_labels[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sel_hint,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rdy_file,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rdy_preview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rdy_hint,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_file, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_prog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_prog_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pause_file, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pause_text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pause_hint, LV_OBJ_FLAG_HIDDEN);
}

static void show_file_select(void)
{
    hide_all();
    lv_obj_clear_flag(s_sel_title, LV_OBJ_FLAG_HIDDEN);
    int shown = 0;
    for (int i = 0; i < s_file_cnt && shown < MAX_VISIBLE_FILES; i++, shown++) {
        lv_obj_clear_flag(s_file_labels[shown], LV_OBJ_FLAG_HIDDEN);
        if (i == s_file_idx) {
            char buf[64];
            snprintf(buf, sizeof(buf), "> %s", s_file_names[i]);
            lv_label_set_text(s_file_labels[shown], buf);
            lv_obj_set_style_text_color(s_file_labels[shown], lv_color_hex(0xD4A04A), 0);
            lv_obj_set_style_text_font(s_file_labels[shown], &lv_font_xiaozhi_cn_16, 0);
        } else {
            lv_label_set_text(s_file_labels[shown], s_file_names[i]);
            lv_obj_set_style_text_color(s_file_labels[shown], lv_color_hex(0x888888), 0);
            lv_obj_set_style_text_font(s_file_labels[shown], &lv_font_xiaozhi_cn_16, 0);
        }
    }
}

static void update_status_labels(void)
{
    char b[32];
    snprintf(b,sizeof(b),"VOL %d/%d", s_vol_level+1, s_vol_max+1);
    lv_label_set_text(s_vol_label,b);

    snprintf(b,sizeof(b),"SPD %d/%d", s_spd_level+1, s_spd_max+1);
    lv_label_set_text(s_spd_label,b);
}

static void show_ready(void)
{
    hide_all();
    lv_obj_clear_flag(s_rdy_file,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_rdy_preview, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_rdy_file, s_filename[0]?s_filename:"(N/A)");
    lv_label_set_text(s_rdy_preview, s_text[0]?s_text:"(empty)");
    scroll_reset(&s_rdy_scroll);
}

static void show_playing(void)
{
    hide_all();
    lv_obj_clear_flag(s_play_file, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_play_text, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_play_prog, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_play_file, s_filename[0]?s_filename:"");
    lv_label_set_text(s_play_text, s_text[0]?s_text:"(empty)");
    scroll_reset(&s_play_scroll);
    if (s_prog_total>0) {
        int pct = (s_prog_cur*100)/s_prog_total;
        if (pct>100) pct=100;
        lv_bar_set_value(s_play_prog, pct, LV_ANIM_OFF);
    }
}

static void show_paused(void)
{
    hide_all();
    lv_obj_clear_flag(s_pause_file, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_pause_text, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_pause_file, s_filename[0]?s_filename:"");
    lv_label_set_text(s_pause_text, s_text[0]?s_text:"(empty)");
    scroll_reset(&s_pause_scroll);
    if (s_prog_total>0) {
        int pct = (s_prog_cur*100)/s_prog_total;
        if (pct>100) pct=100;
        lv_bar_set_value(s_play_prog, pct, LV_ANIM_OFF);
        lv_obj_clear_flag(s_play_prog, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── LVGL task ────────────────────────────────── */
static void lvgl_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_ui_lock && xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_dirty) {
                s_dirty=false;
                update_status_labels();
                switch (s_mode) {
                case DISP_MODE_FILE_SELECT: show_file_select(); break;
                case DISP_MODE_READY:       show_ready();       break;
                case DISP_MODE_PLAYING:    show_playing();    break;
                case DISP_MODE_PAUSED:     show_paused();     break;
                case DISP_MODE_NO_SD:
                    hide_all();
                    lv_label_set_text(s_rdy_preview, "NO TF CARD\nINSERT TF CARD AND RESTART");
                    lv_obj_clear_flag(s_rdy_preview, LV_OBJ_FLAG_HIDDEN);
                    break;
                case DISP_MODE_NO_FILES:
                    hide_all();
                    lv_label_set_text(s_rdy_preview, "NO .TXT FILES\nCOPY FILES TO TF CARD");
                    lv_obj_clear_flag(s_rdy_preview, LV_OBJ_FLAG_HIDDEN);
                    break;
                default: break;
                }
            }
            xSemaphoreGive(s_ui_lock);
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        text_scroll_state_t *st = current_scroll_state();
        if (st) {
            scroll_step(st, now_ms);
        }

        uint32_t ms = lv_timer_handler();
        if (ms<5) ms=5;
        if (ms>20) ms=20;
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

/* ── UI construction ───────────────────────────── */
static void build_ui(void)
{
    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0xF5F0EB), 0);

    /* Volume / Speed at bottom corners */
    s_vol_label = lv_label_create(s_scr);
    lv_label_set_text(s_vol_label, "VOL 3/5");
    lv_obj_align(s_vol_label, LV_ALIGN_BOTTOM_LEFT, 10, -16);
    lv_obj_set_style_text_font(s_vol_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(0x4A7A4A), 0);

    s_spd_label = lv_label_create(s_scr);
    lv_label_set_text(s_spd_label, "SPD 3/5");
    lv_obj_align(s_spd_label, LV_ALIGN_BOTTOM_RIGHT, -10, -16);
    lv_obj_set_style_text_font(s_spd_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_spd_label, lv_color_hex(0x4A4A7A), 0);

    /* Version label (always visible) */
    s_ver_label = lv_label_create(s_scr);
    lv_label_set_text(s_ver_label, APP_VERSION);
    lv_obj_align(s_ver_label, LV_ALIGN_TOP_RIGHT, -8, 10);
    lv_obj_set_style_text_font(s_ver_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_ver_label, lv_color_hex(0xBBBBBB), 0);

    /* ── File-select list ── */
    s_sel_title = lv_label_create(s_scr);
    lv_label_set_text(s_sel_title, "CHOOSE FILE");
    lv_obj_align(s_sel_title, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(s_sel_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sel_title, lv_color_hex(0x888888), 0);

    int y = 40;
    for (int i = 0; i < MAX_VISIBLE_FILES; i++) {
        s_file_labels[i] = lv_label_create(s_scr);
        lv_label_set_text(s_file_labels[i], "");
        lv_obj_set_pos(s_file_labels[i], 24, y);
        lv_obj_set_style_text_font(s_file_labels[i], &lv_font_xiaozhi_cn_16, 0);
        lv_obj_add_flag(s_file_labels[i], LV_OBJ_FLAG_HIDDEN);
        y += 30;
    }

    s_sel_hint = lv_label_create(s_scr);
    lv_label_set_text(s_sel_hint, "K1 NEXT              K4 OK");
    lv_obj_align(s_sel_hint, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_text_font(s_sel_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_sel_hint, lv_color_hex(0xD4A04A), 0);

    /* ── Ready ── */
    s_rdy_file = lv_label_create(s_scr);
    lv_label_set_text(s_rdy_file, "article.txt");
    lv_obj_align(s_rdy_file, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(s_rdy_file, &lv_font_xiaozhi_cn_16, 0);
    lv_obj_set_style_text_color(s_rdy_file, lv_color_hex(0x333333), 0);

    s_rdy_preview = lv_label_create(s_scr);
    lv_label_set_text(s_rdy_preview, "(preview)");
    lv_obj_set_width(s_rdy_preview, LCD_W - (TXT_SIDE_MARGIN * 2));
    lv_obj_set_height(s_rdy_preview, fit_text_height(380));
    lv_obj_align(s_rdy_preview, LV_ALIGN_TOP_MID, 0, TXT_TOP_Y);
    lv_label_set_long_mode(s_rdy_preview, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_rdy_preview, &lv_font_xiaozhi_cn_16, 0);
    lv_obj_set_style_text_line_space(s_rdy_preview, TXT_LINE_SPACE, 0);
    lv_obj_set_style_text_color(s_rdy_preview, lv_color_hex(0x555555), 0);
    lv_obj_add_flag(s_rdy_preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_rdy_preview, LV_SCROLLBAR_MODE_OFF);
    scroll_state_init(&s_rdy_scroll, s_rdy_preview, s_rdy_preview);

    s_rdy_hint = lv_label_create(s_scr);
    lv_label_set_text(s_rdy_hint, "K1 BACK              K4 PLAY");
    lv_obj_align(s_rdy_hint, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_text_font(s_rdy_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_rdy_hint, lv_color_hex(0xD4A04A), 0);

    /* ── Playing ── */
    s_play_file = lv_label_create(s_scr);
    lv_label_set_text(s_play_file, "article.txt");
    lv_obj_align(s_play_file, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(s_play_file, &lv_font_xiaozhi_cn_16, 0);
    lv_obj_set_style_text_color(s_play_file, lv_color_hex(0x555555), 0);

    s_play_text = lv_label_create(s_scr);
    lv_label_set_text(s_play_text, "(reading)");
    lv_obj_set_width(s_play_text, LCD_W - (TXT_SIDE_MARGIN * 2));
    lv_obj_set_height(s_play_text, fit_text_height(350));
    lv_obj_align(s_play_text, LV_ALIGN_TOP_MID, 0, TXT_TOP_Y);
    lv_label_set_long_mode(s_play_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_play_text, &lv_font_xiaozhi_cn_16, 0);
    lv_obj_set_style_text_line_space(s_play_text, TXT_LINE_SPACE, 0);
    lv_obj_set_style_text_color(s_play_text, lv_color_hex(0x222222), 0);
    lv_obj_add_flag(s_play_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_play_text, LV_SCROLLBAR_MODE_OFF);
    scroll_state_init(&s_play_scroll, s_play_text, s_play_text);

    s_play_prog = lv_bar_create(s_scr);
    lv_obj_set_size(s_play_prog, LCD_W-140, 6);
    lv_obj_align(s_play_prog, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_bar_set_range(s_play_prog, 0, 100);
    lv_obj_set_style_bg_color(s_play_prog, lv_color_hex(0xE0D8C8), 0);
    lv_obj_set_style_bg_color(s_play_prog, lv_color_hex(0xD4A04A), LV_PART_INDICATOR);

    s_play_prog_label = lv_label_create(s_scr);
    lv_label_set_text(s_play_prog_label, "0 / 0");
    lv_obj_align(s_play_prog_label, LV_ALIGN_BOTTOM_MID, 0, -44);
    lv_obj_set_style_text_font(s_play_prog_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_play_prog_label, lv_color_hex(0x999999), 0);

    s_play_hint = lv_label_create(s_scr);
    lv_label_set_text(s_play_hint, "K1 BACK              K4 PAUSE");
    lv_obj_align(s_play_hint, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_text_font(s_play_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_play_hint, lv_color_hex(0xD4A04A), 0);

    /* ── Paused ── */
    s_pause_file = lv_label_create(s_scr);
    lv_label_set_text(s_pause_file, "article.txt");
    lv_obj_align(s_pause_file, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_text_font(s_pause_file, &lv_font_xiaozhi_cn_16, 0);
    lv_obj_set_style_text_color(s_pause_file, lv_color_hex(0x555555), 0);

    s_pause_text = lv_label_create(s_scr);
    lv_label_set_text(s_pause_text, "(paused)");
    lv_obj_set_width(s_pause_text, LCD_W - (TXT_SIDE_MARGIN * 2));
    lv_obj_set_height(s_pause_text, fit_text_height(380));
    lv_obj_align(s_pause_text, LV_ALIGN_TOP_MID, 0, TXT_TOP_Y);
    lv_label_set_long_mode(s_pause_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_pause_text, &lv_font_xiaozhi_cn_16, 0);
    lv_obj_set_style_text_line_space(s_pause_text, TXT_LINE_SPACE, 0);
    lv_obj_set_style_text_color(s_pause_text, lv_color_hex(0x999999), 0);
    lv_obj_add_flag(s_pause_text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_pause_text, LV_SCROLLBAR_MODE_OFF);
    scroll_state_init(&s_pause_scroll, s_pause_text, s_pause_text);

    s_pause_hint = lv_label_create(s_scr);
    lv_label_set_text(s_pause_hint, "K1 BACK              K4 CONT");
    lv_obj_align(s_pause_hint, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_text_font(s_pause_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_pause_hint, lv_color_hex(0x888888), 0);

    hide_all();
}

/* ── Public API ────────────────────────────────── */
esp_err_t app_display_init(void)
{
    if (!s_ui_lock) {
        s_ui_lock = xSemaphoreCreateMutex();
        if (!s_ui_lock) return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(bsp_spi2_lcd_init(), TAG, "spi");
    ESP_RETURN_ON_ERROR(lcd_st7796_init(),   TAG, "lcd");

    lv_init();

    /* LVGL draw buffer */
    size_t buf_sz = LCD_W * 20 * sizeof(lv_color_t);
    lv_color_t *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if (!buf) buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LCD_W*20);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_W;
    disp_drv.ver_res = LCD_H;
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;

    ESP_RETURN_ON_ERROR(
        lcd_st7796_register_event_callbacks(
            &(esp_lcd_panel_io_callbacks_t){.on_color_trans_done = lvgl_flush_ready_cb},
            &disp_drv),
        TAG, "flush cb");

    lv_disp_drv_register(&disp_drv);

    build_ui();

    /* LVGL tick: 2 ms */
    const esp_timer_create_args_t ta = {.callback = lvgl_tick_cb, .name = "lvgl"};
    esp_timer_handle_t th;
    esp_timer_create(&ta, &th);
    esp_timer_start_periodic(th, 2000);

    xTaskCreate(lvgl_task, "lvgl_task", 6144, NULL, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Display ready (%dx%d)", LCD_W, LCD_H);
    return ESP_OK;
}

void app_display_set_mode(disp_mode_t mode)
{
    if (!s_ui_lock) return;
    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_mode=mode;
        s_dirty=true;
        xSemaphoreGive(s_ui_lock);
    }
}

void app_display_set_filename(const char *n)
{
    if (!s_ui_lock) return;
    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if(n) strncpy(s_filename,n,63);
        s_filename[63]=0;
        s_dirty=true;
        xSemaphoreGive(s_ui_lock);
    }
}

void app_display_set_text(const char *t)
{
    if (!s_ui_lock) return;
    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (t) strncpy(s_text,t,sizeof(s_text)-1);
        s_text[sizeof(s_text)-1]=0;
        s_dirty=true;
        xSemaphoreGive(s_ui_lock);
    }
}

void app_display_set_file_list(const char names[][64], int cnt, int sel)
{
    if (!s_ui_lock) return;
    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_file_cnt = cnt>64?64:cnt;
        s_file_idx = sel;
        memcpy(s_file_names, names, (size_t)s_file_cnt*64);
        s_dirty=true;
        xSemaphoreGive(s_ui_lock);
    }
}

void app_display_set_progress(int cur, int tot)
{
    if (!s_ui_lock) return;
    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_prog_cur=cur;
        s_prog_total=tot;
        s_dirty=true;
        xSemaphoreGive(s_ui_lock);
    }
}

void app_display_set_volume(int lv, int mx)
{
    if (!s_ui_lock) return;
    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_vol_level=lv;
        s_vol_max=mx;
        s_dirty=true;
        xSemaphoreGive(s_ui_lock);
    }
}

void app_display_set_speed(int lv, int mx)
{
    if (!s_ui_lock) return;
    if (xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_spd_level=lv;
        s_spd_max=mx;
        s_dirty=true;
        xSemaphoreGive(s_ui_lock);
    }
}
