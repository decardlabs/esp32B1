/**
 * @file task_qa_lvgl.c
 * @brief LVGL-based Q&A dialog UI for the 320x480 ST7796 display.
 *
 * Layout (4 zones summing to 480 px vertically):
 *   Top bar     (y=  0, h= 36) — dark blue bg, title label, Wi‑Fi label
 *   Chat area   (y= 36, h=264) — scrollable user / assistant messages
 *   Process log (y=300, h=140) — scrollable step‑info messages
 *   Bottom bar  (y=440, h= 40) — status text + hint text
 *
 * Thread safety is achieved through a FreeRTOS queue (length 32).
 * Every public UI function merely posts a message to the queue; the LVGL
 * task drains the queue in its main loop.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "lcd_st7796.h"
#include "task_qa_lvgl.h"

/* ===================== Fonts ========================= */
LV_FONT_DECLARE(lv_font_xiaozhi_cn_16);

/* ===================== Constants ===================== */
static const char *TAG = "QA_LVGL";

#define QA_QUEUE_LEN            32

/* Layout dimensions (320 x 480 ST7796) */
#define TOP_BAR_H               36
#define CHAT_Y                  36
#define CHAT_H                  264
#define LOG_Y                   300
#define LOG_H                   140
#define BOT_BAR_Y               440
#define BOT_BAR_H               40

/* Display buffer — 320 * 20 * sizeof(lv_color_t) = 12800 bytes */
#define LCD_BUF_ROWS            20
#define LCD_BUF_SIZE            (LCD_ST7796_H_RES * LCD_BUF_ROWS * sizeof(lv_color_t))

/* LVGL timing */
#define QA_LVGL_TICK_MS         2
#define QA_LVGL_HANDLER_MS      33

/* ===================== Colors ======================== */
#define COL_BG          0x05050A
#define COL_BAR         0x0D1B2A
#define COL_TITLE       0xD9E2F0
#define COL_WIFI_ON     0x67E8F9
#define COL_USER        0x63A7FF
#define COL_ASSISTANT   0x63E59C
#define COL_LOG         0x8FA3C2
#define COL_STATUS      0xA8B6C9
#define COL_PANEL       0x0C1018

/* ====================== Types ======================== */
typedef enum {
    QA_MSG_USER,
    QA_MSG_ASSISTANT,
    QA_MSG_LOG,
    QA_MSG_STATUS,
    QA_MSG_CLEAR,
} qa_msg_type_t;

typedef struct {
    qa_msg_type_t type;
    char text[512];
} qa_msg_t;

/* ==================== Static globals ================= */

/* FreeRTOS queue (core thread‑safety mechanism) */
static QueueHandle_t s_qa_queue = NULL;

/* Display buffer allocated via heap_caps_malloc(…, MALLOC_CAP_DMA) */
static lv_color_t *s_display_buf = NULL;

/* Pointer to the LVGL driver that LVGL passes into flush_cb.
 * Captured in flush_cb and consumed in the transfer‑done ISR callback. */
static lv_disp_drv_t *s_flush_drv = NULL;

/* Current memory‑degradation level (set from main loop, read by LVGL task) */
static volatile qa_degrade_level_t s_degrade = QA_DEGRADE_NONE;

/* UI object handles — cached so the message processor can update them */
static lv_obj_t *s_top_bar         = NULL;
static lv_obj_t *s_title_label     = NULL;
static lv_obj_t *s_wifi_label      = NULL;
static lv_obj_t *s_chat_cont       = NULL;
static lv_obj_t *s_log_cont        = NULL;
static lv_obj_t *s_chat_placeholder = NULL;
static lv_obj_t *s_bot_bar         = NULL;
static lv_obj_t *s_status_label    = NULL;
static lv_obj_t *s_hint_label      = NULL;

/* ================== Forward declarations ============= */
static void lvgl_task_entry(void *arg);
static void lvgl_tick_cb(void *arg);
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_p);
static bool lvgl_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx);
static void lvgl_create_ui(void);
static void qa_ui_process_msg(const qa_msg_t *msg);

/* ================================================================
 *  Public API
 * ================================================================ */

