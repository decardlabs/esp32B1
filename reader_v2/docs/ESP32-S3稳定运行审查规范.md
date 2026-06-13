# ESP32-S3稳定运行审查规范 v1.0

> 文档同步版本：v2.1.0（2026-06-13）

状态: Draft
版本: v1.0
适用范围: ESP32-S3 + ESP-IDF 5.x 项目（含 Wi-Fi、LVGL、音频播放、外部 PSRAM）

## 1. 目标

本规范用于约束 ESP32-S3 项目的内存、任务、外设和模式切换行为，避免以下问题:

- 随机重启、Guru Meditation、Load/StoreProhibited
- 音频断流、UI 卡顿、任务栈溢出
- 链接期 `.dram0.bss overflowed`
- 运行期总空闲内存看似充足但分配失败（碎片化）

本规范优先级: 稳定性 > 实时性 > 吞吐量 > 内存利用率。

## 2. 内存分层与放置原则

### 2.1 IRAM（内部指令 RAM）

ESP32-S3 内部总 SRAM 512KB，典型 Kconfig 拆分为 IRAM ~192KB + DRAM ~320KB（可配置）。

用途:

- 中断上下文必须可执行的代码（IRAM-safe 路径）
- 对时延敏感的关键小函数

规则:

- 仅放关键路径代码，避免把普通业务逻辑塞入 IRAM
- 与中断相关的函数及其直接依赖必须保证在 cache 不可用窗口仍可运行
- 使用 `IRAM_ATTR` 标记中断处理函数和 cache miss 期间必须可执行的小函数
- 用 `idf.py map` 观察 IRAM 占用，超过 180KB 即为风险信号

### 2.2 DRAM（内部数据 RAM）

用途:

- 任务栈、控制结构、频繁读写小对象
- DMA 关键缓冲（仅限 `MALLOC_CAP_DMA` 区域）

规则:

- 高频访问、小块数据优先内部 RAM
- 禁止无预算地扩大任务栈
- 禁止放置超大静态数组
- DMA 缓冲必须使用 `MALLOC_CAP_DMA` 分配——不是所有内部 RAM 地址都支持 DMA

### 2.3 PSRAM（外部 RAM）

ESP32-S3 PSRAM 通过 CPU cache（I-cache 32KB + D-cache 32KB）访问，cache miss 代价约 200ns。这对时序敏感路径有影响。

用途:

- 大块缓存、低频访问数据、可容忍延迟的数据结构
- UI 大缓冲、文本缓存、网络响应缓存

规则:

