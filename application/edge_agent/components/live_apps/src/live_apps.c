/* Adapter for the MVP 0.4.0 feed/UI components on the c38a2762 BLE baseline.
 * Does not start Classic Bluetooth, replace Wi-Fi ownership, or install apps. */
#include "live_apps.h"
#include "app_lua_modules.h"
#include "clip_feed.h"
#include "claw_paths.h"
#include "claw_skill.h"
#include "dev_button.h"
#include "display_service.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lua_module_clip_ui.h"
#include "nvs.h"
#include "system_ui.h"
#include "wifi_manager.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static struct {
    clip_feed_handle_t feeds;
    clip_live_handle_t live;
    SemaphoreHandle_t lock;
    QueueHandle_t keys;
} s_live;

esp_err_t live_apps_import_wifi(app_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    if (config->wifi_ssid[0]) return ESP_OK;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("pisen-live", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t size = sizeof(config->wifi_ssid);
    err = nvs_get_str(nvs, "ssid", config->wifi_ssid, &size);
    if (err == ESP_OK && config->wifi_ssid[0]) {
        size = sizeof(config->wifi_password);
        err = nvs_get_str(nvs, "password", config->wifi_password, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK; /* open network */
        if (err == ESP_OK) err = app_config_save(config);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    nvs_close(nvs);
    return err;
}

static bool wifi_status(char *ip, size_t size, void *ctx) {
    (void)ctx;
    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    snprintf(ip, size, "%s", status.sta_ip ? status.sta_ip : "");
    return status.sta_connected;
}

esp_err_t live_apps_feed_read(const char *id, char **text, int64_t *age, void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_live.lock, portMAX_DELAY);
    esp_err_t err = clip_feed_read(s_live.feeds, id, esp_timer_get_time() / 1000, text, age);
    xSemaphoreGive(s_live.lock);
    if (err == ESP_OK) {
        cJSON *payload = cJSON_Parse(*text);
        cJSON *via = cJSON_GetObjectItemCaseSensitive(payload, "via");
        /* Phone/SPP cache is retained but never advertised as a live BLE feed. */
        if (!cJSON_IsString(via) || strcmp(via->valuestring, "wifi") ||
            !clip_live_wifi_connected(s_live.live)) *age = -1;
        cJSON_Delete(payload);
    }
    return err;
}

static esp_err_t publish_feed(const char *id, const cJSON *payload, void *ctx) {
    (void)ctx;
    xSemaphoreTake(s_live.lock, portMAX_DELAY);
    esp_err_t err = clip_feed_update(s_live.feeds, id, payload, esp_timer_get_time() / 1000);
    xSemaphoreGive(s_live.lock);
    return err;
}

static bool paused(void *ctx) {
    (void)ctx;
    return system_ui_workbuddy_audio_capturing();
}
static bool refresh(const char *id, void *ctx) {
    (void)ctx;
    if (!clip_feed_id_valid(id)) return false;
    clip_live_refresh(s_live.live, id);
    return true;
}
static esp_err_t add_stock(const char *symbol, void *ctx) {
    (void)ctx;
    return clip_live_add_stock(s_live.live, symbol);
}
static esp_err_t action(const char *raw, char **response, void *ctx) {
    (void)ctx;
    cJSON *request = cJSON_Parse(raw), *result = NULL;
    esp_err_t err = clip_live_action(s_live.live, request, &result);
    cJSON_Delete(request);
    *response = result ? cJSON_PrintUnformatted(result) : NULL;
    cJSON_Delete(result);
    return err;
}

static const char *pop_key(void *ctx) {
    (void)ctx;
    unsigned key;
    if (xQueueReceive(s_live.keys, &key, 0) != pdTRUE) return NULL;
    const char *names[] = {"right", "left", "confirm", "exit"};
    return key < 4 ? names[key] : NULL;
}
static void key_down(void *button, void *ctx) {
    (void)button;
    unsigned key = (unsigned)(uintptr_t)ctx;
    if (lua_module_clip_ui_active()) xQueueSend(s_live.keys, &key, 0);
}
static esp_err_t register_ui(const char *root) {
    (void)root;
    return lua_module_clip_ui_register(pop_key, NULL);
}

esp_err_t live_apps_prepare(void) {
    if (s_live.live) return ESP_ERR_INVALID_STATE;
    s_live.lock = xSemaphoreCreateMutex();
    s_live.keys = xQueueCreate(16, sizeof(unsigned));
    if (!s_live.lock || !s_live.keys) return ESP_ERR_NO_MEM;
    esp_err_t err = clip_feed_create(claw_paths_get(CLAW_PATH_DATA), &s_live.feeds);
    if (err != ESP_OK) return err;
    clip_live_config_t config = {.publish = publish_feed, .read = live_apps_feed_read,
                                .paused = paused, .wifi_status = wifi_status};
    err = clip_live_create(&config, &s_live.live);
    if (err != ESP_OK) return err;
    lua_module_clip_ui_register_feeds(live_apps_feed_read, refresh, NULL);
    lua_module_clip_ui_register_stocks(add_stock);
    lua_module_clip_ui_register_actions(action);
    const app_lua_module_external_t module = {
        .module_id = "clip_ui", .display_name = "Weather / AIHOT / stocks", .reg = register_ui};
    return app_lua_modules_register_external(&module);
}

esp_err_t live_apps_start(void) {
    unsigned mask = 0;
    const char *ids[] = {"live_weather", "live_stocks", "live_aihot"};
    for (unsigned i = 0; i < 3; i++) {
        claw_skill_catalog_entry_t entry;
        if (claw_skill_get_catalog_entry(ids[i], &entry) == ESP_OK && entry.execution &&
            entry.execution->entry && entry.execution->visible) mask |= 1U << i;
    }
    clip_live_set_apps(s_live.live, mask);
    void *raw = NULL, *raw_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle("adc_button_group", &raw);
    if (err != ESP_OK || !raw) return ESP_ERR_NOT_FOUND;
    err = esp_board_manager_get_device_config("adc_button_group", &raw_config);
    if (err != ESP_OK || !raw_config) return ESP_ERR_NOT_FOUND;
    dev_button_handles_t *buttons = raw;
    dev_button_config_t *cfg = raw_config;
    const char *labels[] = {"VOLUME_UP", "VOLUME_DOWN", "MODE", "SET"};
    for (int i = 0; i < buttons->num_buttons; i++) {
        const char *label = cfg->sub_cfg.adc.multi.button_labels[i];
        for (unsigned key = 0; label && key < 4; key++) {
            if (strcmp(label, labels[key])) continue;
            err = iot_button_register_cb(buttons->button_handles[i], BUTTON_PRESS_DOWN,
                                         NULL, key_down, (void *)(uintptr_t)key);
            if (err != ESP_OK) return err;
        }
    }
    err = live_apps_cli_register();
    if (err != ESP_OK) return err;
    err = clip_live_start(s_live.live);
    if (err == ESP_OK) ESP_LOGI("live_apps", "BLE-compatible feeds ready mask=%u", mask);
    return err;
}

clip_live_handle_t live_apps_network(void) { return s_live.live; }
