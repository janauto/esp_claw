/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include <string.h>

#include "claw_paths.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "ui_squareline.h"

void system_ui_home_update_locked(void)
{
    /* The SquareLine main screen manages its own content; no dynamic label updates needed here. */
}

/* A SquareLine screen asked to start a Lua app. Runs in the LVGL event context,
 * so this only resolves the path and posts a work event; the UI event task then
 * hands it to the same launcher callback that launcher tiles use. */
static void system_ui_home_app_launch_cb(const char *script_rel_path, void *user_ctx)
{
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_LAUNCHER_SELECT,
        .generation = s_ui.generation,
    };
    const char *name;

    (void)user_ctx;

    /* Screens ship with the firmware, so their scripts live under the read-only
     * system root rather than the writable data partition. */
    if (claw_paths_join(CLAW_PATH_SYSTEM, script_rel_path,
                        event.launcher_action, sizeof(event.launcher_action)) != ESP_OK) {
        ESP_LOGE(SYSTEM_UI_TAG, "resolve app script path failed: %s", script_rel_path);
        return;
    }

    /* Use the script's file name as the job name so a second press replaces the
     * running instance instead of stacking another one. */
    name = strrchr(script_rel_path, '/');
    name = name ? name + 1 : script_rel_path;
    strlcpy(event.launcher_id, name, sizeof(event.launcher_id));
    strlcpy(event.launcher_title, name, sizeof(event.launcher_title));

    (void)system_ui_post_work_event(&event, 0);
}

/* Carousel selection.
 *
 * Registering this replaces ui_squareline's default dispatch, so every page that
 * is not handled here has to be forwarded to the Lua launcher by hand.
 *
 * Pages whose app_id names a native screen must be matched by name: the page
 * table gives them a NULL script, and there is no path to derive an app_id from.
 * Keep this in sync with s_default_pages[] in ui_page.c. */
#define SYSTEM_UI_HOME_APP_ID_WORKBUDDY "workbuddy"
#define SYSTEM_UI_HOME_APP_ID_BUSINESSCARD "local_apps_8"
#define SYSTEM_UI_HOME_APP_ID_MEETING_MINUTES "local_apps_6"
#define SYSTEM_UI_HOME_APP_ID_INFRARED_REMOTE "local_apps_7"

