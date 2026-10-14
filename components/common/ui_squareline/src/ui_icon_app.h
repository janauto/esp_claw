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

/** Sentinel icon name for installed apps, which ship no carousel artwork. */
#define UI_ICON_APP_NAME "ui_icon_app"

/**
 * @brief Generic installed-app icon, drawn with LVGL on first use.
 *
 * Same contract as ui_icon_2048_src(): a 256x256 RGB565 buffer usable with
 * lv_image_set_src(), cached until ui_icon_app_release(). @p selected picks the
 * full-contrast variant for the centre slot.
 */
const void *ui_icon_app_src(lv_obj_t *parent, bool selected);

/** @brief Free both cached buffers. Only call once no image still shows them. */
void ui_icon_app_release(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif
