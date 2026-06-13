#include "audio_player.h"
#include "board_pins.h"
#include "es8388.h"
#include "xl9555.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

static const char *TAG = "AUDIO";

/* Ring buffer for PCM data */
#define PCM_RING_SIZE       (32 * 1024)   /* 32KB */
#define I2S_DMA_DESC_NUM    8
#define I2S_DMA_FRAME_NUM   256

static i2s_chan_handle_t s_tx_handle = NULL;

static volatile audio_state_t s_state = AUDIO_IDLE;
static volatile int s_volume_db = -12;
static volatile bool s_stop_requested = false;

/* PCM ring buffer (single producer = HTTP, single consumer = I2S task) */
static uint8_t s_ring[PCM_RING_SIZE];
static volatile size_t s_ring_wpos = 0;
static volatile size_t s_ring_rpos = 0;
static volatile bool s_ring_eos = false;   /* end-of-stream flag */

static audio_stopped_cb_t s_on_stopped = NULL;

static TaskHandle_t s_playback_task = NULL;
static SemaphoreHandle_t s_ring_sem = NULL;

/* PI for tone generation */
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

void audio_player_feed_pcm(const uint8_t *data, size_t len)
{
    if (s_state != AUDIO_PLAYING || s_stop_requested) {
        return;
    }

    size_t written = 0;
    while (written < len) {
        size_t wpos = s_ring_wpos % PCM_RING_SIZE;
        size_t rpos = s_ring_rpos % PCM_RING_SIZE;

        /* Available write space (one byte margin to distinguish full vs empty) */
        size_t avail;
        if (wpos >= rpos) {
            avail = PCM_RING_SIZE - wpos + rpos - 1;
        } else {
            avail = rpos - wpos - 1;
        }
        if (avail == 0) {
            /* Ring full — wait for consumer */
            taskYIELD();
            continue;
        }

        size_t chunk = len - written;
        if (chunk > avail) chunk = avail;
        if (chunk > PCM_RING_SIZE - wpos) chunk = PCM_RING_SIZE - wpos;

        memcpy(s_ring + wpos, data + written, chunk);
        s_ring_wpos += chunk;
        written += chunk;
    }

    /* Notify consumer */
    xSemaphoreGive(s_ring_sem);
}

static size_t ring_read(uint8_t *buf, size_t len)
{
    size_t read = 0;
    while (read < len) {
        size_t wpos = s_ring_wpos % PCM_RING_SIZE;
        size_t rpos = s_ring_rpos % PCM_RING_SIZE;

        size_t avail;
        if (wpos >= rpos) {
            avail = wpos - rpos;
        } else {
            avail = PCM_RING_SIZE - rpos + wpos;
        }
        if (avail == 0) break;

        size_t chunk = len - read;
        if (chunk > avail) chunk = avail;
        if (chunk > PCM_RING_SIZE - rpos) chunk = PCM_RING_SIZE - rpos;

        memcpy(buf, s_ring + rpos, chunk);
        s_ring_rpos += chunk;
        read += chunk;
    }
    return read;
}

static void playback_task_func(void *arg)
{
    (void)arg;
    int16_t pcm_buf[256];           /* mono samples (512 bytes) */

    while (1) {
        if (s_state != AUDIO_PLAYING || s_stop_requested) {
            /* Wait for work */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /* Read PCM from ring buffer */
        size_t bytes = ring_read((uint8_t *)pcm_buf, sizeof(pcm_buf));
        if (bytes == 0) {
            if (s_ring_eos) {
                /* Stream finished */
                ESP_LOGI(TAG, "Stream playback complete");
                s_state = AUDIO_IDLE;
                es8388_stop_playback();
                if (s_on_stopped) {
                    audio_stopped_cb_t cb = s_on_stopped;
                    s_on_stopped = NULL;
                    cb();
                }
                continue;
            }
            /* Wait for more data */
            xSemaphoreTake(s_ring_sem, pdMS_TO_TICKS(100));
            continue;
        }

        /* I2S is configured as Philips, 16-bit, mono.
         * Write PCM data directly (16-bit mono samples). */
        size_t written = 0;
        i2s_channel_write(s_tx_handle, (uint8_t *)pcm_buf, bytes, &written, portMAX_DELAY);

        if (written < bytes) {
            ESP_LOGW(TAG, "I2S underrun: wrote %d / %d bytes", written, bytes);
        }
    }
}

esp_err_t audio_player_init(void)
{
    if (s_tx_handle != NULL) return ESP_OK;

    /* I2S TX channel */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = I2S_DMA_DESC_NUM,
        .dma_frame_num = I2S_DMA_FRAME_NUM,
        .auto_clear = true,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL), TAG, "i2s new channel");

    /* Default config (16kHz, 16bit, mono) — updated dynamically per stream */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S0_MCLK,
            .bclk = PIN_I2S0_BCLK,
            .ws = PIN_I2S0_LRCK,
            .dout = PIN_I2S0_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {false, false, false},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "i2s init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "i2s enable");

    /* Ring buffer semaphore */
    s_ring_sem = xSemaphoreCreateBinary();
    if (!s_ring_sem) return ESP_ERR_NO_MEM;

    /* Playback task */
    xTaskCreate(playback_task_func, "audio_play", 4096, NULL, 5, &s_playback_task);

    ESP_LOGI(TAG, "Audio player initialized (rate=%d, ring=%dKB)", AUDIO_SAMPLE_RATE, PCM_RING_SIZE / 1024);
    return ESP_OK;
}

