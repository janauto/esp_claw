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

/** Glyph drawn at the left of a sheet row. */
typedef enum {
    UI_PAGE_MENU_ICON_TRASH = 0,
    UI_PAGE_MENU_ICON_INFO,
    UI_PAGE_MENU_ICON_CLOSE,
} ui_page_menu_icon_t;

typedef struct {
    const char          *caption;
    ui_page_menu_icon_t  icon;
    bool                 danger;    /* drawn in red whether focused or not */
} ui_page_menu_row_t;

/** One "key  value" line of an information sheet. */
typedef struct {
    const char *key;
    const char *value;
} ui_page_menu_field_t;

/**
 * @brief Called exactly once per opened sheet, after it has been taken down,
 *        with the index of the chosen row.
 *
 * Dismissals (tapping the scrim, ui_page_menu_close()) report the config's
 * cancel_row. Runs in the LVGL context with the lock held; it may open the next
 * sheet straight away.
 */
typedef void (*ui_page_menu_done_cb_t)(uint8_t row);

typedef struct {
    lv_obj_t                   *parent;       /* screen the sheet is laid over          */
    int32_t                     scrim_h;      /* height dimmed from the top; the strip
                                                 below stays live as the key hint       */
    int32_t                     panel_bottom; /* y of the panel's bottom edge; the
                                                 panel grows upwards to fit             */
    int32_t                     panel_w;
    const char                 *heading;      /* optional, bright and centred           */
    const char                 *message;      /* optional, muted and centred            */
    const ui_page_menu_field_t *fields;       /* optional key/value lines               */
    uint8_t                     field_count;
    const ui_page_menu_row_t   *rows;
    uint8_t                     row_count;
    uint8_t                     focus;        /* row focused when the sheet opens       */
    uint8_t                     cancel_row;   /* reported for dismissals; also set apart
                                                 from the rows above it by a divider    */
    ui_page_menu_done_cb_t      done_cb;
} ui_page_menu_config_t;

/**
 * @brief Font for every sheet and toast. It must cover the Chinese captions,
 *        which the compiled-in SquareLine fonts do not, so the owner hands in the
 *        system TTF. NULL falls back to LV_FONT_DEFAULT.
 */
void ui_page_menu_set_font(const lv_font_t *font);
const lv_font_t *ui_page_menu_get_font(void);

/**
 * @brief Dim the screen and show a sheet.
 *
 * @return The full-screen layer the sheet lives on, so the caller can lift its
 *         own widgets above the scrim; NULL if a sheet is already open, the
 *         config is invalid or allocation failed.
 */
lv_obj_t *ui_page_menu_open(const ui_page_menu_config_t *config);

/** @brief Take the sheet down as a dismissal. No-op when closed. */
void ui_page_menu_close(void);

bool ui_page_menu_is_open(void);

/** @brief Move the focus by @p step rows, stopping at either end (physical keys). */
void ui_page_menu_step(int step);

/** @brief Choose the focused row (physical key). */
void ui_page_menu_confirm(void);

/**
 * @brief Show a short message centred at @p y on @p parent for a couple of
 *        seconds, replacing any toast still on screen.
 */
void ui_page_menu_toast(lv_obj_t *parent, int32_t y, const char *text);

/** @brief Remove the toast now, e.g. before its screen is deleted. */
void ui_page_menu_toast_clear(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
