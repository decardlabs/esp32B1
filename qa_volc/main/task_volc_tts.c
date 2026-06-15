/*
 * task_volc_tts.c - 火山引擎 TTS（语音合成）HTTP SSE 客户端
 *
 * Receives text via a queue, sends to Volcengine TTS API, receives
 * base64-encoded PCM audio via SSE, decodes and plays via I2S.
 *
 * Flow:
 *   text → HTTP POST → SSE stream → base64 PCM → I2S playback
 */

#include "task_volc_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "bsp_audio.h"
#include "es8388.h"
#include "esp_crt_bundle.h"
#include "task_qa_lvgl.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

static const char *TAG = "VOLC_TTS";

#define TTS_API_URL               "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
#define TTS_DEFAULT_RESOURCE_ID   "seed-tts-2.0"
#define TTS_DEFAULT_SPEAKER       "zh_female_vv_uranus_bigtts"

#define TTS_TASK_STACK            16384
#define TTS_TASK_PRIO             5
#define TTS_QUEUE_LEN             4
#define TTS_TEXT_MAX              1024

#define TTS_HTTP_TIMEOUT_MS       30000
#define TTS_READ_BUF_SIZE         2048

/* Max PCM buffer for one TTS response (24000Hz mono 16-bit, ~65s = 3.0MB) */
#define TTS_PCM_BUF_SIZE          (3 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/*  Message queue                                                     */
/* ------------------------------------------------------------------ */

static QueueHandle_t s_tts_queue = NULL;

/* ------------------------------------------------------------------ */
/*  Playback state                                                    */
/* ------------------------------------------------------------------ */

static volatile bool s_tts_playing = false;

/* ------------------------------------------------------------------ */
/*  Generate RFC 4122 v4 UUID string                                  */
/* ------------------------------------------------------------------ */

