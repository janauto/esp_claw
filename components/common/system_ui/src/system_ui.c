/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"

EXT_RAM_BSS_ATTR system_ui_state_t s_ui;

static const char *SYSTEM_UI_DISPLAY_OWNER_NAME = "system_ui";

#define SYSTEM_UI_JOBS_SWIPE_EDGE_PX 32
#define SYSTEM_UI_JOBS_SWIPE_TRIGGER_PX 54
#define SYSTEM_UI_JOBS_SWIPE_HORIZONTAL_TOL_PX 48
#define SYSTEM_UI_BOTTOM_SWIPE_EDGE_PX 40
#define SYSTEM_UI_BOTTOM_SWIPE_TRIGGER_PX 54
#define SYSTEM_UI_BOTTOM_SWIPE_HORIZONTAL_TOL_PX 64
#define SYSTEM_UI_EVENT_TASK_STACK 6144
#define SYSTEM_UI_EVENT_TASK_PRIO 4

/* 底部上滑退出手势开关。宏存在时手势被禁用。1.0 按首页设计启用：
 * 底部上滑永远回首页（应用里 = 退出应用，首页 = 应用总览）。 */
/* #define SYSTEM_UI_BOTTOM_SWIPE_EXIT_DISABLED */
#include "home_priv.h"

#define SYSTEM_UI_HOME_TOP_EDGE_PX 72
#define SYSTEM_UI_HOME_BOTTOM_EDGE_PX 110
#define SYSTEM_UI_HOME_SWIPE_TRIGGER_PX 60

/* 开机动画开关。关闭此宏则直接进入主界面。 */
#define SYSTEM_UI_BOOT_SPLASH_ENABLED

static void system_ui_handle_home_gesture(const system_ui_work_event_t *event);

static int32_t system_ui_abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}

esp_err_t system_ui_callback_lock(void)
{
    if (s_ui.callback_mutex == NULL) {
        s_ui.callback_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_ui.callback_mutex != NULL, ESP_ERR_NO_MEM,
                            SYSTEM_UI_TAG, "create callback mutex failed");
    }
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_ui.callback_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, SYSTEM_UI_TAG, "callback mutex timeout");
    return ESP_OK;
}

void system_ui_callback_unlock(void)
{
    if (s_ui.callback_mutex != NULL) {
        xSemaphoreGive(s_ui.callback_mutex);
    }
}

esp_err_t system_ui_post_work_event(const system_ui_work_event_t *event, TickType_t wait_ticks)
{
    ESP_RETURN_ON_FALSE(event != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "work event missing");
    ESP_RETURN_ON_FALSE(s_ui.event_queue != NULL, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "event queue not ready");
    ESP_RETURN_ON_FALSE(xQueueSend(s_ui.event_queue, event, wait_ticks) == pdTRUE,
                        ESP_ERR_TIMEOUT, SYSTEM_UI_TAG, "event queue full");
    return ESP_OK;
}

static void system_ui_handle_jobs_refresh_event(uint32_t generation)
{
    system_ui_job_item_t items[SYSTEM_UI_JOBS_MAX_ITEMS] = {0};
    size_t count = 0;
    system_ui_jobs_provider_cb_t provider_cb = NULL;
    void *provider_user_ctx = NULL;

    if (system_ui_callback_lock() == ESP_OK) {
        provider_cb = s_ui.jobs_provider_cb;
        provider_user_ctx = s_ui.jobs_provider_user_ctx;
        system_ui_callback_unlock();
    }

    if (provider_cb != NULL) {
        count = provider_cb(items, SYSTEM_UI_JOBS_MAX_ITEMS, provider_user_ctx);
        if (count > SYSTEM_UI_JOBS_MAX_ITEMS) {
            count = SYSTEM_UI_JOBS_MAX_ITEMS;
        }
    }

    if (system_ui_lock() == ESP_OK) {
        if (s_ui.started && generation == s_ui.generation) {
            s_ui.jobs_refresh_running = false;
            system_ui_jobs_refresh_snapshot_locked(items, count);
            if (s_ui.jobs_refresh_pending) {
                system_ui_jobs_request_refresh_locked();
            }
        }
        system_ui_unlock();
    }
}