static void system_ui_home_page_select_cb(uint32_t page, const char *app_id,
                                          const char *script, void *user_ctx)
{
    (void)page;
    (void)user_ctx;

    /* Reports failure and posts a work event internally, so a page whose screen
     * is unavailable does not fall through to a script it does not have. */
    if (app_id != NULL && strcmp(app_id, SYSTEM_UI_HOME_APP_ID_WORKBUDDY) == 0) {
        esp_err_t err = system_ui_show_workbuddy();
        if (err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "open workbuddy failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (app_id != NULL && strcmp(app_id, SYSTEM_UI_HOME_APP_ID_BUSINESSCARD) == 0) {
        esp_err_t err = system_ui_show_businesscard();
        if (err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "open businesscard failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (app_id != NULL && strcmp(app_id, SYSTEM_UI_HOME_APP_ID_MEETING_MINUTES) == 0) {
        esp_err_t err = system_ui_show_meeting_minutes();
        if (err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "open meeting minutes failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (app_id != NULL && strcmp(app_id, SYSTEM_UI_HOME_APP_ID_INFRARED_REMOTE) == 0) {
        esp_err_t err = system_ui_show_infrared_remote();
        if (err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "open infrared remote failed: %s", esp_err_to_name(err));
        }
        return;
    }

    /* Installed apps carry no script in the page table; start them by id. */
    if (script == NULL && system_ui_home_apps_launch(app_id)) {
        return;
    }

    ui_squareline_launch_app(script);
}

/* A confirmed uninstall from the long-press menu. Runs in the LVGL event
 * context, so the delete and the carousel rebuild go to the UI event task. */
static void system_ui_home_page_action_cb(uint32_t page, const char *app_id,
                                          ui_squareline_page_action_t action, void *user_ctx)
{
    (void)user_ctx;

    if (action == UI_SQUARELINE_PAGE_ACTION_UNINSTALL) {
        system_ui_home_apps_request_uninstall(page, app_id);
    }
}

static void system_ui_home_apps_request_rename_cb(uint32_t page, const char *id, const char *title, void *ctx)
{
    (void)ctx; system_ui_home_apps_request_rename(page, id, title);
}

esp_err_t system_ui_create_home_locked(void)
{
    /* Provide the runtime storage base path so screen files can load image
     * assets from the SD card or flash fatfs via the LVGL POSIX FS driver. */
    ui_squareline_set_img_base_path(claw_paths_get(CLAW_PATH_DATA));
    ui_squareline_set_app_launch_cb(system_ui_home_app_launch_cb, NULL);
    /* Take over selection so pages with a native screen can be routed to it. */
    ui_squareline_set_page_select_cb(system_ui_home_page_select_cb, NULL);
    ui_squareline_set_page_action_cb(system_ui_home_page_action_cb, NULL);
    ui_squareline_set_app_rename_cb(system_ui_home_apps_request_rename_cb, NULL);
    /* The long-press menu's 卸载 / 详细 are missing from the SquareLine fonts. */
    ui_squareline_set_menu_font(s_ui.font);
    /* Installed apps follow the built-in pages; the dots are sized at init. */
    system_ui_home_apps_register_locked();

    s_ui.home_screen = ui_squareline_init();
    ESP_RETURN_ON_FALSE(s_ui.home_screen != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG,
                        "create SquareLine home screen failed");
    system_ui_load_screen_locked(s_ui.home_screen);
    return ESP_OK;
}

esp_err_t system_ui_show_home(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.home_screen, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "runtime not started");
    ESP_RETURN_ON_FALSE(!display_service_has_exclusive_session(), ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "exclusive display session active");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    system_ui_load_screen_locked(s_ui.home_screen);
    system_ui_unlock();
    return ESP_OK;
}

esp_err_t system_ui_reload_home(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "runtime not started");
    ESP_RETURN_ON_FALSE(!display_service_has_exclusive_session(), ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "exclusive display session active");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    system_ui_delete_home_locked();
    esp_err_t err = system_ui_create_home_locked();
    if (err == ESP_OK) {
        display_service_set_default_screen_locked(s_ui.home_screen);
    } else {
        system_ui_delete_home_locked();
        display_service_set_default_screen_locked(NULL);
    }
    system_ui_unlock();
    return err;
}

void system_ui_delete_home_locked(void)
{
    ui_squareline_set_app_launch_cb(NULL, NULL);
    ui_squareline_set_page_select_cb(NULL, NULL);
    ui_squareline_set_page_action_cb(NULL, NULL);
    ui_squareline_set_app_rename_cb(NULL, NULL);
    if (s_ui.home_screen) {
        ui_squareline_destroy();
    }
    system_ui_home_apps_close_locked();
    ui_squareline_set_menu_font(NULL);
    s_ui.home_screen = NULL;
    s_ui.home_tile = NULL;
    s_ui.tileview = NULL;
    s_ui.launcher_first_tile = NULL;
    s_ui.status_label = NULL;
    s_ui.time_label = NULL;
    s_ui.date_label = NULL;
}

esp_err_t system_ui_set_network_status(bool sta_connected, const char *ap_ssid)
{
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_NETWORK_STATUS,
    };

    /* Network callbacks run from the system event task, so keep this path non-blocking and let the UI event task take the LVGL lock. */
    ESP_RETURN_ON_FALSE(s_ui.started, ESP_ERR_INVALID_STATE, SYSTEM_UI_TAG, "runtime not started");
    event.generation = s_ui.generation;
    event.network_status.sta_connected = sta_connected;
    strlcpy(event.network_status.ap_ssid, ap_ssid ? ap_ssid : "", sizeof(event.network_status.ap_ssid));
    return system_ui_post_work_event(&event, pdMS_TO_TICKS(100));
}
