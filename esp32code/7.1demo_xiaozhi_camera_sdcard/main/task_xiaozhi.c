#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "app_wifi_store.h"
#include "task_lcd_lvgl.h"
#include "task_sdcard.h"
#include "task_ws2812.h"
#include "task_xiaozhi.h"
#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "cJSON.h"
#include "es8388.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "opus.h"
#include "xl9555.h"
#include "xiaozhi_activation.h"
#include "xiaozhi_ui_status.h"

#define XIAOZHI_WIFI_CONNECTED_BIT   BIT0
#define XIAOZHI_WS_READY_BIT         BIT1
#define XIAOZHI_TIMEZONE             "CST-8"
#define XIAOZHI_NTP_SERVER           "ntp.aliyun.com"

#define XIAOZHI_WS_PROTOCOL_DEFAULT  1
#define XIAOZHI_WS_TX_TIMEOUT_MS     1000
#define XIAOZHI_WS_BUFFER_SIZE       1024
#define XIAOZHI_WS_HEADER_SIZE       1024
#define XIAOZHI_WS_TASK_STACK        6144

#define XIAOZHI_TASK_STACK           16384
#define XIAOZHI_AUDIO_STACK          40960
#define XIAOZHI_BUTTON_TASK_STACK    4096
#define XIAOZHI_TASK_PRIO            10
#define XIAOZHI_AUDIO_TASK_PRIO      8
#define XIAOZHI_BUTTON_TASK_PRIO     9

#define XIAOZHI_KEY_SCAN_MS          20
#define XIAOZHI_KEY_DEBOUNCE_MS      40
#define XIAOZHI_KEY3_LONG_MS         1500
#define XIAOZHI_RECORD_TAIL_MS       240
#define XIAOZHI_OPUS_MAX_PACKET_SIZE 1276
#define XIAOZHI_PLAYBACK_QUEUE_LEN   8
#define XIAOZHI_ENCODE_SAMPLE_RATE   16000
#define XIAOZHI_SERVER_FRAME_MS      60
#define XIAOZHI_OPUS_BITRATE         16000
#define XIAOZHI_OPUS_COMPLEXITY      3
#define XIAOZHI_PLAYBACK_MAX_FRAME_MS 60

#define XIAOZHI_PCM_INPUT_SAMPLES    (BSP_AUDIO_FRAME_SAMPLES * BSP_AUDIO_CHANNELS)
#define XIAOZHI_PCM_ENCODE_SAMPLES   ((XIAOZHI_ENCODE_SAMPLE_RATE * BSP_AUDIO_FRAME_DURATION_MS) / 1000)
#define XIAOZHI_PCM_MONO_MAX_SAMPLES ((48000 * XIAOZHI_PLAYBACK_MAX_FRAME_MS) / 1000)
#define XIAOZHI_PCM_STEREO_OUT_MAX   (((BSP_AUDIO_SAMPLE_RATE * XIAOZHI_PLAYBACK_MAX_FRAME_MS) / 1000) * BSP_AUDIO_CHANNELS)

typedef struct {
    size_t len;
    uint8_t data[XIAOZHI_OPUS_MAX_PACKET_SIZE];
} xiaozhi_opus_packet_t;

typedef struct {
    uint8_t *data;
    size_t size;
    uint8_t opcode;
} xiaozhi_ws_frame_t;

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t type;
    uint32_t reserved;
    uint32_t timestamp;
    uint32_t payload_size;
    uint8_t payload[];
} xiaozhi_binary_protocol_v2_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} xiaozhi_binary_protocol_v3_t;

typedef struct {
    EventGroupHandle_t event_group;
    QueueHandle_t playback_queue;
    SemaphoreHandle_t ws_mutex;
    SemaphoreHandle_t codec_mutex;
    esp_websocket_client_handle_t ws_client;
    OpusEncoder *encoder;
    xiaozhi_ws_frame_t rx_frame;
    char session_id[64];
    char device_id[XIAOZHI_DEVICE_ID_LEN];
    char client_id[XIAOZHI_CLIENT_ID_LEN];
    char wifi_ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char wifi_ip[XIAOZHI_UI_IP_LEN];
    char ws_url[XIAOZHI_WS_URL_LEN];
    char ws_token[XIAOZHI_WS_TOKEN_LEN];
    char ws_headers[XIAOZHI_WS_HEADER_SIZE];
    uint8_t ws_bin_buf[sizeof(xiaozhi_binary_protocol_v2_t) + XIAOZHI_OPUS_MAX_PACKET_SIZE];
    int ws_version;
    volatile bool recording;
    volatile bool tts_active;
    volatile bool playing;
    volatile uint32_t server_sample_rate;
    volatile uint32_t server_frame_duration;
    uint32_t rx_audio_packets;
    uint32_t played_audio_packets;
    OpusDecoder *decoder;
    uint32_t decoder_rate;
    volatile bool local_command_active;
    volatile bool ws_paused_for_camera;
    volatile bool ws_client_started;
    volatile bool stop_pending;
    int64_t stop_request_us;
    int64_t record_start_us;
    uint32_t record_packet_count;
    uint32_t record_send_fail_count;
    uint16_t record_peak_left;
    uint16_t record_peak_right;
    uint16_t record_peak_mono;
} xiaozhi_ctx_t;

static const char *TAG = "TASK_XIAOZHI";
static TaskHandle_t g_xiaozhi_button_task_handle = NULL;

static bool xiaozhi_key1_is_pressed(void)
{
    bool key_level = true;

    xl9555_get_pin_level(KEY_PORT, KEY1_PIN, &key_level);
    return (key_level == 0);
}

static bool xiaozhi_key3_is_pressed(void)
{
    bool key_level = true;

    xl9555_get_pin_level(KEY_PORT, KEY3_PIN, &key_level);
    return (key_level == 0);
}
static xiaozhi_ctx_t xiaozhi_ctx = {
    .server_sample_rate = BSP_AUDIO_SAMPLE_RATE,
    .server_frame_duration = XIAOZHI_SERVER_FRAME_MS,
    .ws_version = XIAOZHI_WS_PROTOCOL_DEFAULT,
};

#ifdef CONFIG_XIAOZHI_ENABLE_LCD_STATUS
static bool g_xiaozhi_lcd_started = false;
#endif
static bool g_xiaozhi_audio_workers_started = false;
static TaskHandle_t g_xiaozhi_audio_handle = NULL;

static void xiaozhi_audio_task(void *pvParameters);
static void xiaozhi_log_heap(const char *stage);
static uint32_t xiaozhi_normalize_opus_rate(uint32_t sample_rate);
static bool xiaozhi_prepare_encoder_state(void);
static void xiaozhi_release_encoder_state(void);
static void xiaozhi_release_decoder_state(void);
static void xiaozhi_codec_lock(void);
static void xiaozhi_codec_unlock(void);
static void xiaozhi_send_listen(const char *state);
static void xiaozhi_send_abort(const char *reason);
static void xiaozhi_ui_refresh_connect_state(void);
static void xiaozhi_ui_refresh_network_info(void);
static bool xiaozhi_wifi_is_connected(void);
static void xiaozhi_time_sync_init(void);
static void xiaozhi_time_sync_start(void);
static void xiaozhi_pause_ws_for_camera(void);
static void xiaozhi_resume_ws_after_camera(void);
static void xiaozhi_request_wifi_portal_reboot(void);
static void xiaozhi_finish_manual_recording(void);

static bool g_xiaozhi_sntp_initialized = false;
static bool g_xiaozhi_sntp_started = false;

static uint16_t xiaozhi_abs16(int16_t sample)
{
    return (uint16_t)((sample >= 0) ? sample : -(int32_t)sample);
}

static int16_t xiaozhi_mix_stereo_samples(int16_t left, int16_t right)
{
    uint32_t abs_left = xiaozhi_abs16(left);
    uint32_t abs_right = xiaozhi_abs16(right);

    if (abs_left >= (abs_right * 2U)) {
        return left;
    }

    if (abs_right >= (abs_left * 2U)) {
        return right;
    }

    return (int16_t)(((int32_t)left + (int32_t)right) / 2);
}

