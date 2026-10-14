/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct lua_State lua_State;

#ifdef __cplusplus
extern "C" {
#endif

/* Appearance and service UUID an external advertiser must include for a host
 * to recognise this device as a HID peripheral. */
#define LUA_MODULE_BLE_HID_UUID16      0x1812

int luaopen_ble_hid(lua_State *L);
esp_err_t lua_module_ble_hid_register(void);

typedef struct {
    /* Name reported through DIS / the GAP device name. NULL keeps the default. */
    const char *device_name;
} lua_module_ble_hid_attach_config_t;

/**
 * @brief Register the HID GATT services onto a NimBLE host owned by someone else.
 *
 * Use this when another component (e.g. a resident GATT bridge) already owns the
 * controller, the NimBLE host task and advertising, and HID has to live in the
 * same GATT server rather than bring up a second stack.
 *
 * Ordering is not optional: NimBLE only accepts new services while its GATT
 * database is still mutable, which stops being true once advertising starts or a
 * connection exists. Call this after esp_nimble_init() and before
 * nimble_port_freertos_init().
 *
 * In this mode the module never touches the controller, the host task or
 * advertising; the owner is responsible for advertising the HID appearance and
 * LUA_MODULE_BLE_HID_UUID16. Connection and encryption state is still tracked
 * through a passive GAP listener, so the ble_hid.* Lua API keeps working.
 *
 * @param config Optional configuration, may be NULL.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if the module already owns a
 *         stack of its own.
 */
esp_err_t lua_module_ble_hid_attach(const lua_module_ble_hid_attach_config_t *config);

/** @brief True once lua_module_ble_hid_attach() has succeeded. */
bool lua_module_ble_hid_is_attached(void);

/** @brief HID appearance an external advertiser should advertise. */
uint16_t lua_module_ble_hid_appearance(void);

#ifdef __cplusplus
}
#endif
