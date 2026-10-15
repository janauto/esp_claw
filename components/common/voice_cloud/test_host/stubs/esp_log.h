/* 宿主测试用的 esp_log.h 桩：设置环境变量 VC_LOG=1 时输出到 stderr */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#define HOST_ESP_LOG(level, tag, fmt, ...)                                         \
    do {                                                                           \
        if (getenv("VC_LOG")) {                                                    \
            fprintf(stderr, "%s %s: " fmt "\n", level, tag, ##__VA_ARGS__);       \
        }                                                                          \
    } while (0)
#define ESP_LOGE(tag, fmt, ...) HOST_ESP_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_ESP_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_ESP_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)
