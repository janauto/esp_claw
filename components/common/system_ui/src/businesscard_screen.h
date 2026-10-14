/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the business card screen
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t system_ui_create_businesscard_locked(void);

/**
 * @brief Delete the business card screen
 */
void system_ui_delete_businesscard_locked(void);

/**
 * @brief Show the business card screen
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t system_ui_show_businesscard(void);

/**
 * @brief Hide the business card screen and return to home
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t system_ui_hide_businesscard(void);

/**
 * @brief Show the previous card (wraps around). Must be called with LVGL lock held.
 */
void system_ui_businesscard_prev_locked(void);

/**
 * @brief Show the next card (wraps around). Must be called with LVGL lock held.
 */
void system_ui_businesscard_next_locked(void);

#ifdef __cplusplus
}
#endif
