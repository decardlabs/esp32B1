#include "task_xiaozhi.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "bsp_wifi.h"
#include "es8388_audio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "task_lcd_lvgl.h"
#include "xl9555.h"

#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"

#define XIAOZHI_WIFI_SSID             "wfeng"
#define XIAOZHI_WIFI_PASSWORD         "wf05430543"

#define XIAOZHI_TASK_STACK            (32 * 1024)
#define XIAOZHI_PLAY_TASK_STACK       (16 * 1024)
#define XIAOZHI_AUDIO_QUEUE_LEN       8
#define XIAOZHI_KEY_SCAN_MS           20
#define XIAOZHI_AUDIO_FRAME_MS        60
#define XIAOZHI_OPUS_BITRATE          24000
#define XIAOZHI_OPUS_COMPLEXITY       1
#define XIAOZHI_WIFI_WAIT_MS          20000
#define XIAOZHI_RECORD_LOG_INTERVAL   20
#define XIAOZHI_AUDIO_PKT_LOG_INTERVAL 20

typedef struct {
    uint8_t *data;
    int len;
} xiaozhi_audio_packet_t;

static const char *TAG = "TASK_XIAOZHI";

static esp_xiaozhi_chat_handle_t s_chat_handle = 0;
static esp_mcp_t *s_mcp_engine = NULL;
static QueueHandle_t s_audio_play_queue = NULL;
static bool s_xiaozhi_connected = false;
static bool s_audio_channel_opened = false;
static bool s_recording = false;
static uint32_t s_record_start_tick = 0;
static uint32_t s_record_frame_count = 0;
static uint32_t s_audio_rx_packet_count = 0;
static uint32_t s_audio_play_packet_count = 0;

