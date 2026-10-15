/* SPDX-License-Identifier: Apache-2.0 */
#include "ble_host_apps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "claw_paths.h"
#include "display_service.h"
#include "local_app_store.h"
#include "mbedtls/base64.h"
#include "system_ui.h"
#include "cap_lua.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_PATH_MAX 256
#define APP_BASE64_CAPACITY (4 * ((LOCAL_APP_CHUNK_MAX + 2) / 3) + 1)

static const char *field(const cJSON *obj, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t write_chunk(local_app_store_handle_t store, const cJSON *request)
{
    const char *data = field(request, "data");
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(request, "offset");
    if (!data || strlen(data) > 4 * ((LOCAL_APP_CHUNK_MAX + 2) / 3) || !cJSON_IsNumber(offset) ||
        offset->valuedouble < 0 || offset->valuedouble > LOCAL_APP_ICON_SIZE ||
        offset->valuedouble != (double)(size_t)offset->valuedouble)
        return ESP_ERR_INVALID_ARG;
    uint8_t *bytes = malloc(LOCAL_APP_CHUNK_MAX);
    if (!bytes)
        return ESP_ERR_NO_MEM;
    size_t size = 0;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (mbedtls_base64_decode(bytes, LOCAL_APP_CHUNK_MAX, &size, (const unsigned char *)data, strlen(data)) ==
        0) {
        err = local_app_store_write(store, field(request, "app_id"), field(request, "file"),
                                    (size_t)offset->valuedouble, bytes, size);
    }
    free(bytes);
    return err;
}

static esp_err_t get_info(local_app_store_handle_t store, const char *id, cJSON **out)
{
    cJSON *manifest = NULL;
    esp_err_t err = local_app_store_get(store, id, &manifest);
    if (err != ESP_OK)
        return err;
    cJSON *info = cJSON_CreateObject();
    char *path = malloc(APP_PATH_MAX);
    char *directory = malloc(APP_PATH_MAX);
    if (directory)
        err = local_app_store_directory(store, id, directory, APP_PATH_MAX);
    if (!directory)
        err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        cJSON_Delete(manifest);
        cJSON_Delete(info);
        free(path);
        free(directory);
        return err;
    }
    if (!info || !path || !cJSON_AddItemToObject(info, "manifest", manifest)) {
        cJSON_Delete(manifest);
        cJSON_Delete(info);
        free(path);
        free(directory);
        return ESP_ERR_NO_MEM;
    }
    bool ok = cJSON_AddStringToObject(info, "source", "user") &&
              cJSON_AddStringToObject(info, "storage_root", claw_paths_get(CLAW_PATH_DATA));
    snprintf(path, APP_PATH_MAX, LOCAL_APP_DIRECTORY "/%s", directory);
    ok = ok && cJSON_AddStringToObject(info, "directory", path);
    snprintf(path, APP_PATH_MAX, LOCAL_APP_DIRECTORY "/%s/main.lua", directory);
    ok = ok && cJSON_AddStringToObject(info, "script", path);
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(manifest, "files");
    const char *keys[] = {"icon_selected", "icon_unselected"};
    for (size_t i = 0; i < 2 && ok; ++i) {
        snprintf(path, APP_PATH_MAX, "%s.bin", keys[i]);
        if (cJSON_HasObjectItem(files, path)) {
            snprintf(path, APP_PATH_MAX, LOCAL_APP_DIRECTORY "/%s/%s.bin", directory, keys[i]);
            ok = cJSON_AddStringToObject(info, keys[i], path) != NULL;
        } else {
            ok = cJSON_AddNullToObject(info, keys[i]) != NULL;
        }
    }
    free(directory);
    free(path);
    if (!ok) {
        cJSON_Delete(info);
        return ESP_ERR_NO_MEM;
    }
    *out = info;
    return ESP_OK;
}