- 大块数据默认放 PSRAM，避免挤占内部 RAM
- 不将强实时关键数据放入 PSRAM
- 涉及 DMA 时必须按外设能力验证——ESP32-S3 的 GDMA 可经 cache 走 PSRAM，但增加延迟，高吞吐实时音频应评估混放方案
- 紧耦合代码/只读数据也可经 Kconfig 放置在 PSRAM（`CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `CONFIG_SPIRAM_RODATA`），但需验证 cache miss 性能影响

## 3. 内存预算（初始基线）

适用硬件: ESP32-S3，8MB PSRAM。
说明: 预算为起步值，需根据实测迭代。

### 3.1 内部 RAM 预算（运行稳态）

ESP32-S3 内部 DRAM（IRAM 之后）典型可用 **~320KB**。以下预算需保证总和 ＜ 320KB:

- 系统与协议栈保留（WiFi + LWIP + mbedTLS + TinyUSB）: 130KB 到 160KB
  - WiFi 驱动强制占 ~30KB-40KB 内部 RAM，不可配置
  - mbedTLS SSL context ~40KB-60KB，应分配至 PSRAM
  - LWIP 协议栈 ~20KB
- 任务栈总量: 32KB 到 56KB
- DMA/音频关键缓冲: 16KB 到 32KB（仅放 DMA 必需的小缓冲，大音频环形缓冲走 PSRAM）
- 控制结构与高频小对象: 16KB 到 24KB
- 安全余量: 不低于 40KB（硬红线 30KB）

### 3.2 PSRAM 预算

- LVGL/显示大缓冲: 0.5MB 到 2MB
- 文本/文件缓存: 128KB 到 1MB
- 云端响应/中间缓存: 256KB 到 1MB
- 预留扩展池: 不低于 1MB

### 3.3 PSRAM Kconfig 策略

ESP-IDF 提供多个 `CONFIG_SPIRAM_USE_*` 选项，选择错误会直接破坏稳定性:

| 选项 | 效果 | 本项目推荐 |
|------|------|-----------|
| `CONFIG_SPIRAM_USE_CAPS_ALLOC` | `malloc()` 仍走内部 RAM，仅 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 走 PSRAM | **强制选用** |
| `CONFIG_SPIRAM_USE_MALLOC` | `malloc()` 默认走 PSRAM | **禁止**——WiFi/TCP 栈缓冲会被推到 PSRAM，性能不可控 |
| `CONFIG_SPIRAM_USE_TRYALLOC` | 内部不足时尝试 PSRAM | 不推荐——行为不确定，难以验证 |

附加推荐:
- `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` = n（不从 PSRAM 执行代码，本项目无此需求）
- `CONFIG_SPIRAM_RODATA` = n（不从 PSRAM 加载只读数据）

### 3.4 预算验收条件

- 内部最小空闲堆 >= 40KB（硬红线 30KB）
- 内部最大连续空闲块 >= 32KB
- 无关键路径分配失败

## 4. 分配 API 规范

### 4.1 禁止项

- 禁止在关键模块中直接使用 `malloc()` / `calloc()` / `realloc()`（除非有明确理由）
- 禁止未判空即使用分配结果

### 4.2 必选项

- 使用 `heap_caps_malloc()` / `heap_caps_calloc()` 按能力分配
- 释放时使用 `free()` 即可——ESP-IDF 的 `heap_caps_free()` 与 `free()` 等价，不再强制区分
- 分配失败必须执行降级策略；日志输出应使用 `ESP_DRAM_LOGE()`（IRAM-safe，避免在 OOM 时因日志二次分配导致异常）

### 4.3 推荐能力映射

- 高频小对象: `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`
- 大块低频缓存: `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`
- DMA 缓冲: `MALLOC_CAP_DMA`（默认优先内部内存）

示例:

```c
void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!buf) {
    ESP_DRAM_LOGE(TAG, "alloc failed: size=%u caps=SPIRAM", (unsigned)size);
    return ESP_ERR_NO_MEM;
}
```

## 5. 静态内存与链接约束

### 5.1 规则

- 禁止定义超大全局/静态数组进入 `.bss`
- 大数组改为运行期分配，静态区仅保留指针
- 如需静态常驻大对象，优先评估放置到外部内存段的可行性

### 5.2 风险信号

- 链接报错: `.dram0.bss' overflowed`
- 程序可编译但运行期可用内部堆明显偏低

## 6. 任务栈规范（FreeRTOS）

### 6.1 栈分配策略

- 实时任务: 先保守给足，再基于高水位回收
- 非关键任务: 以最小可行为基线逐步上调
- 禁止一次性”拍脑袋”分配超大栈

### 6.2 本项目任务栈参考

> **注意（AI 辅助编程重点）**: ESP-IDF `xTaskCreateStatic` / `xTaskCreate` 系列的栈大小参数单位**全部是字节（bytes）**，不是 FreeRTOS 文档里的 words。下表统一使用字节标注，避免歧义。

| 任务 | 栈大小（bytes） | 说明 |
|------|----------------|------|
| `app_main`（状态机调度） | 14336 B (14KB) | 状态机主线，调用链较深 |
| `app_display`（LVGL） | 8192 B (8KB) | LVGL 递归渲染 + 字体解析 + 快照局部缓冲；2026-06-13 因新增 2KB 局部缓冲从 4KB 上调 |
| `app_tts`（TTS 引擎） | 16384 B (16KB) | 回调链 + esp_tts 内部状态 |
| `app_buttons`（按键扫描） | 8192 B (8KB) | 20ms 轮询，栈很浅 |
| `tusb_task`（USB MSC） | 8192 B (8KB) | TinyUSB 回调驱动 |
| 总计 | ~55296 B (~54KB) | 实际运行中部分任务不同时活跃 |