static void xiaozhi_safe_copy(char *dst, size_t dst_size, const char *src)
{
    if ((dst == NULL) || (dst_size == 0)) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void xiaozhi_codec_lock(void)
{
    if (xiaozhi_ctx.codec_mutex != NULL) {
        xSemaphoreTake(xiaozhi_ctx.codec_mutex, portMAX_DELAY);
    }
}

static void xiaozhi_codec_unlock(void)
{
    if (xiaozhi_ctx.codec_mutex != NULL) {
        xSemaphoreGive(xiaozhi_ctx.codec_mutex);
    }
}

static bool xiaozhi_prepare_encoder_state(void)
{
    int opus_err = OPUS_OK;
    bool ok = false;

    xiaozhi_codec_lock();
    if (xiaozhi_ctx.encoder != NULL) {
        xiaozhi_codec_unlock();
        return true;
    }

    xiaozhi_ctx.encoder = opus_encoder_create(XIAOZHI_ENCODE_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_err);
    if ((xiaozhi_ctx.encoder == NULL) || (opus_err != OPUS_OK)) {
        xiaozhi_ctx.encoder = NULL;
        xiaozhi_codec_unlock();
        ESP_LOGE(TAG, "opus encoder create failed, err=%d", opus_err);
        return false;
    }

    if ((opus_encoder_ctl(xiaozhi_ctx.encoder, OPUS_SET_BITRATE(XIAOZHI_OPUS_BITRATE)) != OPUS_OK) ||
        (opus_encoder_ctl(xiaozhi_ctx.encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_OK) ||
        (opus_encoder_ctl(xiaozhi_ctx.encoder, OPUS_SET_COMPLEXITY(XIAOZHI_OPUS_COMPLEXITY)) != OPUS_OK) ||
        (opus_encoder_ctl(xiaozhi_ctx.encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_60_MS)) != OPUS_OK)) {
        ESP_LOGE(TAG, "opus encoder ctl failed");
        opus_encoder_destroy(xiaozhi_ctx.encoder);
        xiaozhi_ctx.encoder = NULL;
        xiaozhi_codec_unlock();
        return false;
    }

    ok = true;
    xiaozhi_codec_unlock();
    xiaozhi_log_heap("encoder_ready");
    return ok;
}

static void xiaozhi_release_encoder_state(void)
{
    bool released = false;

    xiaozhi_codec_lock();
    if (xiaozhi_ctx.encoder != NULL) {
        opus_encoder_destroy(xiaozhi_ctx.encoder);
        xiaozhi_ctx.encoder = NULL;
        released = true;
    }
    xiaozhi_codec_unlock();

    if (released) {
        xiaozhi_log_heap("encoder_released");
    }
}

static void xiaozhi_release_decoder_state(void)
{
    bool released = false;

    xiaozhi_codec_lock();
    if (xiaozhi_ctx.decoder != NULL) {
        opus_decoder_destroy(xiaozhi_ctx.decoder);
        xiaozhi_ctx.decoder = NULL;
        xiaozhi_ctx.decoder_rate = 0;
        released = true;
    }
    xiaozhi_codec_unlock();

    if (released) {
        xiaozhi_log_heap("decoder_released");
    }
}

static void xiaozhi_try_start_lcd(void)
{
#ifdef CONFIG_XIAOZHI_ENABLE_LCD_STATUS
    if (!g_xiaozhi_lcd_started) {
        if (lcd_lvgl_task_create() == pdPASS) {
            g_xiaozhi_lcd_started = true;
        } else {
            ESP_LOGW(TAG, "lcd task create failed");
        }
    }
#endif
}

static void xiaozhi_prepare_decoder_state(void)
{
    bool ready = false;

    xiaozhi_codec_lock();
    if (xiaozhi_ctx.decoder == NULL) {
        int opus_err = OPUS_OK;
        uint32_t sample_rate = xiaozhi_normalize_opus_rate(xiaozhi_ctx.server_sample_rate);
        uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

        xiaozhi_ctx.decoder = opus_decoder_create(sample_rate, 1, &opus_err);
        if ((xiaozhi_ctx.decoder == NULL) || (opus_err != OPUS_OK)) {
            xiaozhi_ctx.decoder = NULL;
            xiaozhi_ctx.decoder_rate = 0;
            xiaozhi_codec_unlock();
            xiaozhi_log_heap("decoder_prepare_failed");
            ESP_LOGW(TAG,
                     "decoder create failed, rate=%" PRIu32 ", err=%d, largest=%u",
                     sample_rate,
                     opus_err,
                     (unsigned)heap_caps_get_largest_free_block(caps));
            return;
        }
        xiaozhi_ctx.decoder_rate = sample_rate;
        ready = true;
    }
    xiaozhi_codec_unlock();

    if (ready) {
        ESP_LOGI(TAG, "decoder ready, rate=%" PRIu32, xiaozhi_normalize_opus_rate(xiaozhi_ctx.server_sample_rate));
    }
}

static void xiaozhi_start_audio_workers(void)
{
    uint32_t internal_largest = 0;
    uint32_t spiram_largest = 0;

    if (g_xiaozhi_audio_workers_started) {
        return;
    }

    xiaozhi_log_heap("before_audio_workers");
    if (xTaskCreateWithCaps(xiaozhi_audio_task,
                            "xiaozhi_audio",
                            XIAOZHI_AUDIO_STACK,
                            NULL,
                            XIAOZHI_AUDIO_TASK_PRIO,
                            &g_xiaozhi_audio_handle,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        internal_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        spiram_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGE(TAG,
                 "create audio task failed, stack=%u, internal_largest=%u, spiram_largest=%u",
                 (unsigned)XIAOZHI_AUDIO_STACK,
                 (unsigned)internal_largest,
                 (unsigned)spiram_largest);
        return;
    }

    g_xiaozhi_audio_workers_started = true;
    xiaozhi_log_heap("audio_workers_started");
}

static int xiaozhi_get_ws_version(void)
{
    if ((xiaozhi_ctx.ws_version < 1) || (xiaozhi_ctx.ws_version > 3)) {
        return XIAOZHI_WS_PROTOCOL_DEFAULT;
    }

    return xiaozhi_ctx.ws_version;
}

static void xiaozhi_audio_stop_playback(void)
{
    xiaozhi_ctx.tts_active = false;
    xiaozhi_ctx.playing = false;
    if (xiaozhi_ctx.playback_queue != NULL) {
        xQueueReset(xiaozhi_ctx.playback_queue);
    }
    xiaozhi_release_decoder_state();
    es8388_set_mute(true);
    es8388_speaker_enable(false);
}

static void xiaozhi_finish_manual_recording(void)
{
    int64_t duration_ms = 0;

    if (!xiaozhi_ctx.recording) {
        xiaozhi_ctx.stop_pending = false;
        xiaozhi_ctx.stop_request_us = 0;
        return;
    }

    xiaozhi_ctx.recording = false;
    xiaozhi_ctx.stop_pending = false;
    xiaozhi_ctx.stop_request_us = 0;

    if (xiaozhi_ctx.record_start_us > 0) {
        duration_ms = (esp_timer_get_time() - xiaozhi_ctx.record_start_us) / 1000;
    }

    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_THINKING);
    xiaozhi_ui_status_set_note("问题已发送，等待小智回答");
    xiaozhi_ui_status_set_stt_text("等待语音识别结果...");
    xiaozhi_ui_status_set_llm_text("问题已上传，正在等待推理结果");
    xiaozhi_ui_status_set_tts_text("合成完成后会开始播报");
    xiaozhi_send_listen("stop");
    ESP_LOGI(TAG,
             "listen stop, duration=%" PRIi64 "ms, packets=%" PRIu32 ", send_fail=%" PRIu32
             ", peak_l=%u, peak_r=%u, peak_mono=%u",
             duration_ms,
             xiaozhi_ctx.record_packet_count,
             xiaozhi_ctx.record_send_fail_count,
             (unsigned)xiaozhi_ctx.record_peak_left,
             (unsigned)xiaozhi_ctx.record_peak_right,
             (unsigned)xiaozhi_ctx.record_peak_mono);
    if (xiaozhi_ctx.record_packet_count < 3) {
        ESP_LOGW(TAG, "recording too short, keep KEY1 pressed until speaking is finished");
    }
}

static bool xiaozhi_ws_is_ready(void)
{
    EventBits_t bits;

    if (xiaozhi_ctx.event_group == NULL) {
        return false;
    }

    bits = xEventGroupGetBits(xiaozhi_ctx.event_group);
    return ((bits & XIAOZHI_WS_READY_BIT) != 0);
}

void xiaozhi_enter_camera_view(void)
{
    xiaozhi_ctx.local_command_active = false;

    if (xiaozhi_ctx.recording) {
        xiaozhi_ctx.recording = false;
        xiaozhi_send_listen("stop");
        ESP_LOGI(TAG, "listen stop due to camera view");
    }

    if (xiaozhi_ctx.playing || xiaozhi_ctx.tts_active) {
        xiaozhi_send_abort("camera_view");
        xiaozhi_audio_stop_playback();
        ESP_LOGI(TAG, "playback aborted due to camera view");
    }

    xiaozhi_pause_ws_for_camera();
    xiaozhi_ui_status_set_note("摄像头预览中，按 KEY2 返回小智");
}

void xiaozhi_leave_camera_view(void)
{
    xiaozhi_resume_ws_after_camera();

    if (xiaozhi_ws_is_ready()) {
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_READY);
        xiaozhi_ui_status_set_note("已切回小智，按住 KEY1 开始说话");
    } else if (xiaozhi_wifi_is_connected()) {
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_CONNECTING);
        xiaozhi_ui_status_set_note("已切回小智，正在恢复语音链路");
    } else {
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_DISCONNECTED);
        xiaozhi_ui_status_set_note("已切回小智，语音服务未就绪");
    }

    xiaozhi_ui_status_set_stt_text("按住 KEY1 开始说话");
    xiaozhi_ui_status_set_tts_text("小智的语音回复会显示在这里");
}

