/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "esp_err.h"

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
} lua_audio_shared_format_t;

int luaopen_audio(lua_State *L);
esp_err_t lua_module_audio_register(void);

esp_err_t lua_audio_shared_area_write(const char *name,
                                      const void *data,
                                      size_t size,
                                      const lua_audio_shared_format_t *format,
                                      uint32_t *seq_out);
esp_err_t lua_audio_shared_area_read(const char *name,
                                     void *data,
                                     size_t capacity,
                                     size_t *size_out,
                                     lua_audio_shared_format_t *format_out,
                                     uint32_t *seq_out,
                                     uint32_t after_seq,
                                     bool clear_after_read);
esp_err_t lua_audio_shared_area_wait(const char *name,
                                     uint32_t after_seq,
                                     uint32_t timeout_ms);
esp_err_t lua_audio_shared_area_info(const char *name,
                                     size_t *size_out,
                                     lua_audio_shared_format_t *format_out,
                                     uint32_t *seq_out);
