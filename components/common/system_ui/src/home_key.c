/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board ADC keys -> 1.0 home.
 *
 * VOL- / VOL+ move the focus (press-down, so navigation feels immediate).
 * MODE and SET are timed here from press-down / press-up instead of the board
 * YAML long press (2 s is far too slow for push-to-talk): a hold of 0.6 s is a
 * long press. MODE long = push-to-talk (release sends), SET long = control
 * centre; short presses are "open" and "back / overview".
 *
 * The subscription path is the one lua_module_button uses, just from C:
 *   esp_board_manager_get_device_handle() -> dev_button_handles_t
 *   -> iot_button_register_cb(...)
 * Key identity comes from the YAML button_labels entry, same as the Lua module's
 * evt.label.
 */
#include "system_ui_private.h"
#include "home_priv.h"

#include <string.h>

#include "display_service.h"
#include "esp_check.h"
#include "esp_timer.h"

#ifdef CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT
#include "dev_button.h"
#include "esp_board_manager.h"
#endif /* CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT */

#define SYSTEM_UI_HOME_KEY_DEVICE_NAME "adc_button_group"
#define SYSTEM_UI_HOME_KEY_LONG_US     600000

typedef enum { HK_VOL_DOWN = 0, HK_VOL_UP, HK_MODE, HK_SET } hk_id_t;

/* ---------------------------------------------- 投递（实体键 / UART 共用） */

void system_ui_home_key_post(hv_key_t key)
{
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_HOME_KEY,
        .generation = s_ui.generation,
        .home_key = (system_ui_home_key_t)key,
    };
    if (s_ui.started) {
        (void)system_ui_post_work_event(&event, 0);
    }
}

static void hk_delayed_cb(void *arg)
{
    system_ui_home_key_post((hv_key_t)(intptr_t)arg);
}

void system_ui_home_key_post_delayed(hv_key_t key, uint32_t ms)
{
    esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t args = {
        .callback = hk_delayed_cb,
        .arg = (void *)(intptr_t)key,
        .name = "hk_delay",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        /* 一次性：触发后不再使用，句柄泄漏量可以忽略（仅测试注入用） */
        (void)esp_timer_start_once(t, (uint64_t)ms * 1000);
    }
}

#ifdef CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT

typedef struct {
    button_handle_t handle;
    hk_id_t id;
} system_ui_home_key_binding_t;

static system_ui_home_key_binding_t s_bindings[CONFIG_ADC_BUTTON_MAX_BUTTON_PER_CHANNEL];
static int s_binding_count;
static esp_timer_handle_t s_long_timer[2];      /* MODE, SET */
static volatile bool s_long_fired[2];
static volatile bool s_down[2];

static bool system_ui_home_key_id_from_label(const char *label, hk_id_t *out)
{
    if (label == NULL) {
        return false;
    }
    if (strcmp(label, "VOLUME_DOWN") == 0) {
        *out = HK_VOL_DOWN;
        return true;
    }
    if (strcmp(label, "VOLUME_UP") == 0) {
        *out = HK_VOL_UP;
        return true;
    }
    if (strcmp(label, "MODE") == 0) {
        *out = HK_MODE;
        return true;
    }
    if (strcmp(label, "SET") == 0) {
        *out = HK_SET;
        return true;
    }
    return false;
}

static void hk_long_cb(void *arg)
{
    int which = (int)(intptr_t)arg;          /* 0 = MODE, 1 = SET */
    if (!s_down[which]) {
        return;
    }
    s_long_fired[which] = true;
    system_ui_home_key_post(which == 0 ? HV_KEY_M_LONG_START : HV_KEY_SET_LONG);
}

static int hk_find(void *button_handle)
{
    for (int i = 0; i < s_binding_count; i++) {
        if (s_bindings[i].handle == (button_handle_t)button_handle) {
            return i;
        }
    }
    return -1;
}

/* Runs in the iot_button esp_timer task, so it must not touch LVGL. */
static void system_ui_home_key_down_cb(void *button_handle, void *user_data)
{
    int i = hk_find(button_handle);
    (void)user_data;
    if (i < 0 || !s_ui.started) {
        return;
    }
    switch (s_bindings[i].id) {
    case HK_VOL_DOWN:
        system_ui_home_key_post(HV_KEY_PREV);
        break;
    case HK_VOL_UP:
        system_ui_home_key_post(HV_KEY_NEXT);
        break;
    case HK_MODE:
    case HK_SET: {
        int which = s_bindings[i].id == HK_MODE ? 0 : 1;
        s_down[which] = true;
        s_long_fired[which] = false;
        if (s_long_timer[which]) {
            (void)esp_timer_stop(s_long_timer[which]);
            (void)esp_timer_start_once(s_long_timer[which], SYSTEM_UI_HOME_KEY_LONG_US);
        }
        break;
    }
    }
}

static void system_ui_home_key_up_cb(void *button_handle, void *user_data)
{
    int i = hk_find(button_handle);
    (void)user_data;
    if (i < 0 || !s_ui.started) {
        return;
    }
    if (s_bindings[i].id == HK_MODE || s_bindings[i].id == HK_SET) {
        int which = s_bindings[i].id == HK_MODE ? 0 : 1;
        if (!s_down[which]) {
            return;
        }
        s_down[which] = false;
        if (s_long_timer[which]) {
            (void)esp_timer_stop(s_long_timer[which]);
        }
        if (s_long_fired[which]) {
            if (which == 0) {
                system_ui_home_key_post(HV_KEY_M_LONG_END);
            }
        } else {
            system_ui_home_key_post(which == 0 ? HV_KEY_M : HV_KEY_SET);
        }
    }
}

