/*
 * task_volc_asr.c - 火山引擎 Flash ASR HTTP 客户端
 *
 * Receives WAV file paths via a queue, reads the file from the SD card,
 * base64-encodes the audio data, and sends it to the Volcengine Flash ASR
 * API for speech recognition.  The recognized text is displayed on the UI
 * and forwarded to the LLM task.
 */

#include "task_volc_asr.h"

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
#include "task_qa_lvgl.h"

/* ------------------------------------------------------------------ */
/*  Forward declarations                                              */
/* ------------------------------------------------------------------ */

/* Defined in task_volc_llm.c -- submits ASR result text to the LLM. */
extern esp_err_t volc_llm_submit(const char *text);

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

static const char *TAG = "VOLC_ASR";

#define ASR_API_URL               "https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash"
#define ASR_DEFAULT_RESOURCE_ID   "volc.seedasr.auc"

#define ASR_TASK_STACK            8192
#define ASR_TASK_PRIO             5
#define ASR_QUEUE_LEN             4
#define ASR_WAV_PATH_MAX          64
#define ASR_HTTP_TIMEOUT_MS       30000
#define ASR_BUF_SIZE              4096
#define ASR_MAX_FILE_SIZE         (10 * 1024 * 1024)   /* 10 MB limit */

/* ------------------------------------------------------------------ */
/*  Message queue for WAV file paths                                   */
/* ------------------------------------------------------------------ */

static QueueHandle_t s_asr_queue = NULL;

/* ------------------------------------------------------------------ */
/*  UUID generation (RFC 4122 v4 / random)                            */
/* ------------------------------------------------------------------ */

static void generate_uuid(char *buf, size_t buf_size)
{
    uint8_t bytes[16];

    for (int i = 0; i < 16; i++) {
        bytes[i] = (uint8_t)esp_random();
    }

    /* Set version 4 (0100 in nibble 1 of byte 7) */
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    /* Set variant 10xx (bits 6-7 of byte 9) */
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
/*  HTTP response / header context                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;                          /* response body (heap)       */
    size_t len;                          /* bytes used                */
    size_t cap;                          /* allocated capacity        */
    char api_status_code[24];            /* X-Api-Status-Code header  */
} asr_resp_ctx_t;

