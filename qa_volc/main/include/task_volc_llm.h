#ifndef TASK_VOLC_LLM_H
#define TASK_VOLC_LLM_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "config_parser.h"

BaseType_t volc_llm_task_create(const config_t *cfg);
esp_err_t volc_llm_submit(const char *question);

#endif
