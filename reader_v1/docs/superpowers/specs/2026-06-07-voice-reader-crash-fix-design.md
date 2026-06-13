# Voice Reader 稳定性修复设计

> 日期: 2026-06-07
> 版本: v1.0

## 问题概述

ESP32-S3 Voice Reader 设备存在两个导致重启的稳定性问题：

1. **USB 挂载过程 crash**：PC 枚举 USB 设备过程中发生重启（看门狗或 panic）
2. **长时间阅读后 crash**：连续朗读 10-30 分钟后设备重启

两种 crash 的表现均为系统重启，非死机。

## 根因分析

### 问题一：USB 枚举时 crash

| 因子 | 详情 | 影响 |
|------|------|------|
| USB 任务栈过小 | 仅 4096 字节，TinyUSB 枚举时控制传输 + 描述符 + SCSI 调用栈深 | `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` 检测到溢出触发 panic |
| USB deinit 为空壳 | `tusb_msc_deinit()` 只设 `s_card=NULL`，PHY 无法关闭、任务无法删除 | USB 一旦开启无法关闭，模式切换时后台 USB 任务仍在跑 |
| SPI 总线双重锁 | LCD 用 `bsp_spi2_bus_lock` 信号量，SD 卡用 SPI 驱动内部事务队列 | 不同步机制同时访问 SPI2，CS 信号可能冲突 |
| FATFS 未隔离 | USB 模式下 FATFS 仍挂载，USB 原始扇区读写破坏 FATFS 缓存状态 | 文件系统损坏、crash |
| USB PHY handle 丢失 | `usb_phy_init()` 将 `phy_hdl` 声明为局部变量（tusb_msc.c:22-23） | 无法调用 `usb_del_phy()` |

### 问题二：长时间阅读后 crash

| 因子 | 详情 | 影响 |
|------|------|------|
| TTS 引擎内存泄漏 | `esp_tts_parse_chinese()` + `esp_tts_stream_play()` 内部堆分配未完全释放 | 每句数十字节泄漏，600 句（~30 分钟）累积到数十 KB，耗尽 LVGL 堆（32KB）或内部 RAM |
| I2S 无超时保护 | `i2s_channel_write(..., portMAX_DELAY)` 可能永久阻塞 | I2S DMA 异常时任务无限阻塞 |
| FATFS 扇区不匹配 | `CONFIG_FATFS_SECTOR_4096=y` 但 TF 卡物理扇区为 512B | 跨扇区读写的边界 bug 可能触发 assert |

## 整体架构设计

```
┌─────────────────────────────────────────┐
│  app_main.c  (状态机编排, 聚焦模式切换)     │
│  可读性：仅做初始化和模式切换消息分发          │
├─────────────────────────────────────────┤
│  驱动/服务层                             │
│  sd_card.c (拆分 FATFS mount/unmount)    │
│  tusb_msc.c (真正的 deinit)             │
│  app_tts.c (引擎生命周期管理)             │
│  app_display.c (SPI暂停机制)             │
│  app_buttons.c (不变)                   │
├─────────────────────────────────────────┤
│  硬件层 (不变)                            │
│  bsp_spi.c lcd_st7796.c i2c_bus.c ...   │
└─────────────────────────────────────────┘
```

### 模式切换生命周期

```
  Main Menu ──KEY1──→ USB Copy
      ↑                    │
      │                    │ KEY1
      │  usb_exit():       │
      │  - tusb_msc_deinit │  usb_enter():
      │  - sd_card_fatfs   │  - app_display_suspend
      │    _mount          │  - sd_card_fatfs_unmount
      │                    │  - tusb_msc_init
      │                    │  - app_display_resume
      │                    ↓
      └──KEY1(lp)── Reading Mode
```

## 详细设计

### 1. USB 任务栈扩容

**文件**: `tusb_msc.c:182`

```
- xTaskCreate(usb_device_task, "usbd", 4096, NULL, 5, NULL);
+ xTaskCreate(usb_device_task, "usbd", 8192, NULL, 5, &s_usb_task);
```

