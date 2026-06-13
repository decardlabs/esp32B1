# qa_volc 分步修复计划

## 背景

当前最小版本已成功启动到 Wi-Fi 连接。完整版本在以下操作中崩溃：
1. TF 卡挂载（`tf_sdcard_mount`）→ 日志系统 `strcmp` 崩溃
2. LVGL 任务创建 → 疑似相同根因

**崩溃模式完全相同**：SD 卡 SPI 驱动在调用 `ESP_LOGI`/`ESP_LOGE` 时，日志标签链表已被损坏。

**根因定位线索**：
- 最小版本（不含 `lv_font_xiaozhi_cn_16.c`）→ 完全正常
- 完整版本（含 1.7MB 字体文件）→ 堆损坏 → 日志系统崩溃
- 字体文件的巨大 `.rodata` 段改变了内存布局，导致某个分配操作越界写入日志系统内部结构

---

## 修复步骤

### Step 1: 恢复 LCD 和 ES8388 的 ESP_LOGI

当前 `lcd_st7796.c:180` 和 `es8388.c:186` 被改为 `printf`。确认最小版本中 ESP_LOGI 正常工作后，改回 ESP_LOGI。

```bash
sed -i '' 's/printf("LCD init done\\n"); fflush(stdout);/ESP_LOGI(TAG, "LCD init done");/' components/device/lcd_st7796.c
sed -i '' 's/printf("es8388 init ok\\n"); fflush(stdout);/ESP_LOGI(TAG, "es8388 init ok");/' components/device/es8388.c
```

**验证**：编译烧录，确认启动日志正常显示 `LCD init done` 和 `es8388 init ok`。

---

### Step 2: 恢复 TF 卡挂载（使用 SPI 总线锁验证）

将 main.c 中的 TF 卡挂载恢复，但包裹在错误处理中。同时确认 SPI 总线锁（`bsp_spi2_bus_lock`）在 SD 卡操作前后正确使用。

```c
// 3. Mount TF card (retry with timeout)
if (tf_sdcard_mount() != ESP_OK) {
    ESP_LOGW(TAG, "TF card mount failed, continuing without");
} else {
    log_mem_snapshot("sdcard_ok");
}
```

**验证**：无论 TF 卡是否插入，系统不应崩溃。如果崩溃，则问题在 SD SPI 驱动本身。

---

### Step 3: 单独添加 LVGL 任务（不含字体文件）

先创建简单的 LVGL 界面，使用内置默认字体（非大字体文件），确认 LVGL 初始化不造成堆损坏。

```c
// 4. Create LVGL task (basic, no large font)
lcd_lvgl_reserve_buffer();
lcd_lvgl_task_create();
ESP_LOGI(TAG, "LVGL task created");
```

**验证**：如果崩溃，问题在 LVGL 缓冲区分配或任务栈。如果成功，问题在大字体文件。

---

### Step 4: 添加大字体文件

将 `lv_font_xiaozhi_cn_16.c` 加回，但不实例化为 LVGL 字体（只作为 `.rodata` 数据）。观察是否造成崩溃。

```c
// main/CMakeLists.txt 中添加 lv_font_xiaozhi_cn_16.c
// 但不调用 LV_FONT_DECLARE
```

**验证**：如果编译后烧录崩溃，问题直接与字体文件的 `.rodata` 布局有关。

---

### Step 5: 逐个添加功能任务

按以下顺序逐个解注释 main.c 中的任务创建，每次只加一个：

| 顺序 | 任务 | 说明 |
|------|------|------|
| 1 | `ws2812_task_create()` | LED 控制，最简单的任务 |
| 2 | `audio_capture_task_create(&cfg)` | 录音任务 |
| 3 | `volc_asr_task_create(&cfg)` | ASR HTTP 客户端 |
| 4 | `volc_llm_task_create(&cfg)` | LLM SSE 客户端 |

每个任务添加后编译烧录，确认正常后再加下一个。

**验证**：每个步骤后确认 ESP_LOGI 正常、Wi-Fi 正常、system 不崩溃。

---

### Step 6: 恢复 config.ini 解析

TF 卡挂载成功后，恢复 config.ini 解析和配置读取逻辑。

### Step 7: 稳定性验证

- 内存监控日志正常工作
- OOM 降级可触发
- 连续按 KEY3/KEY4 无异常

---

## 出问题时如何处理

如果某个步骤触发崩溃（日志标签链表损坏）：

1. **LVGL 字体文件** → 将字体文件放在外部 flash 分区或减少字体大小
2. **TF 卡 SPI** → 改用 SDMMC 接口或降低 SPI 时钟速度
3. **PSRAM 分配** → 确保 `CONFIG_SPIRAM_USE_MALLOC=y` 且在 menuconfig 中正确配置 Octal PSRAM
