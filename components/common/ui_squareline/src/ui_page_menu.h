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

/** Rows of the long-press app menu, top to bottom. */
typedef enum {
    UI_PAGE_MENU_UNINSTALL = 0,
    UI_PAGE_MENU_DETAIL,
    UI_PAGE_MENU_CANCEL,
    UI_PAGE_MENU_ITEM_COUNT,
} ui_page_menu_item_t;

/**
 * @brief Called exactly once per opened menu, after it has been taken down.
 *
 * Dismissals (tapping the scrim, ui_page_menu_close()) report
 * UI_PAGE_MENU_CANCEL. Runs in the LVGL context with the lock held.
 */
typedef void (*ui_page_menu_done_cb_t)(ui_page_menu_item_t item);

typedef struct {
    lv_obj_t              *parent;     /* screen the menu is laid over            */
    int32_t                scrim_h;    /* height dimmed from the top; the strip
                                          below stays live as the key hint        */
    int32_t                panel_top;  /* y of the panel's top edge in @p parent  */
    ui_page_menu_done_cb_t done_cb;
} ui_page_menu_config_t;

/**
 * @brief Font for the menu captions. It must cover 卸载 / 详细 / 取消, which the
 *        compiled-in SquareLine fonts do not, so the owner hands in the system
 *        TTF. NULL falls back to LV_FONT_DEFAULT.
 */
void ui_page_menu_set_font(const lv_font_t *font);

/**
 * @brief Dim the screen and show the menu with 取消 focused.
 *
 * @return The full-screen layer the menu lives on, so the caller can lift its
 *         own widgets above the scrim; NULL if a menu is already open or
 *         allocation failed.
 */
lv_obj_t *ui_page_menu_open(const ui_page_menu_config_t *config);

/** @brief Take the menu down as a dismissal. No-op when closed. */
void ui_page_menu_close(void);

bool ui_page_menu_is_open(void);

/** @brief Move the focus by @p step rows, stopping at either end (physical keys). */
void ui_page_menu_step(int step);

/** @brief Choose the focused row (physical key). */
void ui_page_menu_confirm(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
