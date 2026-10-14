/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the single carousel screen shared by every app page.
 *
 * The chrome is created once; page switching only re-binds the per-page data
 * (icons, title, accent colour, indicator dot) from the runtime page table.
 * Call ui_page_register() for any extra pages before calling this function —
 * the dot-widget row is allocated at init time and cannot grow afterwards.
 *
 * Must be called with the LVGL lock held. Safe to call twice (second call is a
 * no-op and returns the existing screen).
 */
void ui_page_screen_init(void);

/** @brief Destroy the screen and reset all cached object pointers. */
void ui_page_screen_destroy(void);

/** @brief The screen object, or NULL before init / after destroy. */
lv_obj_t *ui_page_get_screen(void);

/** @brief Number of pages currently in the carousel ring. */
uint8_t ui_page_count(void);

/** @brief Index of the page currently bound to the screen. */
uint8_t ui_page_current(void);

/**
 * @brief Register an additional app page at runtime.
 *
 * Must be called BEFORE ui_page_screen_init(). The page is appended after the
 * compile-time defaults and becomes part of the carousel ring immediately.
 *
 * @param title        Big centred caption shown on the home screen.
 * @param app_id       Stable identifier passed to the page-select callback.
 * @param script       Lua script path relative to the filesystem root, or NULL.
 * @param frame_color  Accent colour for the frame around the selected icon (RGB24).
 * @param pill_color   Colour for the small pill above the frame (RGB24).
 * @param icon_sel     Image name for the selected (centre) slot.
 * @param icon_unsel   Image name for the unselected (flanking) slots.
 * @return true on success, false if the page table is full or the screen was
 *         already built.
 */
bool ui_page_register(const char *title, const char *app_id, const char *script,
                      uint32_t frame_color, uint32_t pill_color,
                      const char *icon_sel, const char *icon_unsel);

/**
 * @brief The three user actions the carousel offers, shared by the on-screen hit
 *        targets and the board's physical keys.
 *
 * ui_page_prev() / ui_page_next() play the slide animation and re-bind when it
 * finishes; a call made while a slide is still in flight is ignored.
 * ui_page_select() reports the current page through ui_squareline_page_selected().
 *
 * All three must be called with the LVGL lock held and do nothing if the screen
 * has not been built.
 */
void ui_page_prev(void);
void ui_page_next(void);
void ui_page_select(void);

/**
 * @brief Bind a page to the screen immediately, without a slide animation.
 *
 * Out-of-range indices are ignored. Does nothing if the screen is not built.
 */
void ui_page_show(uint8_t page);

#ifdef __cplusplus
} /*extern "C"*/
#endif
