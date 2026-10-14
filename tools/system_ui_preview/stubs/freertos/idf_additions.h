#pragma once
/* host preview stub */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define tskNO_AFFINITY 0x7FFFFFFF
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack,
                                           void *arg, UBaseType_t prio, TaskHandle_t *out,
                                           BaseType_t core, UBaseType_t caps);
void vTaskDeleteWithCaps(TaskHandle_t task);
