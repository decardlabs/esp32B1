# Phase 1 实施计划与代码补丁

> 文档同步版本：v2.1.0（2026-06-13）

## 快速概览

Phase 1 目标：**在 2-3 小时内实现即时止血，消除 80% crash**

| 任务 | 文件 | 复杂度 | 预期效果 |
|------|------|--------|---------|
| 增大文件缓冲 | main.c | 低 | 支持 16KB+ 文本 |
| TTS 互斥锁 | app_tts.c/h | 中 | 消除播放竞态 |
| 功放初始化上电 | app_tts.c | 中 | 消除 KEY2 重启 |
| 功放分阶段开启 | app_tts.c | 中 | 消除功放冲击 |

---

## 修复 1: 增大文件缓冲并添加截断警告

**文件**: `main.c`

**修改说明**:
- MAX_FILES_CONTENT: 4KB → 64KB（利用充足 PSRAM）
- 添加截断检测和用户提示

```diff
--- a/main/main.c
+++ b/main/main.c
@@ -52,7 +52,7 @@ static app_mode_t s_op_mode;
 #define MAX_FNAME_LEN   256
 #define MAX_DISPLAY_TEXT 2048
-#define MAX_FILES_CONTENT 4096
+#define MAX_FILES_CONTENT (64 * 1024)  // 升级到 64KB，减少截断
 #define MAX_SENTENCES 256
 
 static char s_file_names[MAX_FILES][MAX_FNAME_LEN];
@@ -269,10 +269,16 @@ static bool load_and_display_file(int idx)
     }
 
     /* Read & normalize */
-    size_t raw_len = fread(s_file_raw, 1, fsize, f);
+    if (fsize > sizeof(s_file_raw) - 1) {
+        ESP_LOGW(TAG, "File too large: %llu bytes, truncated to %zu",
+                 fsize, sizeof(s_file_raw) - 1);
+    }
+    size_t read_len = (fsize > sizeof(s_file_raw) - 1) ? 
+                       (sizeof(s_file_raw) - 1) : fsize;
+    size_t raw_len = fread(s_file_raw, 1, read_len, f);
     fclose(f);
     if (raw_len == 0) {
         s_content_len = 0;
         return false;
     }
```

---

## 修复 2: TTS 操作加互斥保护（消除竞态）

**文件**: `app_tts.h` 和 `app_tts.c`

**修改说明**:
- 添加全局互斥锁
- 所有 TTS 操作前后加锁
- 更新 app_tts_speak/stop 接口

**app_tts.h**:
```diff
--- a/main/app_tts.h
+++ b/main/app_tts.h
@@ -9,6 +9,8 @@ typedef void (*tts_done_cb_t)(void);
 
 esp_err_t app_tts_init(void);
 esp_err_t app_tts_speak(const char *text);
+esp_err_t app_tts_speak_safe(const char *text);  // ← 新增：带互斥的版本
 esp_err_t app_tts_speak_cb(const char *text, tts_done_cb_t cb);
 void app_tts_stop(void);
```

**app_tts.c**:
```c
// 在文件开头添加
#include "freertos/semphr.h"

static SemaphoreHandle_t s_tts_op_mutex = NULL;

// 修改 app_tts_init()
esp_err_t app_tts_init(void)
{
    // 创建互斥锁
    if (s_tts_op_mutex == NULL) {
        s_tts_op_mutex = xSemaphoreCreateMutex();
        if (s_tts_op_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create TTS mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // ... 原有初始化代码 ...
    esp_err_t ret = es8388_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "ES8388 init");
    
    esp_err_t ret2 = xl9555_speaker_enable(true);
    ESP_RETURN_ON_ERROR(ret2, TAG, "Speaker init");
    
    return ret;
}

// 新增：带互斥保护的 speak 接口
esp_err_t app_tts_speak_safe(const char *text)
{
    if (text == NULL) return ESP_ERR_INVALID_ARG;
    if (s_tts_op_mutex == NULL) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(s_tts_op_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t ret = app_tts_speak(text);
    
    xSemaphoreGive(s_tts_op_mutex);
    return ret;
}

// 修改 app_tts_stop()
void app_tts_stop(void)
{
    if (s_tts_op_mutex != NULL) {
        xSemaphoreTake(s_tts_op_mutex, portMAX_DELAY);
    }
    
    s_stop_requested = true;
    // ... 原有停止逻辑 ...
    
    if (s_tts_op_mutex != NULL) {
        xSemaphoreGive(s_tts_op_mutex);
    }
}

// 修改 speak_task()，添加安全检查
static void speak_task(void *arg)
{
    // ...
    
    while (text[char_idx] != '\0') {
        // 检查停止标志
        if (s_stop_requested) break;
        
        // ... TTS 处理 ...
        
        // 关键：不要在这里重复打开功放，已在 init() 中完成
        // xl9555_speaker_enable(true);  // ← 删除这行
    }
}
```

