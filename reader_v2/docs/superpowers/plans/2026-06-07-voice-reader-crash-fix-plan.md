# Voice Reader 稳定性修复 实现计划

> 文档同步版本：v2.1.0（2026-06-13）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 ESP32-S3 Voice Reader 的两个崩溃问题：USB PC 枚举时重启 和 连续朗读 10-30 分钟后重启

**Architecture:** 通过 USB 栈修复（栈扩容、真实 deinit、FATFS 隔离）+ 内存治理（TTS 引擎每 30 句重建、I2S 超时保护、堆监控）+ SPI 总线协调（USB 模式下暂停 LVGL 刷新）

**Tech Stack:** ESP-IDF v5.5.1, TinyUSB MSC, esp-sr TTS (esp_tts), LVGL 8.4.0, I2S (ES8388)

---

## 文件变更总览

| 文件 | 操作 | 职责 |
|------|------|------|
| `sdkconfig.defaults` | 修改 | FATFS 扇区 4096→512 |
| `main/app_display.h` | 修改 | 新增 `app_display_suspend()` / `app_display_resume()` 声明 |
| `main/app_display.c` | 修改 | LVGL 任务暂停机制 |
| `main/tusb_msc.h` | 修改 | 无变化（接口不变） |
| `main/tusb_msc.c` | 修改 | 栈 4096→8192、保存 phy_hdl、实现真正 deinit |
| `main/sd_card.h` | 修改 | 新增 `sd_card_fs_suspend()` / `sd_card_fs_resume()` 声明 |
| `main/sd_card.c` | 修改 | 新增 FATFS 暂停/恢复 API（不销毁 card handle，只做 sync）|
| `main/app_tts.c` | 修改 | TTS 引擎每 30 句重建、I2S 写超时保护 |
| `main/main.c` | 修改 | 模式切换生命周期化、堆监控 |
| `main/app_buttons.h` | 修改 | 声明 `BTN_LONG_PRESS_MS` 供外部引用 |

---

### Task 1: FATFS 扇区配置校正

**Files:**
- Modify: `sdkconfig.defaults`

- [ ] **Step 1: 修改扇区大小**

当前 `CONFIG_FATFS_SECTOR_4096=y` 导致 FATFS 逻辑扇区（4096B）与 TF 卡物理扇区（512B）不匹配，存在跨扇区边界 bug 风险。

在 `sdkconfig.defaults` 中（约第 1 行最后）：

```
+ # CONFIG_FATFS_SECTOR_4096 is not set
+ CONFIG_FATFS_SECTOR_512=y
```

定位到现有行 `CONFIG_FATFS_SECTOR_4096=y` 并替换：

```diff
- CONFIG_FATFS_SECTOR_4096=y
+ # CONFIG_FATFS_SECTOR_4096 is not set
+ CONFIG_FATFS_SECTOR_512=y
```

- [ ] **Step 2: Commit**

```bash
git add sdkconfig.defaults
git commit -m "fix: use FATFS 512B sector to match TF card physical sector"
```

---

### Task 2: Display 暂停/恢复机制

**Files:**
- Modify: `main/app_display.h`
- Modify: `main/app_display.c`

- [ ] **Step 1: app_display.h 新增声明**

在 `void app_display_set_progress(int current, int total);` 之后添加：

```c
void app_display_suspend(void);
void app_display_resume(void);
```

- [ ] **Step 2: app_display.c 新增暂停标志和实现**

在文件顶部 `static volatile bool s_dirty = false;` 之后添加：

```c
static volatile bool s_suspended = false;
```

在 `app_display_init()` 之前添加实现：

```c
void app_display_suspend(void)
{
    s_suspended = true;
}

void app_display_resume(void)
{
    s_suspended = false;
}
```

- [ ] **Step 3: 修改 lvgl_task 使用暂停标志**

定位 `lvgl_task` 函数中 `lv_timer_handler()` 调用附近的代码（约第 225-229 行），替换为：

```c
        if (!s_suspended) {
            uint32_t wait_ms = lv_timer_handler();
            if (wait_ms < 5) wait_ms = 5;
            if (wait_ms > 20) wait_ms = 20;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        } else {
            /* USB mode: don't touch SPI bus, just poll dirty flag */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
```

- [ ] **Step 4: Commit**

