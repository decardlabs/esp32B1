/*
 * task_volc_llm.c - 火山引擎 Responses API SSE streaming 客户端
 *
 * Receives text questions via a queue, sends them to the Volcengine
 * Responses API with SSE streaming enabled, and displays the answer
 * tokens in real-time on the UI.  This is the last step in the pipeline
 * before returning to the IDLE state.
 *
 * API docs (verified via Python):
 *   POST <LLM_ENDPOINT>
 *   Authorization: Bearer <LLM_API_KEY>
 *   Body: { "model": "<LLM_MODEL>", "stream": true, "input": [...] }
 *   SSE events carry "response.output_text.delta" and "response.done".
 */

#include "task_volc_llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "task_qa_lvgl.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

static const char *TAG = "VOLC_LLM";

#define LLM_TASK_STACK            16384
#define LLM_TASK_PRIO             5
#define LLM_QUEUE_LEN             4
#define LLM_QUESTION_MAX          1024
#define LLM_HTTP_TIMEOUT_MS       60000
#define LLM_READ_BUF_SIZE         4096
#define LLM_SSE_LINE_MAX          4096

/* ------------------------------------------------------------------ */
/*  Message queue for question strings                                 */
/* ------------------------------------------------------------------ */

static QueueHandle_t s_llm_queue = NULL;

/* ------------------------------------------------------------------ */
/*  SSE line buffer (persistent across read chunks)                   */
/* ------------------------------------------------------------------ */

static char sse_line[LLM_SSE_LINE_MAX];
static int  sse_line_len = 0;

static void sse_line_reset(void)
{
    sse_line[0] = '\0';
    sse_line_len = 0;
}

/* ------------------------------------------------------------------ */
/*  Process a single SSE "data: ..." line                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Parse and handle one SSE data line (JSON after "data: " prefix).
 *
 * Recognised types:
 *   - response.output_text.delta : extract & stream the delta token
 *   - response.done              : mark the stream as finished
 *
 * @param[in]  data_str  JSON string (the part after "data: ")
 * @param[out] answer    Accumulated answer buffer (realloc'd on demand)
 * @param[in,out] len    Current length of @p answer
 * @param[in,out] cap    Current capacity of @p answer
 * @param[out] done      Set to true on "response.done"
 */
