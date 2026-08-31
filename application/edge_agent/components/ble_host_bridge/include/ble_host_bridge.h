/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

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

#ifdef __cplusplus
}
#endif