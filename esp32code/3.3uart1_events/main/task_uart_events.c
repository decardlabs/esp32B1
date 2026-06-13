#include "task_uart_events.h"

static const char *TAG = "UART1_EVENTS"; 

QueueHandle_t uart_event_queue;

void uart1_events_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t *data = (uint8_t *)malloc(UART_BUFFER_SIZE);
    bsp_uart1_events_init(UART_QUEUE_SIZE, &uart_event_queue);
    while(1)
    {
        // 等待串口事件
        if (xQueueReceive(uart_event_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            switch (event.type) {
                // 接收数据
                case UART_DATA:
                    ESP_LOGI(TAG, "收到数据长度: %d", event.size);
                    // 读取数据
                    int len = uart_read_bytes(UART_PORT, data, event.size, 100 / portTICK_PERIOD_MS);
                    if (len > 0) {
                        ESP_LOGI(TAG, "收到数据: ");
                        for (int i = 0; i < len; i++) {
                            printf(" 0x%X ", data[i]);
                        }
                        printf("\n");
                        // 可以在这里添加数据处理逻辑
                    }
                    break;                
                // 其他事件
                case UART_FIFO_OVF:
                    ESP_LOGI(TAG, "UART FIFO 溢出\n");
                    uart_flush_input(UART_PORT);
                    xQueueReset(uart_event_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGI(TAG, "UART 缓冲区满\n");
                    uart_flush_input(UART_PORT);
                    xQueueReset(uart_event_queue);
                    break;
                case UART_BREAK:
                    ESP_LOGI(TAG, "UART 中断\n");
                    break;
                case UART_PARITY_ERR:
                    ESP_LOGI(TAG, "UART 校验错误\n");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGI(TAG, "UART 帧错误\n");
                    break;
                default:
                    ESP_LOGI(TAG, "未知 UART 事件: %d\n", event.type);
                    break;
            }
        }
    }
    
    free(data);
    vTaskDelete(NULL);
    
}

BaseType_t uart1_events_task_create(void)
{
    return xTaskCreate(uart1_events_task, "uart1_events_task", 4096, NULL, 10, NULL);
}