static void process_sse_data(const char *data_str,
                             char **answer, size_t *len, size_t *cap,
                             bool *done)
{
    cJSON *root;
    cJSON *type;
    cJSON *delta = NULL;
    size_t dlen;
    char *new_answer;

    if (data_str == NULL || *data_str == '\0') {
        return;
    }

    root = cJSON_Parse(data_str);
    if (root == NULL) {
        ESP_LOGW(TAG, "SSE JSON parse error: %s", data_str);
        return;
    }

    type = cJSON_GetObjectItem(root, "type");

    /* Check for Chat Completions format (no "type" field, uses "choices") */
    if (type == NULL || !cJSON_IsString(type) || type->valuestring == NULL) {
        /* Chat Completions SSE: {"choices":[{"delta":{"content":"..."},"index":0}]} */
        cJSON *cc_choices = cJSON_GetObjectItem(root, "choices");
        if (cJSON_IsArray(cc_choices) && cJSON_GetArraySize(cc_choices) > 0) {
            cJSON *first = cJSON_GetArrayItem(cc_choices, 0);
            if (first) {
                /* Check finish_reason == "stop" or "length" */
                cJSON *fr = cJSON_GetObjectItem(first, "finish_reason");
                if (cJSON_IsString(fr) && fr->valuestring != NULL
                    && (strcmp(fr->valuestring, "stop") == 0
                        || strcmp(fr->valuestring, "length") == 0)) {
                    *done = true;
                    cJSON_Delete(root);
                    return;
                }
                /* Extract delta.content */
                cJSON *delta_obj = cJSON_GetObjectItem(first, "delta");
                if (delta_obj) {
                    delta = cJSON_GetObjectItem(delta_obj, "content");
                }
            }
        }
        if (delta != NULL && cJSON_IsString(delta) && delta->valuestring != NULL) {
            dlen = strlen(delta->valuestring);
            if (dlen == 0) {
                cJSON_Delete(root);
                return;
            }
            goto append_delta;
        }
        cJSON_Delete(root);
        return;
    }

    /* ---- Responses API: response.output_text.delta ---- */
    if (strcmp(type->valuestring, "response.output_text.delta") == 0) {
        delta = cJSON_GetObjectItem(root, "delta");
        if (delta != NULL && cJSON_IsString(delta) && delta->valuestring != NULL) {
            dlen = strlen(delta->valuestring);
            if (dlen == 0) {
                cJSON_Delete(root);
                return;
            }
            goto append_delta;
        }
    }
    /* ---- Responses API: response.done ---- */
    else if (strcmp(type->valuestring, "response.done") == 0) {
        *done = true;
    }

    cJSON_Delete(root);
    return;

    /* Shared: append delta token to answer buffer */
append_delta:
    if (*len + dlen + 1 > *cap) {
        do {
            *cap *= 2;
        } while (*cap < *len + dlen + 1);

        new_answer = heap_caps_realloc(*answer, *cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (new_answer == NULL) {
            ESP_LOGE(TAG, "realloc answer failed (needed %zu)", *cap);
            cJSON_Delete(root);
            return;
        }
        *answer = new_answer;
    }

    memcpy(*answer + *len, delta->valuestring, dlen);
    *len += dlen;
    (*answer)[*len] = '\0';

    /* Stream the delta token to the UI in real-time */
    qa_ui_add_assistant_msg(delta->valuestring);

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/*  SSE context for event-driven HTTP                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *answer;          /* accumulated answer (realloc'd) */
    size_t len;             /* current length */
    size_t cap;             /* allocated capacity */
    bool   stream_done;     /* set on "response.done" */
    bool   error;           /* set on transport error */
} llm_sse_ctx_t;

/* ------------------------------------------------------------------ */
/*  Core LLM request logic                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief HTTP event handler — accumulates SSE response data.
 *
 * Called by esp_http_client_perform() for each chunk of the response body.
 * Reuses the existing process_sse_data() for SSE event parsing.
 */
static esp_err_t llm_http_event_handler(esp_http_client_event_t *evt)
{
    llm_sse_ctx_t *ctx = (llm_sse_ctx_t *)evt->user_data;
    if (ctx == NULL) return ESP_OK;

    switch (evt->event_id) {

    case HTTP_EVENT_ON_DATA:
        if (evt->data == NULL || evt->data_len == 0) break;

        for (int i = 0; i < evt->data_len; i++) {
            char c = ((const char *)evt->data)[i];

            if (c == '\n') {
                sse_line[sse_line_len] = '\0';

                if (strncmp(sse_line, "data: ", 6) == 0) {
                    process_sse_data(sse_line + 6,
                                     &ctx->answer, &ctx->len, &ctx->cap,
                                     &ctx->stream_done);
                }

                sse_line_reset();
            } else if (sse_line_len < LLM_SSE_LINE_MAX - 1) {
                sse_line[sse_line_len++] = c;
            }
        }
        break;

    case HTTP_EVENT_ON_FINISH:
        if (!ctx->stream_done) {
            ESP_LOGW(TAG, "SSE stream finished without response.done");
        }
        break;

    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTP transport error during SSE read");
        ctx->error = true;
        break;

    default:
        break;
    }

    return ESP_OK;
}

/**
 * @brief Send a single question to the Responses API and stream the answer.
 *
 * Steps:
 *   1. Build JSON request body with model, stream=true, and input.
 *   2. POST via esp_http_client_perform() with SSE event handler.
 *   3. Parse SSE events and stream delta tokens to the UI.
 *   4. On completion or error, log the outcome and update status.
 */
static void process_question(const char *question,
                              const char *api_key,
                              const char *endpoint,
                              const char *model)
{
    esp_http_client_handle_t client = NULL;
    char *answer = NULL;
    size_t answer_cap = 1024;
    long http_status = 0;
    char auth_header[512];
    llm_sse_ctx_t sse_ctx;

    cJSON *root           = NULL;
    char  *json_str       = NULL;

    /* ------------------------------------------------------------------ */
    /*  1. Build JSON request body                                         */
    /* ------------------------------------------------------------------ */

    root = cJSON_CreateObject();
    if (root == NULL) {
        qa_ui_add_log("[ERR] JSON创建失败");
        return;
    }

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddNumberToObject(root, "max_tokens", 512);

    /* Chat Completions format: {"messages":[{"role":"user","content":"..."}]} */
    cJSON *msgs_arr = cJSON_AddArrayToObject(root, "messages");
    if (msgs_arr != NULL) {
        cJSON *msg_obj = cJSON_CreateObject();
        if (msg_obj != NULL) {
            cJSON_AddStringToObject(msg_obj, "role", "user");
            cJSON_AddStringToObject(msg_obj, "content", question);
            cJSON_AddItemToArray(msgs_arr, msg_obj);
        }
    }

    json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    root = NULL;

    if (json_str == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
        qa_ui_add_log("[ERR] JSON序列化失败");
        return;
    }

    ESP_LOGI(TAG, "Request body: %zu bytes", strlen(json_str));

    /* Allocate answer accumulation buffer */
    answer = heap_caps_malloc(answer_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (answer == NULL) {
        ESP_LOGE(TAG, "malloc answer buffer failed");
        qa_ui_add_log("[ERR] 内存不足");
        free(json_str);
        return;
    }
    answer[0] = '\0';

    /* ------------------------------------------------------------------ */
    /*  2. Initialise SSE context and HTTP client                          */
    /* ------------------------------------------------------------------ */

    memset(&sse_ctx, 0, sizeof(sse_ctx));
    sse_ctx.answer = answer;
    sse_ctx.cap    = answer_cap;

    esp_http_client_config_t http_cfg = {
        .url            = endpoint,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = LLM_HTTP_TIMEOUT_MS,
        .buffer_size    = LLM_READ_BUF_SIZE,
        .buffer_size_tx = LLM_READ_BUF_SIZE,
        .skip_cert_common_name_check = true,
        .keep_alive_enable = false,
        .event_handler  = llm_http_event_handler,
        .user_data      = (void *)&sse_ctx,
    };

    client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        qa_ui_add_log("[ERR] HTTP客户端初始化失败");
        free(json_str);
        free(answer);
        return;
    }

    /* Set headers and post body */
    esp_http_client_set_header(client, "Content-Type", "application/json");
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    /* ------------------------------------------------------------------ */
    /*  3. Perform HTTP request (TLS handshake + write + SSE streaming)    */
    /* ------------------------------------------------------------------ */

    qa_ui_add_log("[LLM] 正在请求...");
    qa_ui_set_status("思考中...");
    sse_line_reset();

    esp_err_t err = esp_http_client_perform(client);

    /* After perform, the SSE stream is fully consumed */
    http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %ld, err=%s", http_status, esp_err_to_name(err));

    /* ------------------------------------------------------------------ */
    /*  4. Check result                                                    */
    /* ------------------------------------------------------------------ */

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        qa_ui_add_log("[ERR] 网络不可用");
        qa_ui_set_status("网络不可用");
        goto cleanup;
    }

    if (http_status != 200) {
        ESP_LOGE(TAG, "HTTP error: %ld", http_status);
        qa_ui_add_log("[ERR] LLM错误 (%ld)", http_status);
        qa_ui_set_status("LLM错误");
        goto cleanup;
    }

    /* Update answer pointer (may have been realloc'd by process_sse_data) */
    answer = sse_ctx.answer;
    answer_cap = sse_ctx.cap;

    if (sse_ctx.stream_done) {
        ESP_LOGI(TAG, "LLM answer complete (%zu bytes)", sse_ctx.len);
        qa_ui_add_log("[OK] 回答完成");
        qa_ui_set_status("回答完成");
    } else if (sse_ctx.error) {
        ESP_LOGW(TAG, "LLM stream interrupted by transport error (got %zu bytes before drop)",
                 sse_ctx.len);
        if (sse_ctx.len > 0) {
            qa_ui_add_log("[WARN] 网络中断，回答不完整");
        } else {
            qa_ui_add_log("[ERR] 网络不可用");
        }
        qa_ui_set_status(sse_ctx.len > 0 ? "回答不完整" : "网络不可用");
    } else if (sse_ctx.len > 0) {
        ESP_LOGW(TAG, "LLM stream ended without stop event (%zu bytes received)",
                 sse_ctx.len);
        qa_ui_add_log("[OK] 回答完成");
        qa_ui_set_status("回答完成");
    } else {
        ESP_LOGW(TAG, "LLM: no SSE data received (server returned HTTP %ld)",
                 http_status);
        qa_ui_add_log("[ERR] LLM超时");
        qa_ui_set_status("LLM超时");
    }

    /* ------------------------------------------------------------------ */
    /*  5. Cleanup                                                         */
    /* ------------------------------------------------------------------ */

cleanup:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    free(json_str);
    free(answer);
}

