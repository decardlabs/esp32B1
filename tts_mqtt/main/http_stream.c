#include "http_stream.h"
#include "audio_player.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "HTTP_STREAM";

static TaskHandle_t s_fetch_task = NULL;
static volatile bool s_abort = false;

/* WAV header parsing */
bool wav_parse_header(const uint8_t *data, size_t len,
                      uint32_t *sample_rate, size_t *data_offset)
{
    if (len < 44) return false;
    if (memcmp(data, "RIFF", 4) != 0) return false;
    if (memcmp(data + 8, "WAVE", 4) != 0) return false;

    uint16_t audio_fmt = data[20] | (data[21] << 8);
    if (audio_fmt != 1) return false;      /* must be PCM */

    uint16_t channels = data[22] | (data[23] << 8);
    if (channels != 1) {
        ESP_LOGW(TAG, "WAV: %d channels (expecting mono)", channels);
    }

    *sample_rate = (uint32_t)(data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24));

    uint16_t bits = data[34] | (data[35] << 8);
    if (bits != 16) {
        ESP_LOGW(TAG, "WAV: %d bits (expecting 16)", bits);
    }

    /* Find "data" chunk */
    *data_offset = 12;
    while (*data_offset + 8 <= len) {
        uint32_t chunk_size = (uint32_t)(data[*data_offset + 4] |
                              (data[*data_offset + 5] << 8) |
                              (data[*data_offset + 6] << 16) |
                              (data[*data_offset + 7] << 24));
        if (memcmp(data + *data_offset, "data", 4) == 0) {
            *data_offset += 8;
            return true;
        }
        *data_offset += 8 + chunk_size;
    }
    return false;
}

static void fetch_task_func(void *arg)
{
    const char *url = (const char *)arg;
    s_abort = false;

    ESP_LOGI(TAG, "Fetching: %s", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        goto finish_err;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        goto finish_err;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "HTTP status=%d, content-length=%lld",
             esp_http_client_get_status_code(client), content_length);

    /* Read initial data for WAV header parsing */
    uint8_t header_buf[256];
    int header_read = esp_http_client_read(client, (char *)header_buf, sizeof(header_buf));
    if (header_read <= 0) {
        ESP_LOGE(TAG, "HTTP read failed (header)");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto finish_err;
    }

    uint32_t sample_rate = AUDIO_SAMPLE_RATE;
    size_t data_offset = 0;

    if (!wav_parse_header(header_buf, header_read, &sample_rate, &data_offset)) {
        ESP_LOGE(TAG, "Not a valid PCM WAV stream");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto finish_err;
    }

    ESP_LOGI(TAG, "WAV: rate=%dHz, data_offset=%d", sample_rate, data_offset);

    /* Start audio player */
    if (audio_player_start_stream(sample_rate, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Audio player start failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto finish_err;
    }

    /* Feed the bytes after the WAV header */
    if ((size_t)header_read > data_offset) {
        audio_player_feed_pcm(header_buf + data_offset, header_read - data_offset);
    }

    /* Stream remaining data */
    uint8_t buf[2048];
    int read_len;
    while (!s_abort) {
        read_len = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error: %d", read_len);
            break;
        }
        if (read_len == 0) {
            /* End of stream */
            break;
        }
        audio_player_feed_pcm(buf, read_len);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (s_abort) {
        ESP_LOGI(TAG, "Fetch aborted");
        audio_player_stop();
    } else {
        ESP_LOGI(TAG, "Fetch complete");
        audio_player_finish_stream();
    }

    s_fetch_task = NULL;
    vTaskDelete(NULL);
    return;

finish_err:
    audio_player_stop();
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t http_stream_start(const char *url,
                            void (*on_pcm)(const uint8_t *, size_t),
                            void (*on_finish)(const char *))
{
    if (s_fetch_task != NULL) {
        ESP_LOGW(TAG, "Fetch already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    /* Note: on_pcm and on_finish callbacks are provided via the audio_player
     * feed mechanism — audio_player_feed_pcm is called directly in the fetch task.
     * The function signatures are kept for API compatibility. */
    (void)on_pcm;
    (void)on_finish;

    size_t url_len = strlen(url) + 1;
    char *url_copy = malloc(url_len);
    if (!url_copy) return ESP_ERR_NO_MEM;
    memcpy(url_copy, url, url_len);

    xTaskCreate(fetch_task_func, "http_fetch", 8192, url_copy, 4, &s_fetch_task);
    return ESP_OK;
}

void http_stream_abort(void)
{
    s_abort = true;
}

bool http_stream_is_busy(void)
{
    return s_fetch_task != NULL;
}