static esp_err_t read_chunk(local_app_store_handle_t store, const cJSON *request, cJSON **out)
{
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(request, "offset");
    const cJSON *length = cJSON_GetObjectItemCaseSensitive(request, "length");
    if (!cJSON_IsNumber(offset) || offset->valuedouble < 0 || offset->valuedouble > LOCAL_APP_ICON_SIZE ||
        offset->valuedouble != (double)(size_t)offset->valuedouble || !cJSON_IsNumber(length) ||
        length->valuedouble < 1 || length->valuedouble > LOCAL_APP_CHUNK_MAX ||
        length->valuedouble != (double)(size_t)length->valuedouble)
        return ESP_ERR_INVALID_ARG;
    uint8_t *bytes = malloc(LOCAL_APP_CHUNK_MAX);
    char *encoded = malloc(APP_BASE64_CAPACITY);
    if (!bytes || !encoded) {
        free(bytes);
        free(encoded);
        return ESP_ERR_NO_MEM;
    }
    size_t size = 0, total = 0, encoded_size = 0;
    const char *file = field(request, "file");
    esp_err_t err =
        local_app_store_read(store, field(request, "app_id"), file, field(request, "sha256"),
                             (size_t)offset->valuedouble, bytes, (size_t)length->valuedouble, &size, &total);
    if (err == ESP_OK &&
        mbedtls_base64_encode((unsigned char *)encoded, APP_BASE64_CAPACITY, &encoded_size, bytes, size) != 0)
        err = ESP_FAIL;
    if (err == ESP_OK) {
        encoded[encoded_size] = '\0';
        cJSON *result = cJSON_CreateObject();
        bool ok = result && cJSON_AddStringToObject(result, "file", file) &&
                  cJSON_AddNumberToObject(result, "offset", offset->valuedouble) &&
                  cJSON_AddNumberToObject(result, "size", total) &&
                  cJSON_AddBoolToObject(result, "eof", (size_t)offset->valuedouble + size == total) &&
                  cJSON_AddStringToObject(result, "data", encoded);
        if (ok)
            *out = result;
        else {
            cJSON_Delete(result);
            err = ESP_ERR_NO_MEM;
        }
    }
    free(bytes);
    free(encoded);
    return err;
}

/* A background Lua job may still read assets even without an exclusive display.
 * Use its path rather than the job name, since run_lua permits arbitrary names. */
static bool app_is_running(local_app_store_handle_t store, const char *id)
{
    char *relative = malloc(APP_PATH_MAX), *needle = malloc(APP_PATH_MAX), *jobs = calloc(16384, 1);
    if (!relative || !needle || !jobs) {
        free(relative);
        free(needle);
        free(jobs);
        return true;
    }
    bool busy = false;
    if (local_app_store_directory(store, id, relative, APP_PATH_MAX) == ESP_OK) {
        int n = snprintf(needle, APP_PATH_MAX, "path=%s/apps/%s/", claw_paths_get(CLAW_PATH_DATA), relative);
        busy = n < 0 || n >= APP_PATH_MAX || cap_lua_list_jobs("running", jobs, 16384) != ESP_OK ||
               strstr(jobs, needle) != NULL;
        if (!busy)
            busy = cap_lua_list_jobs("queued", jobs, 16384) != ESP_OK || strstr(jobs, needle) != NULL;
    }
    free(relative);
    free(needle);
    free(jobs);
    return busy;
}

/* 1.0：手机上确认卸载时，背夹前台若有应用在独占显示，先把它停掉回到首页——
 * 运行中的应用占着自己的文件删不掉，以前直接回 invalid_state，手机上看着像“卸载没反应”。
 * 必须在拿 display 锁之前做：应用退出时自己也要用这把锁。 */