验证方法: 压力场景下 `uxTaskGetStackHighWaterMark()` 打印各任务余量。余量 < 15% 即为风险。

### 6.3 强制监控

- 每个任务必须打点 `uxTaskGetStackHighWaterMark()`
- 压力场景下记录最小栈余量
- 栈余量低于阈值（建议 < 15%）即判定为风险

### 6.4 任务函数改动时的静态栈预算检查（**变更触发型红线**）

**背景**: ESP-IDF `xTaskCreateStatic` / `xTaskCreateStaticPinnedToCore` 的栈大小单位是**字节**，不是 FreeRTOS 文档中的 words。代码审查或 AI 辅助修复在任务函数体内新增局部缓冲区时，极易导致栈溢出，且编译器不会报错。

**每次对任务函数做改动（包括 AI 生成的补丁）必须执行以下检查:**

1. **列出局部变量总字节数**: 统计任务函数及其直接内联调用路径上所有局部数组/结构体的大小之和。
2. **与栈分配值对比**: 局部变量总量 + 调用链最深帧估算（至少 512B）不得超过栈分配值的 75%。
3. **超出阈值时的处置**:
   - 将大局部缓冲（> 256B）改为 `static` 局部变量（任务栈外、DRAM 段）；或
   - 按需改为堆分配（`MALLOC_CAP_INTERNAL` 优先），函数退出前释放；或
   - 同步上调任务栈大小并更新 §6.2 表格。

**本项目已知触发案例（2026-06-13）:**  
`lvgl_task` 内新增 `char text[2048]` 局部缓冲后，与 `char filename[64]` 等其他局部变量叠加，超出 4096B 栈分配，造成栈溢出。修复方式: 将任务栈提升至 8KB（8192B）。

**代码审查检查项（新增到 §11.1）:**
- [ ] 改动是否在任务函数内新增了局部数组或结构体？
- [ ] 该任务的栈分配值（字节）是否足以容纳新增的局部变量？

## 7. 碎片化控制

### 7.1 设计规则

- 长生命周期对象尽量一次分配，减少反复申请释放
- 高频路径优先环形缓冲/对象池
- 模式切换时执行统一释放顺序，避免悬挂碎片

### 7.2 监控指标

必须同时监控:

- `esp_get_free_heap_size()`
- `esp_get_minimum_free_heap_size()`
- `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`
- `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`
- `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`
- `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)`

## 8. 运行时监控点位

至少在以下时机打印内存快照:

- 开机初始化完成后
- Wi-Fi 连接成功后
- 文件加载完成后
- 开始播放前
- 模式切换后（USB/阅读/云端）
- 异常恢复后

建议统一日志格式:

```text
[MEM] stage=play_start free=xxxxx min=xxxxx int_free=xxxxx int_largest=xxxxx ps_free=xxxxx ps_largest=xxxxx
```

## 9. 降级策略（OOM 防御）

当任一红线触发时，按顺序降级（每个级别实现后重新检测内存，满足则停止）:

1. LVGL 显示缓冲降级: `disp_drv.buffer` 行数减半（64 → 32 → 16 行），关闭帧率统计和动画
   - 对应修改: `lv_disp_drv_t` 的 `buffer` 重建
2. 缩小 TTS 文本预读缓冲和网络 I/O 缓冲
3. 关闭 LVGL 动画 (`lv_anim_enable(false)`) 和次要 UI 元素
4. 延迟或拒绝新建非关键任务；清理文件缓存
5. 仍不足时，切换至 local TTS（断网降级），释放云端 HTTP/TLS 连接占用的内存
6. 最终: 弹出低内存提示，停止播放并回到主菜单

要求:

- 降级路径必须可重入
- 禁止 OOM 后继续解引用空指针

## 10. 模块落位建议（语音阅读器场景）

- 音频 DMA 缓冲: 内部 RAM（DMA 能力）
- TTS 控制结构/状态机: 内部 RAM
- LVGL 大缓冲: PSRAM
- 文本全文/分句缓存: PSRAM
- 文件 I/O 中间缓冲: 优先 PSRAM
- Wi-Fi/HTTP 临时大块: PSRAM，关键控制对象留内部 RAM

