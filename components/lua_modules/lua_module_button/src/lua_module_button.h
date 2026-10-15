/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "lua.h"
#include "esp_err.h"

int luaopen_button(lua_State *L);
esp_err_t lua_module_button_register(void);
/* 自动验收：按板级按键组的 label（MODE/SET/VOLUME_UP/VOLUME_DOWN）投递一个事件给正在运行的 Lua 脚本 */
esp_err_t lua_module_button_inject(const char *label, const char *event);