static bool xiaozhi_wifi_is_connected(void)
{
    EventBits_t bits;

    if (xiaozhi_ctx.event_group == NULL) {
        return false;
    }

    bits = xEventGroupGetBits(xiaozhi_ctx.event_group);
    return ((bits & XIAOZHI_WIFI_CONNECTED_BIT) != 0);
}

static void xiaozhi_ui_refresh_connect_state(void)
{
    xiaozhi_ui_status_set_connect_state(xiaozhi_wifi_is_connected(), xiaozhi_ws_is_ready());
}

static void xiaozhi_ui_refresh_network_info(void)
{
    xiaozhi_ui_status_set_network_info(xiaozhi_ctx.wifi_ssid, xiaozhi_ctx.wifi_ip);
}

static void xiaozhi_time_sync_notification_cb(struct timeval *tv)
{
    time_t now = 0;
    struct tm local_time = {0};
    char time_buf[40];

    (void)tv;

    setenv("TZ", XIAOZHI_TIMEZONE, 1);
    tzset();
    time(&now);
    if ((localtime_r(&now, &local_time) != NULL) &&
        (strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC+8", &local_time) > 0)) {
        ESP_LOGI(TAG, "network time synchronized: %s", time_buf);
    } else {
        ESP_LOGI(TAG, "network time synchronized");
    }
}

static void xiaozhi_time_sync_init(void)
{
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(XIAOZHI_NTP_SERVER);

    if (g_xiaozhi_sntp_initialized) {
        return;
    }

    sntp_config.start = false;
    sntp_config.wait_for_sync = false;
    sntp_config.sync_cb = xiaozhi_time_sync_notification_cb;
    sntp_config.renew_servers_after_new_IP = true;
    sntp_config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;

    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));
    g_xiaozhi_sntp_initialized = true;
}

static void xiaozhi_time_sync_start(void)
{
    esp_err_t ret;

    if (!g_xiaozhi_sntp_initialized) {
        return;
    }

    ret = esp_netif_sntp_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start network time sync failed: %s", esp_err_to_name(ret));
        return;
    }

    if (!g_xiaozhi_sntp_started) {
        ESP_LOGI(TAG, "start network time sync via %s", XIAOZHI_NTP_SERVER);
        g_xiaozhi_sntp_started = true;
    }
}

static void xiaozhi_pause_ws_for_camera(void)
{
    esp_err_t ret;

    if ((xiaozhi_ctx.ws_client == NULL) || !xiaozhi_ctx.ws_client_started) {
        xiaozhi_ctx.ws_paused_for_camera = true;
        xEventGroupClearBits(xiaozhi_ctx.event_group, XIAOZHI_WS_READY_BIT);
        xiaozhi_ui_refresh_connect_state();
        return;
    }

    xiaozhi_ctx.ws_paused_for_camera = true;
    xiaozhi_ctx.session_id[0] = '\0';
    xEventGroupClearBits(xiaozhi_ctx.event_group, XIAOZHI_WS_READY_BIT);
    xiaozhi_ui_refresh_connect_state();
    xiaozhi_log_heap("before_ws_stop_camera");

    xSemaphoreTake(xiaozhi_ctx.ws_mutex, portMAX_DELAY);
    ret = esp_websocket_client_stop(xiaozhi_ctx.ws_client);
    xSemaphoreGive(xiaozhi_ctx.ws_mutex);

    if (ret == ESP_OK) {
        xiaozhi_ctx.ws_client_started = false;
        xiaozhi_log_heap("after_ws_stop_camera");
        ESP_LOGI(TAG, "websocket paused for camera view");
    } else {
        ESP_LOGW(TAG, "pause websocket for camera failed: %s", esp_err_to_name(ret));
    }
}

static void xiaozhi_resume_ws_after_camera(void)
{
    esp_err_t ret;

    xiaozhi_ctx.ws_paused_for_camera = false;

    if ((xiaozhi_ctx.ws_client == NULL) || xiaozhi_ctx.ws_client_started || !xiaozhi_wifi_is_connected()) {
        xiaozhi_ui_refresh_connect_state();
        return;
    }

    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_CONNECTING);
    xiaozhi_ui_status_set_note("正在恢复小智语音链路");
    xiaozhi_log_heap("before_ws_resume_camera");

    xSemaphoreTake(xiaozhi_ctx.ws_mutex, portMAX_DELAY);
    ret = esp_websocket_client_start(xiaozhi_ctx.ws_client);
    xSemaphoreGive(xiaozhi_ctx.ws_mutex);

    if (ret == ESP_OK) {
        xiaozhi_ctx.ws_client_started = true;
        xiaozhi_log_heap("after_ws_resume_camera");
        ESP_LOGI(TAG, "websocket resumed after camera view");
    } else {
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_DISCONNECTED);
        xiaozhi_ui_status_set_note("语音链路恢复失败，稍后重试");
        ESP_LOGW(TAG, "resume websocket after camera failed: %s", esp_err_to_name(ret));
    }

    xiaozhi_ui_refresh_connect_state();
}

static void xiaozhi_request_wifi_portal_reboot(void)
{
    esp_err_t ret;

    if (xiaozhi_ctx.recording) {
        xiaozhi_ctx.recording = false;
        xiaozhi_send_listen("stop");
    }

    if (xiaozhi_ctx.playing || xiaozhi_ctx.tts_active) {
        xiaozhi_send_abort("wifi_portal");
        xiaozhi_audio_stop_playback();
    }

    xiaozhi_ui_status_set_view(XIAOZHI_UI_VIEW_SETUP);
    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_CONNECTING);
    xiaozhi_ui_status_set_note("KEY3 长按生效，正在重启进入配网");
    xiaozhi_ui_status_set_stt_text("重启后请连接设备热点并打开 192.168.4.1");
    xiaozhi_ui_status_set_tts_text("设备即将回到 Wi-Fi 配网页面");

    ret = app_wifi_store_request_portal_once();
    if (ret != ESP_OK) {
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_ERROR);
        xiaozhi_ui_status_set_note("进入配网失败，请稍后重试");
        ESP_LOGE(TAG, "request wifi portal reboot failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGW(TAG, "KEY3 long press detected, reboot into wifi portal");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void xiaozhi_log_heap(const char *stage)
{
    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    ESP_LOGD(TAG,
             "heap[%s]: free=%u, largest=%u, min=%u",
             (stage != NULL) ? stage : "n/a",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps));
}

static int xiaozhi_ws_send_text(const char *text)
{
    int ret = -1;

    if ((text == NULL) || (xiaozhi_ctx.ws_client == NULL)) {
        return -1;
    }
    if (!esp_websocket_client_is_connected(xiaozhi_ctx.ws_client)) {
        return -1;
    }

    xSemaphoreTake(xiaozhi_ctx.ws_mutex, portMAX_DELAY);
    ret = esp_websocket_client_send_text(xiaozhi_ctx.ws_client, text, strlen(text), pdMS_TO_TICKS(XIAOZHI_WS_TX_TIMEOUT_MS));
    xSemaphoreGive(xiaozhi_ctx.ws_mutex);
    return ret;
}