esp_err_t lcd_lvgl_reserve_buffer(void)
{
    if (s_display_buf) {
        return ESP_OK;
    }

    s_display_buf = (lv_color_t *)heap_caps_malloc(LCD_BUF_SIZE,
                                                    MALLOC_CAP_DMA);
    if (!s_display_buf) {
        ESP_LOGE(TAG, "Failed to allocate display buffer (%d bytes) "
                 "from DMA-capable memory", LCD_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Display buffer allocated: %d bytes (DMA)", LCD_BUF_SIZE);
    return ESP_OK;
}

BaseType_t lcd_lvgl_task_create(void)
{
    if (!s_display_buf) {
        ESP_LOGE(TAG, "Call lcd_lvgl_reserve_buffer() first");
        return pdFALSE;
    }

    s_qa_queue = xQueueCreate(QA_QUEUE_LEN, sizeof(qa_msg_t));
    if (!s_qa_queue) {
        ESP_LOGE(TAG, "Failed to create QA queue (len=%d, msg_sz=%zu)",
                 QA_QUEUE_LEN, sizeof(qa_msg_t));
        return pdFALSE;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        lvgl_task_entry,
        "qa_lvgl_task",
        8192,       /* stack size in words */
        NULL,       /* task parameter */
        5,          /* priority */
        NULL,       /* task handle (not needed) */
        1           /* core 1 (APP core) */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        vQueueDelete(s_qa_queue);
        s_qa_queue = NULL;
    }

    return ret;
}

void qa_ui_add_user_msg(const char *text)
{
    if (!s_qa_queue) return;

    qa_msg_t msg = { .type = QA_MSG_USER };
    strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';
    xQueueSend(s_qa_queue, &msg, 0);
}

void qa_ui_add_assistant_msg(const char *text)
{
    if (!s_qa_queue) return;

    qa_msg_t msg = { .type = QA_MSG_ASSISTANT };
    strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';
    xQueueSend(s_qa_queue, &msg, 0);
}

void qa_ui_add_log(const char *fmt, ...)
{
    if (!s_qa_queue) return;

    qa_msg_t msg = { .type = QA_MSG_LOG };
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg.text, sizeof(msg.text), fmt, args);
    va_end(args);
    xQueueSend(s_qa_queue, &msg, 0);
}

void qa_ui_set_status(const char *text)
{
    if (!s_qa_queue) return;

    qa_msg_t msg = { .type = QA_MSG_STATUS };
    strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';
    xQueueSend(s_qa_queue, &msg, 0);
}

void qa_ui_clear_all(void)
{
    if (!s_qa_queue) return;

    qa_msg_t msg = { .type = QA_MSG_CLEAR };
    xQueueSend(s_qa_queue, &msg, 0);
}

qa_degrade_level_t qa_degrade_get_level(void)
{
    return s_degrade;
}

esp_err_t qa_degrade_step_up(void)
{
    if (s_degrade >= QA_DEGRADE_MINIMAL) {
        return ESP_FAIL;
    }
    s_degrade++;
    ESP_LOGW(TAG, "Degradation stepped up to level %d", (int)s_degrade);
    return ESP_OK;
}

void qa_degrade_reset(void)
{
    s_degrade = QA_DEGRADE_NONE;
    ESP_LOGI(TAG, "Degradation reset to NONE");
}

/* ================================================================
 *  LVGL internal helpers
 * ================================================================ */

/**
 * @brief Periodic tick callback — drives LVGL's internal timekeeping.
 *
 * Called every QA_LVGL_TICK_MS via an esp_timer (ISR or task context).
 */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(QA_LVGL_TICK_MS);
}

/**
 * @brief LVGL display flush callback.
 *
 * Called from within lv_timer_handler() whenever a display buffer needs
 * to be pushed to the physical LCD.  We hand the buffer to the ST7796
 * driver via draw_bitmap_owned() with the LVGL ownership flag.
 *
 * On success the transfer runs asynchronously — the ST7796 ISR will
 * eventually invoke lvgl_trans_done_cb() which calls lv_disp_flush_ready().
 * On error we must call lv_disp_flush_ready() immediately to avoid
 * deadlocking LVGL.
 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_p)
{
    s_flush_drv = drv;

    esp_err_t ret = lcd_st7796_draw_bitmap_owned(
        area->x1,
        area->y1,
        area->x2 + 1,       /* draw_bitmap_owned expects exclusive end */
        area->y2 + 1,
        (const void *)color_p,
        LCD_ST7796_TRANSFER_LVGL
    );

    if (ret != ESP_OK) {
        /* Transfer did not start — signal LVGL so it can continue */
        lv_disp_flush_ready(drv);
    }
}

/**
 * @brief ST7796 color‑transfer complete callback (called from SPI ISR).
 *
 * Unconditionally calls lv_disp_flush_ready() to tell LVGL the current
 * flush buffer is free to use for the next frame.
 */
static bool lvgl_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    if (s_flush_drv) {
        lv_disp_flush_ready(s_flush_drv);
    }

    return false;
}