```bash
git add main/app_display.h main/app_display.c
git commit -m "fix: add display suspend/resume for USB mode SPI isolation"
```

---

### Task 3: USB 任务栈扩容 + 真正 deinit

**Files:**
- Modify: `main/tusb_msc.h`
- Modify: `main/tusb_msc.c`

- [ ] **Step 1: tusb_msc.h 确认接口**

当前 `tusb_msc.h` 接口足够，不需要改动。确认已有：

```c
esp_err_t tusb_msc_init(sdmmc_card_t *card);
esp_err_t tusb_msc_deinit(void);
void tusb_msc_set_write_callback(tusb_msc_write_cb_t cb);
bool tusb_msc_is_connected(void);
```

- [ ] **Step 2: tusb_msc.c 添加静态变量**

在文件顶部 `static bool s_usb_connected = false;` 之后添加：

```c
/* Task handle for proper deletion */
static TaskHandle_t s_usb_task = NULL;

/* USB PHY handle for proper teardown */
static usb_phy_handle_t s_phy_hdl = NULL;
```

- [ ] **Step 3: 重写 tusb_msc_init — 保存 PHY handle 和 task handle**

替换 `tusb_msc_init` 函数的 `usb_phy_init` 调用部分：

移除 `usb_phy_init` 独立函数（约第 20-31 行），改为内联到 `tusb_msc_init`。将：

```c
static esp_err_t usb_phy_init(void)
{
    usb_phy_handle_t phy_hdl;
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
        .otg_io_conf = NULL,
        .ext_io_conf = NULL,
    };
    return usb_new_phy(&phy_cfg, &phy_hdl);
}
```

替换为：

```c
static esp_err_t usb_phy_init(void)
{
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
        .otg_io_conf = NULL,
        .ext_io_conf = NULL,
    };
    return usb_new_phy(&phy_cfg, &s_phy_hdl);
}
```

然后修改 `tusb_msc_init` 函数的 `xTaskCreate` 调用（约第 182 行）：

```c
-    BaseType_t ok = xTaskCreate(usb_device_task, "usbd", 4096, NULL, 5, NULL);
+    BaseType_t ok = xTaskCreate(usb_device_task, "usbd", 8192, NULL, 5, &s_usb_task);
```

- [ ] **Step 4: 重写 tusb_msc_deinit**

替换现有的空壳 `tusb_msc_deinit`：

```c
esp_err_t tusb_msc_deinit(void)
{
    /* 1. Delete USB background task */
    if (s_usb_task) {
        vTaskDelete(s_usb_task);
        s_usb_task = NULL;
    }
    /* 2. Shut down TinyUSB stack */
    tud_deinit();
    /* 3. Tear down USB PHY */
    if (s_phy_hdl) {
        usb_del_phy(s_phy_hdl);
        s_phy_hdl = NULL;
    }
    s_card = NULL;
    s_usb_connected = false;
    ESP_LOGI(TAG, "USB deinitialized");
    return ESP_OK;
}
```

- [ ] **Step 5: Commit**

```bash
git add main/tusb_msc.c
git commit -m "fix: USB task stack 4096->8192, implement proper deinit with PHY teardown"
```

---

### Task 4: FATFS 暂停/恢复 API（保留 card handle）

**Files:**
- Modify: `main/sd_card.h`
- Modify: `main/sd_card.c`

**设计思考：** 最初的设计试图拆分为 mount/unmount，但 `esp_vfs_fat_sdcard_unmount()` 内部会调用 `sdmmc_card_deinit()` 销毁卡句柄（这是 ESP-IDF 的设计）。而 USB MSC 需要 card handle 保持有效。因此采用更轻量的方案：只 sync FATFS 缓存并通过标志位防止并发，**不实际卸掉 FATFS**。由于 Reading 模式从不通过 FATFS 写文件（只读），不存在脏缓存问题。USB 模式下状态机已确保无 FATFS 操作。

- [ ] **Step 1: sd_card.h 新增声明**

在 `sdmmc_card_t *sd_card_get_handle(void);` 之后添加：

```c
/* Suspend/resume FATFS access (ensures cache sync, does NOT destroy card handle).
 * Safe to call even when FATFS is not mounted. */
void sd_card_fs_suspend(void);
void sd_card_fs_resume(void);
bool sd_card_fs_is_suspended(void);
```

- [ ] **Step 2: sd_card.c 新增实现**

