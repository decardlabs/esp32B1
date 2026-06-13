ESP32-S3代码审查核心规范

文档同步版本：v2.1.0（2026-06-13）

1. 内存与对象创建红线
严禁动态创建：所有 FreeRTOS 对象（任务、队列、信号量等）必须使用静态创建接口（如 `xTaskCreateStatic`、`xQueueCreateStatic`、`xSemaphoreCreateMutexStatic`）。
禁止循环内分配：严禁在循环内执行 `malloc` / `new`，必须使用静态内存池。
PSRAM 访问限制：DMA 缓冲区及任务栈严禁分配在 PSRAM 中（必须使用 `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`）；严禁在中断服务函数（ISR）中访问 PSRAM。
2. 任务栈与双核调度红线
栈余量检测：所有任务必须通过 `uxTaskGetStackHighWaterMark()` 验证，栈余量必须 ≥ 20%。
核心绑定：WiFi/蓝牙任务必须固定在 Core 0；摄像头、SD卡等外设任务必须固定在 Core 1。
防饥饿机制：高优先级任务（尤其是 Core 0 的通信任务）单次执行时长建议 ≤ 50ms，必须包含 `vTaskDelay()` 或阻塞等待以出让 CPU。
3. 中断（ISR）安全红线
API 隔离：ISR 内仅允许调用带 `FromISR` 后缀的 FreeRTOS API，严禁调用普通版本。
代码驻留：ISR 函数必须使用 `IRAM_ATTR` 宏声明；ISR 中使用的常量数据必须使用 `DRAM_ATTR` 宏声明。
4. 外设与总线同步红线
互斥保护：SD卡、SPI、I2C 等共享总线外设，在多任务环境下必须使用互斥量（Mutex）或自旋锁（Spinlock）进行同步保护。
SD卡约束：SDIO 时钟频率需 ≤ 20MHz；CMD 和 DATA 线需硬件上拉；3.3V Flash 需烧录 eFuse 解除 DAT2 引脚冲突。
5. 电源管理与看门狗红线
任务看门狗：关键任务必须集成 TWDT（任务看门狗），防止任务死锁或饥饿。
睡眠前处理：进入 Light/Deep Sleep 前，必须挂起 Core 1 的外设任务；RTC 内存数据操作需确保 ESP-IDF 版本 ≥ v4.4.8 或 v5.0.7。
6. 性能与启动时序红线
初始化顺序：`app_main()` 中必须遵循”先初始化硬件外设 → 再创建 FreeRTOS 对象 → 最后创建任务”的严格时序，启动阶段禁止创建任何任务。
优先级避让：摄像头任务优先级必须低于 WiFi/BT 系统任务（系统默认优先级为 23），防止抢占通信资源。