同时保存 TaskHandle_t 以便后续 vTaskDelete。

### 2. USB 完整 deinit

**文件**: `tusb_msc.c`

将 `phy_hdl` 从局部变量改为静态全局变量 `s_phy_hdl`：

```c
static usb_phy_handle_t s_phy_hdl = NULL;

esp_err_t tusb_msc_init(sdmmc_card_t *card) {
    s_card = card;
    ESP_RETURN_ON_ERROR(
        usb_new_phy(&(usb_phy_config_t){
            .controller = USB_PHY_CTRL_OTG,
            .target = USB_PHY_TARGET_INT,
            .otg_mode = USB_OTG_MODE_DEVICE,
            .otg_speed = USB_PHY_SPEED_HIGH,
        }, &s_phy_hdl), TAG, "USB PHY init");
    tusb_init();
    xTaskCreate(usb_device_task, "usbd", 8192, NULL, 5, &s_usb_task);
    return ESP_OK;
}

esp_err_t tusb_msc_deinit(void) {
    // 1. 删除 USB 后台任务
    if (s_usb_task) {
        vTaskDelete(s_usb_task);
        s_usb_task = NULL;
    }
    // 2. 关闭 TinyUSB 栈
    tud_deinit();
    // 3. 拆除 USB PHY
    if (s_phy_hdl) {
        usb_del_phy(s_phy_hdl);
        s_phy_hdl = NULL;
    }
    s_card = NULL;
    s_usb_connected = false;
    return ESP_OK;
}
```

### 3. FATFS mount/unmount 拆分

**文件**: `sd_card.c`

将 s_card 句柄保存为静态变量。提供三个 API：

```c
// 初始化 SD 卡硬件（仅一次）
esp_err_t sd_card_init(sdmmc_card_t **out_card);
// 挂载 FATFS + VFS（在已有 card handle 上）
esp_err_t sd_card_fatfs_mount(void);
// 卸载 FATFS，保留 card handle（USB 用）
esp_err_t sd_card_fatfs_unmount(void);
```

关键：`sd_card_fatfs_unmount` 只做 `f_mount(NULL, "", 1)` + VFS 注销，**不**调用 `sdmmc_card_deinit()`，保持 `s_card` 句柄有效。

### 4. FATFS 扇区配置校正

**文件**: `sdkconfig.defaults`

```
- CONFIG_FATFS_SECTOR_4096=y
+ # CONFIG_FATFS_SECTOR_4096 is not set
+ CONFIG_FATFS_SECTOR_512=y
```

SD/TF 卡物理扇区为 512B，FATFS 逻辑扇区保持一致避免边界 bug。

### 5. SPI 总线协调

**文件**: `app_display.c`

新增暂停/恢复机制，在 USB 模式下停止 LVGL 定时器刷新：

```c
static bool s_suspended = false;

void app_display_suspend(void) {
    s_suspended = true;
}

void app_display_resume(void) {
    s_suspended = false;
}
```

LVGL 任务中：

```c
static void lvgl_task(void *arg) {
    while (1) {
        if (s_dirty) {
            s_dirty = false;
            // ... 刷新显示文字
        }
        if (!s_suspended) {
            uint32_t wait_ms = lv_timer_handler();
            if (wait_ms < 5) wait_ms = 5;
            if (wait_ms > 20) wait_ms = 20;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        } else {
            // 暂停时不跑 lv_timer_handler，仅等待唤醒
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}
```

### 6. TTS 引擎生命周期管理

**文件**: `app_tts.c`

每 30 句重建一次 TTS 引擎，释放累积内存：