/* ================================================================
 *  UI construction
 * ================================================================ */

static void lvgl_create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* Screen background */
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);

    /* ---------------------------------------------------------------
     *  Top bar (y=0, h=36)
     * --------------------------------------------------------------- */
    s_top_bar = lv_obj_create(scr);
    lv_obj_set_size(s_top_bar, LCD_ST7796_H_RES, TOP_BAR_H);
    lv_obj_set_pos(s_top_bar, 0, 0);
    lv_obj_set_style_bg_color(s_top_bar, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_border_width(s_top_bar, 0, 0);
    lv_obj_set_style_pad_all(s_top_bar, 0, 0);
    lv_obj_set_style_radius(s_top_bar, 0, 0);

    /* Title */
    s_title_label = lv_label_create(s_top_bar);
    lv_label_set_text(s_title_label, "Q&A系统");
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_title_label, LV_ALIGN_LEFT_MID, 8, 0);

    /* Wi-Fi status */
    s_wifi_label = lv_label_create(s_top_bar);
    lv_label_set_text(s_wifi_label, "o 在线");
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(COL_WIFI_ON), 0);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_wifi_label, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ---------------------------------------------------------------
     *  Chat area (y=36, h=264)
     * --------------------------------------------------------------- */
    s_chat_cont = lv_obj_create(scr);
    lv_obj_set_size(s_chat_cont, LCD_ST7796_H_RES, CHAT_H);
    lv_obj_set_pos(s_chat_cont, 0, CHAT_Y);
    lv_obj_set_style_bg_color(s_chat_cont, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(s_chat_cont, 0, 0);
    lv_obj_set_style_pad_all(s_chat_cont, 4, 0);
    lv_obj_set_style_radius(s_chat_cont, 0, 0);
    lv_obj_set_scrollbar_mode(s_chat_cont, LV_SCROLLBAR_MODE_OFF);

    /* Placeholder shown before the first message */
    s_chat_placeholder = lv_label_create(s_chat_cont);
    lv_label_set_text(s_chat_placeholder, "开始对话后，消息将显示在这里");
    lv_obj_set_style_text_color(s_chat_placeholder, lv_color_hex(COL_LOG), 0);
    lv_obj_set_style_text_font(s_chat_placeholder, &lv_font_xiaozhi_cn_16, 0);
    lv_obj_center(s_chat_placeholder);

    /* ---------------------------------------------------------------
     *  Process log area (y=300, h=140)
     * --------------------------------------------------------------- */
    s_log_cont = lv_obj_create(scr);
    lv_obj_set_size(s_log_cont, LCD_ST7796_H_RES, LOG_H);
    lv_obj_set_pos(s_log_cont, 0, LOG_Y);
    lv_obj_set_style_bg_color(s_log_cont, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_width(s_log_cont, 0, 0);
    lv_obj_set_style_pad_all(s_log_cont, 4, 0);
    lv_obj_set_style_radius(s_log_cont, 0, 0);
    lv_obj_set_scrollbar_mode(s_log_cont, LV_SCROLLBAR_MODE_OFF);

    /* ---------------------------------------------------------------
     *  Bottom bar (y=440, h=40)
     * --------------------------------------------------------------- */
    s_bot_bar = lv_obj_create(scr);
    lv_obj_set_size(s_bot_bar, LCD_ST7796_H_RES, BOT_BAR_H);
    lv_obj_set_pos(s_bot_bar, 0, BOT_BAR_Y);
    lv_obj_set_style_bg_color(s_bot_bar, lv_color_hex(COL_BAR), 0);
    lv_obj_set_style_border_width(s_bot_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bot_bar, 0, 0);
    lv_obj_set_style_radius(s_bot_bar, 0, 0);

    /* Status text */
    s_status_label = lv_label_create(s_bot_bar);
    lv_label_set_text(s_status_label, "o 待命中");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COL_STATUS), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 8, 0);

    /* Hint text */
    s_hint_label = lv_label_create(s_bot_bar);
    lv_label_set_text(s_hint_label, "按住KEY3说话");
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(COL_STATUS), 0);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_RIGHT_MID, -8, 0);
}

/* ================================================================
 *  Queue message processing (runs inside LVGL task context)
 * ================================================================ */

