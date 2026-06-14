
# qa_volc 稳定性修复计划

> 依据 `ESP32-S3稳定运行审查规范.md` 审查结论制定。

---

### Task 1: 添加运行时内存监控日志 (C1)

**Files:** `main/main.c`

在关键节点插入内存快照日志。定义统一宏，在所有状态切换点调用。

**步骤：**

- [ ] Step 1: 在 main.c 头部添加辅助函数

```c
#include "esp_heap_caps.h"

static void log_mem_snapshot(const char *stage) {
    ESP_LOGI("MEM", "stage=%s heap_free=%d heap_min=%d"
             " int_free=%d int_largest=%d"
             " ps_free=%d ps_largest=%d",
             stage,
             esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
```

- [ ] Step 2: 在以下时机调用 `log_mem_snapshot()`

```c
// 启动完成 -> BSP init 后 (line ~65)
log_mem_snapshot("boot_done");

// Wi-Fi 连接成功后 (在 my_wifi_cb 中, connected=true 时)
log_mem_snapshot("wifi_connected");

// TF 卡挂载后 (line ~67)
log_mem_snapshot("sdcard_mounted");
```

- [ ] Step 3: 编译验证

```bash
idf.py build 2>&1 | grep -E 'error:|warning:' | head -10
```

Expected: 无编译错误。

- [ ] Step 4: 提交

```bash
git add main/main.c
git commit -m "fix: add runtime memory snapshot logging at key checkpoints"
```

---

### Task 2: 添加任务栈高水位监控 (C2)

**Files:** `main/main.c`

在所有 `xTaskCreate` 之后，延迟1秒打印栈高水位。

- [ ] Step 1: 在 main.c 的 `app_main()` 末尾（Wi-Fi 连接后、创建所有任务后），添加栈水位打印：

```c
static void log_stack_watermarks(void) {
    // 使用 extern 引用所有任务句柄，或通过任务名查找
    // 由于各任务句柄在各 .c 文件中是 static 的，最简单的方案是用任务名查找
    TaskHandle_t task_list[] = {
        xTaskGetHandle("qa_lvgl_task"),
        xTaskGetHandle("volc_asr"),
        xTaskGetHandle("volc_llm"),
        xTaskGetHandle("audio_capture"),
        xTaskGetHandle("sdcard_task"),
        xTaskGetHandle("ws2812_task"),
        xTaskGetHandle("timer_task"),
    };
    const char *task_names[] = {
        "lvgl", "asr", "llm", "audio", "sdcard", "ws2812", "timer",
    };
    for (int i = 0; i < 7; i++) {
        if (task_list[i] != NULL) {
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(task_list[i]);
            ESP_LOGI("STACK", "task=%-8s watermark=%u bytes (min=%u%%)",
                     task_names[i], watermark * sizeof(StackType_t),
                     (unsigned)(watermark * 100 / 4096)); // use 4096 as approximate divisor
        }
    }
}
```

- [ ] Step 2: 在所有任务创建后调用（如 `app_main` 末尾、`vTaskDelay(3000)` 后）：

```c
// 等待所有任务启动完成
vTaskDelay(pdMS_TO_TICKS(2000));
log_stack_watermarks();
log_mem_snapshot("stack_check");
```

- [ ] Step 3: 编译验证

```bash
idf.py build 2>&1 | grep -E 'error:|warning:' | head -10
```

Expected: 无编译错误。

- [ ] Step 4: 提交

```bash
git add main/main.c
git commit -m "fix: add task stack watermark logging"
```

---

### Task 3: 添加 OOM 降级策略 (C3)

**Files:** `main/main.c`, `main/task_qa_lvgl.c`, `main/task_qa_lvgl.h`

- [ ] Step 1: 在 `task_qa_lvgl.h` 中添加降级接口：

```c
/** 内存降级级别 */
typedef enum {
    QA_DEGRADE_NONE = 0,
    QA_DEGRADE_LVGL_BUF_HALF,
    QA_DEGRADE_NO_ANIM,
    QA_DEGRADE_MINIMAL,
} qa_degrade_level_t;

/** 执行降级，返回当前降级级别 */
qa_degrade_level_t qa_degrade_get_level(void);
esp_err_t qa_degrade_step_up(void);
void qa_degrade_reset(void);
```

- [ ] Step 2: 在 `task_qa_lvgl.c` 中实现降级逻辑：

