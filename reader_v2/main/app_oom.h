#ifndef APP_OOM_H
#define APP_OOM_H

#include <stddef.h>

/**
 * @file app_oom.h
 * OOM (Out-Of-Memory) degradation system — spec §9.
 *
 * Call app_oom_check() from mem_snapshot() after each sample.
 * The system tracks internal free heap and largest block, escalates
 * through degradation levels when redlines are breached.
 */

/* Degradation levels — each is more severe than the last */
typedef enum {
    OOM_LEVEL_NONE = 0,

    /* Level 1: shrink LVGL display buffer */
    OOM_LEVEL_LVGL_BUFFER_HALF,

    /* Level 2: reduce TTS pre-read / network buffer */
    OOM_LEVEL_TTS_PREBUF_SHRINK,

    /* Level 3: disable animations, trim UI */
    OOM_LEVEL_DISABLE_UI_FX,

    /* Level 4: delay new tasks, trim caches */
    OOM_LEVEL_THROTTLE,

    /* Level 5: force local TTS, release TLS connections */
    OOM_LEVEL_LOCAL_TTS_FALLBACK,

    /* Level 6: stop playback, show error, free everything */
    OOM_LEVEL_STOP_AND_ALERT,
} oom_level_t;

/**
 * @brief Check memory and escalate degradation if needed.
 *
 * Call this from every mem_snapshot() call.  Idempotent — safe to call
 * at high frequency; only acts when a redline is crossed.
 *
 * Redlines (from spec §3.4):
 *   - int_free < 30KB (hard redline)
 *   - int_largest < 32KB
 */
void app_oom_check(size_t int_free, size_t int_largest);

/**
 * @brief Get current active degradation level.
 */
oom_level_t app_oom_get_level(void);

/**
 * @brief Reset degradation (e.g. after mode switch / recovery).
 */
void app_oom_reset(void);

#endif /* APP_OOM_H */