static esp_err_t resp_ctx_init(asr_resp_ctx_t *ctx)
{
    ctx->data = heap_caps_malloc(ASR_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ctx->data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->data[0] = '\0';
    ctx->len = 0;
    ctx->cap = ASR_BUF_SIZE;
    ctx->api_status_code[0] = '\0';
    return ESP_OK;
}

static void resp_ctx_cleanup(asr_resp_ctx_t *ctx)
{
    free(ctx->data);
    ctx->data = NULL;
    ctx->len = 0;
    ctx->cap = 0;
    ctx->api_status_code[0] = '\0';
}

static esp_err_t resp_ctx_append(asr_resp_ctx_t *ctx, const char *data, size_t data_len)
{
    size_t needed = ctx->len + data_len + 1;

    if (needed > ctx->cap) {
        size_t new_cap = ctx->cap * 2;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char *new_data = heap_caps_realloc(ctx->data, new_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (new_data == NULL) {
            return ESP_ERR_NO_MEM;
        }
        ctx->data = new_data;
        ctx->cap = new_cap;
    }

    memcpy(ctx->data + ctx->len, data, data_len);
    ctx->len += data_len;
    ctx->data[ctx->len] = '\0';
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  HTTP event handler                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    asr_resp_ctx_t *ctx = (asr_resp_ctx_t *)evt->user_data;

    switch (evt->event_id) {

    case HTTP_EVENT_ON_HEADER:
        if (ctx != NULL && evt->header_key != NULL && evt->header_value != NULL) {
            if (strcasecmp(evt->header_key, "X-Api-Status-Code") == 0) {
                strncpy(ctx->api_status_code, evt->header_value,
                        sizeof(ctx->api_status_code) - 1);
                ctx->api_status_code[sizeof(ctx->api_status_code) - 1] = '\0';
            }
        }
        break;

    case HTTP_EVENT_ON_DATA:
        if (ctx != NULL && evt->data != NULL && evt->data_len > 0) {
            resp_ctx_append(ctx, (const char *)evt->data, evt->data_len);
        }
        break;

    default:
        break;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Recursive ASR text extraction from response JSON                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Extract the recognized text from an ASR response JSON tree.
 *
 * Tries the following paths in order:
 *   1. result.text
 *   2. result.result.text
 *   3. result.utterances[0].text
 *   4. utterances[0].text
 *
 * @param[in] root  Parsed cJSON tree (not modified, not freed).
 * @return pointer into the cJSON tree, or NULL if not found.
 */
static const char *extract_asr_text(const cJSON *root)
{
    const cJSON *result;
    const cJSON *text;
    const cJSON *utterances;

    if (root == NULL) {
        return NULL;
    }

    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (result == NULL || !cJSON_IsObject(result)) {
        return NULL;
    }

    /* 1. result.text */
    text = cJSON_GetObjectItemCaseSensitive(result, "text");
    if (cJSON_IsString(text) && text->valuestring != NULL &&
        text->valuestring[0] != '\0') {
        return text->valuestring;
    }

    /* 2. result.result.text */
    const cJSON *sub_result = cJSON_GetObjectItemCaseSensitive(result, "result");
    if (cJSON_IsObject(sub_result)) {
        text = cJSON_GetObjectItemCaseSensitive(sub_result, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL &&
            text->valuestring[0] != '\0') {
            return text->valuestring;
        }
    }

    /* 3. result.utterances[0].text */
    utterances = cJSON_GetObjectItemCaseSensitive(result, "utterances");
    if (cJSON_IsArray(utterances) && cJSON_GetArraySize(utterances) > 0) {
        const cJSON *first = cJSON_GetArrayItem(utterances, 0);
        if (cJSON_IsObject(first)) {
            text = cJSON_GetObjectItemCaseSensitive(first, "text");
            if (cJSON_IsString(text) && text->valuestring != NULL &&
                text->valuestring[0] != '\0') {
                return text->valuestring;
            }
        }
    }

    /* 4. root.utterances[0].text */
    utterances = cJSON_GetObjectItemCaseSensitive(root, "utterances");
    if (cJSON_IsArray(utterances) && cJSON_GetArraySize(utterances) > 0) {
        const cJSON *first = cJSON_GetArrayItem(utterances, 0);
        if (cJSON_IsObject(first)) {
            text = cJSON_GetObjectItemCaseSensitive(first, "text");
            if (cJSON_IsString(text) && text->valuestring != NULL &&
                text->valuestring[0] != '\0') {
                return text->valuestring;
            }
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Core ASR request logic                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Process a single WAV file through the Volcengine Flash ASR API.
 *
 * Steps:
 *   1. Read the WAV file from the SD card.
 *   2. Base64-encode the entire file content.
 *   3. Build a JSON request payload.
 *   4. Send HTTP POST to the Volcengine ASR endpoint.
 *   5. Parse the response and extract the recognized text.
 *   6. Display the result on the UI and forward it to the LLM task.
 */
static void process_wav_file(const char *wav_path,
                             const char *api_key,
                             const char *resource_id)
{
    FILE *f = NULL;
    uint8_t *wav_data = NULL;
    uint8_t *b64_data = NULL;
    char uuid_str[40];
    cJSON *root = NULL;
    char *json_str = NULL;
    esp_http_client_handle_t client = NULL;
    asr_resp_ctx_t resp_ctx;
    long http_status = 0;
    char dur_buf[24] = "";

    /* ------------------------------------------------------------------ */
    /*  1. Read the WAV file                                              */
    /* ------------------------------------------------------------------ */
    f = fopen(wav_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "[ERR] 无法打开音频文件: %s", wav_path);
        qa_ui_add_log("[ERR] 无法打开音频文件");
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > ASR_MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "WAV file size invalid: %ld", file_size);
        qa_ui_add_log("[ERR] 音频文件大小异常");
        fclose(f);
        return;
    }

    wav_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (wav_data == NULL) {
        ESP_LOGE(TAG, "malloc(%ld) for WAV data failed", file_size);
        qa_ui_add_log("[ERR] 内存不足");
        fclose(f);
        return;
    }

    if (fread(wav_data, 1, file_size, f) != (size_t)file_size) {
        ESP_LOGE(TAG, "fread failed for %s", wav_path);
        qa_ui_add_log("[ERR] 读取音频文件失败");
        fclose(f);
        free(wav_data);
        return;
    }
    fclose(f);
    f = NULL;

    ESP_LOGI(TAG, "Read WAV: %s (%ld bytes)", wav_path, file_size);

    /* ------------------------------------------------------------------ */
    /*  2. Base64-encode the WAV data via mbedtls                         */
    /* ------------------------------------------------------------------ */
    size_t b64_len = 0;

    int mret = mbedtls_base64_encode(NULL, 0, &b64_len, wav_data, file_size);
    if (mret != 0 && mret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGE(TAG, "mbedtls_base64_encode (query) failed: %d", mret);
        qa_ui_add_log("[ERR] Base64编码失败");
        free(wav_data);
        return;
    }

    b64_data = (uint8_t *)heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64_data == NULL) {
        ESP_LOGE(TAG, "malloc(%zu) for base64 data failed", b64_len + 1);
        qa_ui_add_log("[ERR] 内存不足");
        free(wav_data);
        return;
    }

    mret = mbedtls_base64_encode(b64_data, b64_len, &b64_len, wav_data, file_size);
    if (mret != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_encode failed: %d", mret);
        qa_ui_add_log("[ERR] Base64编码失败");
        free(b64_data);
        free(wav_data);
        return;
    }
    b64_data[b64_len] = '\0';

    free(wav_data);
    wav_data = NULL;

    ESP_LOGI(TAG, "Base64 output: %zu bytes", b64_len);

    /* ------------------------------------------------------------------ */
    /*  3. Build JSON request payload                                     */
    /* ------------------------------------------------------------------ */
    generate_uuid(uuid_str, sizeof(uuid_str));

    root = cJSON_CreateObject();
    if (root == NULL) {
        qa_ui_add_log("[ERR] JSON创建失败");
        free(b64_data);
        return;
    }

    /* user */
    cJSON *user_obj = cJSON_AddObjectToObject(root, "user");
    if (user_obj) {
        cJSON_AddStringToObject(user_obj, "uid", "anonymous");
    }

    /* audio */
    cJSON *audio_obj = cJSON_AddObjectToObject(root, "audio");
    if (audio_obj) {
        cJSON_AddStringToObject(audio_obj, "data", (const char *)b64_data);
        cJSON_AddStringToObject(audio_obj, "format", "wav");
        cJSON_AddNumberToObject(audio_obj, "rate", 16000);
        cJSON_AddNumberToObject(audio_obj, "bits", 16);
        cJSON_AddNumberToObject(audio_obj, "channel", 1);
    }

    /* request */
    cJSON *request_obj = cJSON_AddObjectToObject(root, "request");
    if (request_obj) {
        cJSON_AddStringToObject(request_obj, "model_name", "bigmodel");
        cJSON_AddBoolToObject(request_obj, "enable_punc", true);
        cJSON_AddBoolToObject(request_obj, "enable_itn", true);
    }

    json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    root = NULL;

    if (json_str == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
        qa_ui_add_log("[ERR] JSON序列化失败");
        free(b64_data);
        return;
    }

    ESP_LOGI(TAG, "Request body: %zu bytes", strlen(json_str));

    /* ------------------------------------------------------------------ */
    /*  4. Send HTTP POST                                                 */
    /* ------------------------------------------------------------------ */

    /* Initialise the response context first so the event handler can use it */
    if (resp_ctx_init(&resp_ctx) != ESP_OK) {
        ESP_LOGE(TAG, "resp_ctx_init failed");
        qa_ui_add_log("[ERR] 内存不足");
        free(json_str);
        free(b64_data);
        return;
    }

    esp_http_client_config_t http_cfg = {
        .url            = ASR_API_URL,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = ASR_HTTP_TIMEOUT_MS,
        .buffer_size    = ASR_BUF_SIZE,
        .event_handler  = http_event_handler,
        .user_data      = (void *)&resp_ctx,
    };

    client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        qa_ui_add_log("[ERR] HTTP客户端初始化失败");
        resp_ctx_cleanup(&resp_ctx);
        free(json_str);
        free(b64_data);
        return;
    }

    /* Set headers */
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Api-Key", api_key);
    esp_http_client_set_header(client, "X-Api-Resource-Id", resource_id);
    esp_http_client_set_header(client, "X-Api-Request-Id", uuid_str);
    esp_http_client_set_header(client, "X-Api-Sequence", "-1");

    /* Set POST body */
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    qa_ui_add_log("[ASR] 正在识别...");
    qa_ui_set_status("语音识别中...");

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        qa_ui_add_log("[ERR] 网络不可用，请检查网络");
        qa_ui_set_status("网络不可用");
        esp_http_client_cleanup(client);
        resp_ctx_cleanup(&resp_ctx);
        free(json_str);
        free(b64_data);
        return;
    }

    http_status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP status: %ld, X-Api-Status-Code: %s",
             http_status,
             resp_ctx.api_status_code[0] ? resp_ctx.api_status_code : "(none)");

    /* ------------------------------------------------------------------ */
    /*  5. Parse response JSON                                            */
    /* ------------------------------------------------------------------ */
    if (http_status != 200) {
        ESP_LOGE(TAG, "HTTP error: %ld, body: %s",
                 http_status, resp_ctx.data);
        qa_ui_add_log("[ERR] 服务器返回错误 (%ld)", http_status);
        esp_http_client_cleanup(client);
        resp_ctx_cleanup(&resp_ctx);
        free(json_str);
        free(b64_data);
        return;
    }

    ESP_LOGI(TAG, "Response (%zu bytes): %s", resp_ctx.len, resp_ctx.data);

    root = cJSON_Parse(resp_ctx.data);
    if (root == NULL) {
        const char *err_ptr = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "JSON parse error: %s", err_ptr ? err_ptr : "unknown");
        qa_ui_add_log("[ERR] ASR响应解析失败");
        esp_http_client_cleanup(client);
        resp_ctx_cleanup(&resp_ctx);
        free(json_str);
        free(b64_data);
        return;
    }

    /* Check the API-level status code */
    if (resp_ctx.api_status_code[0] != '\0' &&
        strcmp(resp_ctx.api_status_code, "20000000") != 0) {
        const char *msg = "未知错误";
        cJSON *err_msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(err_msg) && err_msg->valuestring != NULL) {
            msg = err_msg->valuestring;
        }
        ESP_LOGE(TAG, "ASR API error: code=%s, msg=%s",
                 resp_ctx.api_status_code, msg);
        qa_ui_add_log("[ERR] ASR失败: %s", msg);
        cJSON_Delete(root);
        esp_http_client_cleanup(client);
        resp_ctx_cleanup(&resp_ctx);
        free(json_str);
        free(b64_data);
        return;
    }

    /* Extract recognised text */
    const char *result_text = extract_asr_text(root);
    if (result_text == NULL) {
        ESP_LOGE(TAG, "no result text in ASR response");
        qa_ui_add_log("[ERR] ASR失败: 未识别到文本");
        cJSON_Delete(root);
        esp_http_client_cleanup(client);
        resp_ctx_cleanup(&resp_ctx);
        free(json_str);
        free(b64_data);
        return;
    }

    ESP_LOGI(TAG, "ASR result: %s", result_text);

    /* Extract audio duration for logging */
    cJSON *audio_info = cJSON_GetObjectItemCaseSensitive(root, "audio_info");
    if (cJSON_IsObject(audio_info)) {
        cJSON *dur = cJSON_GetObjectItemCaseSensitive(audio_info, "duration");
        if (cJSON_IsNumber(dur)) {
            snprintf(dur_buf, sizeof(dur_buf), "(%.1fs)", dur->valuedouble / 1000.0);
        }
    }

    cJSON_Delete(root);
    root = NULL;

    /* ------------------------------------------------------------------ */
    /*  6. Display result and forward to LLM task                         */
    /* ------------------------------------------------------------------ */
    qa_ui_add_log("[ASR] 识别完成 %s", dur_buf);
    qa_ui_set_status("识别完成");
    qa_ui_add_user_msg(result_text);

    err = volc_llm_submit(result_text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "volc_llm_submit returned %s", esp_err_to_name(err));
    }

    /* ------------------------------------------------------------------ */
    /*  7. Cleanup                                                        */
    /* ------------------------------------------------------------------ */
    esp_http_client_cleanup(client);
    resp_ctx_cleanup(&resp_ctx);
    free(json_str);
    free(b64_data);
}

