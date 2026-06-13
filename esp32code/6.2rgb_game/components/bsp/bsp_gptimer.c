#include "bsp_gptimer.h"

gptimer_handle_t gptimer_handle = NULL;
uint32_t gptimer_count = 0;
QueueHandle_t gptimer_queue_handle = NULL;
static bool bsp_gptimer_isr(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    gptimer_count++;
    gptimer_count %= 4;
    xQueueSendFromISR(gptimer_queue_handle, &gptimer_count, NULL);
    return false;
}


void bsp_gptimer_init(QueueHandle_t queue_handle)
{  
    gptimer_queue_handle = queue_handle;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 0,
    };
    gptimer_new_timer(&timer_config, &gptimer_handle);

     gptimer_alarm_config_t alarm_config = {
        .alarm_count = 1000000,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(gptimer_handle, &alarm_config);

    gptimer_event_callbacks_t cbs = {
        .on_alarm = bsp_gptimer_isr,
    };
    gptimer_register_event_callbacks(gptimer_handle, &cbs, NULL);

    gptimer_enable(gptimer_handle);

    gptimer_start(gptimer_handle);
}