static void stop_foreground_app(void)
{
    char *out = calloc(1, 1024);
    if (out) {
        (void)cap_lua_stop_all_jobs("display", 3000, out, 1024);
        free(out);
    }
    (void)system_ui_show_home();
    for (int i = 0; i < 30 && display_service_has_exclusive_session(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t ble_host_apps_handle(const char *op, const cJSON *request, char **output)
{
    *output = NULL;
    if (strcmp(op, "app_remove") == 0 && display_service_is_started() && display_service_has_exclusive_session()) {
        stop_foreground_app();
    }
    char *root = malloc(APP_PATH_MAX);
    if (!root)
        return ESP_ERR_NO_MEM;
    esp_err_t err = claw_paths_join(CLAW_PATH_DATA, LOCAL_APP_DIRECTORY, root, APP_PATH_MAX);
    local_app_store_handle_t store = NULL;
    const local_app_store_config_t config = {.root = root};
    /* Home readers hold this same lock. Publication/recovery must not overlap
     * a reader between group and summary writes. Headless devices have no UI. */
    bool locked = false;
    if (err == ESP_OK && display_service_is_started()) {
        err = display_service_lock();
        locked = err == ESP_OK;
    }
    if (err == ESP_OK)
        err = local_app_store_create(&config, &store);
    free(root);
    if (err != ESP_OK) {
        if (locked)
            display_service_unlock();
        return err;
    }
    const char *id = field(request, "app_id");
    /* 1.0 统一 ID：应用包可带 "app:" 前缀（apps_all 返回的就是这种） */
    if (id && strncmp(id, "app:", 4) == 0) {
        id += 4;
    }
    cJSON *result = NULL;
    bool refresh = false;
    if (strcmp(op, "app_list") == 0) {
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(request, "offset");
        if (offset &&
            (!cJSON_IsNumber(offset) || offset->valuedouble < 0 || offset->valuedouble > UINT32_MAX ||
             offset->valuedouble != (double)(uint32_t)offset->valuedouble))
            err = ESP_ERR_INVALID_ARG;
        else
            err = local_app_store_list(store, offset ? (size_t)offset->valuedouble : 0, &result);
    } else if (strcmp(op, "app_info") == 0) {
        err = get_info(store, id, &result);
    } else if (strcmp(op, "app_read") == 0) {
        err = read_chunk(store, request, &result);
    } else if (strcmp(op, "app_begin") == 0) {
        err = local_app_store_begin(store, cJSON_GetObjectItemCaseSensitive(request, "manifest"));
    } else if (strcmp(op, "app_write") == 0) {
        err = write_chunk(store, request);
    } else if (strcmp(op, "app_commit") == 0) {
        err = local_app_store_commit(store, id);
        refresh = err == ESP_OK;
    } else if (strcmp(op, "app_abort") == 0) {
        err = local_app_store_abort(store, id);
    } else if (strcmp(op, "app_remove") == 0) {
        /* Running scripts may still open assets. Stop them before removal. */
        err = (display_service_has_exclusive_session() || app_is_running(store, id))
                  ? ESP_ERR_INVALID_STATE
                  : local_app_store_remove(store, id);
        refresh = err == ESP_OK;
    } else if (strcmp(op, "app_rename") == 0) {
        err = local_app_store_rename(store, id, field(request, "title"));
        refresh = err == ESP_OK;
    } else if (strcmp(op, "app_reload") == 0) {
        err = local_app_store_reload(store);
        refresh = err == ESP_OK;
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    local_app_store_delete(store);
    if (locked)
        display_service_unlock();
    if (refresh) {
        /* Persistence already succeeded even if the display is unavailable. */
        esp_err_t reload = system_ui_schedule_home_reload();
        result = cJSON_CreateObject();
        if (result) {
            cJSON_AddBoolToObject(result, "saved", strcmp(op, "app_reload") != 0);
            cJSON_AddStringToObject(result, "refresh", reload == ESP_OK ? "when_home" : "restart_required");
        }
    }
    if (err == ESP_OK) {
        *output = result ? cJSON_PrintUnformatted(result) : strdup("{}");
        if (!*output)
            err = ESP_ERR_NO_MEM;
    }
    cJSON_Delete(result);
    return err;
}
