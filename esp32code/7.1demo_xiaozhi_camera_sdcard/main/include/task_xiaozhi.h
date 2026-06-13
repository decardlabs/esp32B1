#ifndef __TASK_XIAOZHI_H__
#define __TASK_XIAOZHI_H__

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

BaseType_t xiaozhi_task_create(void);
void xiaozhi_enter_camera_view(void);
void xiaozhi_leave_camera_view(void);

#endif
