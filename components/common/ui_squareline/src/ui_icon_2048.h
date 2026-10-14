/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel the page table uses in place of an SD-card image name. Resolved by
 * ui_page_icon_src() into the buffer built here rather than a file path. */
#define UI_ICON_2048_NAME "ui_icon_2048"

/**
 * @brief Get the 2048 carousel icon, drawing it on first use.
 *
 * The carousel slots are lv_image widgets scaled by lv_image_set_scale, and
 * lv_image_set_src() accepts an lv_draw_buf_t directly, so an icon rendered with
 * the LVGL drawing API can stand in for an image file with no changes to the
 * slide animations.
 *
 * Two variants exist because the selected slot is drawn larger and framed while
 * the neighbours sit in the background: @p selected is full-contrast, the other
 * is mixed towards the black backdrop so it recedes the same way the artwork
 * icons do.
 *
 * Buffers are 256x256 to match the SD-card icons, so the zoom and position
 * values in _ui_carousel_reset() and ui.c apply unchanged.
 *
 * @param parent    Any object already attached to a display; needed only to
 *                  drive the drawing pass, not retained.
 * @param selected  true for the centre slot, false for the neighbours.
 * @return Image source to hand to lv_image_set_src(), or NULL if drawing failed.
 *         Callers must fall back gracefully; a NULL src leaves the slot empty.
 */
const void *ui_icon_2048_src(lv_obj_t *parent, bool selected);

/** @brief Free both variants. Safe to call when nothing was ever drawn. */
void ui_icon_2048_release(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