static int xiaozhi_ws_send_bin(const uint8_t *data, size_t len)
{
    const uint8_t *tx_data = data;
    size_t tx_len = len;
    int ws_version = xiaozhi_get_ws_version();
    int ret = -1;

    if ((data == NULL) || (len == 0) || (xiaozhi_ctx.ws_client == NULL)) {
        return -1;
    }
    if (!xiaozhi_ws_is_ready()) {
        return -1;
    }

    if (ws_version == 2) {
        xiaozhi_binary_protocol_v2_t *packet = (xiaozhi_binary_protocol_v2_t *)xiaozhi_ctx.ws_bin_buf;

        packet->version = htons((uint16_t)ws_version);
        packet->type = htons(0);
        packet->reserved = 0;
        packet->timestamp = htonl((uint32_t)(esp_timer_get_time() / 1000ULL));
        packet->payload_size = htonl((uint32_t)len);
        memcpy(packet->payload, data, len);
        tx_data = xiaozhi_ctx.ws_bin_buf;
        tx_len = sizeof(xiaozhi_binary_protocol_v2_t) + len;
    } else if (ws_version == 3) {
        xiaozhi_binary_protocol_v3_t *packet = (xiaozhi_binary_protocol_v3_t *)xiaozhi_ctx.ws_bin_buf;

        if (len > UINT16_MAX) {
            return -1;
        }

        packet->type = 0;
        packet->reserved = 0;
        packet->payload_size = htons((uint16_t)len);
        memcpy(packet->payload, data, len);
        tx_data = xiaozhi_ctx.ws_bin_buf;
        tx_len = sizeof(xiaozhi_binary_protocol_v3_t) + len;
    }

    xSemaphoreTake(xiaozhi_ctx.ws_mutex, portMAX_DELAY);
    ret = esp_websocket_client_send_bin(xiaozhi_ctx.ws_client, (const char *)tx_data, tx_len, pdMS_TO_TICKS(XIAOZHI_WS_TX_TIMEOUT_MS));
    xSemaphoreGive(xiaozhi_ctx.ws_mutex);
    return ret;
}

static void xiaozhi_send_hello(void)
{
    char json_buf[256];

    snprintf(json_buf, sizeof(json_buf),
             "{\"type\":\"hello\",\"version\":%d,\"features\":{\"mcp\":true},\"transport\":\"websocket\","
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":1,\"frame_duration\":%d}}",
             xiaozhi_get_ws_version(),
             XIAOZHI_ENCODE_SAMPLE_RATE,
             BSP_AUDIO_FRAME_DURATION_MS);

    if (xiaozhi_ws_send_text(json_buf) < 0) {
        ESP_LOGE(TAG, "send hello failed");
    }
}

static void xiaozhi_send_listen(const char *state)
{
    char json_buf[192];

    if ((state == NULL) || (!xiaozhi_ws_is_ready())) {
        return;
    }

    if (xiaozhi_ctx.session_id[0] != '\0') {
        snprintf(json_buf, sizeof(json_buf),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"}",
                 xiaozhi_ctx.session_id,
                 state);
    } else {
        snprintf(json_buf, sizeof(json_buf),
                 "{\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"manual\"}",
                 state);
    }

    if (xiaozhi_ws_send_text(json_buf) < 0) {
        ESP_LOGE(TAG, "send listen %s failed", state);
    }
}

static void xiaozhi_send_abort(const char *reason)
{
    char json_buf[192];

    if (!xiaozhi_ws_is_ready()) {
        return;
    }

    if (xiaozhi_ctx.session_id[0] != '\0') {
        snprintf(json_buf, sizeof(json_buf),
                 "{\"session_id\":\"%s\",\"type\":\"abort\",\"reason\":\"%s\"}",
                 xiaozhi_ctx.session_id,
                 (reason != NULL) ? reason : "manual_interrupt");
    } else {
        snprintf(json_buf, sizeof(json_buf),
                 "{\"type\":\"abort\",\"reason\":\"%s\"}",
                 (reason != NULL) ? reason : "manual_interrupt");
    }

    if (xiaozhi_ws_send_text(json_buf) < 0) {
        ESP_LOGE(TAG, "send abort failed");
    }
}

static uint32_t xiaozhi_normalize_opus_rate(uint32_t sample_rate)
{
    switch (sample_rate) {
        case 8000:
        case 12000:
        case 16000:
        case 24000:
        case 48000:
            return sample_rate;

        default:
            return BSP_AUDIO_SAMPLE_RATE;
    }
}

static size_t xiaozhi_resample_mono_to_stereo(const int16_t *src, size_t src_samples, uint32_t src_rate,
                                              int16_t *dst, size_t dst_capacity_samples)
{
    size_t out_samples;
    size_t i;

    if ((src == NULL) || (dst == NULL) || (src_samples == 0) || (src_rate == 0)) {
        return 0;
    }

    out_samples = ((uint64_t)src_samples * BSP_AUDIO_SAMPLE_RATE) / src_rate;
    if (out_samples == 0) {
        out_samples = 1;
    }
    if ((out_samples * BSP_AUDIO_CHANNELS) > dst_capacity_samples) {
        out_samples = dst_capacity_samples / BSP_AUDIO_CHANNELS;
    }

    for (i = 0; i < out_samples; i++) {
        int16_t sample;
        uint64_t pos_fixed = ((uint64_t)i * src_rate << 16) / BSP_AUDIO_SAMPLE_RATE;
        size_t index = (size_t)(pos_fixed >> 16);
        uint32_t frac = (uint32_t)(pos_fixed & 0xFFFF);

        if ((index + 1) >= src_samples) {
            sample = src[src_samples - 1];
        } else {
            int32_t sample_a = src[index];
            int32_t sample_b = src[index + 1];
            sample = (int16_t)(((sample_a * (int32_t)(0x10000 - frac)) + (sample_b * (int32_t)frac)) >> 16);
        }

        dst[i * 2] = sample;
        dst[i * 2 + 1] = sample;
    }

    return out_samples * BSP_AUDIO_CHANNELS;
}

static size_t xiaozhi_resample_stereo_to_mono(const int16_t *src, size_t src_frames, uint32_t src_rate,
                                             uint32_t dst_rate, int16_t *dst, size_t dst_capacity_samples)
{
    size_t out_samples;
    size_t i;

    if ((src == NULL) || (dst == NULL) || (src_frames == 0) || (src_rate == 0) || (dst_rate == 0)) {
        return 0;
    }

    out_samples = ((uint64_t)src_frames * dst_rate) / src_rate;
    if (out_samples == 0) {
        out_samples = 1;
    }
    if (out_samples > dst_capacity_samples) {
        out_samples = dst_capacity_samples;
    }

    for (i = 0; i < out_samples; i++) {
        int16_t left_sample;
        int16_t right_sample;
        uint64_t pos_fixed = ((uint64_t)i * src_rate << 16) / dst_rate;
        size_t index = (size_t)(pos_fixed >> 16);
        uint32_t frac = (uint32_t)(pos_fixed & 0xFFFF);

        if ((index + 1) >= src_frames) {
            size_t last_offset = (src_frames - 1) * BSP_AUDIO_CHANNELS;
            left_sample = src[last_offset];
            right_sample = src[last_offset + 1];
        } else {
            size_t offset_a = index * BSP_AUDIO_CHANNELS;
            size_t offset_b = (index + 1) * BSP_AUDIO_CHANNELS;
            int32_t left_a = src[offset_a];
            int32_t left_b = src[offset_b];
            int32_t right_a = src[offset_a + 1];
            int32_t right_b = src[offset_b + 1];

            left_sample = (int16_t)(((left_a * (int32_t)(0x10000 - frac)) + (left_b * (int32_t)frac)) >> 16);
            right_sample = (int16_t)(((right_a * (int32_t)(0x10000 - frac)) + (right_b * (int32_t)frac)) >> 16);
        }

        dst[i] = xiaozhi_mix_stereo_samples(left_sample, right_sample);
    }

    return out_samples;
}