esp_err_t audio_player_start_stream(uint32_t sample_rate, audio_stopped_cb_t on_stopped)
{
    if (s_state == AUDIO_PLAYING) {
        audio_player_stop();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Reset ring buffer */
    s_ring_wpos = 0;
    s_ring_rpos = 0;
    s_ring_eos = false;
    s_stop_requested = false;
    s_on_stopped = on_stopped;

    /* Reconfigure I2S if sample rate changed */
    if (sample_rate != AUDIO_SAMPLE_RATE) {
        i2s_channel_disable(s_tx_handle);
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = PIN_I2S0_MCLK,
                .bclk = PIN_I2S0_BCLK,
                .ws = PIN_I2S0_LRCK,
                .dout = PIN_I2S0_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {false, false, false},
            },
        };
        i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
        i2s_channel_enable(s_tx_handle);
    }

    s_state = AUDIO_PLAYING;
    es8388_set_volume(s_volume_db);
    es8388_start_playback();

    /* Wake playback task */
    if (s_playback_task) {
        xTaskNotifyGive(s_playback_task);
    }

    ESP_LOGI(TAG, "Stream start: rate=%d vol=%ddB", sample_rate, s_volume_db);
    return ESP_OK;
}

void audio_player_finish_stream(void)
{
    s_ring_eos = true;
    xSemaphoreGive(s_ring_sem);
}

void audio_player_pause(void)
{
    if (s_state != AUDIO_PLAYING) return;
    s_state = AUDIO_PAUSED;
    es8388_stop_playback();
    ESP_LOGI(TAG, "Paused");
}

void audio_player_resume(void)
{
    if (s_state != AUDIO_PAUSED) return;
    s_state = AUDIO_PLAYING;
    es8388_start_playback();
    if (s_playback_task) {
        xTaskNotifyGive(s_playback_task);
    }
    ESP_LOGI(TAG, "Resumed");
}

void audio_player_stop(void)
{
    if (s_state == AUDIO_IDLE) return;
    s_stop_requested = true;
    s_ring_eos = true;
    s_ring_wpos = 0;
    s_ring_rpos = 0;
    es8388_stop_playback();
    s_state = AUDIO_IDLE;
    ESP_LOGI(TAG, "Stopped");
}

esp_err_t audio_player_set_volume(int vol_db)
{
    s_volume_db = vol_db;
    if (s_state == AUDIO_PLAYING) {
        return es8388_set_volume(vol_db);
    }
    return ESP_OK;
}

int audio_player_get_volume(void)
{
    return s_volume_db;
}

audio_state_t audio_player_get_state(void)
{
    return s_state;
}

const char *audio_player_get_state_str(void)
{
    switch (s_state) {
        case AUDIO_IDLE:    return "idle";
        case AUDIO_PLAYING: return "playing";
        case AUDIO_PAUSED:  return "paused";
        case AUDIO_ERROR:   return "error";
    }
    return "unknown";
}

void audio_player_play_tone(uint16_t freq_hz, uint16_t duration_ms)
{
    audio_player_stop();
    vTaskDelay(pdMS_TO_TICKS(30));

    uint32_t sample_rate = AUDIO_SAMPLE_RATE;
    uint32_t total_samples = (sample_rate * duration_ms) / 1000;

    /* Configure I2S for 16kHz if not already */
    i2s_channel_disable(s_tx_handle);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S0_MCLK,
            .bclk = PIN_I2S0_BCLK,
            .ws = PIN_I2S0_LRCK,
            .dout = PIN_I2S0_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {false, false, false},
        },
    };
    i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    i2s_channel_enable(s_tx_handle);

    es8388_set_volume(s_volume_db);
    es8388_start_playback();

    ESP_LOGI(TAG, "Playing tone: %dHz %dms", freq_hz, duration_ms);

    int16_t sample[2];
    for (uint32_t i = 0; i < total_samples; i++) {
        int16_t v = (int16_t)(sinf((2.0f * M_PI * freq_hz * i) / sample_rate) * 12000);
        sample[0] = v;
        sample[1] = v;
        size_t written = 0;
        i2s_channel_write(s_tx_handle, sample, 4, &written, portMAX_DELAY);
    }

    es8388_stop_playback();
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Restore I2S config to default mode */
    i2s_channel_disable(s_tx_handle);
    i2s_std_config_t restore_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S0_MCLK,
            .bclk = PIN_I2S0_BCLK,
            .ws = PIN_I2S0_LRCK,
            .dout = PIN_I2S0_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {false, false, false},
        },
    };
    i2s_channel_init_std_mode(s_tx_handle, &restore_cfg);
    i2s_channel_enable(s_tx_handle);
    s_state = AUDIO_IDLE;
    ESP_LOGI(TAG, "Tone done");
}
