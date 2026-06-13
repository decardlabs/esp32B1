#include "task_uart.h"

static const char *TAG = "UART1";  // 日志标签



// 状态机状态
typedef enum {
    STATE_WAIT_HEAD = 0,        // 等待帧头状态
    STATE_WAIT_LEN,             // 等待长度字段状态
    STATE_WAIT_DATA,            // 等待数据字段状态
    STATE_WAIT_CHECKSUM,        // 等待校验和状态
} proto_state_t;


static proto_state_t state = STATE_WAIT_HEAD;     // 当前协议解析状态
static uint8_t frame_buf[FRAME_MAX_LEN];          // 帧数据缓冲区
static uint16_t data_len = 0;                     // 数据长度
static uint16_t data_cnt = 0;                     // 已接收数据计数


/**
 * @brief UART1任务函数
 * 
 * 此任务负责初始化UART并持续监听串口数据。当接收到数据时，
 * 将数据传递给协议解析函数进行处理。
 * 
 * @param pvParameters 任务参数（未使用）
 */
void uart1_task(void *pvParameters)
{
    uint8_t data;
    bsp_uart1_init();  // 初始化UART1硬件
    ESP_LOGI(TAG, "uart1_task start\n");

    while (1)
    {
        // 从UART端口读取一个字节数据，阻塞等待
        // portMAX_DELAY表示无限期等待直到有数据到达
        int ret = uart_read_bytes(UART_PORT, &data, 1, portMAX_DELAY);
        if (ret > 0) {
            // 如果读取成功，则将数据传入协议解析函数进行处理
            proto_parse_byte(data);
        }
    }
}



/**
 * @brief 创建UART1任务
 * 
 * 使用FreeRTOS的xTaskCreate函数创建一个新的任务来处理UART通信。
 * 
 * @return BaseType_t 任务创建结果，pdPASS表示成功，其他值表示失败
 */
BaseType_t uart1_task_create(void)
{
    // 创建UART1任务：
    // - 任务函数：uart1_task
    // - 任务名称："uart1_task"
    // - 栈深度：4096字节
    // - 任务参数：NULL（不传递参数）
    // - 任务优先级：10（数字越大优先级越高）
    // - 任务句柄：NULL（不需要保存任务句柄）
    return xTaskCreate(uart1_task, "uart1_task", 4096, NULL, 10, NULL);
}


/**
 * @brief 计算数据校验和
 * 
 * 使用简单的累加求和算法计算数据块的校验和。
 * 
 * @param data 指向要计算校验和的数据的指针
 * @param len 数据长度（字节数）
 * @return uint8_t 校验和结果（只保留低8位）
 */
uint8_t checksum_sum(const uint8_t *data, uint16_t len)
{
    uint16_t sum = 0;
    // 遍历所有数据字节并累加
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    // 只保留低8位作为校验和结果
    return (uint8_t)(sum & 0xFF);
}


/**
 * @brief 处理完整帧数据
 * 
 * 解析并执行来自上位机的完整命令帧。
 * 目前支持的命令：
 * - 0x01: 控制LED开关
 * 
 * @param data 指向帧数据的指针（不包含帧头、长度和校验字段）
 * @param len 数据长度
 */
void process_frame(const uint8_t *data, uint16_t len)
{
    // 至少需要包含命令字和参数值两个字节
    if (len < 2) {
        ESP_LOGW(TAG, "帧长度不足");
        return;
    }
    ESP_LOGI(TAG, "收到完整帧:");   

    uint8_t cmd = data[0];   // 提取命令字（第一个字节）
    uint8_t val = data[1];   // 提取参数值（第二个字节）
    char buf[20] = {0};      // 发送响应数据的缓冲区
  
    // 根据命令字处理不同指令
    switch (cmd)
    {
        case 0x01:  // 控制 LED 命令
            // 根据参数值控制LED状态
            if (val == 0x00) {
                // 参数0x00：关闭LED（注意硬件连接可能为低电平有效）
                bsp_gpio_set_level(LED_GPIO, 0);
                ESP_LOGI(TAG, "LED 已打开");                 
            } else if (val == 0x01) 
            {
                // 参数0x01：打开LED（注意硬件连接可能为高电平有效）
                bsp_gpio_set_level(LED_GPIO, 1);
                ESP_LOGI(TAG, "LED 已关闭");            
            } else {
                ESP_LOGW(TAG, "无效的LED控制参数: 0x%02X", val);
            }

            // 向上位机发送确认响应
            uart_write_bytes(UART_PORT, "UART1 Recv Data:", strlen("UART1 Recv Data:"));
            sprintf(buf, "0x%X 0x%X", cmd, val);                
            uart_write_bytes(UART_PORT, buf, strlen(buf));
            uart_write_bytes(UART_PORT,  "\n", 1); 

            // 退出switch语句，结束本次命令处理
            break;
            
        default:
            // 不支持的命令
            ESP_LOGW(TAG, "未知命令: 0x%02X", cmd);
            // 退出switch语句
            break;
    }
}

