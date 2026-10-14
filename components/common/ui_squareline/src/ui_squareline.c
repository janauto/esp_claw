/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_squareline.h"

#include "ui.h"
#include "ui_img_sd.h"
#include "ui_page.h"
#include "ui_page_menu.h"
#include "ui_squareline_priv.h"

static ui_squareline_app_launch_cb_t s_app_launch_cb;
static void *s_app_launch_user_ctx;

static ui_squareline_page_select_cb_t s_page_select_cb;
static void *s_page_select_user_ctx;

static ui_squareline_page_action_cb_t s_page_action_cb;
static void *s_page_action_user_ctx;

void ui_squareline_set_img_base_path(const char *base_path)
{
    ui_img_sd_set_base_path(base_path);
}

void ui_squareline_set_app_launch_cb(ui_squareline_app_launch_cb_t cb, void *user_ctx)
{
    s_app_launch_cb = cb;
    s_app_launch_user_ctx = user_ctx;
}

void ui_squareline_set_page_select_cb(ui_squareline_page_select_cb_t cb, void *user_ctx)
{
    s_page_select_cb = cb;
    s_page_select_user_ctx = user_ctx;
}

void ui_squareline_set_page_action_cb(ui_squareline_page_action_cb_t cb, void *user_ctx)
{
    s_page_action_cb = cb;
    s_page_action_user_ctx = user_ctx;
}

void ui_squareline_set_menu_font(const lv_font_t *font)
{
    ui_page_menu_set_font(font);
}

void ui_squareline_launch_app(const char *script_rel_path)
{
    /* Called from LVGL button callbacks, i.e. with the LVGL lock already held.
     * The handler must not block on it; system_ui only posts a work event. */
    if (s_app_launch_cb != NULL && script_rel_path != NULL && script_rel_path[0] != '\0') {
        s_app_launch_cb(script_rel_path, s_app_launch_user_ctx);
    }
}

const char *ui_squareline_resolve_img(const char *name)
{
    /* ui_img_sd.h is private to this component, so its own screens get the raw
     * helper while everyone above goes through here. */
    return ui_img_sd_path(name);
}

void ui_squareline_page_selected(uint8_t page, const char *app_id, const char *script)
{
    /* A page-select handler takes over the decision entirely; without one, fall
     * back to the page's own script so the default behaviour still works. */
    if (s_page_select_cb != NULL) {
        s_page_select_cb(page, app_id, script, s_page_select_user_ctx);
        return;
    }
    ui_squareline_launch_app(script);
}

void ui_squareline_page_action(uint8_t page, const char *app_id,
                               ui_squareline_page_action_t action)
{
    if (s_page_action_cb != NULL) {
        s_page_action_cb(page, app_id, action, s_page_action_user_ctx);
    }
}

uint8_t ui_squareline_page_count(void)
{
    return ui_page_count();
}

uint8_t ui_squareline_current_page(void)
{
    return ui_page_current();
}

void ui_squareline_show_page(uint8_t page)
{
    ui_page_show(page);
}

void ui_squareline_prev_page(void)
{
    ui_page_prev();
}

void ui_squareline_next_page(void)
{
    ui_page_next();
}

void ui_squareline_select_page(void)
{
    ui_page_select();
}

lv_obj_t *ui_squareline_init(void)
{
    ui_page_screen_init();
    return ui_page_get_screen();
}

void ui_squareline_destroy(void)
{
    ui_page_screen_destroy();
}
