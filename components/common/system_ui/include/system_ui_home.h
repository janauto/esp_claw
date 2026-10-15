/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 1.0 系统首页对外接口：应用层注入状态来源，BLE / UART 共用同一套操作处理。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_UI_RELEASE "1.0.0"

typedef struct {
    /* 手机已连接且订阅了语音特征（背夹模式语音可用） */
    bool (*phone_linked)(void);
    /* 有 BLE 中心设备连接 */
    bool (*ble_connected)(void);
    /* 读取实时数据缓存（weather / stocks / aihot），返回 malloc 的 JSON 文本 */
    char *(*feed_read)(const char *id, int64_t *age_ms);
} system_ui_home_providers_t;

esp_err_t system_ui_home_set_providers(const system_ui_home_providers_t *providers);

/* 手机或调试口推来的语音状态：state = listening|recognizing|thinking|reply|error|hide。线程安全。 */
esp_err_t system_ui_home_assistant_show(const char *state, const char *heard, const char *text);

/* 兼容 0.6.0 App 的 workbuddy_ui 字段：映射到首页语音卡片。线程安全。 */
void system_ui_home_note_workbuddy(const char *status, const char *reply_status, const char *reply_text);

/*
 * 1.0 新操作（mode_get/mode_set/ai_config_get/ai_config_set/apps_all/app_launch/
 * home_get/home_set/assistant_show，UART 另有 ui_key/ui_touch/home_state/voice_text）。
 * 不认识的 op 返回 ESP_ERR_NOT_SUPPORTED；*out 为结果对象（调用者释放），
 * 失败时 *out 可能带 {"error":"中文原因"}。
 */
esp_err_t system_ui_home_handle_op(const char *op, const cJSON *request, bool uart, cJSON **out);

/* hello 里要列出的新操作名，逗号分隔的 JSON 字符串片段（"a","b"）。 */
const char *system_ui_home_ops_json(void);

/* 本轮语音的设备状态（天气、模式、可打开的应用），BLE voice_event 带给手机。 */
const char *system_ui_home_voice_context(void);

/* 当前模式："clip" / "dock" */
const char *system_ui_home_mode_name(void);

/* 模式变了（背夹控制中心、语音指令或手机切换）时回调，参数为 "clip" / "dock"。
 * BLE 桥用它主动告诉手机，手机上的模式卡不再显示旧状态。在调用切换的任务里回调。 */
typedef void (*system_ui_home_mode_listener_t)(const char *mode);
void system_ui_home_set_mode_listener(system_ui_home_mode_listener_t cb);

#ifdef __cplusplus
}
#endif