```c
static qa_degrade_level_t s_degrade = QA_DEGRADE_NONE;

qa_degrade_level_t qa_degrade_get_level(void) { return s_degrade; }

esp_err_t qa_degrade_step_up(void) {
    switch (s_degrade) {
        case QA_DEGRADE_NONE:
            // Level 1: LVGL buffer half
            s_degrade = QA_DEGRADE_LVGL_BUF_HALF;
            lcd_lvgl_reserve_buffer_half(); // realloc buffer with half rows
            ESP_LOGW(TAG, "DEGRADE: LVGL buffer halved");
            break;
        case QA_DEGRADE_LVGL_BUF_HALF:
            // Level 2: no animations
            s_degrade = QA_DEGRADE_NO_ANIM;
            lv_anim_enable(false);
            ESP_LOGW(TAG, "DEGRADE: animations disabled");
            break;
        case QA_DEGRADE_NO_ANIM:
            // Level 3: clear cached messages
            s_degrade = QA_DEGRADE_MINIMAL;
            qa_ui_clear_all();
            qa_ui_add_log("[SYS] 内存不足，已清除历史");
            ESP_LOGW(TAG, "DEGRADE: history cleared");
            break;
        case QA_DEGRADE_MINIMAL:
            return ESP_FAIL; // can't degrade further
    }
    return ESP_OK;
}

void qa_degrade_reset(void) {
    s_degrade = QA_DEGRADE_NONE;
}
```

- [ ] Step 3: 在 `main.c` 中添加分配失败后的降级触发：

```c
// 在 log_mem_snapshot 或分配失败回调用
static void check_oom_and_degrade(void) {
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    bool oom = false;
    if (int_free < 30 * 1024) oom = true;          // 硬红线 30KB
    if (int_largest < 24 * 1024) oom = true;       // 连续块不足

    if (oom) {
        if (qa_degrade_step_up() != ESP_OK) {
            ESP_DRAM_LOGE(TAG, "OOM: cannot degrade further");
            qa_ui_add_log("[ERR] 内存不足，请重启设备");
        }
    }
}
```

- [ ] Step 4: 编译验证

```bash
idf.py build 2>&1 | grep -E 'error:|warning:' | head -10
```

Expected: 无编译错误。

- [ ] Step 5: 提交

```bash
git add main/main.c main/task_qa_lvgl.c main/task_qa_lvgl.h
git commit -m "fix: add OOM degradation strategy (LVGL buf -> no anim -> clear history)"
```

---

### Task 4: 修复裸 malloc → heap_caps_malloc (W1/W2)

**Files:** `main/task_volc_asr.c`, `main/task_volc_llm.c`

- [ ] Step 1: 在 `task_volc_asr.c` 中替换所有裸 `malloc()`/`realloc()`/`calloc()`：

```c
// 替换前:
uint8_t *wav_data = (uint8_t *)malloc(file_size);
// 替换后:
uint8_t *wav_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

受影响的位置：
- `task_volc_asr.c:300` — WAV 数据缓冲（最大 ~1MB）
- `task_volc_asr.c:333` — base64 编码缓冲（~1.3MB）
- `task_volc_asr.c:96` — HTTP 响应收集缓冲（初始 4KB，可保留内部 RAM，小缓冲）
- `task_volc_asr.c:125` — realloc HTTP 缓冲（可保留 `MALLOC_CAP_INTERNAL`，仍在合理范围）

- [ ] Step 2: 在 `task_volc_llm.c` 中替换所有裸 `malloc()`/`realloc()`：

受影响的位置：
- `task_volc_llm.c:232` — LLM 回答累积缓冲（可能很大，放 PSRAM）
- `task_volc_llm.c:124` — realloc 回答缓冲

```c
// 替换前:
*answer = malloc(answer_cap);
// 替换后:
*answer = heap_caps_malloc(answer_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
```

- [ ] Step 3: 在全局搜索确认没有遗漏：

```bash
grep -rn '\bmalloc\b\|\bcalloc\b\|\brealloc\b' main/ --include="*.c" | grep -v heap_caps | grep -v '#define'
```

Expected: 仅剩有明确注释说明的保留项（如小缓冲保留内部 RAM）。

- [ ] Step 4: 编译验证

```bash
idf.py build 2>&1 | grep -E 'error:|warning:' | head -10
```

Expected: 无编译错误。

- [ ] Step 5: 提交

```bash
git add main/task_volc_asr.c main/task_volc_llm.c
git commit -m "fix: use heap_caps_malloc with SPIRAM for large buffers"
```

---

### Task 5: 确认 PSRAM Kconfig 设置 (W3)

- [ ] Step 1: 检查当前 sdkconfig 中的 PSRAM 策略：

```bash
grep -i 'SPIRAM_USE' sdkconfig
```

Expected 输出应包含 `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`。

- [ ] Step 2: 如果不是，通过 sdkconfig.defaults 修正：

```bash
echo "CONFIG_SPIRAM_USE_CAPS_ALLOC=y" >> sdkconfig.defaults
rm -rf build sdkconfig
idf.py build 2>&1 | tail -3
```

- [ ] Step 3: 提交

```bash
git add sdkconfig.defaults
git commit -m "fix: enforce SPIRAM_USE_CAPS_ALLOC for memory stability"
```
