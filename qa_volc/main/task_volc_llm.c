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

#define LLM_TASK_STACK            8192
#define LLM_TASK_PRIO             5
#define LLM_QUEUE_LEN             4
#define LLM_QUESTION_MAX          1024
#define LLM_HTTP_TIMEOUT_MS       60000
#define LLM_READ_BUF_SIZE         256
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
    cJSON *delta;
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
    if (type == NULL || !cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }

    /* ---- response.output_text.delta ---- */
    if (strcmp(type->valuestring, "response.output_text.delta") == 0) {
        delta = cJSON_GetObjectItem(root, "delta");
        if (delta != NULL && cJSON_IsString(delta) && delta->valuestring != NULL) {
            dlen = strlen(delta->valuestring);
            if (dlen == 0) {
                cJSON_Delete(root);
                return;
            }

            /* Grow answer buffer if needed */
            if (*len + dlen + 1 > *cap) {
                /* Double until sufficient */
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
        }
    }
    /* ---- response.done ---- */
    else if (strcmp(type->valuestring, "response.done") == 0) {
        *done = true;
    }

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/*  Core LLM request logic                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Send a single question to the Responses API and stream the answer.
 *
 * Steps:
 *   1. Build JSON request body with model, stream=true, and input.
 *   2. Open HTTP connection, write request body, fetch response headers.
 *   3. Read the SSE stream incrementally via esp_http_client_read().
 *   4. Parse each SSE event and stream delta tokens to the UI.
 *   5. On completion or error, log the outcome and update status.
 */
static void process_question(const char *question,
                              const char *api_key,
                              const char *endpoint,
                              const char *model)
{
    esp_http_client_handle_t client = NULL;
    char *answer = NULL;
    size_t answer_len = 0;
    size_t answer_cap = 1024;
    bool stream_done = false;
    long http_status = 0;
    int written;
    char auth_header[512];

    cJSON *root           = NULL;
    cJSON *input_arr      = NULL;
    cJSON *msg_obj        = NULL;
    cJSON *content_arr    = NULL;
    cJSON *content_item   = NULL;
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

    /* "input": [ { "role": "user", "content": [ { "type": "input_text", "text": "..." } ] } ] */
    input_arr = cJSON_AddArrayToObject(root, "input");
    if (input_arr != NULL) {
        msg_obj = cJSON_CreateObject();
        if (msg_obj != NULL) {
            cJSON_AddStringToObject(msg_obj, "role", "user");

            content_arr = cJSON_AddArrayToObject(msg_obj, "content");
            if (content_arr != NULL) {
                content_item = cJSON_CreateObject();
                if (content_item != NULL) {
                    cJSON_AddStringToObject(content_item, "type", "input_text");
                    cJSON_AddStringToObject(content_item, "text", question);
                    cJSON_AddItemToArray(content_arr, content_item);
                    content_item = NULL;  /* ownership transferred */
                }
            }
            cJSON_AddItemToArray(input_arr, msg_obj);
            msg_obj = NULL;  /* ownership transferred */
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
    /*  2. Initialise HTTP client and send request                         */
    /* ------------------------------------------------------------------ */

    esp_http_client_config_t http_cfg = {
        .url            = endpoint,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = LLM_HTTP_TIMEOUT_MS,
        .buffer_size    = LLM_READ_BUF_SIZE,
        .buffer_size_tx = 512,
        .is_async       = false,
    };

    client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        qa_ui_add_log("[ERR] HTTP客户端初始化失败");
        free(json_str);
        free(answer);
        return;
    }

    /* Set headers */
    esp_http_client_set_header(client, "Content-Type", "application/json");
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);

    qa_ui_add_log("[LLM] 正在请求...");
    qa_ui_set_status("思考中...");

    /*
     * Open the connection.  esp_http_client_open() sends the request line
     * and headers and returns immediately.  We then write the body.
     */
    esp_err_t err = esp_http_client_open(client, strlen(json_str));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        qa_ui_add_log("[ERR] 网络不可用");
        qa_ui_set_status("网络不可用");
        goto cleanup;
    }

    written = esp_http_client_write(client, json_str, strlen(json_str));
    if (written < 0 || (size_t)written != strlen(json_str)) {
        ESP_LOGE(TAG, "HTTP write failed: written=%d", written);
        qa_ui_add_log("[ERR] 网络不可用");
        qa_ui_set_status("网络不可用");
        esp_http_client_close(client);
        goto cleanup;
    }

    /* Read response headers (may block until server starts responding) */
    esp_http_client_fetch_headers(client);
    http_status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP status: %ld", http_status);

    if (http_status != 200) {
        ESP_LOGE(TAG, "HTTP error: %ld", http_status);
        qa_ui_add_log("[ERR] LLM错误 (%ld)", http_status);
        qa_ui_set_status("LLM错误");
        esp_http_client_close(client);
        goto cleanup;
    }

    /* ------------------------------------------------------------------ */
    /*  3. SSE streaming read loop                                        */
    /* ------------------------------------------------------------------ */

    {
        char buf[LLM_READ_BUF_SIZE];
        int  read_len;

        sse_line_reset();
        qa_ui_set_status("回答中...");

        while (!stream_done) {
            read_len = esp_http_client_read(client, buf, sizeof(buf) - 1);
            if (read_len < 0) {
                ESP_LOGE(TAG, "HTTP read error: %d", read_len);
                qa_ui_add_log("[ERR] 网络不可用");
                qa_ui_set_status("网络不可用");
                break;
            }
            if (read_len == 0) {
                /* Connection closed by server or timeout; stop reading */
                ESP_LOGI(TAG, "HTTP read returned 0 (EOF or timeout)");
                break;
            }

            buf[read_len] = '\0';

            /* Character-by-character SSE line parsing */
            for (int i = 0; i < read_len; i++) {
                char c = buf[i];

                if (c == '\n') {
                    /* End of line: process if it is a "data: " line */
                    sse_line[sse_line_len] = '\0';

                    if (strncmp(sse_line, "data: ", 6) == 0) {
                        process_sse_data(sse_line + 6,
                                         &answer, &answer_len, &answer_cap,
                                         &stream_done);
                    }
                    /* Lines that do not start with "data: " (e.g. empty
                     * keep-alive lines, comments) are silently ignored. */

                    sse_line_reset();
                } else if (sse_line_len < LLM_SSE_LINE_MAX - 1) {
                    sse_line[sse_line_len++] = c;
                }
                /* else: line too long, silently drop excess characters */
            }
        }

        esp_http_client_close(client);
    }

    /* ------------------------------------------------------------------ */
    /*  4. Finalise                                                        */
    /* ------------------------------------------------------------------ */

    if (stream_done) {
        ESP_LOGI(TAG, "LLM answer complete (%zu bytes)", answer_len);
        qa_ui_add_log("[OK] 回答完成");
        qa_ui_set_status("回答完成");
    } else {
        if (answer_len > 0) {
            /* Partial answer received but stream terminated early */
            ESP_LOGW(TAG, "LLM stream ended before response.done (%zu bytes)",
                     answer_len);
            qa_ui_add_log("[WARN] 回答不完整");
            qa_ui_set_status("回答不完整");
        } else if (http_status == 200) {
            /* Connection opened and 200 returned, but no data arrived */
            ESP_LOGW(TAG, "LLM timeout (no SSE data received)");
            qa_ui_add_log("[ERR] LLM超时");
            qa_ui_set_status("LLM超时");
        }
        /* If http_status != 200, the error was already reported above */
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

    /* Validate required config keys */
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

    ESP_LOGI(TAG, "Volcengine LLM task started (endpoint=%s, model=%s)",
             endpoint, model);

    char question[LLM_QUESTION_MAX];

    while (1) {
        if (xQueueReceive(s_llm_queue, question, portMAX_DELAY) == pdTRUE) {
            process_question(question, api_key, endpoint, model);
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
