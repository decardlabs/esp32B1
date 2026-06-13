# Code Review Fix Plan

> 文档同步版本：v2.1.0（2026-06-13）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the critical and medium-severity issues identified in the comprehensive code review, focusing on task interlocking, memory management, and state machine correctness.

**Architecture:** Changes span four modules — `main.c` (state machine + sentence chaining), `app_tts.c` (local TTS worker + I2S handle), `app_cloud_tts.c` (cloud TTS playback), and `app_display.c` (cross-task display buffers). The core pattern is adding mutual exclusion via mutexes and atomics where shared state crosses task boundaries, without restructuring the existing architecture.

**Tech Stack:** ESP-IDF 5.5.1, FreeRTOS, ESP32-S3 single-core

**Priority Order:** Critical (crashes/silent failure) → Medium (data races/perf) → Low (cleanup)

---

### Task 1: Fix cloud TTS fallback breaking sentence chaining

**Files:**
- Modify: `main/main.c:528-532`

**Problem:** `speak_current_sentence_cloud()` falls back to `app_tts_speak()` (no callback) when cloud TTS fails. The next sentence never fires `on_tts_finished`, so playback silently stops after the fallback sentence.

- [ ] **Step 1: Change fallback to use callback version**

Replace `app_tts_speak(sentence)` with `app_tts_speak_cb(sentence, on_tts_finished)` at [main.c:531](main/main.c#L531):

```c
// In speak_current_sentence_cloud(), change:
        esp_err_t local_ret = app_tts_speak(sentence);
// to:
        esp_err_t local_ret = app_tts_speak_cb(sentence, on_tts_finished);
```

The rest of the fallback logic (setting `s_playing = false` on failure) remains unchanged since `speak_current_sentence_cloud` does not call `on_tts_finished` itself.

- [ ] **Step 2: Verify the edit**

```bash
grep -n 'app_tts_speak_cb' main/main.c
# Expected: appears in both speak_current_sentence() and speak_current_sentence_cloud()
```

---

### Task 2: Add I2S handle mutex for local/cloud TTS coexistence

**Files:**
- Modify: `main/app_tts.c`
- Modify: `main/app_tts.h`

**Problem:** Cloud TTS (`app_cloud_tts_speak`) gets the raw I2S handle from local TTS and writes to it without checking whether the local TTS worker is actively using it. Both tasks can call `i2s_channel_write()` on `I2S_NUM_0` simultaneously, corrupting audio output.

**Solution:** Add a mutex-protected I2S reservation API. Local TTS worker must release the I2S bus before cloud TTS can acquire it.

- [ ] **Step 1: Add I2S bus mutex and reserve/release API to app_tts.c**

Add at the end of [app_tts.c](main/app_tts.c) (before the I2S handle sharing section):

```c
/* ── I2S bus arbitration (shared with cloud TTS) ──────────────────── */

static SemaphoreHandle_t s_i2s_mutex = NULL;

esp_err_t app_tts_i2s_acquire(TickType_t timeout)
{
    if (!s_i2s_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_i2s_mutex, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void app_tts_i2s_release(void)
{
    if (s_i2s_mutex) {
        xSemaphoreGive(s_i2s_mutex);
    }
}

bool app_tts_i2s_is_acquired(void)
{
    if (!s_i2s_mutex) return false;
    return (uxSemaphoreGetCount(s_i2s_mutex) == 0);
}
```

- [ ] **Step 2: Init the mutex in app_tts_init()**

In `app_tts_init()`, after the I2S channel init (around [app_tts.c:539](main/app_tts.c#L539), before or after `i2s_channel_enable`):

```c
    s_i2s_mutex = xSemaphoreCreateMutex();
    if (!s_i2s_mutex) return ESP_ERR_NO_MEM;
    /* Start acquired — local TTS holds the I2S bus by default */
    app_tts_i2s_acquire(0);
```

- [ ] **Step 3: Hold mutex during local TTS playback in worker task**

In `tts_worker_task()`, the I2S writes happen inside the `while (!s_stop_requested)` loop at [app_tts.c:425-449](main/app_tts.c#L425-L449). The mutex is already held (acquired in init, never released by local TTS). No changes needed in the worker's inner loop — the mutex is held permanently by the local TTS path.

However, add release/acquire around the I2S write window so cloud TTS can only grab the bus between utterances. Wrap the playback section:

In `tts_worker_task()`, before the work processing loop starts (around line 342 after `s_speaking = true`):

```c
        s_speaking = true;
        s_stop_requested = false;

        /* Ensure I2S bus is held (should already be from init, but in case
         * cloud TTS grabbed it while this utterance was queued, wait for it). */
        app_tts_i2s_acquire(portMAX_DELAY);
```

And after playback completes (around line 476 `s_speaking = false`):

```c
        es8388_stop_playback();
        app_tts_i2s_release();  /* Allow cloud TTS to use I2S between utterances */
        s_speaking = false;
```

Wait, this is wrong — if we release after every utterance, cloud TTS could grab it while the next utterance is queued. Better approach: keep a separate mechanism.

Actually, simpler approach: Local TTS never releases the mutex explicitly. Cloud TTS must call `app_tts_stop()` first, then acquire the I2S mutex. Since `app_tts_stop()` waits for `s_speaking = false`, and the worker holds the mutex during playback, once the worker finishes and the callback chains the next sentence...

Hmm, this is getting complex. Let me think of a simpler approach.

**Simpler approach:** Add a `extern` function to check if local TTS is busy, and have cloud TTS call `app_tts_stop()` before using I2S. The mutex approach above is actually cleaner but adds complexity.

Let me go with the mutex approach, but with a clearer release point:

- Local TTS worker acquires the mutex when starting an utterance and holds it during playback
- Between utterances (in the idle wait for `xQueueReceive`), the worker releases the mutex
- Cloud TTS acquires the mutex before using I2S and releases when done
- `app_tts_stop()` ensures the worker is not actively playing before cloud TTS tries to acquire

- [ ] **Step 3 (revised): Hold mutex during playback, release during idle**

Modify `tts_worker_task()`:

After line 343 (`s_stop_requested = false`), acquire I2S:
```c
        s_speaking = true;
        s_stop_requested = false;
        app_tts_i2s_acquire(portMAX_DELAY);
```

Before line 477 (`s_speaking = false`), release I2S:
```c
        app_tts_i2s_release();
        es8388_stop_playback();
        s_speaking = false;
```

In the idle wait loop (around line 332-334), ensure I2S is released:
```c
        if (xQueueReceive(s_work_queue, &work, pdMS_TO_TICKS(5000)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }
        // When work arrives, I2S was released during idle. Acquire before playback.
```

- [ ] **Step 4: Update cloud TTS to acquire I2S before use**

Modify `app_cloud_tts_speak()` in [app_cloud_tts.c](main/app_cloud_tts.c) to acquire I2S before starting.

Around line 292-303:

```c
    /* Acquire I2S bus (blocks until local TTS releases it) */
    esp_err_t i2s_err = app_tts_i2s_acquire(pdMS_TO_TICKS(3000));
    if (i2s_err != ESP_OK) {
        ESP_LOGE(TAG, "I2S acquire timeout (local TTS still busy)");
        s_cloud_busy = false;
        if (s_done_cb) {
            cloud_tts_done_cb_t cb = s_done_cb;
            s_done_cb = NULL;
            cb();
        }
        return ESP_ERR_TIMEOUT;
    }

    /* Get I2S handle from local TTS (shared I2S_NUM_0) */
    s_tx_handle = (i2s_chan_handle_t)app_tts_get_i2s_handle();
```

And release after playback finishes — in `playback_task()` after the drain loop, around line 161-167:

```c
    es8388_stop_playback();
    app_tts_i2s_release();   /* <-- add this */
    s_cloud_busy = false;
```

Also release on the error/stop path in `app_cloud_tts_stop()`:

```c
void app_cloud_tts_stop(void)
{
    s_stop_requested = true;
    s_ring_done = true;
    s_cloud_busy = false;
    es8388_stop_playback();
    app_tts_i2s_release();   /* <-- add this */
}
```

- [ ] **Step 5: Declare the new functions in app_tts.h**

Add to [app_tts.h](main/app_tts.h) before the `I2S handle` section:

```c
/* I2S bus arbitration (shared with cloud TTS) */
esp_err_t app_tts_i2s_acquire(TickType_t timeout);
void app_tts_i2s_release(void);
bool app_tts_i2s_is_acquired(void);
```

---

### Task 3: Protect display buffers with mutex

**Files:**
- Modify: `main/app_display.c`

**Problem:** `s_text`, `s_mode`, `s_filename` are written from button-event task and TTS worker task, read from LVGL task. No synchronization → LVGL can read partially-written UTF-8 text.

- [ ] **Step 1: Add display mutex to app_display.c**

Add near the top of [app_display.c](main/app_display.c) after `#include` directives:

```c
#include "freertos/semphr.h"

static SemaphoreHandle_t s_display_mutex = NULL;
```

- [ ] **Step 2: Init mutex in app_display_init()**

In `app_display_init()`, right before the DMA buffer allocation (around line 354):

```c
    s_display_mutex = xSemaphoreCreateMutex();
    if (!s_display_mutex) {
        return ESP_ERR_NO_MEM;
    }
```

- [ ] **Step 3: Wrap write-side access in setter functions**

Modify `app_display_set_text()`:

```c
void app_display_set_text(const char *text)
{
    if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    strncpy(s_text, text ? text : "", sizeof(s_text) - 1);
    s_text[sizeof(s_text) - 1] = '\0';
    s_dirty = true;
    if (s_display_mutex) xSemaphoreGive(s_display_mutex);
}
```

Modify `app_display_set_mode()`:

```c
void app_display_set_mode(disp_mode_t mode)
{
    if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    s_mode = mode;
    s_dirty = true;
    if (s_display_mutex) xSemaphoreGive(s_display_mutex);
}
```

Modify `app_display_set_filename()`:

```c
void app_display_set_filename(const char *name)
{
    if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    strncpy(s_filename, name ? name : "", sizeof(s_filename) - 1);
    s_filename[sizeof(s_filename) - 1] = '\0';
    s_dirty = true;
    if (s_display_mutex) xSemaphoreGive(s_display_mutex);
}
```

Modify `app_display_set_progress()`:

```c
void app_display_set_progress(int current, int total)
{
    if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    s_progress_cur = current;
    s_progress_total = total;
    s_dirty = true;
    if (s_display_mutex) xSemaphoreGive(s_display_mutex);
}
```

- [ ] **Step 4: Wrap LVGL read-side in lvgl_task()**

In `lvgl_task()`, after `if (s_dirty)` and the `s_dirty = false` line, acquire mutex before reading the buffers:

```c
        if (s_dirty) {
            s_dirty = false;

            if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);

            lv_label_set_text(s_label_mode, mode_string(s_mode));

            if (s_filename[0]) {
                lv_label_set_text(s_label_file, s_filename);
                lv_obj_set_style_text_align(s_label_file, LV_TEXT_ALIGN_CENTER, 0);
            } else {
                lv_label_set_text(s_label_file, "");
            }

            if (s_text[0]) {
                sanitize_text_for_font(s_text, s_text_render, sizeof(s_text_render), &lv_font_xiaozhi_cn_16);
                lv_label_set_text(s_label_text, s_text_render);
            } else {
                lv_label_set_text(s_label_text, "(Empty file)");
            }

            if (s_progress_total > 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d / %d", s_progress_cur, s_progress_total);
                lv_label_set_text(s_label_progress, buf);
            } else {
                lv_label_set_text(s_label_progress, "");
            }

            if (s_display_mutex) xSemaphoreGive(s_display_mutex);

            /* WiFi indicator (extern ref, not protected by mutex) */
            {
                extern bool app_wifi_is_connected(void);
                lv_label_set_text(s_label_wifi, app_wifi_is_connected() ? "[WiFi]" : "");
            }
        }
```

---

### Task 4: Mark cross-task state flags as volatile

**Files:**
- Modify: `main/main.c`

**Problem:** `s_playing` in main.c is accessed from 3 task contexts (button-event, TTS worker callback, poll task) without `volatile`. Compiler may optimize reads into registers, causing stale values.

- [ ] **Step 1: Add volatile to s_playing**

At [main.c:91](main/main.c#L91):

```c
static volatile bool s_playing = false;
```

Also check `s_file_loaded` at [main.c:73](main/main.c#L73) — accessed from button-event task (read `do_skip`, `do_next_file`) and TTS callback (`speak_current_sentence`):

```c
static volatile bool s_file_loaded = false;
```

And `s_current_sentence` at line 86 — modified by button task (`do_skip`) and TTS callback (`on_tts_finished`):

```c
static volatile int s_current_sentence = 0;
```

Note: `s_sentence_offset` at line 87 is also cross-task but updating to volatile:

```c
static volatile size_t s_sentence_offset = 0;
```

---

### Task 5: Remove blocking delay from TTS completion callback

**Files:**
- Modify: `main/main.c:556`

**Problem:** `vTaskDelay(pdMS_TO_TICKS(30))` in `on_tts_finished()` adds 30ms between every sentence with no functional benefit. It blocks the TTS worker task unnecessarily.

- [ ] **Step 1: Remove the delay**

At [main.c:556](main/main.c#L556):

```c
// Remove this line:
    vTaskDelay(pdMS_TO_TICKS(30));
```

The comment says "Update display to follow reading progress" but `app_display_set_text()` and `app_display_set_mode()` are called before this line — the display data was already updated. The LVGL task (priority 5, higher than worker at priority 2) will preempt the worker to render as soon as the worker yields or blocks.

---

### Task 6: Relocate large file buffers to PSRAM

**Files:**
- Modify: `main/main.c`

**Problem:** `s_file_content` (64KB) and `s_file_raw` (64KB) are static globals in BSS, consuming 128KB of internal SRAM. On ESP32-S3 with 8MB PSRAM, these can be moved to PSRAM to free internal RAM for wifi/LVGL/DMA buffers.

- [ ] **Step 1: Replace static arrays with PSRAM pointers**

At [main.c:69-70](main/main.c#L69-L70):

```c
// Change from:
static char s_file_content[MAX_FILES_CONTENT] = {0};
static uint8_t s_file_raw[MAX_FILES_CONTENT] = {0};
// To:
static char *s_file_content = NULL;
static uint8_t *s_file_raw = NULL;
```

Remove the `{0}` initializers — these arrays will be heap-allocated at boot.

- [ ] **Step 2: Allocate from PSRAM at boot**

In `app_main()`, right before hardware init or after the first init step, add:

```c
    /* Allocate large file buffers from PSRAM to conserve internal SRAM */
    s_file_content = (char *)heap_caps_malloc(MAX_FILES_CONTENT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_file_raw = (uint8_t *)heap_caps_malloc(MAX_FILES_CONTENT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_file_content || !s_file_raw) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM file buffers");
        // Fallback to internal RAM
        if (!s_file_content) {
            s_file_content = (char *)malloc(MAX_FILES_CONTENT);
        }
        if (!s_file_raw) {
            s_file_raw = (uint8_t *)malloc(MAX_FILES_CONTENT);
        }
    }
    memset(s_file_content, 0, MAX_FILES_CONTENT);
    memset(s_file_raw, 0, MAX_FILES_CONTENT);
```

- [ ] **Step 3: Verify all uses work with pointers**

All existing uses of `s_file_content` and `s_file_raw` use `sizeof()` — e.g. `sizeof(s_file_content)` returns the array size for stack arrays but the pointer size for heap pointers. Need to use `MAX_FILES_CONTENT` instead.

Search and fix:

```bash
grep -n 'sizeof(s_file_content)\|sizeof(s_file_raw)' main/main.c
```

Replace `sizeof(s_file_content)` with `MAX_FILES_CONTENT` (or the appropriate literal). The key locations are:

- [main.c:334](main/main.c#L334) — `sizeof(s_file_content) - 1` → `MAX_FILES_CONTENT`
- [main.c:352-358](main/main.c#L352-L358) — `sizeof(s_file_content)` used via `normalize_text_to_utf8()` arg (already uses separate param)

---

### Task 7: Reduce poll task heartbeat logging frequency

**Files:**
- Modify: `main/main.c:957-959`

**Problem:** `"poll alive (N)"` logged every second — noisy in production. Reduce to every 60 seconds or remove.

- [ ] **Step 1: Reduce heartbeat frequency**

At [main.c:957-959](main/main.c#L957-L959):

```c
// Change from:
        if (++heartbeat % 10 == 0) {
            ESP_LOGI(TAG, "poll alive (%d)", heartbeat);
        }
// To:
        if (++heartbeat % 600 == 0) {
            ESP_LOGI(TAG, "poll alive (%d)", heartbeat);
        }
```

This reduces from every 1 second to every 60 seconds (600 × 100ms).

---

### Task 8: Fix O(n²) strlen in sanitize_text_for_font

**Files:**
- Modify: `main/app_display.c:100`

**Problem:** `strlen(&src[i])` is called every loop iteration, re-scanning from the current position to end-of-string. For an N-byte string, this is O(N²/2).

- [ ] **Step 1: Precompute string length**

At [app_display.c:98](main/app_display.c#L98), change:

```c
// Before:
    while (src[i] != '\0' && out + 1 < dst_size) {
        size_t consumed = 0;
        uint32_t cp = utf8_decode_one(&src[i], strlen(&src[i]), &consumed);
// After:
    size_t src_len = strlen(src);
    while (i < src_len && out + 1 < dst_size) {
        size_t consumed = 0;
        uint32_t cp = utf8_decode_one(&src[i], src_len - i, &consumed);
```

---

## Self-Review Checklist

1. **Spec coverage:**
   - Task 1 covers the cloud TTS fallback chain break (critical)
   - Task 2 covers I2S mutex for local/cloud TTS conflict (critical)
   - Task 3 covers display buffer data race (critical)
   - Task 4 covers cross-task volatile flags (medium)
   - Task 5 covers blocking delay in callback (medium)
   - Task 6 covers PSRAM relocation of file buffers (medium)
   - Task 7 covers poll task logging (low)
   - Task 8 covers O(n²) in display sanitize (low)
   - Not covered: es8388 `|=` error masking (low severity, cosmetic)
   - Not covered: config parser inefficiency (low severity, cosmetic)
   - Not covered: USB PHY handle leak (won't-fix in current design — USB never deinits)
   - Not covered: button scan err_count static (low, cosmetic)

2. **Placeholder scan:** All steps contain exact code or commands. No "TBD", "TODO", "implement later".

3. **Type consistency:** Function signatures referenced in later tasks match those defined in Task 2. `app_tts_i2s_acquire(TickType_t timeout)` is consistent across step 1 → 2 → 4 → 5.