static void qa_ui_process_msg(const qa_msg_t *msg)
{
    switch (msg->type) {

    case QA_MSG_USER:
        /* Remove placeholder on first message */
        if (s_chat_placeholder) {
            lv_obj_del(s_chat_placeholder);
            s_chat_placeholder = NULL;
        }
        {
            lv_obj_t *l = lv_label_create(s_chat_cont);
            lv_label_set_text(l, msg->text);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_USER), 0);
            lv_obj_set_style_text_font(l, &lv_font_xiaozhi_cn_16, 0);
            lv_obj_set_width(l, LCD_ST7796_H_RES - 8);
            lv_obj_scroll_to_y(s_chat_cont, LV_COORD_MAX, LV_ANIM_OFF);
        }
        break;

    case QA_MSG_ASSISTANT:
        /* Remove placeholder on first message */
        if (s_chat_placeholder) {
            lv_obj_del(s_chat_placeholder);
            s_chat_placeholder = NULL;
        }
        {
            lv_obj_t *l = lv_label_create(s_chat_cont);
            lv_label_set_text(l, msg->text);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_ASSISTANT), 0);
            lv_obj_set_style_text_font(l, &lv_font_xiaozhi_cn_16, 0);
            lv_obj_set_width(l, LCD_ST7796_H_RES - 8);
            lv_obj_scroll_to_y(s_chat_cont, LV_COORD_MAX, LV_ANIM_OFF);
        }
        break;

    case QA_MSG_LOG:
        {
            lv_obj_t *l = lv_label_create(s_log_cont);
            lv_label_set_text(l, msg->text);
            lv_obj_set_style_text_color(l, lv_color_hex(COL_LOG), 0);
            lv_obj_set_style_text_font(l, &lv_font_xiaozhi_cn_16, 0);
            lv_obj_set_width(l, LCD_ST7796_H_RES - 8);
            lv_obj_scroll_to_y(s_log_cont, LV_COORD_MAX, LV_ANIM_OFF);
        }
        break;

    case QA_MSG_STATUS:
        if (s_status_label) {
            lv_label_set_text(s_status_label, msg->text);
        }
        break;

    case QA_MSG_CLEAR:
        /* Wipe chat and log panels */
        lv_obj_clean(s_chat_cont);
        lv_obj_clean(s_log_cont);

        /* Restore chat placeholder */
        s_chat_placeholder = lv_label_create(s_chat_cont);
        lv_label_set_text(s_chat_placeholder, "开始对话后，消息将显示在这里");
        lv_obj_set_style_text_color(s_chat_placeholder,
                                    lv_color_hex(COL_LOG), 0);
        lv_obj_set_style_text_font(s_chat_placeholder,
                                   &lv_font_xiaozhi_cn_16, 0);
        lv_obj_center(s_chat_placeholder);

        /* Reset status text */
        if (s_status_label) {
            lv_label_set_text(s_status_label, "o 待命中");
        }
        break;
    }
}

/* ================================================================
 *  LVGL main task
 * ================================================================ */

static void lvgl_task_entry(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "LVGL task started on core %d", xPortGetCoreID());

    /* -------- Initialise LVGL core -------- */
    lv_init();

    /* -------- Display buffer -------- */
    static lv_disp_draw_buf_t draw_buf;
    uint32_t buf_px_cnt = LCD_ST7796_H_RES * LCD_BUF_ROWS;

    /* Apply degradation: use half the buffer rows */
    if (s_degrade >= QA_DEGRADE_LVGL_BUF_HALF) {
        buf_px_cnt = LCD_ST7796_H_RES * (LCD_BUF_ROWS / 2);
    }

    lv_disp_draw_buf_init(&draw_buf, s_display_buf, NULL, buf_px_cnt);

    /* -------- Display driver -------- */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LCD_ST7796_H_RES;
    disp_drv.ver_res  = LCD_ST7796_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;

    if (s_degrade >= QA_DEGRADE_MINIMAL) {
        disp_drv.full_refresh = 1;
    }

    lv_disp_drv_register(&disp_drv);

    /* -------- Register LCD transfer-complete callback -------- */
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = lvgl_trans_done_cb,
    };
    lcd_st7796_register_event_callbacks(&cbs, NULL);

    /* -------- LVGL tick timer (2 ms period) -------- */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
                                              QA_LVGL_TICK_MS * 1000));

    /* -------- Build the UI -------- */
    lvgl_create_ui();

    /* -------- Main loop: drain queue + service LVGL -------- */
    qa_msg_t msg;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        /* Drain all messages currently in the queue */
        while (xQueueReceive(s_qa_queue, &msg, 0) == pdTRUE) {
            qa_ui_process_msg(&msg);
        }

        /* Let LVGL handle pending work (redraw, timer callbacks etc.) */
        lv_timer_handler();

        /* Wait for the next cycle (~33 ms) */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(QA_LVGL_HANDLER_MS));
    }
}