## 11. 开发流程要求

### 11.1 代码评审（CR）必查项

- 是否使用能力分配 API
- 是否每次分配后判空
- 是否引入超大静态数组
- 是否记录并验证任务栈高水位
- 是否在关键节点输出内存快照
- **改动是否在任务函数内新增了局部数组或结构体？如有，是否按 §6.4 规则验证栈预算？**
- **栈大小单位是否确认为字节（bytes）而非 words？**（ESP-IDF xTaskCreate 系列均以字节为单位）
- **UI 文案/模式是否通过原子 API 一次提交？**（禁止在多个 setter 中分步更新同一帧 UI 状态）
- **并发修复是否有最小化原则？**（禁止“先加互斥试试”的盲改；必须说明锁粒度、所有者和错误路径）

### 11.2 开发期验证工具
- `idf.py size` / `idf.py size-components`——快速检查 `.bss` / `.data` 段大小，发现异常膨胀的目标文件
- `heap_trace`——ESP-IDF 内置分配追踪，可记录每次分配的 caller PC，联调阶段用于定位泄漏和超额分配。启用方式: `idf.py menuconfig → Heap Memory Debugging → Enable heap tracing`
- `idf.py map`——查看 IRAM 占用明细
- **AI 补丁专用**：改动落地后立即运行以下命令检查新增的裸 malloc 和任务内大数组:
  ```bash
  # 检查是否有新增裸 malloc（应全部使用 heap_caps_malloc）
  grep -rn '\bmalloc\b\|\bcalloc\b\|\brealloc\b' main/ --include="*.c" | grep -v heap_caps | grep -v '#define'

  # 检查任务函数内是否有超过 256B 的局部数组
  grep -n 'char \+[a-zA-Z_][a-zA-Z0-9_]*\[' main/app_display.c main/app_tts.c main/app_cloud_tts.c main/main.c
  ```

### 11.3 联调与发布门槛

- 压力运行 >= 2 小时无重启
- 无关键路径分配失败
- 关键任务栈余量满足阈值
- 内部最小空闲堆与最大连续块满足预算红线

### 11.4 AI 生成补丁的典型违规模式（**AI 辅助编程专项**）

以下是 AI 辅助编程环境中，代码审查工具（包括 AI 本身）最容易引入的违规，每次 AI 补丁落地后必须逐条核对:

| # | 违规模式 | 后果 | 检查方法 |
|---|---------|------|---------|
| P1 | 任务函数内新增大局部缓冲（> 256B） | 栈溢出，编译器不报错 | 人工核查补丁 diff；或用 §11.2 的 grep 命令 |
| P2 | 把 `heap_caps_malloc()` 简化为裸 `malloc()` | 大缓冲意外落入内部 RAM，挤爆内部堆 | `grep -rn '\bmalloc\b' main/ --include="*.c"` |
| P3 | 加锁修复只覆盖 happy path，错误返回路径漏了 `unlock` | 锁永久持有，下一次加锁死锁 | 检查同一函数内 `lock()` 与所有 `return` 前是否均有对应 `unlock()` |
| P4 | 绕过已有互斥接口直接访问共享资源 | 竞态，覆盖其他任务刚写的状态 | 搜索直接访问 `s_card`、I2C、SPI 的新增代码，确认走统一接口 |
| P5 | 在 LVGL task 之外直接调用 `lv_*` API | UI 对象竞态，随机花屏或 crash | 搜索 `lv_label_set_text`、`lv_obj_*` 等调用，确认只出现在 `lvgl_task` 内部 |
| P6 | 将 `static` 局部变量用于跨任务共享状态 | 多任务并发读写无保护 | 审查新增 `static` 变量是否跨任务可见，是否需要加锁 |
| P7 | 栈大小数字沿用旧值，未跟随局部变量增长同步上调 | 栈不够用但编译通过，运行才崩 | 每次修改任务函数后对照 §6.2 表格检查栈预算 |
| P8 | ISR 回调内调用非 `FromISR` 后缀的 FreeRTOS API | 随机崩溃，仅在高负载或特定时序下触发 | 审查所有 `IRAM_ATTR` 标记的函数，确认只用 `xQueueSendFromISR` 等 ISR-safe API |
| P9 | 同一屏 UI 状态分多次 setter 提交（如先 set_mode 再 set_text） | 读到撕裂状态，出现乱码/错行/状态错位 | 检查菜单/状态切换函数是否使用单一原子接口（如 `set_mode_text()`） |
| P10 | linter/AI 自动引入互斥但未定义“单写者”模型 | 任务间竞争、死锁或重启（如 KEY 连按触发） | 评审中必须写明：谁写、谁读、是否需要锁；优先单写者+消息队列，谨慎加锁 |

