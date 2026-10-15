/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "system_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t system_ui_start(const system_ui_config_t *config);
void system_ui_stop(void);
bool system_ui_is_started(void);

esp_err_t system_ui_show_home(void);
esp_err_t system_ui_reload_home(void);
esp_err_t system_ui_set_callbacks(const system_ui_callbacks_t *callbacks, void *user_ctx);
esp_err_t system_ui_update_network(const system_ui_network_state_t *state);
esp_err_t system_ui_set_activity(bool active);
esp_err_t system_ui_show_task_panel(bool visible);
esp_err_t system_ui_refresh_tasks(void);
void system_ui_click_feedback(void);

typedef struct {
    const char *online;
    const char *tasks;
    const char *credits;
    const char *battery;
    const char *record;
    const char *reply_status;
    const char *reply_text;
    const char *workbuddy_status;
} system_ui_workbuddy_update_t;

esp_err_t system_ui_update_workbuddy(const system_ui_workbuddy_update_t *update);

typedef esp_err_t (*system_ui_workbuddy_audio_tx_cb_t)(const uint8_t *opus,
                                                       size_t opus_size,
                                                       uint32_t seq,
                                                       void *user_ctx);

esp_err_t system_ui_workbuddy_set_audio_tx_callback(system_ui_workbuddy_audio_tx_cb_t cb,
                                                    void *user_ctx);
esp_err_t system_ui_workbuddy_start_audio_session(bool capture_mic);
esp_err_t system_ui_workbuddy_stop_audio_session(void);
esp_err_t system_ui_workbuddy_play_opus(const uint8_t *opus, size_t opus_size);

/*
 * Mic capture session state, for hosts that turn the uplink into speech
 * recognition. `source` names who started it ("button", "meeting", "host", ...)
 * and `reason` who stopped it ("button", "host", "timeout", "disconnect", ...).
 * The callback runs in the caller's context, which can be an LVGL event
 * handler holding the UI lock: it must only record state and return.
 */
typedef void (*system_ui_workbuddy_audio_state_cb_t)(bool capturing,
                                                     const char *source_or_reason,
                                                     void *user_ctx);

esp_err_t system_ui_workbuddy_set_audio_state_callback(system_ui_workbuddy_audio_state_cb_t cb,
                                                       void *user_ctx);
esp_err_t system_ui_workbuddy_start_audio_session_from(bool capture_mic, const char *source);
/* Same capture, but runs the mic script in its own job group without going
 * through the launcher, so an on-screen Lua app keeps running. */
esp_err_t system_ui_workbuddy_start_capture_background(const char *source);
esp_err_t system_ui_workbuddy_stop_audio_session_from(const char *reason);
bool system_ui_workbuddy_audio_capturing(void);
/* Mic chunks the uplink missed in the current/last session (single-slot handoff). */
uint32_t system_ui_workbuddy_audio_mic_gaps(void);
esp_err_t system_ui_show_workbuddy(void);

#ifdef __cplusplus
}
#endif