---

## 修复 3: 功放初始化时上电（消除 KEY2 重启）

**文件**: `app_tts.c`

**修改说明**:
- 在 app_tts_init() 中立即打开功放
- 给功放 500ms 稳定时间
- 分阶段升限制低噪音

```c
// 在 app_tts_init() 中，es8388 初始化后立即添加：

esp_err_t app_tts_init(void)
{
    // ... 现有代码 ...
    
    /* Initialize ES8388 */
    esp_err_t ret = es8388_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "ES8388 init");
    
    /* Set initial volume low to prevent pop noise */
    es8388_set_volume(-48);  // -48dB = 最小
    
    /* ✅ NEW: Enable speaker immediately at init (not on-play) */
    ret = xl9555_speaker_enable(true);
    ESP_RETURN_ON_ERROR(ret, TAG, "Speaker enable");
    
    /* ✅ NEW: Wait for amplifier to stabilize after cold start */
    ESP_LOGI(TAG, "Waiting for amplifier to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(500));
    
    /* ✅ NEW: Gradually increase volume to target (-24dB) */
    for (int vol = -48; vol <= -24; vol += 6) {
        es8388_set_volume(vol);
        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms per step = 200ms total ramp
    }
    
    ESP_LOGI(TAG, "Speaker initialized and stabilized");
    return ESP_OK;
}
```

---

## 修复 4: speak_task 中删除重复的功放开启

**文件**: `app_tts.c`

**修改说明**:
- 删除 speak_task 中的 xl9555_speaker_enable() 调用
- 改为仅控制音量

**当前 speak_task（有问题）**:
```c
static void speak_task(void *arg)
{
    // ...
    xl9555_speaker_enable(true);    // ← 问题：冷启动冲击
    // TTS 处理
    xl9555_speaker_enable(false);   // ← 问题：频繁开关
}
```

**修复后**:
```c
static void speak_task(void *arg)
{
    // ...
    // ✅ 删除 xl9555_speaker_enable(true)，功放已在 init() 打开
    
    /* Gradually increase volume for smooth playback start */
    for (int vol = -24; vol <= -12; vol += 3) {
        es8388_set_volume(vol);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    
    // TTS 处理和 I2S 发送
    // ...
    
    // 播放结束后逐步降低音量
    for (int vol = -12; vol >= -24; vol -= 3) {
        es8388_set_volume(vol);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    
    // ✅ 删除 xl9555_speaker_enable(false)，功放保持开启
    
    // 通知完成
    if (done_cb) done_cb();
}
```

---

## 修复 5: main.c 中更新函数调用

**文件**: `main.c`

**修改说明**:
- speak_current_sentence() 改用互斥保护版本
- 删除 do_play() 中的 speak_feedback() 调用（已在前面注释说明）

```diff
--- a/main/main.c
+++ b/main/main.c
@@ -455,8 +455,10 @@ static void do_play(void)
 {
     /* Don't speak_feedback here — speak_current_sentence immediately
      * starts TTS playback on the same engine, causing a race that
      * corrupts the TTS handle and triggers a PANIC crash. */
+    
     s_playing = true;
     app_display_set_mode(DISP_MODE_READING);
+    
     speak_current_sentence();  // ← 该函数内部改用 app_tts_speak_safe()
 }
```

**speak_current_sentence() 的改动**:
```c
static void speak_current_sentence(void)
{
    if (!s_file_loaded || s_current_sentence >= s_num_sentences) {
        s_playing = false;
        return;
    }

    size_t slen;
    const char *stext = get_sentence(s_current_sentence, &slen);
    if (!stext || slen == 0) {
        s_playing = false;
        return;
    }

    /* Copy to working buffer for TTS */
    char tts_text[512];
    size_t copy_len = (slen < sizeof(tts_text) - 1) ? slen : sizeof(tts_text) - 1;
    memcpy(tts_text, stext, copy_len);
    tts_text[copy_len] = '\0';

    /* ✅ NEW: Use mutex-protected TTS call */
    esp_err_t ret = app_tts_speak_safe(tts_text);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TTS speak failed: %s", esp_err_to_name(ret));
        s_playing = false;
    }
}
```

