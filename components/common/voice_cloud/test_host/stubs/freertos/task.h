#pragma once
#include "freertos/FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