static void system_ui_handle_jobs_action_event(const system_ui_work_event_t *event)
{
    if (event->generation != s_ui.generation) {
        return;
    }
    if (event->jobs_action.stop_all) {
        if (event->jobs_action.stop_all_cb != NULL) {
            (void)event->jobs_action.stop_all_cb(event->jobs_action.stop_all_user_ctx);
        }
    } else if (event->jobs_action.action_cb != NULL && event->jobs_action.job_id[0]) {
        (void)event->jobs_action.action_cb(event->jobs_action.job_id,
                                           event->jobs_action.action_user_ctx);
    }
    system_ui_handle_jobs_refresh_event(event->generation);
    if (system_ui_lock() == ESP_OK) {
        if (s_ui.started && event->generation == s_ui.generation) {
            s_ui.jobs_stop_task_running = false;
        }
        system_ui_unlock();
    }
}

static void system_ui_handle_network_status_event(const system_ui_work_event_t *event)
{
    if (system_ui_lock() != ESP_OK) {
        return;
    }
    if (s_ui.started && event->generation == s_ui.generation) {
        s_ui.sta_connected = event->network_status.sta_connected;
        strlcpy(s_ui.ap_ssid, event->network_status.ap_ssid, sizeof(s_ui.ap_ssid));
        system_ui_home_update_locked();
    }
    system_ui_unlock();
}