static void xiaozhi_handle_json_message(const char *data, size_t len)
{
    cJSON *root;
    cJSON *type;

    root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "json parse failed");
        return;
    }

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || (type->valuestring == NULL)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
        cJSON *session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        cJSON *audio_params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        cJSON *sample_rate = NULL;
        cJSON *frame_duration = NULL;
        bool lcd_buffer_ready = false;

        if (!cJSON_IsString(transport) || (transport->valuestring == NULL) || (strcmp(transport->valuestring, "websocket") != 0)) {
            cJSON_Delete(root);
            return;
        }

        if (cJSON_IsString(session_id) && (session_id->valuestring != NULL)) {
            snprintf(xiaozhi_ctx.session_id, sizeof(xiaozhi_ctx.session_id), "%s", session_id->valuestring);
        }
        xiaozhi_ctx.local_command_active = false;

        if (cJSON_IsObject(audio_params)) {
            sample_rate = cJSON_GetObjectItemCaseSensitive(audio_params, "sample_rate");
            frame_duration = cJSON_GetObjectItemCaseSensitive(audio_params, "frame_duration");
            if (cJSON_IsNumber(sample_rate)) {
                xiaozhi_ctx.server_sample_rate = xiaozhi_normalize_opus_rate((uint32_t)sample_rate->valuedouble);
            }
            if (cJSON_IsNumber(frame_duration)) {
                xiaozhi_ctx.server_frame_duration = (uint32_t)frame_duration->valuedouble;
            }
        }

        xEventGroupSetBits(xiaozhi_ctx.event_group, XIAOZHI_WS_READY_BIT);
        xiaozhi_ui_status_set_audio_info(xiaozhi_ctx.server_sample_rate,
                                         xiaozhi_ctx.server_frame_duration,
                                         xiaozhi_get_ws_version());
        xiaozhi_ui_refresh_connect_state();
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_READY);
        xiaozhi_ui_status_set_note("按住按键说话，松开后发送");
        xiaozhi_ui_status_set_view(XIAOZHI_UI_VIEW_VOICE);
        xiaozhi_log_heap("hello_before_reserve");
        xiaozhi_start_audio_workers();
        if (!xiaozhi_prepare_encoder_state()) {
            ESP_LOGW(TAG, "encoder init deferred until listen start");
        }
        xiaozhi_log_heap("hello_after_encoder");
        if (lcd_lvgl_reserve_buffer() != ESP_OK) {
            ESP_LOGW(TAG, "lcd buffer reserve failed");
        } else {
            lcd_buffer_ready = true;
        }
        xiaozhi_log_heap("hello_after_lcd");
        if (lcd_buffer_ready) {
            xiaozhi_try_start_lcd();
        }
        ESP_LOGI(TAG, "hello ok, session=%s, playback_rate=%" PRIu32 ", frame_ms=%" PRIu32,
                 xiaozhi_ctx.session_id,
                 xiaozhi_ctx.server_sample_rate,
                 xiaozhi_ctx.server_frame_duration);
    } else if (strcmp(type->valuestring, "stt") == 0) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && (text->valuestring != NULL)) {
            char light_feedback[48] = {0};

            xiaozhi_ui_status_set_stt_text(text->valuestring);
            if (ws2812_handle_voice_command(text->valuestring, light_feedback, sizeof(light_feedback))) {
                xiaozhi_ctx.local_command_active = true;
                xiaozhi_send_abort("local_device_command");
                xiaozhi_audio_stop_playback();
                xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_READY);
                xiaozhi_ui_status_set_note(light_feedback);
                xiaozhi_ui_status_set_tts_text("本地灯光指令已执行");
                ESP_LOGI(TAG, "light command: %s", light_feedback);
            } else {
                xiaozhi_ctx.local_command_active = false;
                xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_THINKING);
                xiaozhi_ui_status_set_note("识别完成，正在生成回答");
                (void)sdcard_task_post_dialog(SDCARD_DIALOG_ROLE_USER, text->valuestring);
            }
            ESP_LOGI(TAG, "stt: %s", text->valuestring);
        }
    } else if (strcmp(type->valuestring, "tts") == 0) {
        cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");

        if (xiaozhi_ctx.local_command_active) {
            ESP_LOGI(TAG, "ignore tts due to local device command");
            cJSON_Delete(root);
            return;
        }

        if (cJSON_IsString(state) && (state->valuestring != NULL)) {
            if (strcmp(state->valuestring, "start") == 0) {
                xiaozhi_release_encoder_state();
                xiaozhi_prepare_decoder_state();
                xiaozhi_ctx.tts_active = true;
                xiaozhi_ctx.playing = true;
                xiaozhi_ctx.rx_audio_packets = 0;
                xiaozhi_ctx.played_audio_packets = 0;
                xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_SPEAKING);
                xiaozhi_ui_status_set_note("正在播放小智回复");
                ESP_LOGI(TAG, "tts start");
            } else if (strcmp(state->valuestring, "stop") == 0) {
                xiaozhi_ctx.tts_active = false;
                xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_READY);
                xiaozhi_ui_status_set_note("可以开始下一轮对话");
                ESP_LOGI(TAG, "tts stop");
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                if (cJSON_IsString(text) && (text->valuestring != NULL)) {
                    xiaozhi_ui_status_set_tts_text(text->valuestring);
                    (void)sdcard_task_post_dialog(SDCARD_DIALOG_ROLE_ASSISTANT, text->valuestring);
                    ESP_LOGI(TAG, "tts text: %s", text->valuestring);
                }
            }
        }
    } else if (strcmp(type->valuestring, "llm") == 0) {
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (xiaozhi_ctx.local_command_active) {
            ESP_LOGI(TAG, "ignore llm due to local device command");
            cJSON_Delete(root);
            return;
        }
        if (cJSON_IsString(text) && (text->valuestring != NULL)) {
            xiaozhi_ui_status_set_llm_text(text->valuestring);
            xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_THINKING);
            xiaozhi_ui_status_set_note("大模型正在组织语音回复");
            ESP_LOGI(TAG, "llm: %s", text->valuestring);
        }
    }

    cJSON_Delete(root);
}

static void xiaozhi_handle_binary_message(const uint8_t *data, size_t len)
{
    xiaozhi_opus_packet_t packet;
    xiaozhi_opus_packet_t drop_packet;
    const uint8_t *payload = data;
    size_t payload_len = len;
    int ws_version = xiaozhi_get_ws_version();
    BaseType_t send_ok;

    if ((data == NULL) || (len == 0)) {
        return;
    }
    if (xiaozhi_ctx.recording) {
        return;
    }
    if (xiaozhi_ctx.local_command_active) {
        return;
    }

    if (ws_version == 2) {
        const xiaozhi_binary_protocol_v2_t *frame;
        uint32_t frame_len;

        if (len <= sizeof(xiaozhi_binary_protocol_v2_t)) {
            return;
        }

        frame = (const xiaozhi_binary_protocol_v2_t *)data;
        frame_len = ntohl(frame->payload_size);
        if ((frame_len == 0) || ((sizeof(xiaozhi_binary_protocol_v2_t) + frame_len) > len)) {
            return;
        }

        payload = frame->payload;
        payload_len = frame_len;
    } else if (ws_version == 3) {
        const xiaozhi_binary_protocol_v3_t *frame;
        uint16_t frame_len;

        if (len <= sizeof(xiaozhi_binary_protocol_v3_t)) {
            return;
        }

        frame = (const xiaozhi_binary_protocol_v3_t *)data;
        frame_len = ntohs(frame->payload_size);
        if ((frame_len == 0) || ((sizeof(xiaozhi_binary_protocol_v3_t) + frame_len) > len)) {
            return;
        }

        payload = frame->payload;
        payload_len = frame_len;
    }

    if (payload_len > sizeof(packet.data)) {
        return;
    }

    packet.len = payload_len;
    memcpy(packet.data, payload, payload_len);
    xiaozhi_ctx.rx_audio_packets++;
    if ((xiaozhi_ctx.rx_audio_packets <= 3) || ((xiaozhi_ctx.rx_audio_packets % 20) == 0)) {
        ESP_LOGD(TAG, "rx tts audio packet=%" PRIu32 ", len=%u, ws_version=%d",
                 xiaozhi_ctx.rx_audio_packets,
                 (unsigned)payload_len,
                 ws_version);
    }

    send_ok = xQueueSend(xiaozhi_ctx.playback_queue, &packet, 0);
    if (send_ok != pdPASS) {
        xQueueReceive(xiaozhi_ctx.playback_queue, &drop_packet, 0);
        xQueueSend(xiaozhi_ctx.playback_queue, &packet, 0);
    }
}

static void xiaozhi_process_ws_frame(uint8_t opcode, const uint8_t *data, size_t len)
{
    if ((data == NULL) || (len == 0)) {
        return;
    }

    if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
        xiaozhi_handle_json_message((const char *)data, len);
    } else if (opcode == WS_TRANSPORT_OPCODES_BINARY) {
        xiaozhi_handle_binary_message(data, len);
    }
}

static void xiaozhi_handle_ws_data_event(const esp_websocket_event_data_t *event)
{
    size_t end_offset;

    if ((event == NULL) || (event->payload_len <= 0) || (event->data_len <= 0)) {
        return;
    }

    if (event->payload_offset == 0) {
        if (xiaozhi_ctx.rx_frame.size < (size_t)event->payload_len) {
            free(xiaozhi_ctx.rx_frame.data);
            xiaozhi_ctx.rx_frame.data = malloc(event->payload_len);
            if (xiaozhi_ctx.rx_frame.data == NULL) {
                xiaozhi_ctx.rx_frame.size = 0;
                return;
            }
            xiaozhi_ctx.rx_frame.size = event->payload_len;
        }
        xiaozhi_ctx.rx_frame.opcode = event->op_code;
    }

    if (xiaozhi_ctx.rx_frame.data == NULL) {
        return;
    }

    end_offset = (size_t)event->payload_offset + (size_t)event->data_len;
    if (end_offset > xiaozhi_ctx.rx_frame.size) {
        return;
    }

    memcpy(xiaozhi_ctx.rx_frame.data + event->payload_offset, event->data_ptr, event->data_len);
    if (event->fin && (end_offset >= (size_t)event->payload_len)) {
        xiaozhi_process_ws_frame(xiaozhi_ctx.rx_frame.opcode, xiaozhi_ctx.rx_frame.data, event->payload_len);
    }
}

