/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board ADC keys -> home carousel.
 *
 * The carousel screen already offers PREV / NEXT / M OPEN as on-screen hit
 * targets. This file subscribes to the board's physical keys and drives the very
 * same ui_squareline_{prev,next,select}_page() entry points, so a key press and a
 * tap are indistinguishable: same slide animation, same settle timer, same select
 * dispatch.
 *
 * The subscription path is the one lua_module_button uses, just from C:
 *   esp_board_manager_get_device_handle() -> dev_button_handles_t
 *   -> iot_button_register_cb(BUTTON_SINGLE_CLICK)
 * Key identity comes from the YAML button_labels entry, same as the Lua module's
 * evt.label, so the mapping below reads like the one in meeting_presenter.lua.
 */
#include "system_ui_private.h"

#include <string.h>

#include "display_service.h"
#include "esp_check.h"
#include "ui_squareline.h"

#ifdef CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT
#include "dev_button.h"
#include "esp_board_manager.h"
#endif /* CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT */

#define SYSTEM_UI_HOME_KEY_DEVICE_NAME "adc_button_group"

#ifdef CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT

/* One entry per physical key we care about. The board may expose more (SET on
 * this hardware); keys with no mapping are simply left unsubscribed. */
typedef struct {
    button_handle_t      handle;
    system_ui_home_key_t action;
} system_ui_home_key_binding_t;

static system_ui_home_key_binding_t s_bindings[CONFIG_ADC_BUTTON_MAX_BUTTON_PER_CHANNEL];
static int  s_binding_count;
static bool s_armed;

/* VOL- goes back, VOL+ goes forward, MODE opens the selected app. This is the
 * mapping the physical labels imply, and it matches meeting_presenter.lua so the
 * keys mean the same thing on the home screen and inside the app. */
static bool system_ui_home_key_action_from_label(const char *label, system_ui_home_key_t *out)
{
    if (label == NULL) {
        return false;
    }
    if (strcmp(label, "VOLUME_DOWN") == 0) {
        *out = SYSTEM_UI_HOME_KEY_PREV;
        return true;
    }
    if (strcmp(label, "VOLUME_UP") == 0) {
        *out = SYSTEM_UI_HOME_KEY_NEXT;
        return true;
    }
    if (strcmp(label, "MODE") == 0) {
        *out = SYSTEM_UI_HOME_KEY_SELECT;
        return true;
    }
    return false;
}

/* Runs in the iot_button esp_timer task, so it must not touch LVGL: post the
 * action and let the UI event task apply it under the display lock. */
static void system_ui_home_key_event_cb(void *button_handle, void *user_data)
{
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_HOME_KEY,
    };

    (void)user_data;

    if (!s_ui.started) {
        return;
    }
    for (int i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].handle != (button_handle_t)button_handle) {
            continue;
        }
        event.generation = s_ui.generation;
        event.home_key = s_bindings[i].action;
        (void)system_ui_post_work_event(&event, 0);
        return;
    }
}

/* iot_button_unregister_cb() drops every callback registered for the event, not
 * just ours, so this is only safe while no other subscriber is active. Callers
 * below make sure of that. */
static void system_ui_home_key_disarm(void)
{
    for (int i = 0; i < s_binding_count; i++) {
        (void)iot_button_unregister_cb(s_bindings[i].handle, BUTTON_SINGLE_CLICK, NULL);
    }
    s_armed = false;
}

static esp_err_t system_ui_home_key_arm(void)
{
    esp_err_t ret = ESP_OK;

    for (int i = 0; i < s_binding_count; i++) {
        esp_err_t err = iot_button_register_cb(s_bindings[i].handle, BUTTON_SINGLE_CLICK,
                                              NULL, system_ui_home_key_event_cb, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "subscribe home key %d failed: %s", i, esp_err_to_name(err));
            ret = err;
        }
    }
    s_armed = true;
    return ret;
}

/* single_click rather than press_down: the carousel slide takes a full second, so
 * a debounced click is what we want, and it keeps the behaviour identical to the
 * Lua scripts driving the same keys. */