static void system_ui_event_task(void *arg)
{
    (void)arg;
    system_ui_work_event_t event = {0};

    while (!s_ui.event_task_stop) {
        if (xQueueReceive(s_ui.event_queue, &event, pdMS_TO_TICKS(250)) != pdTRUE) {
            system_ui_home_apps_process_reload();
            continue;
        }
        if (event.type == SYSTEM_UI_WORK_EVENT_STOP || s_ui.event_task_stop) {
            break;
        }

        switch (event.type) {
        case SYSTEM_UI_WORK_EVENT_SHOW_JOBS:
            if (system_ui_lock() == ESP_OK) {
                if (s_ui.started && event.generation == s_ui.generation) {
                    system_ui_jobs_set_visible_locked(true);
                }
                system_ui_unlock();
            }
            break;
        case SYSTEM_UI_WORK_EVENT_LAUNCHER_SELECT:
        {
            system_ui_launcher_select_cb_t cb = NULL;
            void *user_ctx = NULL;
            if (event.generation == s_ui.generation && system_ui_callback_lock() == ESP_OK) {
                cb = s_ui.launcher_select_cb;
                user_ctx = s_ui.launcher_select_user_ctx;
                system_ui_callback_unlock();
            }
            if (cb != NULL) {
                event.launcher_selection.id = event.launcher_id;
                event.launcher_selection.title = event.launcher_title;
                event.launcher_selection.action = event.launcher_action;
                event.launcher_selection.args_json =
                    event.launcher_args_json[0] ? event.launcher_args_json : NULL;
                cb(&event.launcher_selection, user_ctx);
            }
            break;
        }
        case SYSTEM_UI_WORK_EVENT_JOBS_REFRESH:
            system_ui_handle_jobs_refresh_event(event.generation);
            break;
        case SYSTEM_UI_WORK_EVENT_JOBS_ACTION:
            system_ui_handle_jobs_action_event(&event);
            break;
        case SYSTEM_UI_WORK_EVENT_NETWORK_STATUS:
            system_ui_handle_network_status_event(&event);
            break;
        case SYSTEM_UI_WORK_EVENT_APP_EXIT_SWIPE:
        {
            system_ui_app_exit_swipe_cb_t cb = NULL;
            void *user_ctx = NULL;
            if (event.generation == s_ui.generation && system_ui_callback_lock() == ESP_OK) {
                cb = s_ui.app_exit_swipe_cb;
                user_ctx = s_ui.app_exit_swipe_user_ctx;
                system_ui_callback_unlock();
            }
            if (cb != NULL) {
                esp_err_t err = cb(user_ctx);
                if (err != ESP_OK) {
                    ESP_LOGW(SYSTEM_UI_TAG, "app exit swipe callback failed: %s", esp_err_to_name(err));
                }
            }
            break;
        }
        case SYSTEM_UI_WORK_EVENT_HOME_KEY:
            if (system_ui_lock() == ESP_OK) {
                if (event.generation == s_ui.generation) {
                    system_ui_home_key_apply_locked(event.home_key);
                }
                system_ui_unlock();
            }
            break;
        case SYSTEM_UI_WORK_EVENT_APP_RENAME:
            if (event.generation == s_ui.generation) system_ui_home_apps_handle_rename(&event);
            break;
        case SYSTEM_UI_WORK_EVENT_APP_UNINSTALL:
            if (event.generation == s_ui.generation) {
                system_ui_home_apps_handle_uninstall(&event);
            }
            break;
        case SYSTEM_UI_WORK_EVENT_HOME_GESTURE:
            system_ui_handle_home_gesture(&event);
            break;
        case SYSTEM_UI_WORK_EVENT_VOICE_FINISH: {
            extern void hv_plat_voice_finish_now(void);
            hv_plat_voice_finish_now();
            break;
        }
        case SYSTEM_UI_WORK_EVENT_VOICE_EVENT:
        case SYSTEM_UI_WORK_EVENT_HOME_CALL:
            if (event.home_call.fn) {
                event.home_call.fn(event.home_call.arg, event.home_call.payload);
            }
            break;
        case SYSTEM_UI_WORK_EVENT_STOP:
        default:
            break;
        }
    }

    s_ui.event_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t system_ui_start_event_task(void)
{
    if (s_ui.event_queue == NULL) {
        s_ui.event_queue = xQueueCreate(SYSTEM_UI_EVENT_QUEUE_LEN,
                                        sizeof(system_ui_work_event_t));
        ESP_RETURN_ON_FALSE(s_ui.event_queue != NULL, ESP_ERR_NO_MEM,
                            SYSTEM_UI_TAG, "create event queue failed");
    }
    if (s_ui.event_task != NULL) {
        return ESP_OK;
    }

    s_ui.event_task_stop = false;
    BaseType_t ok = xTaskCreate(system_ui_event_task,
                                "ui_event",
                                SYSTEM_UI_EVENT_TASK_STACK,
                                NULL,
                                SYSTEM_UI_EVENT_TASK_PRIO,
                                &s_ui.event_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create event task failed");
    return ESP_OK;
}

static void system_ui_stop_event_task(void)
{
    TaskHandle_t task = s_ui.event_task;

    if (task != NULL && s_ui.event_queue != NULL) {
        system_ui_work_event_t event = {
            .type = SYSTEM_UI_WORK_EVENT_STOP,
            .generation = s_ui.generation,
        };
        s_ui.event_task_stop = true;
        (void)xQueueSend(s_ui.event_queue, &event, 0);

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SYSTEM_UI_STOP_TIMEOUT_MS);
        while (s_ui.event_task != NULL && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (s_ui.event_task != NULL) {
            vTaskDelete(s_ui.event_task);
            s_ui.event_task = NULL;
        }
    }

    if (s_ui.event_queue != NULL) {
        vQueueDelete(s_ui.event_queue);
        s_ui.event_queue = NULL;
    }
    s_ui.event_task_stop = false;
}

/* 手势识别状态。真实触摸与自动验收注入的触摸各用一份：触摸控制器空闲时也会
 * 不停送“未按下”采样，共用一份会把注入的手势中途清掉。 */
typedef struct {
    system_ui_touch_gesture_t gesture;
    int32_t start_x;
    int32_t start_y;
} system_ui_gesture_state_t;

static system_ui_gesture_state_t s_gesture_real;
static system_ui_gesture_state_t s_gesture_inject;
static volatile uint32_t s_gesture_fired_ms;

bool system_ui_gesture_recent(uint32_t window_ms)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return s_gesture_fired_ms != 0 && now - s_gesture_fired_ms < window_ms;
}

static void system_ui_gesture_feed(system_ui_gesture_state_t *st, const display_service_touch_sample_t *sample)
{
    if (sample->pressed) {
        if (st->gesture == SYSTEM_UI_TOUCH_GESTURE_NONE) {
            bool exclusive = display_service_has_exclusive_session();
            st->start_x = sample->x;
            st->start_y = sample->y;
#ifndef SYSTEM_UI_BOTTOM_SWIPE_EXIT_DISABLED
            if (exclusive && st->start_y >= (int32_t)s_ui.height - SYSTEM_UI_BOTTOM_SWIPE_EDGE_PX) {
                st->gesture = SYSTEM_UI_TOUCH_GESTURE_EXIT_APP;
            } else
#endif
            if (!exclusive && !s_ui.jobs_visible && st->start_y <= SYSTEM_UI_HOME_TOP_EDGE_PX) {
                st->gesture = SYSTEM_UI_TOUCH_GESTURE_HOME_TOP;
            } else if (!exclusive && st->start_y >= (int32_t)s_ui.height - SYSTEM_UI_HOME_BOTTOM_EDGE_PX) {
                st->gesture = SYSTEM_UI_TOUCH_GESTURE_HOME_BOTTOM;
            } else {
                /* 从中间按下的不是系统手势：标记为“已占用”，直到抬起 */
                st->gesture = SYSTEM_UI_TOUCH_GESTURE_BUSY;
            }
        } else if ((st->gesture == SYSTEM_UI_TOUCH_GESTURE_HOME_TOP &&
                    sample->y - st->start_y >= SYSTEM_UI_HOME_SWIPE_TRIGGER_PX) ||
                   (st->gesture == SYSTEM_UI_TOUCH_GESTURE_HOME_BOTTOM &&
                    st->start_y - sample->y >= SYSTEM_UI_HOME_SWIPE_TRIGGER_PX)) {
            if (system_ui_abs_i32(sample->x - st->start_x) <= SYSTEM_UI_BOTTOM_SWIPE_HORIZONTAL_TOL_PX) {
                system_ui_work_event_t event = {
                    .type = SYSTEM_UI_WORK_EVENT_HOME_GESTURE,
                    .generation = s_ui.generation,
                    .home_gesture = st->gesture == SYSTEM_UI_TOUCH_GESTURE_HOME_TOP ?
                                    SYSTEM_UI_HOME_GESTURE_PULL_DOWN_TOP : SYSTEM_UI_HOME_GESTURE_SWIPE_UP_BOTTOM,
                };
                (void)system_ui_post_work_event(&event, 0);
                s_gesture_fired_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                if (s_gesture_fired_ms == 0) {
                    s_gesture_fired_ms = 1;
                }
            }
            st->gesture = SYSTEM_UI_TOUCH_GESTURE_BUSY;     /* 一次按压只触发一次 */
        } else if (st->gesture == SYSTEM_UI_TOUCH_GESTURE_EXIT_APP &&
                   st->start_y - sample->y >= SYSTEM_UI_BOTTOM_SWIPE_TRIGGER_PX &&
                   system_ui_abs_i32(sample->x - st->start_x) <= SYSTEM_UI_BOTTOM_SWIPE_HORIZONTAL_TOL_PX) {
            system_ui_work_event_t event = {
                .type = SYSTEM_UI_WORK_EVENT_APP_EXIT_SWIPE,
                .generation = s_ui.generation,
            };
            (void)system_ui_post_work_event(&event, 0);
            st->gesture = SYSTEM_UI_TOUCH_GESTURE_BUSY;
        }
    } else {
        st->gesture = SYSTEM_UI_TOUCH_GESTURE_NONE;
    }
}

static void system_ui_touch_observer_cb(const display_service_touch_sample_t *sample, void *user_ctx)
{
    (void)user_ctx;
    if (sample == NULL || !s_ui.started) {
        return;
    }
    system_ui_gesture_feed(&s_gesture_real, sample);
    s_ui.touch_gesture = s_gesture_real.gesture;
}

bool system_ui_system_overlay_allowed(void)
{
    return display_service_exclusive_allows_system_overlay();
}

static void system_ui_state_observer_cb(display_service_state_event_t event, void *user_ctx)
{
    (void)user_ctx;

    if (!s_ui.started) {
        return;
    }

    if (system_ui_lock() != ESP_OK) {
        return;
    }

    switch (event) {
    case DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_ENTERED:
        if (!display_service_exclusive_allows_system_overlay()) {
            if (s_ui.jobs_visible) {
                system_ui_jobs_set_visible_locked(false);
            }
        }
        break;
    case DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_ENTERED:
        break;
    case DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_EXITED:
    case DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_EXITED:
        /* A Lua app that used the same board keys unregisters them on the way
         * out, and iot_button_unregister_cb() clears every callback for the
         * event - ours included. Re-subscribe now that the app is gone. */
        system_ui_home_key_rearm();
        /* 回到首页：重读「我的」快照、实时数据与应用列表变化 */
        hv_on_show();
        break;
    default:
        break;
    }

    system_ui_unlock();
}

void system_ui_touch_inject_sample(int32_t x, int32_t y, bool pressed)
{
    display_service_touch_sample_t sample = {0};
    sample.x = x;
    sample.y = y;
    sample.pressed = pressed;
    if (s_ui.started) {
        system_ui_gesture_feed(&s_gesture_inject, &sample);
    }
}

/* 首页手势：在 UI 事件任务里、持锁执行。 */
static void system_ui_handle_home_gesture(const system_ui_work_event_t *event)
{
    if (system_ui_lock() != ESP_OK) {
        return;
    }
    if (event->generation == s_ui.generation && !display_service_has_exclusive_session()) {
        bool native = s_ui.workbuddy_visible || s_ui.meeting_minutes_visible ||
                      s_ui.infrared_remote_visible || s_ui.businesscard_visible;
        if (event->home_gesture == SYSTEM_UI_HOME_GESTURE_SWIPE_UP_BOTTOM) {
            if (native) {
                /* 原生页面：底部上滑回首页 */
                if (s_ui.workbuddy_visible) (void)system_ui_hide_workbuddy();
                if (s_ui.meeting_minutes_visible) (void)system_ui_hide_meeting_minutes();
                if (s_ui.infrared_remote_visible) (void)system_ui_hide_infrared_remote();
                if (s_ui.businesscard_visible) (void)system_ui_hide_businesscard();
            } else if (hv_is_active()) {
                if (hv_view() == HV_VIEW_HOME) {
                    hv_open_overview();
                } else if (hv_view() == HV_VIEW_CONTROL) {
                    hv_close_control();
                } else if (hv_view() == HV_VIEW_OVERVIEW) {
                    hv_close_overview();
                }
            }
        } else if (event->home_gesture == SYSTEM_UI_HOME_GESTURE_PULL_DOWN_TOP && !native && hv_is_active()) {
            if (hv_view() == HV_VIEW_HOME) {
                hv_open_control();
            } else if (hv_view() == HV_VIEW_OVERVIEW) {
                hv_close_overview();
            }
        }
    }
    system_ui_unlock();
}

static bool system_ui_system_overlay_visible_locked(void)
{
    return s_ui.jobs_visible;
}

void system_ui_dummy_draw_suspend_for_overlay_locked(void)
{
    display_service_exclusive_raw_suspend_locked();
}

void system_ui_dummy_draw_resume_if_no_overlay_locked(void)
{
    if (!system_ui_system_overlay_visible_locked()) {
        display_service_exclusive_raw_resume_locked();
    }
}

static void system_ui_clear_callbacks_locked(void)
{
    if (system_ui_callback_lock() != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "clear callbacks skipped: callback lock failed");
        return;
    }
    s_ui.jobs_provider_cb = NULL;
    s_ui.jobs_provider_user_ctx = NULL;
    s_ui.jobs_action_cb = NULL;
    s_ui.jobs_action_user_ctx = NULL;
    s_ui.jobs_stop_all_cb = NULL;
    s_ui.jobs_stop_all_user_ctx = NULL;
    s_ui.app_exit_swipe_cb = NULL;
    s_ui.app_exit_swipe_user_ctx = NULL;
    s_ui.launcher_select_cb = NULL;
    s_ui.launcher_select_user_ctx = NULL;
    system_ui_callback_unlock();
}