static void xiaozhi_log_heap(const char *step)
{
    ESP_LOGI(TAG, "%s heap: total=%u, internal=%u, psram=%u, largest=%u",
             step,
             heap_caps_get_free_size(MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void xiaozhi_log_task_stack(const char *step)
{
    ESP_LOGI(TAG, "%s stack watermark:%u",
             step,
             (unsigned int)uxTaskGetStackHighWaterMark(NULL));
}

static void xiaozhi_log_text(const char *title, const char *text)
{
    ESP_LOGI(TAG, "========== %s ==========", title);
    ESP_LOGI(TAG, "%s", (text != NULL) ? text : "");
    ESP_LOGI(TAG, "==============================");
}

static void xiaozhi_log_pcm_info(const int16_t *pcm, int sample_count)
{
    int16_t min_value = 32767;
    int16_t max_value = -32768;
    uint32_t abs_sum = 0;

    if ((pcm == NULL) || (sample_count <= 0)) {
        return;
    }

    for (int i = 0; i < sample_count; i++) {
        int16_t sample = pcm[i];
        int32_t abs_value = sample;

        if (sample < min_value) {
            min_value = sample;
        }
        if (sample > max_value) {
            max_value = sample;
        }
        if (abs_value < 0) {
            abs_value = -abs_value;
        }
        abs_sum += abs_value;
    }

    ESP_LOGI(TAG, "record frame:%lu, samples:%d, min:%d, max:%d, avg_abs:%lu",
             (unsigned long)s_record_frame_count,
             sample_count,
             min_value,
             max_value,
             (unsigned long)(abs_sum / (uint32_t)sample_count));
}

static esp_err_t xiaozhi_audio_err_to_esp(esp_audio_err_t ret, const char *msg)
{
    if (ret == ESP_AUDIO_ERR_OK) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "%s failed: %d", msg, ret);
    return ESP_FAIL;
}

static esp_mcp_value_t xiaozhi_mcp_get_device_status_cb(const esp_mcp_property_list_t *properties)
{
    char status[160];

    (void)properties;

    snprintf(status, sizeof(status),
             "{\"wifi\":%s,\"xiaozhi\":%s,\"audio_channel\":%s,\"recording\":%s}",
             bsp_wifi_sta_is_connected() ? "true" : "false",
             s_xiaozhi_connected ? "true" : "false",
             s_audio_channel_opened ? "true" : "false",
             s_recording ? "true" : "false");

    return esp_mcp_value_create_string(status);
}

static esp_mcp_value_t xiaozhi_mcp_set_volume_cb(const esp_mcp_property_list_t *properties)
{
    int volume = esp_mcp_property_list_get_property_int(properties, "volume");

    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    return esp_mcp_value_create_bool(es8388_audio_set_output_volume(volume) == ESP_OK);
}

static esp_err_t xiaozhi_mcp_add_tool(esp_mcp_t *mcp_engine, esp_mcp_tool_t *tool)
{
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(tool != NULL, ESP_ERR_NO_MEM, TAG, "create mcp tool failed");

    ret = esp_mcp_add_tool(mcp_engine, tool);
    if (ret != ESP_OK) {
        esp_mcp_tool_destroy(tool);
    }

    return ret;
}

static esp_err_t xiaozhi_mcp_engine_init(void)
{
    esp_mcp_tool_t *volume_tool = NULL;
    esp_err_t ret;

    if (s_mcp_engine != NULL) {
        return ESP_OK;
    }

    ret = esp_mcp_create(&s_mcp_engine);
    ESP_RETURN_ON_ERROR(ret, TAG, "create mcp engine failed");

    ret = xiaozhi_mcp_add_tool(s_mcp_engine,
                               esp_mcp_tool_create("self.get_device_status",
                                                   "Get current ESP32 XiaoZhi demo status.",
                                                   xiaozhi_mcp_get_device_status_cb));
    ESP_GOTO_ON_ERROR(ret, err, TAG, "add status tool failed");

    volume_tool = esp_mcp_tool_create("audio.set_volume",
                                      "Set ES8388 speaker volume, range 0 to 100.",
                                      xiaozhi_mcp_set_volume_cb);
    ESP_GOTO_ON_FALSE(volume_tool != NULL, ESP_ERR_NO_MEM, err, TAG, "create volume tool failed");

    ret = esp_mcp_tool_add_property(volume_tool, esp_mcp_property_create_with_range("volume", 0, 100));
    ESP_GOTO_ON_ERROR(ret, err, TAG, "add volume property failed");

    ret = xiaozhi_mcp_add_tool(s_mcp_engine, volume_tool);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "add volume tool failed");

    ESP_LOGI(TAG, "mcp engine init done");
    return ESP_OK;

err:
    if (volume_tool != NULL) {
        esp_mcp_tool_destroy(volume_tool);
    }
    esp_mcp_destroy(s_mcp_engine);
    s_mcp_engine = NULL;
    return ret;
}

static void xiaozhi_chat_audio_cb(const uint8_t *data, int len, void *ctx)
{
    xiaozhi_audio_packet_t packet = {0};

    (void)ctx;

    if ((s_audio_play_queue == NULL) || (data == NULL) || (len <= 0)) {
        return;
    }

    packet.data = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    if (packet.data == NULL) {
        ESP_LOGW(TAG, "drop audio: no memory");
        return;
    }

    memcpy(packet.data, data, len);
    packet.len = len;
    s_audio_rx_packet_count++;

    if ((s_audio_rx_packet_count <= 3) || ((s_audio_rx_packet_count % XIAOZHI_AUDIO_PKT_LOG_INTERVAL) == 0)) {
        ESP_LOGI(TAG, "audio rx packet:%lu, len:%d",
                 (unsigned long)s_audio_rx_packet_count,
                 len);
    }

    if (xQueueSend(s_audio_play_queue, &packet, 0) != pdTRUE) {
        free(packet.data);
        ESP_LOGW(TAG, "drop audio: queue full");
    }
}

static void xiaozhi_chat_event_cb(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    (void)ctx;

    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        esp_xiaozhi_chat_text_data_t *text_data = (esp_xiaozhi_chat_text_data_t *)event_data;

        if ((text_data == NULL) || (text_data->text == NULL)) {
            break;
        }

        if (text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) {
            xiaozhi_log_text("ASR record text", text_data->text);
            lcd_lvgl_set_record_text(text_data->text);
        } else {
            xiaozhi_log_text("Xiaozhi reply text", text_data->text);
            lcd_lvgl_set_reply_text(text_data->text);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        esp_xiaozhi_chat_tts_state_t *tts_state = (esp_xiaozhi_chat_tts_state_t *)event_data;

        if (tts_state == NULL) {
            break;
        }

        if (tts_state->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            lcd_lvgl_set_status("Speaking...");
        } else if (tts_state->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            lcd_lvgl_set_status("Ready. Hold KEY1 to talk.");
        } else if ((tts_state->state == ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START) && (tts_state->text != NULL)) {
            xiaozhi_log_text("Xiaozhi TTS sentence", tts_state->text);
            lcd_lvgl_set_reply_text(tts_state->text);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        esp_xiaozhi_chat_error_info_t *error = (esp_xiaozhi_chat_error_info_t *)event_data;
        char status[96];

        snprintf(status, sizeof(status), "Xiaozhi error: %s",
                 (error != NULL && error->source != NULL) ? error->source : "unknown");
        lcd_lvgl_set_status(status);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        lcd_lvgl_set_status("Speaking...");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        lcd_lvgl_set_status("Ready. Hold KEY1 to talk.");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        ESP_LOGI(TAG, "system cmd: %s", event_data ? (const char *)event_data : "");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
    default:
        break;
    }
}

static void xiaozhi_esp_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch (event_id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        s_xiaozhi_connected = true;
        lcd_lvgl_set_status("Connected. Hold KEY1 to talk.");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        s_xiaozhi_connected = false;
        s_audio_channel_opened = false;
        lcd_lvgl_set_status("Xiaozhi disconnected.");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        s_audio_channel_opened = true;
        lcd_lvgl_set_status("Audio channel opened.");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        s_audio_channel_opened = false;
        s_recording = false;
        lcd_lvgl_set_status("Audio channel closed.");
        break;
    default:
        break;
    }
}

static void xiaozhi_play_task(void *pvParameters)
{
    void *dec_handle = NULL;
    uint8_t *pcm_buf = NULL;
    const int pcm_buf_size = ES8388_AUDIO_SAMPLE_RATE * ES8388_AUDIO_CHANNELS * XIAOZHI_AUDIO_FRAME_MS *
                             (ES8388_AUDIO_BITS_PER_SAMPLE / 8) / 1000 * 2;
    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();

    (void)pvParameters;

    dec_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    dec_cfg.channel = ESP_AUDIO_MONO;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    dec_cfg.self_delimited = false;

    ESP_ERROR_CHECK(xiaozhi_audio_err_to_esp(esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &dec_handle),
                                             "opus decoder open"));

    pcm_buf = heap_caps_malloc(pcm_buf_size, MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(pcm_buf != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    while (1) {
        xiaozhi_audio_packet_t packet = {0};

        if (xQueueReceive(s_audio_play_queue, &packet, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint8_t *raw_ptr = packet.data;
        uint32_t raw_left = packet.len;

        while (raw_left > 0) {
            esp_audio_dec_in_raw_t raw = {
                .buffer = raw_ptr,
                .len = raw_left,
            };
            esp_audio_dec_out_frame_t frame = {
                .buffer = pcm_buf,
                .len = pcm_buf_size,
            };
            esp_audio_dec_info_t info = {0};
            esp_audio_err_t ret = esp_opus_dec_decode(dec_handle, &raw, &frame, &info);

            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "opus decode failed: %d", ret);
                break;
            }

            if (frame.decoded_size > 0) {
                s_audio_play_packet_count++;
                if ((s_audio_play_packet_count <= 3) || ((s_audio_play_packet_count % XIAOZHI_AUDIO_PKT_LOG_INTERVAL) == 0)) {
                    ESP_LOGI(TAG, "audio play packet:%lu, decoded:%d",
                             (unsigned long)s_audio_play_packet_count,
                             frame.decoded_size);
                }
                if (es8388_audio_write_mono(frame.buffer, frame.decoded_size) != ESP_OK) {
                    ESP_LOGW(TAG, "write speaker pcm failed, bytes:%d", frame.decoded_size);
                }
            }

            if ((raw.consumed == 0) || (raw.consumed > raw_left)) {
                break;
            }
            raw_ptr += raw.consumed;
            raw_left -= raw.consumed;
        }

        free(packet.data);
    }
}

static esp_err_t xiaozhi_get_server_info(esp_xiaozhi_chat_info_t *info)
{
    esp_err_t ret;

    lcd_lvgl_set_status("Getting Xiaozhi server info...");
    ret = esp_xiaozhi_chat_get_info(info);
    if (ret != ESP_OK) {
        lcd_lvgl_set_status("Get server info failed.");
        return ret;
    }

    if (info->has_activation_code) {
        char reply[256];

        snprintf(reply, sizeof(reply), "Activation code: %s\n%s",
                 info->activation_code ? info->activation_code : "",
                 info->activation_message ? info->activation_message : "Bind this device on xiaozhi.me");
        lcd_lvgl_set_reply_text(reply);
    }

    return ESP_OK;
}

static esp_err_t xiaozhi_chat_start(void)
{
    esp_xiaozhi_chat_info_t info = {0};
    bool prefer_websocket = false;
    esp_err_t ret;

    ret = xiaozhi_mcp_engine_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = xiaozhi_get_server_info(&info);

    if (ret != ESP_OK) {
        return ret;
    }

    esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    prefer_websocket = info.has_websocket_config;
    config.audio_callback = xiaozhi_chat_audio_cb;
    config.event_callback = xiaozhi_chat_event_cb;
    config.has_mqtt_config = prefer_websocket ? false : info.has_mqtt_config;
    config.has_websocket_config = info.has_websocket_config;
    config.mcp_engine = s_mcp_engine;
    config.owns_mcp_engine = false;

    ESP_LOGI(TAG, "xiaozhi transport: %s",
             prefer_websocket ? "websocket" : (info.has_mqtt_config ? "mqtt+udp" : "websocket"));

    ret = esp_xiaozhi_chat_init(&config, &s_chat_handle);
    esp_xiaozhi_chat_free_info(&info);
    ESP_RETURN_ON_ERROR(ret, TAG, "xiaozhi chat init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS,
                                                   ESP_EVENT_ANY_ID,
                                                   xiaozhi_esp_event_handler,
                                                   NULL),
                        TAG, "register xiaozhi event failed");

    lcd_lvgl_set_status("Connecting Xiaozhi...");
    ESP_RETURN_ON_ERROR(esp_xiaozhi_chat_start(s_chat_handle), TAG, "xiaozhi chat start failed");
    return ESP_OK;
}

static esp_err_t xiaozhi_opus_encoder_open(void **enc_handle, int *pcm_size, int *opus_size)
{
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    esp_audio_err_t ret;

    enc_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = ESP_AUDIO_BIT16;
    enc_cfg.bitrate = XIAOZHI_OPUS_BITRATE;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = XIAOZHI_OPUS_COMPLEXITY;
    enc_cfg.enable_vbr = true;

    ret = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), enc_handle);
    ESP_RETURN_ON_ERROR(xiaozhi_audio_err_to_esp(ret, "opus encoder open"), TAG, "opus encoder open failed");

    ret = esp_opus_enc_get_frame_size(*enc_handle, pcm_size, opus_size);
    return xiaozhi_audio_err_to_esp(ret, "opus encoder frame size");
}

static esp_err_t xiaozhi_open_audio_channel(void)
{
    esp_xiaozhi_chat_audio_t audio = {
        .format = "opus",
        .sample_rate = ES8388_AUDIO_SAMPLE_RATE,
        .channels = ES8388_AUDIO_CHANNELS,
        .frame_duration = XIAOZHI_AUDIO_FRAME_MS,
    };

    if (s_audio_channel_opened) {
        return ESP_OK;
    }

    return esp_xiaozhi_chat_open_audio_channel(s_chat_handle, &audio, NULL, 0);
}

static esp_err_t xiaozhi_record_start(void *enc_handle)
{
    esp_err_t ret;

    if (!s_xiaozhi_connected) {
        lcd_lvgl_set_status("Wait for Xiaozhi connection...");
        return ESP_ERR_INVALID_STATE;
    }

    ret = xiaozhi_open_audio_channel();
    if (ret != ESP_OK) {
        lcd_lvgl_set_status("Open audio channel failed.");
        return ret;
    }

    esp_opus_enc_reset(enc_handle);
    esp_xiaozhi_chat_send_abort_speaking(s_chat_handle, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
    ret = esp_xiaozhi_chat_send_start_listening(s_chat_handle, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL);
    if (ret != ESP_OK) {
        lcd_lvgl_set_status("Start listening failed.");
        return ret;
    }

    s_recording = true;
    s_record_start_tick = xTaskGetTickCount();
    s_record_frame_count = 0;
    lcd_lvgl_set_status("Recording...");
    lcd_lvgl_set_record_text("Listening...");
    ESP_LOGI(TAG, "record start");
    xiaozhi_log_task_stack("record start");
    return ESP_OK;
}

static void xiaozhi_record_stop(void)
{
    if (!s_recording) {
        return;
    }

    s_recording = false;
    esp_xiaozhi_chat_send_stop_listening(s_chat_handle);
    lcd_lvgl_set_status("Thinking...");
    ESP_LOGI(TAG, "record stop, duration:%lu ms, frames:%lu",
             (unsigned long)((xTaskGetTickCount() - s_record_start_tick) * portTICK_PERIOD_MS),
             (unsigned long)s_record_frame_count);
}

static void xiaozhi_record_send_one_frame(void *enc_handle, uint8_t *pcm_buf, int pcm_size, uint8_t *opus_buf, int opus_size)
{
    bool need_log = false;
    esp_audio_enc_in_frame_t in_frame = {
        .buffer = pcm_buf,
        .len = pcm_size,
    };
    esp_audio_enc_out_frame_t out_frame = {
        .buffer = opus_buf,
        .len = opus_size,
    };
    esp_audio_err_t enc_ret;

    if (es8388_audio_read_mono(pcm_buf, pcm_size) != ESP_OK) {
        ESP_LOGW(TAG, "read pcm failed");
        return;
    }
    s_record_frame_count++;

    need_log = (s_record_frame_count <= 3) || ((s_record_frame_count % XIAOZHI_RECORD_LOG_INTERVAL) == 0);
    if (need_log) {
        xiaozhi_log_pcm_info((const int16_t *)pcm_buf, pcm_size / sizeof(int16_t));
        xiaozhi_log_task_stack("before opus encode");
    }

    enc_ret = esp_opus_enc_process(enc_handle, &in_frame, &out_frame);
    if (need_log) {
        xiaozhi_log_task_stack("after opus encode");
    }

    if ((enc_ret == ESP_AUDIO_ERR_OK) && (out_frame.encoded_bytes > 0)) {
        esp_xiaozhi_chat_send_audio_data(s_chat_handle, (const char *)opus_buf, out_frame.encoded_bytes);
    } else if (enc_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "opus encode failed: %d", enc_ret);
    }
}