esp_err_t system_ui_home_key_init(void)
{
    void *raw_handle = NULL;
    void *raw_config = NULL;
    dev_button_handles_t *dev_handles;
    dev_button_config_t  *dev_cfg;
    bool is_multi;

    if (s_binding_count > 0) {
        return ESP_OK;
    }
    if (!esp_board_manager_check_name(SYSTEM_UI_HOME_KEY_DEVICE_NAME)) {
        ESP_LOGI(SYSTEM_UI_TAG, "board device '%s' not found, home keys disabled",
                 SYSTEM_UI_HOME_KEY_DEVICE_NAME);
        return ESP_OK;
    }

    /* The device may be declared with init_skip, so bring it up on demand -
     * same fallback lua_module_button.c uses. */
    if (esp_board_manager_get_device_handle(SYSTEM_UI_HOME_KEY_DEVICE_NAME, &raw_handle) != ESP_OK ||
            raw_handle == NULL) {
        esp_err_t init_err = esp_board_manager_init_device_by_name(SYSTEM_UI_HOME_KEY_DEVICE_NAME);
        if (init_err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "init '%s' failed: %s, home keys disabled",
                     SYSTEM_UI_HOME_KEY_DEVICE_NAME, esp_err_to_name(init_err));
            return ESP_OK;
        }
        ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(SYSTEM_UI_HOME_KEY_DEVICE_NAME, &raw_handle),
                            SYSTEM_UI_TAG, "get home key device handle failed");
    }
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(SYSTEM_UI_HOME_KEY_DEVICE_NAME, &raw_config),
                        SYSTEM_UI_TAG, "get home key device config failed");
    ESP_RETURN_ON_FALSE(raw_handle != NULL && raw_config != NULL, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "home key device not initialized");

    dev_handles = (dev_button_handles_t *)raw_handle;
    dev_cfg     = (dev_button_config_t *)raw_config;
    is_multi    = dev_cfg->sub_type != NULL && strcmp(dev_cfg->sub_type, "adc_multi") == 0;

    if (!is_multi) {
        ESP_LOGI(SYSTEM_UI_TAG, "board device '%s' is not adc_multi, home keys disabled",
                 SYSTEM_UI_HOME_KEY_DEVICE_NAME);
        return ESP_OK;
    }

    for (int i = 0; i < dev_handles->num_buttons &&
            i < CONFIG_ADC_BUTTON_MAX_BUTTON_PER_CHANNEL; i++) {
        const char *label = dev_cfg->sub_cfg.adc.multi.button_labels[i];
        system_ui_home_key_t action;

        if (dev_handles->button_handles[i] == NULL) {
            continue;
        }
        if (!system_ui_home_key_action_from_label(label, &action)) {
            continue;
        }
        s_bindings[s_binding_count].handle = dev_handles->button_handles[i];
        s_bindings[s_binding_count].action = action;
        s_binding_count++;
        ESP_LOGI(SYSTEM_UI_TAG, "home key '%s' -> action %d", label, (int)action);
    }

    if (s_binding_count == 0) {
        ESP_LOGW(SYSTEM_UI_TAG, "no usable labels on '%s', home keys disabled",
                 SYSTEM_UI_HOME_KEY_DEVICE_NAME);
        return ESP_OK;
    }
    return system_ui_home_key_arm();
}

void system_ui_home_key_deinit(void)
{
    if (s_binding_count == 0) {
        return;
    }
    system_ui_home_key_disarm();
    memset(s_bindings, 0, sizeof(s_bindings));
    s_binding_count = 0;
}

/* A Lua app that subscribed to the same keys drops every callback for the event
 * when it exits (button.off -> iot_button_unregister_cb clears the whole event),
 * which takes ours with it. The script's cleanup runs before it releases the
 * display, so re-arming when the exclusive session ends restores the home keys.
 * Disarm first so a script that left its own subscription behind cannot make one
 * press fire twice. */
void system_ui_home_key_rearm(void)
{
    if (s_binding_count == 0) {
        return;
    }
    system_ui_home_key_disarm();
    (void)system_ui_home_key_arm();
}

void system_ui_home_key_apply_locked(system_ui_home_key_t key)
{
    /* An app owning the display gets the keys to itself; the carousel behind it
     * must not slide, or exiting the app would land on a different page. */
    if (!s_ui.started || s_ui.home_screen == NULL || display_service_has_exclusive_session()) {
        return;
    }
    /* The task panel sits on top of the carousel and has its own controls. */
    if (s_ui.jobs_visible) {
        return;
    }

    /* The workbuddy screen is a plain page rather than an app, so MODE backs out
     * of it the same way it opens it. PREV/NEXT are swallowed: there is no
     * carousel behind the screen to slide, and letting them through would move
     * the page the user returns to. */
    if (s_ui.workbuddy_visible) {
        if (key == SYSTEM_UI_HOME_KEY_SELECT) {
            (void)system_ui_hide_workbuddy();
            system_ui_click_feedback();
        }
        return;
    }

    if (s_ui.meeting_minutes_visible) {
        if (key == SYSTEM_UI_HOME_KEY_SELECT) {
            (void)system_ui_hide_meeting_minutes();
            system_ui_click_feedback();
        }
        return;
    }

    if (s_ui.infrared_remote_visible) {
        if (key == SYSTEM_UI_HOME_KEY_SELECT) {
            (void)system_ui_hide_infrared_remote();
            system_ui_click_feedback();
        }
        return;
    }

    /* Business card screen: VOL- / VOL+ cycle through cards; MODE goes back. */
    if (s_ui.businesscard_visible) {
        switch (key) {
        case SYSTEM_UI_HOME_KEY_PREV:
            system_ui_businesscard_prev_locked();
            system_ui_click_feedback();
            break;
        case SYSTEM_UI_HOME_KEY_NEXT:
            system_ui_businesscard_next_locked();
            system_ui_click_feedback();
            break;
        case SYSTEM_UI_HOME_KEY_SELECT:
            (void)system_ui_hide_businesscard();
            system_ui_click_feedback();
            break;
        default:
            break;
        }
        return;
    }

    switch (key) {
    case SYSTEM_UI_HOME_KEY_PREV:
        ui_squareline_prev_page();
        break;
    case SYSTEM_UI_HOME_KEY_NEXT:
        ui_squareline_next_page();
        break;
    case SYSTEM_UI_HOME_KEY_SELECT:
        ui_squareline_select_page();
        break;
    default:
        return;
    }
    system_ui_click_feedback();
}

#else /* !CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT */

esp_err_t system_ui_home_key_init(void)
{
    ESP_LOGI(SYSTEM_UI_TAG, "board button support disabled, home keys unavailable");
    return ESP_OK;
}

void system_ui_home_key_deinit(void) {}
void system_ui_home_key_rearm(void) {}

void system_ui_home_key_apply_locked(system_ui_home_key_t key)
{
    (void)key;
}

#endif /* CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT */
