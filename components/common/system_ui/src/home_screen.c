/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 1.0: the default screen is the native HomeView (home_view.c). The SquareLine
 * carousel is no longer built; ui_squareline stays linked for its SD image
 * paths and the app-launch hook that the WorkBuddy audio path still uses.
 */
#include "system_ui_private.h"
#include "home_priv.h"

#include <string.h>

#include "claw_paths.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "ui_squareline.h"

void hv_platform_init(void);

void system_ui_home_update_locked(void)
{
    hv_refresh_status();
}

/* A screen asked to start a Lua script that ships with the firmware. Runs in the
 * LVGL event context, so this only resolves the path and posts a work event. */
static void system_ui_home_app_launch_cb(const char *script_rel_path, void *user_ctx)
{
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_LAUNCHER_SELECT,
        .generation = s_ui.generation,
    };
    const char *name;

    (void)user_ctx;
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

esp_err_t system_ui_create_home_locked(void)
{
    static bool platform_ready;

    if (!platform_ready) {
        hv_platform_init();
        platform_ready = true;
    }
    /* Business card images still resolve through ui_squareline's SD paths. */
    ui_squareline_set_img_base_path(claw_paths_get(CLAW_PATH_DATA));
    ui_squareline_set_app_launch_cb(system_ui_home_app_launch_cb, NULL);
    system_ui_home_apps_register_locked();

    s_ui.home_screen = hv_create_screen();
    ESP_RETURN_ON_FALSE(s_ui.home_screen != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG,
                        "create 1.0 home screen failed");
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
    hv_on_show();
    system_ui_unlock();
    return ESP_OK;
}

esp_err_t system_ui_reload_home(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "runtime not started");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    system_ui_home_apps_register_locked();
    hv_reload_apps();
    system_ui_unlock();
    return ESP_OK;
}

void system_ui_delete_home_locked(void)
{
    ui_squareline_set_app_launch_cb(NULL, NULL);
    hv_destroy();
    system_ui_home_apps_close_locked();
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