static void xiaozhi_task(void *pvParameters)
{
    void *opus_encoder = NULL;
    uint8_t *pcm_buf = NULL;
    uint8_t *opus_buf = NULL;
    int pcm_size = 0;
    int opus_size = 0;
    bool last_key_pressed = false;

    (void)pvParameters;

    lcd_lvgl_set_status("WiFi connecting...");
    ESP_ERROR_CHECK(bsp_wifi_sta_connect(XIAOZHI_WIFI_SSID,
                                         XIAOZHI_WIFI_PASSWORD,
                                         pdMS_TO_TICKS(XIAOZHI_WIFI_WAIT_MS)));
    lcd_lvgl_set_status("Audio init...");
    ESP_ERROR_CHECK(es8388_audio_init());
    ESP_ERROR_CHECK(xl9555_keys_gpio_init());

    s_audio_play_queue = xQueueCreate(XIAOZHI_AUDIO_QUEUE_LEN, sizeof(xiaozhi_audio_packet_t));
    ESP_ERROR_CHECK(s_audio_play_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    xiaozhi_log_heap("before xiaozhi chat start");
    ESP_ERROR_CHECK(xiaozhi_chat_start());
    xiaozhi_log_heap("after xiaozhi chat start");

    ESP_ERROR_CHECK(xiaozhi_opus_encoder_open(&opus_encoder, &pcm_size, &opus_size));
    xiaozhi_log_task_stack("after opus encoder open");
    xiaozhi_log_heap("after opus encoder open");

    pcm_buf = heap_caps_malloc(pcm_size, MALLOC_CAP_8BIT);
    opus_buf = heap_caps_malloc(opus_size, MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK((pcm_buf != NULL) && (opus_buf != NULL) ? ESP_OK : ESP_ERR_NO_MEM);
    xiaozhi_log_heap("after record buffer alloc");

    ESP_ERROR_CHECK(xTaskCreate(xiaozhi_play_task, "xiaozhi_play", XIAOZHI_PLAY_TASK_STACK, NULL, 7, NULL) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
    xiaozhi_log_heap("after play task start");

    lcd_lvgl_set_status(s_xiaozhi_connected ? "Ready. Hold KEY1 to talk." : "Waiting Xiaozhi connection...");

    while (1) {
        bool key_pressed = false;

        if (xl9555_key_is_pressed(XL9555_KEY1, &key_pressed) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(XIAOZHI_KEY_SCAN_MS));
            continue;
        }

        if (key_pressed && !last_key_pressed) {
            if (xiaozhi_record_start(opus_encoder) != ESP_OK) {
                s_recording = false;
            }
        }

        if (!key_pressed && last_key_pressed) {
            xiaozhi_record_stop();
        }

        last_key_pressed = key_pressed;

        if (s_recording) {
            xiaozhi_record_send_one_frame(opus_encoder, pcm_buf, pcm_size, opus_buf, opus_size);
        } else {
            vTaskDelay(pdMS_TO_TICKS(XIAOZHI_KEY_SCAN_MS));
        }
    }
}

BaseType_t xiaozhi_task_create(void)
{
    return xTaskCreate(xiaozhi_task, "xiaozhi_task", XIAOZHI_TASK_STACK, NULL, 8, NULL);
}