static void xiaozhi_ws_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = event_data;

    (void)arg;
    (void)event_base;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            xiaozhi_ctx.session_id[0] = '\0';
            xiaozhi_ctx.server_sample_rate = BSP_AUDIO_SAMPLE_RATE;
            xiaozhi_ctx.server_frame_duration = XIAOZHI_SERVER_FRAME_MS;
            xEventGroupClearBits(xiaozhi_ctx.event_group, XIAOZHI_WS_READY_BIT);
            xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_CONNECTING);
            xiaozhi_ui_status_set_audio_info(xiaozhi_ctx.server_sample_rate,
                                             xiaozhi_ctx.server_frame_duration,
                                             xiaozhi_get_ws_version());
            xiaozhi_ui_status_set_note("语音链路已连接，正在握手");
            xiaozhi_ui_refresh_connect_state();
            ESP_LOGI(TAG, "websocket connected");
            xiaozhi_send_hello();
            break;

        case WEBSOCKET_EVENT_DATA:
            xiaozhi_handle_ws_data_event(data);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            xiaozhi_ctx.recording = false;
            xiaozhi_ctx.stop_pending = false;
            xiaozhi_ctx.stop_request_us = 0;
            xiaozhi_ctx.session_id[0] = '\0';
            xEventGroupClearBits(xiaozhi_ctx.event_group, XIAOZHI_WS_READY_BIT);
            xiaozhi_audio_stop_playback();
            if (xiaozhi_ctx.ws_paused_for_camera || (xiaozhi_ui_status_get_view() == XIAOZHI_UI_VIEW_CAMERA)) {
                xiaozhi_ui_status_set_note("摄像头预览中，语音链路已暂停");
                ESP_LOGI(TAG, "websocket paused for camera view");
            } else {
                xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_DISCONNECTED);
                xiaozhi_ui_status_set_note("服务器断开，等待自动重连");
                ESP_LOGW(TAG, "websocket disconnected, waiting auto reconnect");
            }
            xiaozhi_ui_refresh_connect_state();
            break;

        case WEBSOCKET_EVENT_ERROR:
            if (!(xiaozhi_ctx.ws_paused_for_camera || (xiaozhi_ui_status_get_view() == XIAOZHI_UI_VIEW_CAMERA))) {
                xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_ERROR);
                xiaozhi_ui_status_set_note("语音链路异常，正在重连");
                ESP_LOGE(TAG, "websocket error, type=%d, status=%d, sock_errno=%d",
                         data->error_handle.error_type,
                         data->error_handle.esp_ws_handshake_status_code,
                         data->error_handle.esp_transport_sock_errno);
            }
            break;

        default:
            break;
    }
}

static void xiaozhi_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START)) {
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_CONNECTING);
        xiaozhi_ui_status_set_note("正在连接 Wi-Fi\n长按 KEY3 进入配网");
        xiaozhi_ctx.wifi_ip[0] = '\0';
        xiaozhi_ui_refresh_network_info();
        esp_wifi_connect();
    } else if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(xiaozhi_ctx.event_group, XIAOZHI_WIFI_CONNECTED_BIT);
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_DISCONNECTED);
        xiaozhi_ui_status_set_note("Wi-Fi 已断开，正在重连\n长按 KEY3 进入配网");
        xiaozhi_ctx.wifi_ip[0] = '\0';
        xiaozhi_ui_refresh_network_info();
        xiaozhi_ui_refresh_connect_state();
        esp_wifi_connect();
        ESP_LOGW(TAG, "wifi disconnected, reconnecting");
    } else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        ip_event_got_ip_t *event = event_data;
        xEventGroupSetBits(xiaozhi_ctx.event_group, XIAOZHI_WIFI_CONNECTED_BIT);
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_CONNECTING);
        xiaozhi_ui_status_set_note("Wi-Fi 已连接，正在联系小智服务器");
        snprintf(xiaozhi_ctx.wifi_ip, sizeof(xiaozhi_ctx.wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xiaozhi_ui_refresh_network_info();
        xiaozhi_ui_refresh_connect_state();
        ESP_LOGI(TAG, "wifi got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xiaozhi_time_sync_start();
        if ((xiaozhi_ctx.ws_client != NULL) && !xiaozhi_ctx.ws_paused_for_camera && !xiaozhi_ctx.ws_client_started) {
            xiaozhi_resume_ws_after_camera();
        }
    }
}

static void xiaozhi_wifi_init(void)
{
    app_wifi_credentials_t credentials = {0};
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_cfg = {0};
    size_t ssid_len = 0;
    size_t password_len = 0;

    ESP_ERROR_CHECK(app_wifi_store_load(&credentials));
    xiaozhi_safe_copy(xiaozhi_ctx.wifi_ssid, sizeof(xiaozhi_ctx.wifi_ssid), credentials.ssid);
    xiaozhi_ctx.wifi_ip[0] = '\0';
    xiaozhi_ui_refresh_network_info();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    xiaozhi_time_sync_init();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &xiaozhi_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &xiaozhi_wifi_event_handler, NULL));

    ssid_len = strnlen(credentials.ssid, sizeof(wifi_cfg.sta.ssid));
    password_len = strnlen(credentials.password, sizeof(wifi_cfg.sta.password));
    memcpy(wifi_cfg.sta.ssid, credentials.ssid, ssid_len);
    memcpy(wifi_cfg.sta.password, credentials.password, password_len);
    wifi_cfg.sta.scan_method = WIFI_FAST_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG,
             "wifi target ssid=%s, source=%s",
             credentials.ssid,
             (credentials.source == APP_WIFI_SOURCE_NVS) ? "nvs" : "menuconfig");
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void xiaozhi_build_device_headers(void)
{
    char auth_header[XIAOZHI_WS_TOKEN_LEN + 16];
    const char *auth_value = xiaozhi_ctx.ws_token;

    if ((auth_value[0] != '\0') && (strchr(auth_value, ' ') == NULL)) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", auth_value);
        auth_value = auth_header;
    }

    if (auth_value[0] != '\0') {
        snprintf(xiaozhi_ctx.ws_headers, sizeof(xiaozhi_ctx.ws_headers),
                 "Authorization: %s\r\n"
                 "Protocol-Version: %d\r\n"
                 "Device-Id: %s\r\n"
                 "Client-Id: %s\r\n",
                 auth_value,
                 xiaozhi_get_ws_version(),
                 xiaozhi_ctx.device_id,
                 xiaozhi_ctx.client_id);
    } else {
        snprintf(xiaozhi_ctx.ws_headers, sizeof(xiaozhi_ctx.ws_headers),
                 "Protocol-Version: %d\r\n"
                 "Device-Id: %s\r\n"
                 "Client-Id: %s\r\n",
                 xiaozhi_get_ws_version(),
                 xiaozhi_ctx.device_id,
                 xiaozhi_ctx.client_id);
    }
}

static esp_err_t xiaozhi_websocket_init(void)
{
    esp_websocket_client_config_t ws_cfg = {
        .uri = xiaozhi_ctx.ws_url,
        .headers = xiaozhi_ctx.ws_headers,
        .task_prio = 8,
        .task_stack = XIAOZHI_WS_TASK_STACK,
        .buffer_size = XIAOZHI_WS_BUFFER_SIZE,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 1000,
        .enable_close_reconnect = true,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 30,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    xiaozhi_log_heap("before_ws_init");
    xiaozhi_ctx.ws_client = esp_websocket_client_init(&ws_cfg);
    if (xiaozhi_ctx.ws_client == NULL) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_websocket_register_events(xiaozhi_ctx.ws_client,
                                                      WEBSOCKET_EVENT_ANY,
                                                      xiaozhi_ws_event_handler,
                                                      NULL),
                        TAG,
                        "register websocket event failed");

    if (esp_websocket_client_start(xiaozhi_ctx.ws_client) != ESP_OK) {
        xiaozhi_log_heap("ws_start_failed");
        esp_websocket_client_destroy(xiaozhi_ctx.ws_client);
        xiaozhi_ctx.ws_client = NULL;
        xiaozhi_ctx.ws_client_started = false;
        return ESP_FAIL;
    }

    xiaozhi_ctx.ws_client_started = true;
    xiaozhi_log_heap("ws_started");
    return ESP_OK;
}

