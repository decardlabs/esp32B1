#include "bsp_uart.h"

/**
 * @brief 初始化UART1接口
 * 
 * 该函数配置并初始化ESP32的UART1接口，设置波特率、数据位、停止位等参数，
 * 并安装UART驱动程序。此函数应在使用UART功能之前调用。
 * 
 * 配置参数：
 * - 波特率：115200 bps
 * - 数据位：8位
 * - 校验位：无
 * - 停止位：1位
 * - 流控：禁用
 * - 时钟源：默认
 * - TX引脚：GPIO17
 * - RX引脚：GPIO18
 */
void bsp_uart1_init(void)
{
    // 配置UART参数结构体
    uart_config_t uart_config = {
        .baud_rate = 115200,                    // 波特率设置为115200 bps
        .data_bits = UART_DATA_8_BITS,          // 数据位：8位
        .parity = UART_PARITY_DISABLE,          // 校验位：禁用（无校验）
        .stop_bits = UART_STOP_BITS_1,          // 停止位：1位
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,  // 流控：禁用硬件流控
        .source_clk = UART_SCLK_DEFAULT,        // 时钟源：使用默认时钟
    };
    
    // 配置UART参数，如果出错会触发错误检查机制
    // UART_PORT在头文件中定义为UART_NUM_1
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config)); 
    
    // 设置UART引脚分配
    // TX引脚：GPIO17 (UART_TX_PIN)
    // RX引脚：GPIO18 (UART_RX_PIN)
    // RTS和CTS引脚：不使用 (UART_PIN_NO_CHANGE)
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    // 安装UART驱动程序
    // 参数说明：
    // - UART端口：UART_PORT (UART_NUM_1)
    // - 接收缓冲区大小：256字节
    // - 发送缓冲区大小：0 (使用默认值)
    // - 事件队列大小：0 (不使用事件队列)
    // - 中断分配标志：NULL (使用默认值)
    // - 中断标志：0 (使用默认值)
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 0, 0, NULL, 0));
}