/* ------------------------------------------------------------------ */
/*  FreeRTOS task entry point                                         */
/* ------------------------------------------------------------------ */

static void volc_llm_task(void *pv_params)
{
    const config_t *cfg = (const config_t *)pv_params;

    /* Read endpoint.  Supports both full URL (LLM_ENDPOINT) and
     * base URL (LLM_BASE_URL, appended with /v1/chat/completions). */
    const char *endpoint = config_get_string(cfg, "LLM_ENDPOINT", NULL);
    if (endpoint == NULL) {
        ESP_LOGE(TAG, "LLM_ENDPOINT not found in config");
        qa_ui_add_log("[ERR] LLM_ENDPOINT 未配置");
        vTaskDelete(NULL);
        return;
    }

    const char *model = config_get_string(cfg, "LLM_MODEL", NULL);
    if (model == NULL) {
        ESP_LOGE(TAG, "LLM_MODEL not found in config");
        qa_ui_add_log("[ERR] LLM_MODEL 未配置");
        vTaskDelete(NULL);
        return;
    }

    const char *api_key = config_get_string(cfg, "LLM_API_KEY", NULL);
    if (api_key == NULL) {
        ESP_LOGE(TAG, "LLM_API_KEY not found in config");
        qa_ui_add_log("[ERR] LLM_API_KEY 未配置");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "LLM task started (endpoint=%s, model=%s)",
             endpoint, model);
    const char *ep = endpoint;

    char question[LLM_QUESTION_MAX];

    while (1) {
        if (xQueueReceive(s_llm_queue, question, portMAX_DELAY) == pdTRUE) {
            process_question(question, api_key, ep, model);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

BaseType_t volc_llm_task_create(const config_t *cfg)
{
    s_llm_queue = xQueueCreate(LLM_QUEUE_LEN, LLM_QUESTION_MAX);
    if (s_llm_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LLM queue");
        return pdFAIL;
    }

    return xTaskCreate(volc_llm_task, "volc_llm", LLM_TASK_STACK,
                       (void *)cfg, LLM_TASK_PRIO, NULL);
}

esp_err_t volc_llm_submit(const char *question)
{
    if (s_llm_queue == NULL || question == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_llm_queue, question, pdMS_TO_TICKS(100)) == pdTRUE) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "LLM queue full, dropping question");
    return ESP_ERR_TIMEOUT;
}
