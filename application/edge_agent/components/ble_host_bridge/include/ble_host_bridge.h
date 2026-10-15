/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#define BLE_HOST_BRIDGE_DEFAULT_NAME       "esp-claw-host"
#define BLE_HOST_BRIDGE_REQUEST_MAX        24576

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_name;
    const char *storage_base_path;
} ble_host_bridge_config_t;

esp_err_t ble_host_bridge_start(const ble_host_bridge_config_t *config);
esp_err_t ble_host_bridge_stop(void);
bool ble_host_bridge_voice_linked(void);
bool ble_host_bridge_connected(void);
/* 应用包操作（app_list/info/read/begin/write/commit/abort/remove/rename/reload）。
 * 1.0 起 UART 自动验收也走这里，与手机 App 的 BLE 请求完全同一处理。 */
esp_err_t ble_host_apps_handle(const char *op, const cJSON *request, char **output);

#ifdef __cplusplus
}
#endif