static void system_ui_delete_ui_locked(void)
{
    s_ui.started = false;
    display_service_set_default_screen_locked(NULL);
    system_ui_delete_jobs_locked();
    system_ui_delete_overlay_locked();
    system_ui_delete_home_locked();
    system_ui_delete_workbuddy_locked();
    system_ui_delete_businesscard_locked();
    system_ui_delete_meeting_minutes_locked();
    system_ui_delete_infrared_remote_locked();
    system_ui_delete_boot_locked();
    system_ui_destroy_font_locked();
    system_ui_clear_callbacks_locked();
}

esp_err_t system_ui_start(const system_ui_config_t *config)
{
    esp_err_t ret;
    display_service_session_config_t display_session_config = {
        .owner_name = SYSTEM_UI_DISPLAY_OWNER_NAME,
        .mode = DISPLAY_SERVICE_MODE_SHARED_LVGL,
        .display_config = {
            .buffer_lines = config ? config->buffer_lines : 0,
            .tick_ms = config ? config->tick_ms : 0,
            .task_period_ms = config ? config->task_period_ms : 0,
        },
    };

    if (s_ui.started) {
        return ESP_OK;
    }

    hv_mem_init();
    s_ui.generation++;
    ESP_RETURN_ON_ERROR(system_ui_callback_lock(), SYSTEM_UI_TAG, "init callback mutex failed");
    system_ui_callback_unlock();
    ESP_RETURN_ON_ERROR(display_service_open(&display_session_config, &s_ui.display_session),
                        SYSTEM_UI_TAG, "open display session failed");
    lv_display_t *display = display_service_session_display(s_ui.display_session);
    ESP_GOTO_ON_FALSE(display != NULL, ESP_ERR_INVALID_STATE, fail, SYSTEM_UI_TAG, "display service LVGL display is NULL");
    s_ui.width = (uint32_t)lv_display_get_horizontal_resolution(display);
    s_ui.height = (uint32_t)lv_display_get_vertical_resolution(display);
    ESP_GOTO_ON_FALSE(s_ui.width > 0 && s_ui.height > 0, ESP_ERR_INVALID_STATE, fail, SYSTEM_UI_TAG, "display resolution is invalid");
    ESP_GOTO_ON_ERROR(system_ui_start_event_task(), fail, SYSTEM_UI_TAG, "start event task failed");
    ESP_GOTO_ON_ERROR(display_service_set_touch_observer(system_ui_touch_observer_cb, NULL),
                      fail, SYSTEM_UI_TAG, "set touch observer failed");
    ESP_GOTO_ON_ERROR(display_service_set_state_observer(system_ui_state_observer_cb, NULL),
                      fail, SYSTEM_UI_TAG, "set state observer failed");
    ESP_GOTO_ON_ERROR(system_ui_vibration_init(), fail, SYSTEM_UI_TAG, "init vibration failed");

    ESP_GOTO_ON_ERROR(system_ui_lock(), fail, SYSTEM_UI_TAG, "lock failed");
    system_ui_create_font_locked(config && config->font_path ? config->font_path : SYSTEM_UI_DEFAULT_FONT_PATH,
                                  config ? config->font_size : SYSTEM_UI_DEFAULT_FONT_SIZE);
#ifdef SYSTEM_UI_BOOT_SPLASH_ENABLED
    ret = system_ui_create_boot_locked();
    if (ret == ESP_OK) {
        display_service_set_default_screen_locked(s_ui.boot_screen);
        system_ui_load_screen_locked(s_ui.boot_screen);
    }
    system_ui_unlock();
    ESP_GOTO_ON_ERROR(ret, fail, SYSTEM_UI_TAG, "create boot UI failed");

    system_ui_run_boot_animation();

    ESP_GOTO_ON_ERROR(system_ui_lock(), fail, SYSTEM_UI_TAG, "lock failed");
#endif
    ret = system_ui_create_home_locked();
    if (ret == ESP_OK) {
        display_service_set_default_screen_locked(s_ui.home_screen);
    }
    if (ret == ESP_OK) {
        /* Built alongside the carousel but not shown; the home screen stays the
         * default until the workbuddy page asks for it. */
        ret = system_ui_create_workbuddy_locked();
    }
    if (ret == ESP_OK) {
        /* Business card screen, built alongside the carousel. */
        ret = system_ui_create_businesscard_locked();
    }
    if (ret == ESP_OK) {
        ret = system_ui_create_meeting_minutes_locked();
    }
    if (ret == ESP_OK) {
        ret = system_ui_create_infrared_remote_locked();
    }
    if (ret == ESP_OK) {
        ret = system_ui_create_overlay_locked();
    }
    if (ret == ESP_OK) {
        ret = system_ui_create_jobs_locked();
    }
    s_ui.started = ret == ESP_OK;
    if (ret == ESP_OK) {
        system_ui_load_screen_locked(s_ui.home_screen);
#ifdef SYSTEM_UI_BOOT_SPLASH_ENABLED
        system_ui_delete_boot_locked();
#endif
    }
    system_ui_unlock();
    ESP_GOTO_ON_ERROR(ret, fail, SYSTEM_UI_TAG, "create UI failed");

    /* Subscribe the board keys last: the callback checks s_ui.started, and the
     * home screen it drives now exists. A board without keys reports ESP_OK and
     * simply stays touch-only. */
    if (system_ui_home_key_init() != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "home key subscription incomplete, keys may not work");
    }

    return ESP_OK;