/* ------------------------------------------------------------------ */
/*  FreeRTOS task                                                     */
/* ------------------------------------------------------------------ */

static void volc_asr_task(void *pv_params)
{
    const config_t *cfg = (const config_t *)pv_params;

    const char *api_key = config_get_string(cfg, "ASR_API_KEY", NULL);
    if (api_key == NULL) {
        ESP_LOGE(TAG, "ASR_API_KEY not found in config");
        qa_ui_add_log("[ERR] ASR_API_KEY 未配置");
        vTaskDelete(NULL);
        return;
    }

    const char *resource_id = config_get_string(cfg, "ASR_RESOURCE_ID",
                                                ASR_DEFAULT_RESOURCE_ID);

    ESP_LOGI(TAG, "Volcengine ASR task started (resource_id=%s)", resource_id);

    char wav_path[ASR_WAV_PATH_MAX];

    while (1) {
        if (xQueueReceive(s_asr_queue, wav_path, portMAX_DELAY) == pdTRUE) {
            process_wav_file(wav_path, api_key, resource_id);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

BaseType_t volc_asr_task_create(const config_t *cfg)
{
    s_asr_queue = xQueueCreate(ASR_QUEUE_LEN, ASR_WAV_PATH_MAX);
    if (s_asr_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create ASR queue");
        return pdFAIL;
    }

    return xTaskCreate(volc_asr_task, "volc_asr", ASR_TASK_STACK,
                       (void *)cfg, ASR_TASK_PRIO, NULL);
}

esp_err_t volc_asr_submit(const char *wav_path)
{
    if (s_asr_queue == NULL || wav_path == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_asr_queue, wav_path, pdMS_TO_TICKS(100)) == pdTRUE) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "ASR queue full, dropping: %s", wav_path);
    return ESP_ERR_TIMEOUT;
}