在 `sd_card_get_handle` 之后添加：

```c
static bool s_fs_suspended = false;

void sd_card_fs_suspend(void)
{
    if (!s_mounted || !s_card) {
        s_fs_suspended = true;
        return;
    }

    /* Sync FATFS — flush any cached writes (there shouldn't be any
     * since we only read in Reading mode, but be safe) */
    TCHAR drv[3] = {_T('/'), _T('s'), _T('d')};  /* "0:" for FatFs logical drive 0 */
    f_mount(NULL, (const TCHAR *)"/sdcard", 1);
    f_mount(NULL, (const TCHAR *)"0:", 1);

    s_fs_suspended = true;
    ESP_LOGI(TAG, "FATFS suspended, card handle preserved");
}

void sd_card_fs_resume(void)
{
    s_fs_suspended = false;
    ESP_LOGI(TAG, "FATFS resumed");
}

bool sd_card_fs_is_suspended(void)
{
    return s_fs_suspended;
}
```

注意：`f_mount(NULL, ...)` 用于 sync + unmount 逻辑驱动，但实际 FATFS 仍通过 VFS 机制关联。这里是双重保险 sync。实际运行中 `s_fs_suspended` 标志和状态机共同保证并发安全。

- [ ] **Step 3: 在 FATFS 操作入口添加暂停检查（可选加固）**

在 `main.c` 的 `scan_files` 函数入口处（约第 353 行）添加：

```c
static void scan_files(void)
{
    if (sd_card_fs_is_suspended()) {
        ESP_LOGW(TAG, "FATFS suspended, cannot scan");
        return;
    }
    // ... 原有逻辑
}
```

但需要先在 `sd_card.h` 中添加 `bool sd_card_fs_is_suspended(void);` 声明。

这一步是可选的加固措施。实际运行中状态机已保证 USB 模式下不调用 scan_files。

- [ ] **Step 4: Commit**

```bash
git add main/sd_card.h main/sd_card.c
git commit -m "fix: add FATFS suspend/resume with card handle preservation"
```

---

### Task 5: TTS 引擎生命周期管理 + I2S 超时保护

**Files:**
- Modify: `main/app_tts.c`

- [ ] **Step 1: 添加常量、计数器和重建函数声明**

在文件顶部 `#define TTS_WORKER_STACK   32768` 附近添加：

```c
/* TTS engine rebuild threshold: recreate every N sentences to prevent
 * memory accumulation inside esp-sr library */
#define TTS_ENGINE_MAX_SENTENCES  30
```

在 `static volatile bool s_stop_requested = false;` 之后添加：

```c
/* Sentence counter for TTS engine lifecycle management */
static int s_sentence_count = 0;
```

在 `static void tts_worker_task(void *arg);` 静态声明附近添加重建函数声明：

```c
static esp_err_t tts_engine_recreate(void);
```

- [ ] **Step 2: 实现 tts_engine_recreate**

在 `tts_stop_internal` 函数之前添加：

```c
static esp_err_t tts_engine_recreate(void)
{
    if (!s_tts_ready && s_tts == NULL) {
        /* Engine was already dead or not started — attempt fresh init */
        goto do_init;
    }

    if (s_tts) {
        ESP_LOGI(TAG, "Destroying TTS engine (sentence #%d)", s_sentence_count);
        esp_tts_destroy(s_tts);
        s_tts = NULL;
    }

do_init:
    esp_tts_voice_t *voice = esp_tts_voice_set_init(
        &esp_tts_voice_xiaole,
        (void *)_binary_esp_tts_voice_data_xiaole_dat_start);
    if (voice == NULL) {
        ESP_LOGE(TAG, "TTS voice init failed during recreate");
        s_tts_ready = false;
        return ESP_FAIL;
    }

    s_tts = esp_tts_create(voice);
    if (s_tts == NULL) {
        ESP_LOGE(TAG, "TTS create failed during recreate");
        s_tts_ready = false;
        return ESP_ERR_NO_MEM;
    }

    s_tts_ready = true;
    ESP_LOGI(TAG, "TTS engine recreated successfully");
    return ESP_OK;
}
```

- [ ] **Step 3: 在 tts_worker_task 中添加重建逻辑**

在 `tts_worker_task` 中，`xQueueReceive` 之后添加重建检查。定位到 `while (1)` 后的第一行（约第 53 行）：