fail:
    system_ui_stop();
    return ret;
}

void system_ui_stop(void)
{
    s_ui.generation++;
    /* Drop the key subscription before the event queue goes away, so a press
     * cannot post into a queue that is being deleted. */
    system_ui_home_key_deinit();
    system_ui_stop_event_task();

    if (display_service_is_started() && system_ui_lock() == ESP_OK) {
        system_ui_delete_ui_locked();
        system_ui_unlock();
    } else {
        s_ui.started = false;
    }
    (void)display_service_set_touch_observer(NULL, NULL);
    (void)display_service_set_state_observer(NULL, NULL);
    system_ui_vibration_deinit();
    s_ui.touch_gesture = SYSTEM_UI_TOUCH_GESTURE_NONE;
    if (s_ui.display_session != NULL) {
        (void)display_service_close(s_ui.display_session);
        s_ui.display_session = NULL;
    }
}

bool system_ui_is_started(void)
{
    bool started = false;

    if (!display_service_is_started()) {
        return false;
    }
    if (system_ui_lock() == ESP_OK) {
        started = s_ui.started;
        system_ui_unlock();
    }
    return started;
}

esp_err_t system_ui_set_callbacks(const system_ui_callbacks_t *callbacks, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(callbacks != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "system UI callbacks missing");
    ESP_RETURN_ON_ERROR(system_ui_launcher_set_select_callback(callbacks->on_launcher_select, user_ctx),
                        SYSTEM_UI_TAG, "set launcher callback failed");
    ESP_RETURN_ON_ERROR(system_ui_jobs_set_provider(callbacks->get_tasks, user_ctx),
                        SYSTEM_UI_TAG, "set task provider failed");
    ESP_RETURN_ON_ERROR(system_ui_jobs_set_action_callback(callbacks->on_stop_task, user_ctx),
                        SYSTEM_UI_TAG, "set task action callback failed");
    ESP_RETURN_ON_ERROR(system_ui_jobs_set_stop_all_callback(callbacks->on_stop_all_tasks, user_ctx),
                        SYSTEM_UI_TAG, "set stop-all task callback failed");
    ESP_RETURN_ON_ERROR(system_ui_callback_lock(), SYSTEM_UI_TAG, "callback lock failed");
    s_ui.app_exit_swipe_cb = callbacks->on_app_exit_swipe;
    s_ui.app_exit_swipe_user_ctx = user_ctx;
    system_ui_callback_unlock();
    return ESP_OK;
}

esp_err_t system_ui_update_network(const system_ui_network_state_t *state)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "network state missing");
    return system_ui_set_network_status(state->sta_connected, state->ap_ssid);
}

esp_err_t system_ui_set_activity(bool active)
{
    return system_ui_overlay_set_visible(active);
}

esp_err_t system_ui_show_task_panel(bool visible)
{
    return system_ui_jobs_set_visible(visible);
}

esp_err_t system_ui_refresh_tasks(void)
{
    return system_ui_jobs_request_refresh();
}

esp_err_t system_ui_lock(void)
{
    return display_service_lock();
}

void system_ui_unlock(void)
{
    display_service_unlock();
}

void system_ui_load_screen_locked(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    if (display_service_has_exclusive_session()) {
        return;
    }
    lv_screen_load(screen);
}
