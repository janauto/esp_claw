/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "ui_squareline.h"

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
void ui_squareline_page_selected(uint32_t page, const char *app_id, const char *script);

/**
 * @brief Report a long-press menu action on @p page to the registered handler.
 *
 * Called from the LVGL event context with the lock held. No-op when no handler
 * is registered.
 */
void ui_squareline_page_action(uint32_t page, const char *app_id,
                               ui_squareline_page_action_t action);

#ifdef __cplusplus
} /*extern "C"*/
#endif