### 11.5 AI 补丁提交前强制验证步骤（pre-commit checklist）

每次 AI 辅助生成的补丁在提交或烧录前，按顺序执行以下步骤，全部通过才算完成:

**Step 1 — 静态诊断**
```bash
idf.py build 2>&1 | grep -E 'error:|warning:' | head -40
```
无编译错误，无 `-Werror` 级别警告。

**Step 2 — 裸 malloc 扫描**
```bash
grep -rn '\bmalloc\b\|\bcalloc\b\|\brealloc\b' main/ --include="*.c" | grep -v heap_caps | grep -v '#define' | grep -v '^\s*//'
```
结果为空，或有明确注释说明为何可以使用裸分配。

**Step 3 — 任务函数局部大数组扫描**

对每个被改动的任务文件，运行:
```bash
grep -n 'char \+[a-zA-Z_][a-zA-Z0-9_]*\[[0-9]\{3,\}\]' main/<改动文件>.c
```
发现 >= 256B 的局部数组时，核对 §6.4 的栈预算。

**Step 4 — 锁配对检查**

对每个新增或改动的 `_lock()` / `_unlock()` 调用，人工确认：
- 函数所有 `return` 路径（含错误路径）都有对应的 `unlock`
- 没有在持锁期间调用可能再次加同一把锁的函数（避免 non-reentrant mutex 死锁）

**Step 5 — 任务栈预算核算**

对所有被改动的任务函数，对照 §6.2 表格确认：
- 当前分配的字节数
- 函数内所有局部数组/结构体字节数之和 + 500B 调用帧余量 ≤ 栈分配值 × 75%
- 若超出，先按 §6.4 处置，再更新表格

**Step 6 — 首次烧录验证**

烧录后在 monitor 中确认：
- 启动后无 `Guru Meditation`、`LoadProhibited`、`stack smashing`
- 各任务的 `[STACK] watermark` 日志余量 > 15%
- `[MEM]` 快照中 `int_free` ≥ 40KB

**Step 7 — UI 一致性与按键风暴回归（本次问题专项）**

至少执行以下人工回归:
- 主菜单和模式切换页面连续切换 20 次，观察是否出现乱码、错行或模式/文案不一致
- 连续快速点击 KEY1（建议 >= 10 次）后，系统无重启、无异常日志
- 若本次改动涉及 display setter 或锁逻辑，必须附带一段 monitor 关键日志截图（含 mode/text 切换时序）

## 12. 外设稳定性审查矩阵

适用对象: TFT 显示屏、TF 卡、Wi-Fi、蓝牙、Mic、喇叭、摄像头、USB、XL9555 口线扩展。

审查原则: 先看共享资源，再看实时链路，最后看模式切换与降级。

