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
 * @brief Set the storage base path used to build SD-card image paths.
 *
 * Must be called before ui_squareline_init().  Pass the value returned by
 * app_fs_storage_base_path() (e.g. "/fatfs" or the SD card mount point).
 * Images are expected at: <base_path>/ui/<name>.bin
 */
void ui_squareline_set_img_base_path(const char *base_path);

/**
 * @brief Callback invoked when a screen wants to start a Lua app.
 *
 * @param script_rel_path  Script path relative to the read-only system root,
 *                         e.g. "skills/ble_hid/scripts/meeting_presenter.lua".
 * @param user_ctx         Context passed to ui_squareline_set_app_launch_cb().
 */
typedef void (*ui_squareline_app_launch_cb_t)(const char *script_rel_path, void *user_ctx);

/**
 * @brief Register the handler that starts a Lua app on behalf of a screen.
 *
 * This component sits below system_ui in the dependency graph, so screens cannot
 * call into the launcher directly. The owner registers a handler here instead.
 * Passing NULL clears the handler; button presses then become no-ops.
 */
void ui_squareline_set_app_launch_cb(ui_squareline_app_launch_cb_t cb, void *user_ctx);

/**
 * @brief Ask the registered handler to start a Lua app. Safe to call with no
 *        handler registered.
 */
void ui_squareline_launch_app(const char *script_rel_path);

/**
 * @brief Resolve an image name to a path the LVGL filesystem can open.
 *
 * The carousel's icons are named by basename in the page table and resolved
 * against the base path given to ui_squareline_set_img_base_path(), into
 * "<base>/ui/<name>.bin". Screens outside this component that want the same
 * artwork go through here rather than duplicating the path rule.
 *
 * @param name  Image basename, e.g. "workbuddy_select" — no extension.
 * @return Pointer to a rotating internal buffer holding the resolved path, valid
 *         until the next UI_IMG_SD_SLOTS calls, or NULL if @p name is NULL or no
 *         base path has been set. Hand the result straight to lv_image_set_src().
 */
const char *ui_squareline_resolve_img(const char *name);

/**
 * @brief Callback invoked when the user selects the app on the current page.
 *
 * All pages share one screen, so instead of a screen hard-coding what it opens,
 * the selected page is reported here and the owner decides what to do.
 *
 * @param page      Index of the selected page, 0 .. ui_squareline_page_count()-1.
 * @param app_id    Stable identifier for the page's app, or NULL if it has none.
 * @param script    The page's default Lua script, or NULL if it has none.
 * @param user_ctx  Context passed to ui_squareline_set_page_select_cb().
 */
typedef void (*ui_squareline_page_select_cb_t)(uint8_t page, const char *app_id,
                                              const char *script, void *user_ctx);

/**
 * @brief Register the handler called when a page's app is selected.
 *
 * When set, this handler fully replaces the default behaviour. With no handler
 * registered the page's own script is passed to ui_squareline_launch_app()
 * instead. Passing NULL restores that default.
 */
void ui_squareline_set_page_select_cb(ui_squareline_page_select_cb_t cb, void *user_ctx);

/** @brief Number of pages in the carousel. */
uint8_t ui_squareline_page_count(void);

/** @brief Index of the page currently displayed. */
uint8_t ui_squareline_current_page(void);

/**
 * @brief Jump straight to a page, without the slide animation.
 *
 * Out-of-range indices are ignored. Must be called with the LVGL mutex held.
 */
void ui_squareline_show_page(uint8_t page);

/**
 * @brief The three actions the carousel screen offers.
 *
 * These are exactly what the on-screen "PREV" / "NEXT" / "M OPEN" hit targets
 * trigger, exposed so an owner can drive the same behaviour from physical keys.
 * PREV/NEXT play the slide animation (a call during an in-flight slide is
 * ignored); SELECT reports the current page through the page-select callback.
 *
 * Must be called with the LVGL mutex held. No-ops before init / after destroy.
 */
void ui_squareline_prev_page(void);
void ui_squareline_next_page(void);
void ui_squareline_select_page(void);

/**
 * @brief Installed app shown on the carousel after the built-in pages.
 *
 * Installed apps share a generic icon, are launched through the page-select
 * callback by @p app_id (with a NULL script) and are the only pages the
 * long-press menu lets the user uninstall; built-in pages answer 卸载 with
 * "系统应用不可卸载". 详细 is answered by the carousel itself.
 */
typedef struct {
    const char *title;      /**< Caption, may be Chinese; copied, at most 39 bytes   */
    const char *app_id;     /**< Unique id; copied, at most 39 bytes                  */
    const char *location;   /**< Shown in 详细, e.g. the app's directory; may be NULL */
} ui_squareline_app_t;

/**
 * @brief Add an installed app to the carousel. Call before ui_squareline_init().
 *
 * At most 16 pages fit, built-in ones included.
 *
 * @return false if the carousel is already built, full, the id is taken or a
 *         string is too long.
 */
bool ui_squareline_add_app(const ui_squareline_app_t *app);

/** @brief Forget every installed app added so far. Call before ui_squareline_init(). */
void ui_squareline_clear_apps(void);

/**
 * @brief Show a short message above the footer for a couple of seconds.
 *
 * Must be called with the LVGL mutex held. No-op before init / after destroy.
 */
void ui_squareline_show_toast(const char *text);

/** @brief App actions the long-press menu hands to the owner. */
typedef enum {
    UI_SQUARELINE_PAGE_ACTION_UNINSTALL = 0,   /**< 卸载, confirmed, on an installed app */
} ui_squareline_page_action_t;

/**
 * @brief Callback invoked when the user confirms uninstalling an installed app.
 *
 * The menu is already closed and a "正在卸载…" toast is showing. The owner is
 * expected to delete the app and rebuild the carousel without it. Called with the
 * LVGL lock held, so the handler must not block on it or do file I/O; post the
 * work to another task instead.
 *
 * @param page      Index of the page whose icon was long-pressed.
 * @param app_id    The installed app's id.
 * @param action    What the user asked for.
 * @param user_ctx  Context passed to ui_squareline_set_page_action_cb().
 */
typedef void (*ui_squareline_page_action_cb_t)(uint8_t page, const char *app_id,
                                               ui_squareline_page_action_t action,
                                               void *user_ctx);

/**
 * @brief Register the handler for the long-press menu's app actions.
 *
 * With no handler registered a confirmed uninstall does nothing. Passing NULL
 * clears the handler.
 */
void ui_squareline_set_page_action_cb(ui_squareline_page_action_cb_t cb, void *user_ctx);

/**
 * @brief Font for the long-press menu, toasts and installed apps' captions.
 *
 * The compiled-in SquareLine fonts lack 卸载 / 详细 and most Chinese app names,
 * so the owner passes a font that covers them (the system TTF). Call before
 * ui_squareline_init(); NULL falls back to LV_FONT_DEFAULT.
 */
void ui_squareline_set_menu_font(const lv_font_t *font);

/**
 * @brief Initialize the SquareLine UI and build the carousel screen.
 *
 * Must be called while the LVGL mutex is held (display_service / system_ui lock).
 * Returns a pointer to the created screen object, or NULL on failure.
 */
lv_obj_t *ui_squareline_init(void);

/**
 * @brief Destroy the SquareLine UI (releases all screen objects).
 *
 * Must be called while the LVGL mutex is held.
 */
void ui_squareline_destroy(void);

#ifdef __cplusplus
}
#endif