/**
 * @brief 协议解析函数，每接收一个字节调用一次
 * 
 * 使用有限状态机(FSM)解析自定义串口协议数据帧。
 * 帧格式：帧头(1字节) + 长度(1字节) + 数据(N字节) + 校验和(1字节)
 * 
 * 状态转换过程：
 * 1. STATE_WAIT_HEAD: 等待帧头(0xAA)
 * 2. STATE_WAIT_LEN: 等待数据长度字节
 * 3. STATE_WAIT_DATA: 接收指定长度的数据
 * 4. STATE_WAIT_CHECKSUM: 等待并验证校验和
 * 
 * @param byte 接收到的单个字节数据
 */
void proto_parse_byte(uint8_t byte)
{
    // 根据当前状态处理接收到的字节
    switch (state)
    {
        case STATE_WAIT_HEAD:
            // 等待帧头状态
            // 检查是否为有效的帧头标识
            if (byte == FRAME_HEAD) {
                // 收到正确帧头，进入等待长度字段状态
                state = STATE_WAIT_LEN;
            }
            // 如果不是帧头，则继续保持在此状态，丢弃该字节
            // 退出当前case分支
            break;

        case STATE_WAIT_LEN:
            // 等待长度字段状态
            data_len = byte;           // 保存后续数据的长度
            data_cnt = 0;              // 重置数据接收计数器
            // 检查长度是否超出最大允许值
            if (data_len > FRAME_MAX_LEN) {
                // 长度超限，回到等待帧头状态
                ESP_LOGW(TAG, "数据长度超限: %d", data_len);
                state = STATE_WAIT_HEAD;
            } else {
                // 长度合法，进入等待数据状态
                state = STATE_WAIT_DATA;
            }
            // 退出当前case分支
            break;

        case STATE_WAIT_DATA:
            // 接收数据字段状态
            // 将数据存储到缓冲区中
            frame_buf[data_cnt++] = byte;
            // 检查是否已接收完所有数据
            if (data_cnt >= data_len) {
                // 数据接收完成，进入等待校验和状态
                state = STATE_WAIT_CHECKSUM;
            }
            // 否则继续保持在此状态继续接收数据
            // 退出当前case分支
            break;

        case STATE_WAIT_CHECKSUM:
        {
            // 校验和验证状态（使用代码块避免变量作用域问题）
            uint8_t check_recv = byte;                            // 收到的校验和
            uint8_t check_calc = checksum_sum(frame_buf, data_len); // 根据接收数据计算的校验和

            // 比较收到的校验和与计算得出的校验和
            if (check_calc == check_recv) {
                // 校验通过，处理完整帧数据
                ESP_LOGI(TAG, "校验成功，处理数据帧");
                process_frame(frame_buf, data_len);
            } else {
                // 校验失败，记录错误日志
                ESP_LOGE(TAG, "SUM校验错误: recv=0x%02X calc=0x%02X",
                         check_recv, check_calc);
            }
            
            // 无论校验是否通过，都回到等待帧头状态准备接收下一帧
            state = STATE_WAIT_HEAD;
            // 退出当前case分支
            break;
        }
        
        default:
            // 异常状态处理
            // 如果出现未定义的状态，重置为等待帧头状态
            ESP_LOGE(TAG, "未知状态: %d", state);
            state = STATE_WAIT_HEAD;
            // 退出switch语句
            break;
    }
}