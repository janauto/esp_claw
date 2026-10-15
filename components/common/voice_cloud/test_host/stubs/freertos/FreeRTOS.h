/* 宿主测试用的 FreeRTOS 桩（pthread 实现） */
#pragma once
#include <pthread.h>
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskNO_AFFINITY 0x7FFFFFFF
typedef struct {
    pthread_mutex_t m;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {PTHREAD_MUTEX_INITIALIZER}
#define portENTER_CRITICAL(mux) pthread_mutex_lock(&(mux)->m)
#define portEXIT_CRITICAL(mux) pthread_mutex_unlock(&(mux)->m)