static void generate_uuid(char *buf, size_t buf_size)
{
    uint8_t bytes[16];
    for (int i = 0; i < 16; i++) {
        bytes[i] = (uint8_t)esp_random();
    }
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    snprintf(buf, buf_size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

/* ------------------------------------------------------------------ */
/*  HTTP response context                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *pcm_buf;
    size_t  pcm_cap;
    size_t  pcm_len;
    char   *resp_buf;     /* raw response body (accumulated) */
    size_t  resp_len;
    size_t  resp_cap;
    bool    error;
} tts_http_ctx_t;

/* ------------------------------------------------------------------ */
/*  Extract all base64 audio fragments from the response body and
 *  decode them directly into the PCM accumulation buffer.
 *  Searches for every "data":"..." pattern (like reader_v2 does).      */
/* ------------------------------------------------------------------ */

static void process_audio_payload(tts_http_ctx_t *ctx)
{
    const char *cursor = ctx->resp_buf;
    size_t fragments = 0;

    if (cursor == NULL || *cursor == '\0') return;

    while (cursor && *cursor) {
        const char *b64_start = strstr(cursor, "\"data\":\"");
        if (!b64_start) break;
        b64_start += 8;  /* skip past "data":" */

        const char *b64_end = strchr(b64_start, '"');
        if (!b64_end || b64_end <= b64_start) break;

        size_t src_len = (size_t)(b64_end - b64_start);
        if (src_len == 0) { cursor = b64_end + 1; continue; }

        /* Query decoded size */
        size_t dst_len = 0;
        if (mbedtls_base64_decode(NULL, 0, &dst_len,
                (const uint8_t *)b64_start, src_len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
            break;
        }

        if (ctx->pcm_len + dst_len > ctx->pcm_cap) {
            ESP_LOGW(TAG, "PCM buffer full (%zu + %zu > %zu)",
                     ctx->pcm_len, dst_len, ctx->pcm_cap);
            break;
        }

        /* Decode base64 directly into PCM buffer */
        mbedtls_base64_decode(ctx->pcm_buf + ctx->pcm_len, dst_len, &dst_len,
            (const uint8_t *)b64_start, src_len);
        ctx->pcm_len += dst_len;
        fragments++;

        cursor = b64_end + 1;  /* continue searching for more fragments */
    }

    ESP_LOGI(TAG, "audio fragments=%d total_pcm=%zu bytes (%.1fs)",
             fragments, ctx->pcm_len,
             (float)(ctx->pcm_len / 2) / 24000.0f);
}

/* ------------------------------------------------------------------ */
/*  HTTP event handler — accumulates full response body               */
/* ------------------------------------------------------------------ */

static esp_err_t tts_http_event_handler(esp_http_client_event_t *evt)
{
    tts_http_ctx_t *ctx = (tts_http_ctx_t *)evt->user_data;
    if (ctx == NULL) return ESP_OK;

    switch (evt->event_id) {

    case HTTP_EVENT_ON_DATA:
        if (evt->data == NULL || evt->data_len == 0) break;
        /* Accumulate response body (may span multiple ON_DATA events) */
        if (ctx->resp_len + evt->data_len + 1 > ctx->resp_cap) {
            size_t new_cap = ctx->resp_cap ? ctx->resp_cap : 4096;
            while (new_cap < ctx->resp_len + evt->data_len + 1) new_cap *= 2;
            char *newbuf = heap_caps_realloc(ctx->resp_buf, new_cap,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!newbuf) {
                ctx->error = true;
                break;
            }
            ctx->resp_buf = newbuf;
            ctx->resp_cap = new_cap;
        }
        memcpy(ctx->resp_buf + ctx->resp_len, evt->data, evt->data_len);
        ctx->resp_len += evt->data_len;
        break;

    case HTTP_EVENT_ERROR:
        ctx->error = true;
        break;

    default:
        break;
    }

    return ESP_OK;
}

static size_t mono_to_stereo(int16_t *stereo, const int16_t *mono, size_t mono_samples)
{
    for (size_t i = 0; i < mono_samples; i++) {
        stereo[i * 2]     = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
    return mono_samples * 2;
}

/* ------------------------------------------------------------------ */
/*  Core TTS request + playback logic                                 */
/* ------------------------------------------------------------------ */

static void play_tts(const char *text,
                      const char *api_key,
                      const char *resource_id,
                      const char *speaker)
{
    esp_http_client_handle_t client = NULL;
    char uuid_str[40];
    cJSON *root = NULL;
    cJSON *req_params = NULL;
    cJSON *audio_params = NULL;
    char *json_str = NULL;
    esp_err_t err;

    /* Allocate PCM accumulation buffer (PSRAM) */
    uint8_t *pcm_buf = heap_caps_malloc(TTS_PCM_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t pcm_len = 0;

    if (pcm_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate TTS PCM buffer (%d bytes)", TTS_PCM_BUF_SIZE);
        return;
    }

    if (s_tts_playing) {
        ESP_LOGW(TAG, "TTS already playing, dropping request");
        heap_caps_free(pcm_buf);
        return;
    }
    s_tts_playing = true;

    /* ------------------------------------------------------------------ */
    /*  1. Build JSON request body                                        */
    /* ------------------------------------------------------------------ */
    generate_uuid(uuid_str, sizeof(uuid_str));

    root = cJSON_CreateObject();
    if (root == NULL) goto cleanup;

    cJSON_AddStringToObject(root, "request_id", uuid_str);
    cJSON_AddStringToObject(root, "namespace", "BidirectionalTTS");

    /* user */
    cJSON *user_obj = cJSON_AddObjectToObject(root, "user");
    if (user_obj) {
        cJSON_AddStringToObject(user_obj, "uid", "anonymous");
    }

    /* req_params */
    req_params = cJSON_AddObjectToObject(root, "req_params");
    if (req_params) {
        cJSON_AddStringToObject(req_params, "text", text);
        cJSON_AddStringToObject(req_params, "speaker", speaker);
        cJSON_AddStringToObject(req_params, "model", "seed-tts-2.0-standard");

        audio_params = cJSON_AddObjectToObject(req_params, "audio_params");
        if (audio_params) {
            cJSON_AddStringToObject(audio_params, "format", "pcm");
            cJSON_AddNumberToObject(audio_params, "sample_rate", 24000);
        }
    }

    json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    root = NULL;

    if (json_str == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
        goto cleanup;
    }

    ESP_LOGI(TAG, "TTS request: %zu bytes", strlen(json_str));

    /* ------------------------------------------------------------------ */
    /*  2. HTTP request — accumulate full JSON response body              */
    /* ------------------------------------------------------------------ */

    tts_http_ctx_t http_ctx = {
        .pcm_buf    = pcm_buf,
        .pcm_cap    = TTS_PCM_BUF_SIZE,
        .pcm_len    = 0,
        .resp_buf   = NULL,
        .resp_len   = 0,
        .resp_cap   = 0,
        .error      = false,
    };

    esp_http_client_config_t http_cfg = {
        .url            = TTS_API_URL,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = TTS_HTTP_TIMEOUT_MS,
        .buffer_size    = TTS_READ_BUF_SIZE,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler  = tts_http_event_handler,
        .user_data      = (void *)&http_ctx,
    };

    client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        goto cleanup;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream, application/json");
    esp_http_client_set_header(client, "X-Api-Key", api_key);
    esp_http_client_set_header(client, "X-Api-Resource-Id", resource_id);
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    qa_ui_add_log("[TTS] 正在合成语音...");
    qa_ui_set_status("语音合成中...");

    err = esp_http_client_perform(client);

    /* Process accumulated response body — extract all base64 audio fragments */
    if (err == ESP_OK && http_ctx.resp_buf && http_ctx.resp_len > 0) {
        http_ctx.resp_buf[http_ctx.resp_len] = '\0';
        process_audio_payload(&http_ctx);
        pcm_len = http_ctx.pcm_len;
    }

    if (err == ESP_OK) {
        long status = esp_http_client_get_status_code(client);
        if (status == 200 && pcm_len > 0) {
            ESP_LOGI(TAG, "HTTP %ld, audio=%zu bytes (%.1fs), error=%d",
                     status, pcm_len, (float)(pcm_len / 2) / 24000.0f,
                     http_ctx.error);
        } else if (status == 200 && pcm_len == 0) {
            ESP_LOGE(TAG, "TTS: no audio data in response");
            qa_ui_add_log("[ERR] TTS响应为空");
            qa_ui_set_status("TTS失败");
        } else {
            ESP_LOGE(TAG, "TTS HTTP error: %ld", status);
            qa_ui_add_log("[ERR] TTS错误(%ld)", status);
            qa_ui_set_status("TTS失败");
        }
    } else {
        ESP_LOGE(TAG, "TTS request failed: %s", esp_err_to_name(err));
        qa_ui_add_log("[ERR] TTS网络不可用");
        qa_ui_set_status("TTS失败");
    }

    /* Free accumulated response buffer */
    if (http_ctx.resp_buf) {
        heap_caps_free(http_ctx.resp_buf);
        http_ctx.resp_buf = NULL;
    }

    /* ------------------------------------------------------------------ */
    /*  4. Playback PCM via I2S                                          */
    /* ------------------------------------------------------------------ */

    if (pcm_len > 0) {
        /* PCM is 24000Hz mono 16-bit.
         * I2S is 24000Hz stereo.  Convert mono→stereo before writing. */
        size_t mono_samples = pcm_len / sizeof(int16_t);
        size_t stereo_samples = mono_samples * 2;

        /* Allocate stereo buffer (PSRAM — internal RAM is too fragmented) */
        int16_t *stereo_buf = heap_caps_malloc(stereo_samples * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stereo_buf) {
            mono_to_stereo(stereo_buf, (const int16_t *)pcm_buf, mono_samples);

            /* Enable speaker */
            es8388_set_mute(false);
            es8388_speaker_enable(true);

            qa_ui_set_status("播放中...");

            /* Write all stereo samples to I2S */
            size_t written = 0;
            esp_err_t werr = bsp_audio_write(stereo_buf, stereo_samples, &written, 5000);
            if (werr != ESP_OK) {
                ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(werr));
            } else {
                ESP_LOGI(TAG, "Played %zu stereo samples (%.1fs)",
                         written, (float)mono_samples / 24000.0f);
            }

            heap_caps_free(stereo_buf);

            /* Stop speaker */
            es8388_speaker_enable(false);
            es8388_set_mute(true);
        } else {
            ESP_LOGE(TAG, "Failed to allocate stereo buffer");
        }
    }

    qa_ui_add_log("[TTS] 播放完成");
    qa_ui_set_status("待命中 · 按住KEY3说话");

    /* ------------------------------------------------------------------ */
    /*  5. Cleanup                                                        */
    /* ------------------------------------------------------------------ */

cleanup:
    if (client) {
        esp_http_client_cleanup(client);
    }
    free(json_str);
    heap_caps_free(pcm_buf);
    s_tts_playing = false;
}

/* ------------------------------------------------------------------ */
/*  FreeRTOS task                                                     */
/* ------------------------------------------------------------------ */

static void volc_tts_task(void *pv_params)
{
    const config_t *cfg = (const config_t *)pv_params;

    const char *api_key = config_get_string(cfg, "ASR_API_KEY", NULL);
    if (api_key == NULL) {
        ESP_LOGE(TAG, "ASR_API_KEY not found (needed for TTS)");
        vTaskDelete(NULL);
        return;
    }

    const char *resource_id = config_get_string(cfg, "TTS_RESOURCE_ID",
                                                TTS_DEFAULT_RESOURCE_ID);
    const char *speaker = config_get_string(cfg, "TTS_VOICE",
                                            TTS_DEFAULT_SPEAKER);

    ESP_LOGI(TAG, "TTS task started");

    char text[TTS_TEXT_MAX];

    while (1) {
        if (xQueueReceive(s_tts_queue, text, portMAX_DELAY) == pdTRUE) {
            play_tts(text, api_key, resource_id, speaker);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

BaseType_t volc_tts_task_create(const config_t *cfg)
{
    s_tts_queue = xQueueCreate(TTS_QUEUE_LEN, TTS_TEXT_MAX);
    if (s_tts_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create TTS queue");
        return pdFAIL;
    }

    return xTaskCreate(volc_tts_task, "volc_tts", TTS_TASK_STACK,
                       (void *)cfg, TTS_TASK_PRIO, NULL);
}

esp_err_t volc_tts_speak(const char *text)
{
    if (s_tts_queue == NULL || text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_tts_queue, text, pdMS_TO_TICKS(100)) == pdTRUE) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "TTS queue full, dropping: %s", text);
    return ESP_ERR_TIMEOUT;
}