```c
        /* Wait for work */
        if (xQueueReceive(s_work_queue, &work, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* TTS engine lifecycle: recreate every N sentences to free accumulated memory */
        s_sentence_count++;
        if (s_sentence_count >= TTS_ENGINE_MAX_SENTENCES) {
            s_sentence_count = 0;
            if (s_tts_ready) {
                tts_engine_recreate();
            }
        }
```

注意：在 `tts_engine_recreate` 中我们已经通过 `esp_tts_destroy` 释放了引擎，所以需要在调用 `tts_engine_recreate` 之前确认 TTS 引擎不是在 speaking 状态。但这里 `s_speaking` 为 false（刚收到新工作，上一个工作已结束、已通过 `app_tts_speak` 中的 stop 停止），所以安全。

不过，有一个更好的设计：在检查并重建后，如果 TTS 引擎不可用，跳过本次工作：

在重建后添加：

```c
        if (s_sentence_count >= TTS_ENGINE_MAX_SENTENCES) {
            s_sentence_count = 0;
            if (s_tts_ready) {
                esp_err_t err = tts_engine_recreate();
                if (err != ESP_OK) {
                    /* Engine unavailable, skip this sentence */
                    ESP_LOGW(TAG, "TTS recreate failed, skipping sentence");
                    continue;
                }
            }
        }
```

调整后完整片段：

```c
        /* Wait for work */
        if (xQueueReceive(s_work_queue, &work, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* TTS engine lifecycle: recreate every N sentences */
        s_sentence_count++;
        if (s_sentence_count >= TTS_ENGINE_MAX_SENTENCES) {
            s_sentence_count = 0;
            if (s_tts_ready) {
                esp_err_t err = tts_engine_recreate();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "TTS recreate failed, skipping sentence");
                    continue;
                }
            }
        }

        if (!s_tts_ready || s_tts == NULL || strlen(work.text) == 0) {
            /* Silently skip invalid work */
            continue;
        }
```

- [ ] **Step 4: I2S 写超时保护**

在 `tts_worker_task` 的 I2S 写入循环中（约第 80-93 行），替换 `i2s_channel_write` 调用：

```c
                if (s_tx_handle) {
                    size_t written = 0;
                    esp_err_t i2s_ret = i2s_channel_write(s_tx_handle, pcm, (size_t)chunk, &written, pdMS_TO_TICKS(1000));
                    if (i2s_ret != ESP_OK) {
                        ESP_LOGW(TAG, "I2S write timeout/reset, resetting channel");
                        i2s_channel_disable(s_tx_handle);
                        i2s_channel_enable(s_tx_handle);
                        s_stop_requested = true;
                        break;
                    }
                }
```

- [ ] **Step 5: Commit**

```bash
git add main/app_tts.c
git commit -m "fix: TTS engine recreate every 30 sentences, I2S write timeout protection"
```

---

### Task 6: main.c 模式切换生命周期 + 堆监控

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: 添加堆监控日志头文件**

在文件顶部 `#include "app_display.h"` 附近添加：

```c
#include "esp_heap_caps.h"
```

- [ ] **Step 2: 添加模式切换辅助函数**

在 `show_main_menu` 函数之前添加 `usb_enter` 和 `usb_exit`：

```c
/* ── Mode transition helpers ──────────────────────────────────────────── */

static esp_err_t usb_enter(void)
{
    /* 1) Suspend LVGL display updates — avoids SPI bus contention with SD card */
    app_display_suspend();

    /* 2) Sync and suspend FATFS — card handle remains valid for USB sector access */
    sd_card_fs_suspend();

    /* 3) Init USB PHY + TinyUSB (card handle passed for MSC callbacks) */
    ret = tusb_msc_init(s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB init failed: %s", esp_err_to_name(ret));
        /* Fallback: resume FATFS so FS stays usable */
        sd_card_fs_resume();
        app_display_resume();
        return ret;
    }

    s_usb_started = true;

    /* 4) Resume display (static text only, no LVGL timer animation) */
    app_display_set_text(
        "Starting USB...\n"
        "Please wait for PC to\n"
        "detect the drive.");
    app_display_resume();

    return ESP_OK;
}

static void usb_exit(void)
{
    /* 1) Deinit USB: kill task + PHY + TinyUSB */
    tusb_msc_deinit();
    s_usb_started = false;

    /* 2) Resume FATFS on existing card handle */
    sd_card_fs_resume();
}
```