| 外设 | 主要共享资源 | 关键风险 | 审查重点 | 失败时动作 |
|------|--------------|----------|----------|----------|
| TFT 显示屏 | SPI、PSRAM、LVGL 任务栈 | 刷新占用总线，UI 卡顿 | 缓冲是否在 PSRAM，flush 是否短而可让出 CPU，是否与 TF 卡共线加锁 | 降低 buffer 行数，关闭动画和次要 UI |
| TF 卡 | SPI、文件系统、内部堆 | 读写阻塞、挂载抖动、碎片化 | 访问是否集中入口，是否与 LCD 共总线加锁，缓存是否避免频繁小分配 | 冻结写入，延迟非关键读写，必要时切换只读 |
| Wi-Fi | 内部堆、PSRAM、CPU Core 0 | 协议栈挤占内部 RAM，重连风暴 | 是否固定在 Core 0，TLS/HTTP 大块缓冲是否优先 PSRAM，重连是否有退避 | 切 local TTS，释放连接和大块上下文 |
| 蓝牙 | 内部堆、CPU Core 0 | 与 Wi-Fi 共存时抖动明显 | 是否可按模式关闭，扫描/广播频率是否受控，共存时是否限流 | 非主路径默认关闭，降低扫描频率 |
| Mic | I2S、DMA、任务栈 | 采集丢帧、缓冲溢出 | 采集缓冲是否 DMA 友好，是否避开 ISR 复杂处理，是否与播放解耦 | 降低采样负载，暂停采集，保留控制任务 |
| 喇叭 | I2S、codec、DMA | 音频断流、爆音 | 播放线程是否可让出 CPU，输出缓冲是否稳定，是否避免 PSRAM 关键路径 | 降低码率或帧长，必要时停止播放 |
| 摄像头 | DMA、PSRAM、CPU、带宽 | 最大吞吐外设，极易拉爆资源 | 帧缓冲大小、任务优先级、与 Wi-Fi/USB/显示是否并行冲突 | 降分辨率/帧率，必要时完全停用 |
| USB | TinyUSB、文件系统、任务切换 | 枚举/热插拔与 I/O 竞态 | 模式切换是否冻结非必要外设，回调是否短，文件系统边界是否清晰 | 切换到稳定模式后再恢复外设 |
| XL9555 | I2C、控制任务 | 慢速共享总线被阻塞 | 按键/背光/复位是否统一串行化，ISR 是否不直接碰 I2C | 降低访问频率，集中到单任务处理 |

审查结论建议按以下三档输出:

- 通过: 共享资源有锁，关键链路不互相阻塞，降级路径可触发。
- 警告: 资源隔离基本成立，但存在峰值叠加或恢复路径不完整。
- 不通过: 共享总线无保护、关键路径进入 PSRAM、或模式切换后无法恢复。

## 13. 快速检查清单

**通用（每次 PR/提交）**
- [ ] 大块缓存已放 PSRAM
- [ ] DMA 缓冲使用 DMA 能力内存
- [ ] 无超大 `.bss` 静态数组
- [ ] 关键点位内存日志已接入
- [ ] 任务栈高水位日志已接入
- [ ] TFT 与 TF 卡共享总线已加锁
- [ ] Wi-Fi / 蓝牙共存策略已明确
- [ ] Mic 与喇叭链路未在 ISR 中做重活
- [ ] 摄像头具备降分辨率或停用路径
- [ ] USB 模式切换时会冻结非必要外设
- [ ] XL9555 访问集中且无 ISR 直连 I2C
- [ ] OOM 降级路径可验证触发

**AI 补丁专项（每次 AI 辅助生成的改动额外检查）**
- [ ] Step 1：`idf.py build` 无 error / Werror 警告
- [ ] Step 2：裸 `malloc` 扫描为空
- [ ] Step 3：任务函数无 >= 256B 未声明为 `static` 的局部数组
- [ ] Step 4：每处 `_lock()` 在所有返回路径上都有对应 `_unlock()`
- [ ] Step 5：任务栈预算 = 局部变量总量 + 500B ≤ 栈分配值 × 75%，表格已更新
- [ ] Step 6：烧录后 watermark > 15%，`int_free` ≥ 40KB，无 Guru Meditation
- [ ] Step 7：主菜单/模式切换无乱码，KEY1 快速连按无重启

## 14. 版本记录

- v1.0 (2026-06-09): 首版发布，覆盖 ESP32-S3 常见音频 + UI + Wi-Fi 复合场景。
- v1.1 (2026-06-13): 补充外设稳定性审查矩阵（§12）；修正 §6.2 任务栈单位为字节；新增 §6.4 变更触发型栈预算检查；新增 §11.4 AI 违规模式表、§11.5 AI 补丁 pre-commit 流程；扩展 §13 快速检查清单 AI 专项。
- v1.2 (2026-06-13): 针对新增问题补充 UI 原子更新与并发修复最小化规则（§11.1、§11.4）；新增 UI 一致性/按键风暴回归步骤（§11.5 Step 7、§13 AI 专项）。
