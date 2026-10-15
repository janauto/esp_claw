#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, UBaseType_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
/* 测试辅助：接下来 n 次创建任务失败 */
void host_task_fail_next(int n);
