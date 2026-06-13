#include "task_uart.h"

static const char *TAG = "UART";

void task_uart1(void *pvParameters)
{
    ESP_LOGI(TAG, "UART1 Task Start!");
    uint8_t data1 = 0;
    uint8_t data[20] = {0};
    char buf[128] = {0};
    size_t ringbuf_len = 0;
    bsp_uart1_init();
    while(1)
    {
        // uart_get_buffered_data_len(UART1_PORT, &ringbuf_len);
        // if(ringbuf_len > 0)
        // {
        //     ESP_LOGI(TAG, "ringbuf_len = %d", ringbuf_len);
        //     int len = uart_read_bytes(UART1_PORT, &data, ringbuf_len, 10 / portTICK_PERIOD_MS);
        //     if(len > 0)
        //     {
        //         ESP_LOGI(TAG, "len = %d", len);
        //         for(int i = 0; i < len; i++)
        //         {
        //             sprintf(buf, "UART1 Recv 0x%x!\r\n", data[i]);
        //             uart_write_bytes(UART1_PORT, buf, strlen(buf));
        //         }
        //     }
        // }
        // vTaskDelay(10 / portTICK_PERIOD_MS);

/***********************阻塞接收 1字节  控灯*******************************/
        uart_read_bytes(UART1_PORT, &data1, 1, portMAX_DELAY);
                sprintf(buf, "111111111111UART1 Recv 0x%x!\r\n", data1);
        uart_write_bytes(UART1_PORT, buf, strlen(buf));
        if(data1 == 0x0)
        {
            bsp_gpio_set_level(LED_GPIO, 0);
            ESP_LOGI(TAG, "LED ON");          
            ESP_LOGI(TAG, "UART1 Recv 0x0!");
        }
        if(data1 == 0x1)
        {
            bsp_gpio_set_level(LED_GPIO, 1);
            ESP_LOGI(TAG, "LED OFF");
            ESP_LOGI(TAG, "UART1 Recv 0x1!");
        }
        sprintf(buf, "UART1 Recv 0x%x!\r\n", data1);
        uart_write_bytes(UART1_PORT, buf, strlen(buf));
/********************************************************/
    }
}


BaseType_t uart1_task_create(void)
{
    BaseType_t ret = xTaskCreate(task_uart1, "task_uart1", 4096, NULL, 10, NULL);
    return ret;
}