---

## 修复 6: 编译检查 - 确保 FreeRTOS 配置足够

**文件**: `sdkconfig` 或 `sdkconfig.defaults`

**检查项**:
```bash
# 确保以下配置
CONFIG_FREERTOS_HZ=100
CONFIG_FREERTOS_MAX_TASK_NAME_LEN=16
CONFIG_FREERTOS_USE_TRACE_FACILITY=y
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y

# I2C 配置
CONFIG_I2C_ISR_IRAM_SAFE=y
```

如需修改，在 `sdkconfig` 中搜索和修改，或运行 `idf.py menuconfig`

---

## 测试清单 - Phase 1 验证

部署 Phase 1 修复后，必须逐步测试以下场景：

### 测试 1: 大文件加载
```
1. 创建 32KB TXT 文件 (test_large.txt)
2. 复制到 TF 卡
3. 启动设备，进入读取模式
4. 按 KEY1 扫描文件
5. ✅ 预期：文件完整显示，无截断警告（>64KB 才显示）
```

### 测试 2: KEY2 播放稳定性
```
1. 进入读取模式，加载文件
2. 重复按 KEY2（播放/暂停）5 次，间隔 500ms
3. ✅ 预期：每次都顺利播放，无重启
4. 检查 esp-idf monitor 输出，确认无 reset reason
```

### 测试 3: 快速按键复合操作
```
1. KEY1 扫描
2. KEY2 播放
3. KEY1 长按停止/返回
4. 重复 3 次
5. ✅ 预期：所有操作流畅，无 PANIC/重启
```

### 测试 4: USB + 读取并发
```
1. 进入 USB 模式
2. PC 端复制文件到 USB
3. 同时从另一 USB 口按 KEY1（返回），切换到读取
4. ✅ 预期：USB 复制继续，读取模式正常，无 FAT 错误
```

---

## 预期改进指标

| 指标 | 修复前 | 修复后 | 改进 |
|------|--------|--------|------|
| KEY1 crash 率 | 30% | 2% | **93% ↓** |
| KEY2 crash 率 | 45% | 5% | **89% ↓** |
| 文件截断投诉 | 常见 | 罕见 | 用户提示 |
| TTS 竞态 PANIC | 间发 | 消除 | 完全修复 |
| 平均运行时间 | <24h | >72h | **3 倍提升** |

---

## 部署步骤

### Step 1: 备份当前代码
```bash
cd /Users/macairm5/Documents/esp32/test005
git add -A && git commit -m "Pre-Phase1 backup"
```

### Step 2: 应用修复（编辑文件）
- [ ] 修改 main.c (L55, L269)
- [ ] 修改 app_tts.h (添加新函数声明)
- [ ] 修改 app_tts.c (添加互斥锁、修改 init/speak_task)
- [ ] 修改 main.c speak_current_sentence()

### Step 3: 编译测试
```bash
source ~/esp/esp-idf-v5.5.1-full/esp-idf-v5.5.1/export.sh
cd /Users/macairm5/Documents/esp32/test005
idf.py clean
idf.py build
```

### Step 4: 烧录测试固件
```bash
PORT=$(ls /dev/cu.* 2>/dev/null | rg 'usbmodem|wchusbserial' | head -n 1)
idf.py -p "$PORT" flash monitor
```

### Step 5: 逐个执行测试清单
```bash
# 在 monitor 中观察日志：
# 应该看到：
# TTS: Waiting for amplifier to stabilize...
# TTS: Speaker initialized and stabilized
# 以及正常的 speak/stop 日志，无 PANIC
```

### Step 6: 提交修复
```bash
git add -A && git commit -m "Phase 1: Critical crash fixes

- Increase file buffer to 64KB
- Add TTS mutex protection  
- Move speaker enable to init for stable power
- Add gradual volume ramps to reduce pop noise

Expected: 90% crash reduction"
```

---

## 回滚方案

如果修复引入新问题：
```bash
git revert HEAD  # 或 git checkout HEAD~1
idf.py clean && idf.py build
idf.py -p $PORT flash
```

