/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the storage base path from which UI images are loaded.
 *
 * Must be called before any screen that uses images is initialised.
 * The path is the VFS mount point (e.g. "/fatfs" or the SD card mount point
 * returned by app_fs_storage_base_path()).  Images are expected at:
 *   <base_path>/ui/<name>.bin
 *
 * @param base_path  VFS base path, e.g. "/fatfs".  May be NULL to reset.
 */
void ui_img_sd_set_base_path(const char *base_path);

/**
 * @brief Return a stable LVGL path string for the named image asset.
 *
 * Returns a pointer of the form "S:<base_path>/ui/<name>.bin" suitable for
 * passing directly to lv_img_set_src().  The buffer is per-call and must not
 * be freed by the caller.
 *
 * @param name  Image name without path or extension, e.g. "ui_img_1482846779".
 * @return      Static per-slot path string.  NULL if base path is not set.
 */
const char *ui_img_sd_path(const char *name);

#ifdef __cplusplus
}
#endif
