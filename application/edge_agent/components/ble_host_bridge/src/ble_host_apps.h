/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "cJSON.h"
#include "esp_err.h"
/* Caller owns *output. Invoked only by the bridge request task. */
esp_err_t ble_host_apps_handle(const char *op, const cJSON *request, char **output);
