#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH         16
#define LV_COLOR_16_SWAP       0
#define LV_HOR_RES_MAX         320
#define LV_VER_RES_MAX         480
#define LV_DPI_DEFAULT         130

/* Use custom memory allocation from PSRAM */
#define LV_MEM_CUSTOM          1
#include <stdlib.h>
#include "esp_heap_caps.h"
#define lv_malloc(x)           heap_caps_malloc(x, MALLOC_CAP_SPIRAM)
#define lv_free(x)             heap_caps_free(x)
#define lv_realloc(p, s)       heap_caps_realloc(p, s, MALLOC_CAP_SPIRAM)

/* Tick: we'll call lv_tick_inc manually */
#define LV_TICK_CUSTOM         0

/* Enable essential widgets */
#define LV_USE_LABEL           1
#define LV_USE_BTN             1
#define LV_USE_CONT            1

/* Enable CJK font (SimSun 16px ~600KB flash) */
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_SIMSUN_16_CJK  1
#define LV_FONT_DEFAULT        &lv_font_montserrat_14

/* Performance */
#define LV_USE_PERF_MONITOR    0

/* Logging */
#define LV_USE_LOG             0

/* Animations */
#define LV_USE_ANIMATION       0

/* File system not needed */
#define LV_USE_FS_FATFS        0
#define LV_USE_FS_STDIO        0

/* Disp refresh */
#define LV_DISP_DEF_REFR_PERIOD 30

#endif /* LV_CONF_H */