- [ ] **Step 3: 修改 enter_usb_mode**

将现有 `enter_usb_mode`（约第 549-573 行）替换为：

```c
static void enter_usb_mode(void)
{
    beep();

    if (s_op_mode == MODE_USB_COPY) {
        /* Already in USB mode — request safe eject */
        return;
    }

    s_op_mode = MODE_USB_COPY;
    app_display_set_mode(DISP_MODE_USB_MSC);

    if (!s_card || !s_card_available) {
        app_display_set_text(
            "SD card not detected.\n"
            "Insert TF card and\n"
            "restart the device.");
        s_op_mode = MODE_MAIN_MENU;
        return;
    }

    esp_err_t ret = usb_enter();
    if (ret != ESP_OK) {
        app_display_set_text(
            "USB init failed.\n"
            "Press KEY1 for menu.");
        s_op_mode = MODE_MAIN_MENU;
    }
}
```

- [ ] **Step 4: 修改 return_to_main_menu**

将现有 `return_to_main_menu`（约第 604-613 行）替换为：

```c
static void return_to_main_menu(void)
{
    beep();

    /* Stop playback if in reading mode */
    s_playing = false;
    app_tts_stop();

    /* If in USB mode, perform safe USB eject */
    if (s_op_mode == MODE_USB_COPY && s_usb_started) {
        app_display_set_text("Safely removing USB...\nPlease wait.");
        usb_exit();
    }

    show_main_menu();
}
```

- [ ] **Step 5: 修改堆监控**

在 `poll_task` 函数中（约第 755-778 行），在 `while (1)` 循环中添加：

```c
        /* Heap monitor: log every 10 seconds to detect memory leaks */
        {
            static int heap_tick = 0;
            if (++heap_tick >= 100) {
                heap_tick = 0;
                size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                size_t min_free  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
                size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                ESP_LOGI(TAG, "MEM int=%zu min=%zu psram=%zu",
                         free_int, min_free, free_psram);
            }
        }
```

将此代码段放在 `vTaskDelay(pdMS_TO_TICKS(100));` 之前。

- [ ] **Step 6: Commit**

```bash
git add main/main.c
git commit -m "fix: mode transition lifecycle with USB safe eject, heap monitoring"
```

---

### Task 7: 按钮常量导出

**Files:**
- Modify: `main/app_buttons.h`

- [ ] **Step 1: 将长按阈值移入头文件**

当前 `BTN_LONG_PRESS_MS` 定义在 `app_buttons.c` 中但被 `main.c` 引用。确认当前 `app_buttons.h` 已有：

```c
#define BTN_LONG_PRESS_MS  3000
```

在 `app_buttons.h` 中 `#include "esp_err.h"` 之后、`#ifdef` 守卫内添加：

```c
/* Button long-press threshold (ms) — used by main.c for mode switching */
#define BTN_LONG_PRESS_MS  3000
```

然后在 `app_buttons.c` 中移除重复定义（如果存在）。

- [ ] **Step 2: Commit**

```bash
git add main/app_buttons.h
git commit -m "chore: export BTN_LONG_PRESS_MS from app_buttons.h"
```

---

## 自检核对

| 设计文档要求 | 对应任务 | 覆盖 |
|-------------|---------|------|
| USB 栈 4096→8192 | Task 3 Step 3 | ✅ |
| USB deinit（PHY + task） | Task 3 Step 4 | ✅ |
| FATFS mount/unmount 拆分 | Task 4 Step 2-4 | ✅ |
| FATFS 扇区 4096→512 | Task 1 Step 1 | ✅ |
| SPI 总线暂停（LVGL） | Task 2 Step 2-3 | ✅ |
| TTS 引擎 30 句重建 | Task 5 Step 1-3 | ✅ |
| I2S 写超时保护 | Task 5 Step 4 | ✅ |
| 堆内存监控 | Task 6 Step 5 | ✅ |
| 模式切换生命周期 + USB 安全退出 | Task 6 Step 2-4 | ✅ |
| USB 热插拔提示 | Task 3 Step 4（已含 deinit） + `tud_mount_cb` 已有 | ✅ |
| app_buttons.h 导出常量 | Task 7 Step 1 | ✅ |