/* iot_button_unregister_cb() drops every callback registered for the event, not
 * just ours, so this is only safe while no other subscriber is active. */
static void system_ui_home_key_disarm(void)
{
    for (int i = 0; i < s_binding_count; i++) {
        (void)iot_button_unregister_cb(s_bindings[i].handle, BUTTON_PRESS_DOWN, NULL);
        (void)iot_button_unregister_cb(s_bindings[i].handle, BUTTON_PRESS_UP, NULL);
        (void)iot_button_unregister_cb(s_bindings[i].handle, BUTTON_SINGLE_CLICK, NULL);
    }
}

static esp_err_t system_ui_home_key_arm(void)
{
    esp_err_t ret = ESP_OK;

    for (int i = 0; i < s_binding_count; i++) {
        esp_err_t err = iot_button_register_cb(s_bindings[i].handle, BUTTON_PRESS_DOWN, NULL,
                                              system_ui_home_key_down_cb, NULL);
        if (err == ESP_OK && (s_bindings[i].id == HK_MODE || s_bindings[i].id == HK_SET)) {
            err = iot_button_register_cb(s_bindings[i].handle, BUTTON_PRESS_UP, NULL,
                                         system_ui_home_key_up_cb, NULL);
        }
        if (err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "subscribe home key %d failed: %s", i, esp_err_to_name(err));
            ret = err;
        }
    }
    return ret;
}

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
    for (int w = 0; w < 2; w++) {
        if (!s_long_timer[w]) {
            const esp_timer_create_args_t args = {
                .callback = hk_long_cb,
                .arg = (void *)(intptr_t)w,
                .name = w == 0 ? "hk_mode" : "hk_set",
            };
            (void)esp_timer_create(&args, &s_long_timer[w]);
        }
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
        hk_id_t id;

        if (dev_handles->button_handles[i] == NULL) {
            continue;
        }
        if (!system_ui_home_key_id_from_label(label, &id)) {
            continue;
        }
        s_bindings[s_binding_count].handle = dev_handles->button_handles[i];
        s_bindings[s_binding_count].id = id;
        s_binding_count++;
        ESP_LOGI(SYSTEM_UI_TAG, "home key '%s' -> %d", label, (int)id);
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
 * which takes ours with it. Re-arm when the exclusive session ends; disarm first
 * so a script that left its own subscription behind cannot make one press fire
 * twice. */
void system_ui_home_key_rearm(void)
{
    if (s_binding_count == 0) {
        return;
    }
    system_ui_home_key_disarm();
    s_down[0] = s_down[1] = false;
    (void)system_ui_home_key_arm();
}

#else /* !CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT */

esp_err_t system_ui_home_key_init(void)
{
    ESP_LOGI(SYSTEM_UI_TAG, "board button support disabled, home keys unavailable");
    return ESP_OK;
}

void system_ui_home_key_deinit(void) {}
void system_ui_home_key_rearm(void) {}

#endif /* CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT */

/* ------------------------------------------------------------ 分发 */

/* 原生页面（WorkBuddy、名片、会议纪要、红外）盖在首页上：M / SET 都是返回。 */
static bool system_ui_home_key_native_screens(hv_key_t key)
{
    bool back = key == HV_KEY_M || key == HV_KEY_SET || key == HV_KEY_SET_LONG;

    if (s_ui.workbuddy_visible) {
        if (back) {
            (void)system_ui_hide_workbuddy();
            system_ui_click_feedback();
        }
        return true;
    }
    if (s_ui.meeting_minutes_visible) {
        if (back) {
            (void)system_ui_hide_meeting_minutes();
            system_ui_click_feedback();
        }
        return true;
    }
    if (s_ui.infrared_remote_visible) {
        if (back) {
            (void)system_ui_hide_infrared_remote();
            system_ui_click_feedback();
        }
        return true;
    }
    if (s_ui.businesscard_visible) {
        if (key == HV_KEY_PREV) {
            system_ui_businesscard_prev_locked();
            system_ui_click_feedback();
        } else if (key == HV_KEY_NEXT) {
            system_ui_businesscard_next_locked();
            system_ui_click_feedback();
        } else if (back) {
            (void)system_ui_hide_businesscard();
            system_ui_click_feedback();
        }
        return true;
    }
    return false;
}

void system_ui_home_key_apply_locked(system_ui_home_key_t key)
{
    hv_key_t k = (hv_key_t)key;

    if (!s_ui.started || s_ui.home_screen == NULL) {
        return;
    }
    /* An app owning the display gets the keys to itself, except the system
     * escape hatch: holding SET for 0.6 s always leaves the app. */
    if (display_service_has_exclusive_session()) {
        if (k == HV_KEY_SET_LONG) {
            system_ui_work_event_t event = {
                .type = SYSTEM_UI_WORK_EVENT_APP_EXIT_SWIPE,
                .generation = s_ui.generation,
            };
            (void)system_ui_post_work_event(&event, 0);
            ESP_LOGI(SYSTEM_UI_TAG, "HV_KEY set-long: leaving the foreground app");
        }
        return;
    }
    /* The task panel sits on top of the home: M / SET close it. */
    if (s_ui.jobs_visible) {
        if (k == HV_KEY_SET || k == HV_KEY_M || k == HV_KEY_SET_LONG) {
            system_ui_jobs_set_visible_locked(false);
        }
        return;
    }
    if (system_ui_home_key_native_screens(k)) {
        return;
    }
    if (k != HV_KEY_M_LONG_END && k != HV_KEY_M_LONG_START) {
        system_ui_click_feedback();
    }
    ESP_LOGI(SYSTEM_UI_TAG, "HV_KEY %d view=%s", (int)k, hv_view_name(hv_view()));
    hv_key(k);
}
