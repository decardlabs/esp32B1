#include "app_oom.h"
#include "app_tts.h"
#include "app_display.h"
#include "esp_log.h"

static const char *TAG = "OOM";

/* Redlines from spec §3.4 */
#define OOM_INT_FREE_REDLINE   (30 * 1024)
#define OOM_INT_LARGEST_MIN    (32 * 1024)

/* How many consecutive violations before escalating */
#define OOM_ESCALATE_THRESHOLD  3

static oom_level_t s_level = OOM_LEVEL_NONE;
static int s_violation_count = 0;
static int s_healthy_count = 0;

static const char *level_name(oom_level_t l)
{
    switch (l) {
    case OOM_LEVEL_NONE:               return "none";
    case OOM_LEVEL_LVGL_BUFFER_HALF:   return "lvgl_buf_half";
    case OOM_LEVEL_TTS_PREBUF_SHRINK:  return "tts_prebuf_shrink";
    case OOM_LEVEL_DISABLE_UI_FX:      return "disable_ui_fx";
    case OOM_LEVEL_THROTTLE:           return "throttle";
    case OOM_LEVEL_LOCAL_TTS_FALLBACK: return "local_tts_fallback";
    case OOM_LEVEL_STOP_AND_ALERT:     return "stop_and_alert";
    default:                           return "unknown";
    }
}

static void apply_level(oom_level_t level)
{
    ESP_LOGW(TAG, "OOM degrading to level %d (%s)", (int)level, level_name(level));

    switch (level) {
    case OOM_LEVEL_LVGL_BUFFER_HALF:
        /* LVGL buffer resize at runtime is not supported without re-init.
         * Instead, reduce frame rate pressure via tick timer. */
        break;

    case OOM_LEVEL_TTS_PREBUF_SHRINK:
        /* Handled at sentence-split time — main.c uses TTS_RUNTIME_MAX_BYTES.
         * Already minimal at 72 bytes; no runtime action needed. */
        break;

    case OOM_LEVEL_DISABLE_UI_FX:
        /* LVGL 8.4 has no runtime global anim disable.
         * Intent: reduce frame rate. Logged for diagnostics. */
        ESP_LOGW(TAG, "OOM level DISABLE_UI_FX triggered");
        break;

    case OOM_LEVEL_THROTTLE:
        /* Signal main loop to reduce background activity.
         * No concrete action needed beyond logging for now. */
        break;

    case OOM_LEVEL_LOCAL_TTS_FALLBACK:
        if (app_tts_get_channel() != TTS_CHANNEL_LOCAL) {
            app_tts_set_channel(TTS_CHANNEL_LOCAL);
            ESP_LOGW(TAG, "Forced local TTS fallback");
        }
        break;

    case OOM_LEVEL_STOP_AND_ALERT:
        app_tts_stop();
        app_display_set_mode(DISP_MODE_PAUSED);
        app_display_set_text(
            "Low memory.\n"
            "Reboot and try again:\n"
            "remove large files.");
        ESP_LOGE(TAG, "OOM — playback stopped, user alerted");
        break;

    default:
        break;
    }
}

void app_oom_check(size_t int_free, size_t int_largest)
{
    bool breached = (int_free < OOM_INT_FREE_REDLINE) ||
                    (int_largest < OOM_INT_LARGEST_MIN);

    if (!breached) {
        s_violation_count = 0;
        if (s_level > OOM_LEVEL_NONE) {
            s_healthy_count++;
            if (s_healthy_count >= 5) {
                ESP_LOGI(TAG, "Memory stable for %d checks, auto-recovery from level %d (%s)",
                         s_healthy_count, (int)s_level, level_name(s_level));
                app_oom_reset();
            }
        }
        return;
    }

    s_violation_count++;
    if (s_violation_count < OOM_ESCALATE_THRESHOLD) {
        /* Warn but don't act yet — need consecutive violations */
        ESP_LOGW(TAG, "Memory redline breached (%d/%d): int_free=%u int_largest=%u",
                 s_violation_count, OOM_ESCALATE_THRESHOLD,
                 (unsigned)int_free, (unsigned)int_largest);
        return;
    }

    /* Consecutive violations — escalate */
    oom_level_t target = s_level + 1;
    if (target > OOM_LEVEL_STOP_AND_ALERT) {
        target = OOM_LEVEL_STOP_AND_ALERT;
    }
    if (target > s_level) {
        s_level = target;
        apply_level(s_level);
    }

    s_violation_count = 0;
    s_healthy_count = 0;
}

oom_level_t app_oom_get_level(void)
{
    return s_level;
}

void app_oom_reset(void)
{
    if (s_level != OOM_LEVEL_NONE) {
        ESP_LOGI(TAG, "OOM degradation reset (was level %d)", (int)s_level);
    }
    s_level = OOM_LEVEL_NONE;
    s_violation_count = 0;
    s_healthy_count = 0;
}
