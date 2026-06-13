#include "bsp_gptimer.h"


static gptimer_handle_t gptimer_handle = NULL;
static QueueHandle_t gptimer_queue_handle = NULL;
static uint32_t gptimer_count = 0;
static bool IRAM_ATTR bsp_gptimer_on_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    gptimer_count++;
    gptimer_count %= 4;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    xQueueSendFromISR(gptimer_queue_handle, &gptimer_count, &xHigherPriorityTaskWoken); 
    return xHigherPriorityTaskWoken  == pdTRUE;
}

void bsp_gptimer_init(QueueHandle_t queue_handle)
{
    gptimer_queue_handle = queue_handle;
    gptimer_config_t gptimer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 0,
        .flags = {
            .intr_shared = true,
        },
    };

    ESP_ERROR_CHECK(gptimer_new_timer(&gptimer_config, &gptimer_handle));

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = 1000000,    
        .reload_count  = 0,
        .flags = {
            .auto_reload_on_alarm = true,
        },
    };

    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer_handle, &alarm_config));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = bsp_gptimer_on_alarm,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer_handle, &cbs, NULL));

    ESP_ERROR_CHECK(gptimer_enable(gptimer_handle));
    ESP_ERROR_CHECK(gptimer_start(gptimer_handle));
}