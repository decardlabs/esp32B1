#ifndef __TASK_WS2812_H__
#define __TASK_WS2812_H__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ws2812b.h"
#include "xl9555.h"




BaseType_t ws2812_task_create(void);
bool ws2812_handle_voice_command(const char *text, char *feedback, size_t feedback_len);
void ws2812_get_state_text(char *text, size_t text_len);


#endif