static void xiaozhi_audio_task(void *pvParameters)
{
    xiaozhi_opus_packet_t packet;
    int16_t *rx_stereo_buf;
    int16_t *tx_mono_buf;
    uint8_t *opus_buf;
    int16_t *mono_buf;
    int16_t *stereo_buf;
    (void)pvParameters;

    rx_stereo_buf = malloc(XIAOZHI_PCM_INPUT_SAMPLES * sizeof(int16_t));
    tx_mono_buf = malloc(XIAOZHI_PCM_ENCODE_SAMPLES * sizeof(int16_t));
    opus_buf = malloc(XIAOZHI_OPUS_MAX_PACKET_SIZE);
    mono_buf = malloc(XIAOZHI_PCM_MONO_MAX_SAMPLES * sizeof(int16_t));
    stereo_buf = malloc(XIAOZHI_PCM_STEREO_OUT_MAX * sizeof(int16_t));
    if ((rx_stereo_buf == NULL) || (tx_mono_buf == NULL) || (opus_buf == NULL) ||
        (mono_buf == NULL) || (stereo_buf == NULL)) {
        ESP_LOGE(TAG, "audio buffer alloc failed");
        free(rx_stereo_buf);
        free(tx_mono_buf);
        free(opus_buf);
        free(mono_buf);
        free(stereo_buf);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xiaozhi_ctx.recording) {
            size_t samples_read = 0;
            size_t frames_read;
            size_t mono_samples;
            size_t i;
            int encoded_len;

            if (xiaozhi_ctx.stop_pending &&
                ((esp_timer_get_time() - xiaozhi_ctx.stop_request_us) >= (XIAOZHI_RECORD_TAIL_MS * 1000LL))) {
                xiaozhi_finish_manual_recording();
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (!xiaozhi_prepare_encoder_state()) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            if (bsp_audio_read(rx_stereo_buf, XIAOZHI_PCM_INPUT_SAMPLES, &samples_read, 1000) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            if (samples_read < XIAOZHI_PCM_INPUT_SAMPLES) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            frames_read = samples_read / BSP_AUDIO_CHANNELS;
            for (i = 0; i < frames_read; i++) {
                uint16_t left_peak = xiaozhi_abs16(rx_stereo_buf[i * BSP_AUDIO_CHANNELS]);
                uint16_t right_peak = xiaozhi_abs16(rx_stereo_buf[(i * BSP_AUDIO_CHANNELS) + 1]);

                if (left_peak > xiaozhi_ctx.record_peak_left) {
                    xiaozhi_ctx.record_peak_left = left_peak;
                }
                if (right_peak > xiaozhi_ctx.record_peak_right) {
                    xiaozhi_ctx.record_peak_right = right_peak;
                }
            }

            mono_samples = xiaozhi_resample_stereo_to_mono(rx_stereo_buf, frames_read, BSP_AUDIO_SAMPLE_RATE,
                                                           XIAOZHI_ENCODE_SAMPLE_RATE, tx_mono_buf, XIAOZHI_PCM_ENCODE_SAMPLES);
            if (mono_samples != XIAOZHI_PCM_ENCODE_SAMPLES) {
                ESP_LOGW(TAG, "capture resample mismatch, want=%u got=%u",
                         (unsigned)XIAOZHI_PCM_ENCODE_SAMPLES,
                         (unsigned)mono_samples);
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            for (i = 0; i < mono_samples; i++) {
                uint16_t mono_peak = xiaozhi_abs16(tx_mono_buf[i]);
                if (mono_peak > xiaozhi_ctx.record_peak_mono) {
                    xiaozhi_ctx.record_peak_mono = mono_peak;
                }
            }

            xiaozhi_codec_lock();
            if (xiaozhi_ctx.encoder == NULL) {
                xiaozhi_codec_unlock();
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            encoded_len = opus_encode(xiaozhi_ctx.encoder, tx_mono_buf, XIAOZHI_PCM_ENCODE_SAMPLES,
                                      opus_buf, XIAOZHI_OPUS_MAX_PACKET_SIZE);
            xiaozhi_codec_unlock();
            if (encoded_len > 2) {
                if (xiaozhi_ws_send_bin(opus_buf, encoded_len) >= 0) {
                    xiaozhi_ctx.record_packet_count++;
                } else {
                    xiaozhi_ctx.record_send_fail_count++;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (xQueueReceive(xiaozhi_ctx.playback_queue, &packet, pdMS_TO_TICKS(20)) == pdTRUE) {
            size_t out_samples;
            int decoded_samples;
            size_t written = 0;
            uint32_t sample_rate = xiaozhi_normalize_opus_rate(xiaozhi_ctx.server_sample_rate);
            uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
            esp_err_t write_ret;
            bool decoder_ready = true;
            int opus_err = OPUS_OK;

            xiaozhi_codec_lock();
            if ((xiaozhi_ctx.decoder == NULL) || (xiaozhi_ctx.decoder_rate != sample_rate)) {
                if (xiaozhi_ctx.decoder != NULL) {
                    opus_decoder_destroy(xiaozhi_ctx.decoder);
                    xiaozhi_ctx.decoder = NULL;
                    xiaozhi_ctx.decoder_rate = 0;
                }

                xiaozhi_ctx.decoder = opus_decoder_create(sample_rate, 1, &opus_err);
                if ((xiaozhi_ctx.decoder == NULL) || (opus_err != OPUS_OK)) {
                    xiaozhi_ctx.decoder = NULL;
                    decoder_ready = false;
                } else {
                    xiaozhi_ctx.decoder_rate = sample_rate;
                }
            }

            if (!decoder_ready || (xiaozhi_ctx.decoder == NULL)) {
                xiaozhi_codec_unlock();
                xiaozhi_log_heap("decoder_create_failed");
                ESP_LOGE(TAG,
                         "opus decoder create failed, rate=%" PRIu32 ", err=%d, packet_len=%u, frame_ms=%" PRIu32 ", largest=%u",
                         sample_rate,
                         opus_err,
                         (unsigned)packet.len,
                         xiaozhi_ctx.server_frame_duration,
                         (unsigned)heap_caps_get_largest_free_block(caps));
                continue;
            }

            if (xiaozhi_ctx.played_audio_packets == 0) {
                ESP_LOGI(TAG, "playback decoder ready, rate=%" PRIu32 ", frame_ms=%" PRIu32,
                         sample_rate, xiaozhi_ctx.server_frame_duration);
            }

            es8388_speaker_enable(true);
            es8388_set_mute(false);

            decoded_samples = opus_decode(xiaozhi_ctx.decoder, packet.data, packet.len, mono_buf, XIAOZHI_PCM_MONO_MAX_SAMPLES, 0);
            xiaozhi_codec_unlock();
            if (decoded_samples <= 0) {
                ESP_LOGW(TAG, "opus decode failed, ret=%d, len=%u", decoded_samples, (unsigned)packet.len);
                continue;
            }

            out_samples = xiaozhi_resample_mono_to_stereo(mono_buf, decoded_samples, xiaozhi_ctx.decoder_rate,
                                                          stereo_buf, XIAOZHI_PCM_STEREO_OUT_MAX);
            if (out_samples > 0) {
                write_ret = bsp_audio_write(stereo_buf, out_samples, &written, 1000);
                xiaozhi_ctx.played_audio_packets++;
                if ((xiaozhi_ctx.played_audio_packets <= 3) || ((xiaozhi_ctx.played_audio_packets % 20) == 0)) {
                    ESP_LOGD(TAG, "play tts audio packet=%" PRIu32 ", opus=%u, pcm=%d, out=%u, written=%u, stack=%u, ret=%s",
                             xiaozhi_ctx.played_audio_packets,
                             (unsigned)packet.len,
                             decoded_samples,
                             (unsigned)out_samples,
                             (unsigned)written,
                             (unsigned)uxTaskGetStackHighWaterMark(NULL),
                             esp_err_to_name(write_ret));
                }
            }
            xiaozhi_ctx.playing = true;
        } else if ((!xiaozhi_ctx.tts_active) && (uxQueueMessagesWaiting(xiaozhi_ctx.playback_queue) == 0) && xiaozhi_ctx.playing) {
            xiaozhi_audio_stop_playback();
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

static void xiaozhi_button_scan_task(void *pvParameters)
{
    bool raw_pressed = false;
    bool stable_pressed = false;
    bool key3_raw_pressed = false;
    bool key3_stable_pressed = false;
    bool key3_long_handled = false;
    TickType_t debounce_ticks = pdMS_TO_TICKS(XIAOZHI_KEY_DEBOUNCE_MS);
    TickType_t key3_long_ticks = pdMS_TO_TICKS(XIAOZHI_KEY3_LONG_MS);
    TickType_t last_change_tick = xTaskGetTickCount();
    TickType_t key3_last_change_tick = last_change_tick;
    TickType_t key3_press_tick = 0;

    (void)pvParameters;

    while (1) {
        bool key3_level_low = xiaozhi_key3_is_pressed();
        TickType_t now = xTaskGetTickCount();

        if (key3_level_low != key3_raw_pressed) {
            key3_raw_pressed = key3_level_low;
            key3_last_change_tick = now;
        }

        if (((now - key3_last_change_tick) >= debounce_ticks) && (key3_stable_pressed != key3_raw_pressed)) {
            key3_stable_pressed = key3_raw_pressed;

            if (key3_stable_pressed) {
                key3_press_tick = now;
                key3_long_handled = false;
            } else {
                key3_long_handled = false;
            }
        }

        if (key3_stable_pressed &&
            !key3_long_handled &&
            ((now - key3_press_tick) >= key3_long_ticks)) {
            key3_long_handled = true;
            xiaozhi_request_wifi_portal_reboot();
        }

        if (xiaozhi_ui_status_get_view() != XIAOZHI_UI_VIEW_VOICE) {
            raw_pressed = false;
            stable_pressed = false;
            last_change_tick = now;
            vTaskDelay(pdMS_TO_TICKS(XIAOZHI_KEY_SCAN_MS));
            continue;
        }

        bool level_low = xiaozhi_key1_is_pressed();

        if (level_low != raw_pressed) {
            raw_pressed = level_low;
            last_change_tick = now;
        }

        if (((now - last_change_tick) >= debounce_ticks) && (stable_pressed != raw_pressed)) {
            stable_pressed = raw_pressed;

            if (stable_pressed) {
                if (!xiaozhi_ws_is_ready()) {
                    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_DISCONNECTED);
                    xiaozhi_ui_status_set_note("语音服务未就绪，请稍候");
                    ESP_LOGW(TAG, "ws not ready");
                } else {
                    xiaozhi_ctx.local_command_active = false;
                    if (xiaozhi_ctx.playing || xiaozhi_ctx.tts_active) {
                        xiaozhi_send_abort("manual_interrupt");
                        xiaozhi_audio_stop_playback();
                        xiaozhi_ui_status_set_note("当前回复已中断");
                    }
                    if (!xiaozhi_prepare_encoder_state()) {
                        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_ERROR);
                        xiaozhi_ui_status_set_note("录音资源不足，请稍后重试");
                        ESP_LOGW(TAG, "encoder not ready");
                        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_KEY_SCAN_MS));
                        continue;
                    }
                    xiaozhi_ctx.stop_pending = false;
                    xiaozhi_ctx.stop_request_us = 0;
                    xiaozhi_ctx.record_start_us = esp_timer_get_time();
                    xiaozhi_ctx.record_packet_count = 0;
                    xiaozhi_ctx.record_send_fail_count = 0;
                    xiaozhi_ctx.record_peak_left = 0;
                    xiaozhi_ctx.record_peak_right = 0;
                    xiaozhi_ctx.record_peak_mono = 0;
                    xiaozhi_ctx.recording = true;
                    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_LISTENING);
                    xiaozhi_ui_status_set_note("正在聆听，松开发送");
                    xiaozhi_ui_status_set_stt_text("正在采集你的语音");
                    xiaozhi_ui_status_set_llm_text("等待你说完后发送到大模型");
                    xiaozhi_ui_status_set_tts_text("语音回复生成后会显示在这里");
                    xiaozhi_send_listen("start");
                    ESP_LOGI(TAG, "listen start");
                }
            } else if (xiaozhi_ctx.recording && !xiaozhi_ctx.stop_pending) {
                xiaozhi_ctx.stop_pending = true;
                xiaozhi_ctx.stop_request_us = esp_timer_get_time();
                xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_THINKING);
                xiaozhi_ui_status_set_note("正在结束录音，等待识别");
                xiaozhi_ui_status_set_stt_text("正在整理语音尾音...");
                xiaozhi_ui_status_set_llm_text("语音即将上传，请稍候");
                xiaozhi_ui_status_set_tts_text("合成完成后会开始播报");
                ESP_LOGI(TAG, "listen release, tail_capture=%ums", XIAOZHI_RECORD_TAIL_MS);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_KEY_SCAN_MS));
    }
}

static esp_err_t xiaozhi_start_button_scan_task(void)
{
    if (g_xiaozhi_button_task_handle != NULL) {
        return ESP_OK;
    }

    return (xTaskCreate(xiaozhi_button_scan_task,
                        "xiaozhi_btn",
                        XIAOZHI_BUTTON_TASK_STACK,
                        NULL,
                        XIAOZHI_BUTTON_TASK_PRIO,
                        &g_xiaozhi_button_task_handle) == pdPASS) ? ESP_OK : ESP_FAIL;
}

static void xiaozhi_task(void *pvParameters)
{
    xiaozhi_server_config_t server_cfg = {0};

    (void)pvParameters;

    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_BOOT);
    xiaozhi_ui_status_set_note("正在初始化小智语音助手");
    xiaozhi_ui_status_set_binding_text("查询中");

    xiaozhi_ctx.event_group = xEventGroupCreate();
    xiaozhi_ctx.playback_queue = xQueueCreate(XIAOZHI_PLAYBACK_QUEUE_LEN, sizeof(xiaozhi_opus_packet_t));
    xiaozhi_ctx.ws_mutex = xSemaphoreCreateMutex();
    xiaozhi_ctx.codec_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK((xiaozhi_ctx.event_group && xiaozhi_ctx.playback_queue &&
                     xiaozhi_ctx.ws_mutex && xiaozhi_ctx.codec_mutex) ? ESP_OK : ESP_FAIL);

#ifdef CONFIG_XIAOZHI_ENABLE_LCD_STATUS
    if (lcd_lvgl_reserve_buffer() != ESP_OK) {
        ESP_LOGW(TAG, "lcd buffer reserve failed before activation");
    } else {
        xiaozhi_try_start_lcd();
    }
#endif

    ESP_ERROR_CHECK(xiaozhi_start_button_scan_task());

    xiaozhi_wifi_init();
    xEventGroupWaitBits(xiaozhi_ctx.event_group, XIAOZHI_WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    xiaozhi_ui_status_set_note("Wi-Fi 已连接，正在获取服务器配置");

    ESP_ERROR_CHECK(xiaozhi_activation_prepare(&server_cfg));
    snprintf(xiaozhi_ctx.device_id, sizeof(xiaozhi_ctx.device_id), "%s", server_cfg.device_id);
    snprintf(xiaozhi_ctx.client_id, sizeof(xiaozhi_ctx.client_id), "%s", server_cfg.client_id);
    snprintf(xiaozhi_ctx.ws_url, sizeof(xiaozhi_ctx.ws_url), "%s", server_cfg.ws_url);
    snprintf(xiaozhi_ctx.ws_token, sizeof(xiaozhi_ctx.ws_token), "%s", server_cfg.ws_token);
    xiaozhi_ctx.ws_version = server_cfg.ws_version;
    xiaozhi_ui_status_set_audio_info(BSP_AUDIO_SAMPLE_RATE, XIAOZHI_SERVER_FRAME_MS, xiaozhi_ctx.ws_version);
    xiaozhi_ui_status_set_binding_text("已绑定");
    xiaozhi_ui_status_set_view(XIAOZHI_UI_VIEW_VOICE);
    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_CONNECTING);
    xiaozhi_ui_status_set_note("绑定完成，正在进入语音界面");
    ESP_LOGI(TAG, "xiaozhi server ready, ws_version=%d", xiaozhi_ctx.ws_version);

    xiaozhi_log_heap("after_audio_init");

    xiaozhi_build_device_headers();

    while (xiaozhi_websocket_init() != ESP_OK) {
        xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_DISCONNECTED);
        xiaozhi_ui_status_set_note("语音链路启动失败，正在重试");
        ESP_LOGW(TAG, "websocket start failed, retrying");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

BaseType_t xiaozhi_task_create(void)
{
    return xTaskCreate(xiaozhi_task, "xiaozhi_task", XIAOZHI_TASK_STACK, NULL, XIAOZHI_TASK_PRIO, NULL);
}