```c
#define TTS_ENGINE_MAX_SENTENCES  30

static int s_sentence_count = 0;

static void tts_worker_task(void *arg) {
    tts_work_t work;
    while (1) {
        xQueueReceive(s_work_queue, &work, portMAX_DELAY);
        
        // 重建检查：每 30 句重建引擎
        s_sentence_count++;
        if (s_sentence_count >= TTS_ENGINE_MAX_SENTENCES) {
            ESP_LOGI(TAG, "Recreating TTS engine (sentence #%d)", s_sentence_count);
            tts_engine_recreate();
            s_sentence_count = 0;
        }
        
        // ... 原有播放逻辑
    }
}

static esp_err_t tts_engine_recreate(void) {
    if (s_tts) {
        esp_tts_destroy(s_tts);
        s_tts = NULL;
    }
    esp_tts_voice_t *voice = esp_tts_voice_set_init(
        &esp_tts_voice_xiaole,
        (void *)_binary_esp_tts_voice_data_xiaole_dat_start);
    if (!voice) { s_tts_ready = false; return ESP_FAIL; }
    s_tts = esp_tts_create(voice);
    if (!s_tts) { s_tts_ready = false; return ESP_ERR_NO_MEM; }
    s_tts_ready = true;
    return ESP_OK;
}
```

### 7. I2S 写超时保护

**文件**: `app_tts.c`

```c
size_t written = 0;
esp_err_t i2s_ret = i2s_channel_write(s_tx_handle, pcm, chunk,
                                       &written, pdMS_TO_TICKS(1000));
if (i2s_ret != ESP_OK) {
    ESP_LOGW(TAG, "I2S write timeout (%d bytes)", chunk);
    // 重置 I2S 通道
    i2s_channel_disable(s_tx_handle);
    i2s_channel_enable(s_tx_handle);
    break;
}
```

### 8. 堆内存监控

**文件**: `main.c`（poll_task）

```c
static void poll_task(void *arg) {
    while (1) {
        static int tick = 0;
        if (++tick >= 100) {    // 每 10 秒
            tick = 0;
            size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t min_free  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
            size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGI(TAG, "MEM int=%zu min=%zu psram=%zu",
                     free_int, min_free, free_psram);
        }
        // ... 原有逻辑
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

### 9. USB 热插拔提示改进

**文件**: `tusb_msc.c`

```c
void tud_mount_cb(void) {
    s_usb_connected = true;
    ESP_LOGI(TAG, "USB mounted");
    // 显示更新由 main.c 的 poll_task 检测 s_usb_connected 后处理
}

void tud_umount_cb(void) {
    s_usb_connected = false;
    ESP_LOGI(TAG, "USB unmounted");
}
```

## 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `tusb_msc.c` | 重写 deinit | 栈 4096→8192、保存 phy_hdl 和 task handle、实现真正 deinit |
| `sd_card.c` | 新增 API | 拆分 `sd_card_fatfs_mount()` / `sd_card_fatfs_unmount()` |
| `app_tts.c` | 新增逻辑 | TTS 引擎每 30 句重建、I2S 写超时保护 |
| `app_display.c` | 新增 API | `app_display_suspend()` / `app_display_resume()` |
| `main.c` | 重构 | 模式切换生命周期化、堆监控日志 |
| `main/app_buttons.h` | 新增常量 | `BTN_LONG_PRESS_MS` 移到公共头文件（如果需要） |
| `sdkconfig.defaults` | 配置变更 | `CONFIG_FATFS_SECTOR_4096=y` → `CONFIG_FATFS_SECTOR_512=y` |

## 测试方案

1. **USB 枚举测试**：插拔 USB 线 20 次，每次都能被 PC 正确识别
2. **USB 安全退出测试**：进入 USB 模式 → PC 拷贝文件 → KEY1 退出 → 再次进入 Reading 模式朗读 → 没有 crash
3. **长时间朗读测试**：连续朗读 >2 小时，没有重启，观察堆内存日志无持续下降
4. **TTS 引擎重建测试**：每 30 句重建无卡顿感，TTS 输出连续性不受影响
5. **FATFS 扇区测试**：读取各种大小（1KB-64KB）和编码（UTF-8/UTF-16/GBK）的 .txt 文件，内容正确
