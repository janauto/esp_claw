/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Report that the "M OPEN" button was pressed on @p page.
 *
 * Called from the LVGL event context with the LVGL lock already held, so the
 * handler must not block on it.
 *
 * Dispatch order: a page-select callback registered through
 * ui_squareline_set_page_select_cb() wins; otherwise @p script (when non-NULL)
 * is handed to the app-launch callback.
 *
 * @param page    Index of the page whose button was pressed.
 * @param app_id  Stable identifier for the page's app, or NULL if it has none.
 * @param script  Default Lua script for the page, or NULL if it has none.
 */
void ui_squareline_page_selected(uint8_t page, const char *app_id, const char *script);

#ifdef __cplusplus
} /*extern "C"*/
#endif
