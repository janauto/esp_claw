/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Installed apps for the 1.0 home (overview, dock and the BLE/UART apps_all op).
 *
 * Two on-disk formats live side by side:
 *  - skills: DATA/skills/<id>/SKILL.md with a visible execution entry (Web Lua
 *    Studio, MVP companion apps). UI id "skill:<id>"; uninstall deletes the
 *    directory and reloads the skill registry; rename writes manifest.json.
 *  - packages: DATA/apps/gXXXXXXXX/<id>/app.json + main.lua (+ icons), managed by
 *    local_app_store. UI id "app:<id>"; uninstall/rename go through the store.
 * Firmware skills under the read-only system root are never listed here, so
 * they can never be deleted.
 */
#include "home_priv.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cap_lua.h"
#include "local_app_store.h"
#include "cJSON.h"
#include "claw_paths.h"
#include "claw_skill.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"

#define HOME_APPS_SKILLS_SUBDIR     "skills"
#define HOME_APPS_STOP_WAIT_MS      3000
#define HOME_APPS_STOP_OUTPUT_LEN   192
#define HOME_APPS_MAX_DEPTH         6
#define HOME_APPS_MAX_ENTRIES       512     /* per directory, guards a looping delete */
#define HOME_APPS_NAME_LEN          256     /* FAT long names are at most 255 bytes   */

#define HOME_PACKAGE_PREFIX "app:"
#define HOME_SKILL_PREFIX   "skill:"

static local_app_store_handle_t s_package_store;
static bool s_reload_requested;

typedef bool (*hv_plat_app_cb_t)(const hv_app_t *app, void *ctx);

/* "<DATA>/skills", the only place installed apps may live. */
static esp_err_t home_apps_skills_root(char *out, size_t out_size)
{
    return claw_paths_join(CLAW_PATH_DATA, HOME_APPS_SKILLS_SUBDIR, out, out_size);
}

/* True if @p skill_dir is exactly one directory below the data skills root, so
 * deleting it can only ever remove that one app. */
static bool home_apps_dir_is_installed(const char *skill_dir)
{
    char root[SYSTEM_UI_PATH_MAX];
    const char *name;
    size_t len;

    if (skill_dir == NULL || home_apps_skills_root(root, sizeof(root)) != ESP_OK) {
        return false;
    }
    len = strlen(root);
    if (strncmp(skill_dir, root, len) != 0 || skill_dir[len] != '/') {
        return false;
    }
    name = skill_dir + len + 1;
    return name[0] != '\0' && strchr(name, '/') == NULL &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static bool home_apps_entry_is_installed_app(const claw_skill_catalog_entry_t *entry)
{
    return entry != NULL && entry->id != NULL && entry->id[0] != '\0' &&
           entry->execution != NULL && entry->execution->visible &&
           entry->execution->entry != NULL && entry->execution->entry[0] == '/' &&
           home_apps_dir_is_installed(entry->skill_dir);
}

/* The Lua job name an app runs under: its id, cut at a UTF-8 boundary to fit the
 * launcher's id field. Launching and stopping must agree on it. */
static void home_apps_job_name(const char *app_id, char *out, size_t out_size)
{
    if (strncmp(app_id, HOME_PACKAGE_PREFIX, strlen(HOME_PACKAGE_PREFIX)) == 0) {
        if (local_app_store_job_name(app_id + strlen(HOME_PACKAGE_PREFIX), out, out_size) != ESP_OK) {
            out[0] = 0;
        }
        return;
    }
    if (strncmp(app_id, HOME_SKILL_PREFIX, strlen(HOME_SKILL_PREFIX)) == 0) {
        app_id += strlen(HOME_SKILL_PREFIX);
    }
    size_t len = strlcpy(out, app_id, out_size);

    if (len >= out_size) {
        len = out_size - 1;
        while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80) {
            len--;          /* out[len] continues a character: drop that character */
        }
        out[len] = '\0';
    }
}

/* ------------------------------------------------------------ store */

static local_app_store_handle_t home_apps_store(void)
{
    if (!s_package_store) {
        char *root = malloc(SYSTEM_UI_PATH_MAX);
        if (!root) {
            return NULL;
        }
        local_app_store_config_t config = {.root = root};
        esp_err_t err = claw_paths_join(CLAW_PATH_DATA, LOCAL_APP_DIRECTORY, root, SYSTEM_UI_PATH_MAX);
        if (err == ESP_OK) {
            err = local_app_store_create(&config, &s_package_store);
        }
        if (err != ESP_OK) {
            ESP_LOGW(SYSTEM_UI_TAG, "home package index unavailable: %s", esp_err_to_name(err));
            s_package_store = NULL;
        }
        free(root);
    }
    return s_package_store;
}

void system_ui_home_apps_close_locked(void)
{
    local_app_store_delete(s_package_store);
    s_package_store = NULL;
}

void system_ui_home_apps_register_locked(void)
{
    /* Re-open the package index so a commit from BLE/UART is picked up. */
    system_ui_home_apps_close_locked();
    (void)home_apps_store();
}

/* --------------------------------------------------------- enumerate */

static uint32_t home_apps_color(const cJSON *c, uint32_t def)
{
    if (cJSON_IsString(c) && c->valuestring[0] == '#' && strlen(c->valuestring) == 7) {
        return (uint32_t)strtoul(c->valuestring + 1, NULL, 16);
    }
    if (cJSON_IsNumber(c)) {
        return (uint32_t)c->valuedouble;
    }
    return def;
}

static cJSON *home_apps_read_json(const char *path)
{
    FILE *file = fopen(path, "rb");
    cJSON *json = NULL;
    if (file) {
        char *text = calloc(4097, 1);
        if (text) {
            size_t count = fread(text, 1, 4096, file);
            if (count < 4096 && !ferror(file)) {
                json = cJSON_Parse(text);
            }
            free(text);
        }
        fclose(file);
    }
    return json;
}

typedef struct {
    hv_plat_app_cb_t cb;
    void *ctx;
} home_enum_ctx_t;

static esp_err_t home_apps_skill_cb(const claw_skill_catalog_entry_t *entry, void *user_ctx)
{
    home_enum_ctx_t *ec = user_ctx;
    hv_app_t app = {0};
    char path[SYSTEM_UI_PATH_MAX];
    cJSON *manifest;
    const cJSON *title;

    if (!home_apps_entry_is_installed_app(entry)) {
        return ESP_OK;
    }
    snprintf(app.id, sizeof(app.id), HOME_SKILL_PREFIX "%s", entry->id);
    if (strlen(entry->id) + strlen(HOME_SKILL_PREFIX) >= sizeof(app.id)) {
        return ESP_OK;
    }
    strlcpy(app.title, entry->id, sizeof(app.title));
    snprintf(path, sizeof(path), "%s/manifest.json", entry->skill_dir);
    manifest = home_apps_read_json(path);
    title = cJSON_GetObjectItemCaseSensitive(manifest, "title");
    if (cJSON_IsString(title) && title->valuestring[0]) {
        strlcpy(app.title, title->valuestring, sizeof(app.title));
        hv_utf8_clip(app.title, sizeof(app.title) - 1);
    }
    app.kind = HV_KIND_SKILL;
    /* MVP 配套与实时应用归“官方应用”，其余（Web 工作台、手机生成）归“我的创作” */
    app.group = (strncmp(entry->id, "official_", 9) == 0 || strncmp(entry->id, "live_", 5) == 0 ||
                 strncmp(entry->id, "demo_", 5) == 0) ? HV_GROUP_OFFICIAL : HV_GROUP_MINE;
    app.ai = true;
    app.removable = true;
    app.renamable = true;
    app.color = home_apps_color(cJSON_GetObjectItemCaseSensitive(manifest, "color"), 0x35C7A5);
    strlcpy(app.script, entry->execution->entry, sizeof(app.script));
    if (entry->execution->args_json) {
        strlcpy(app.args, entry->execution->args_json, sizeof(app.args));
    }
    cJSON_Delete(manifest);
    ec->cb(&app, ec->ctx);
    return ESP_OK;
}

void hv_plat_enum_installed(hv_plat_app_cb_t cb, void *ctx)
{
    home_enum_ctx_t ec = {.cb = cb, .ctx = ctx};
    local_app_store_handle_t store = home_apps_store();
    cJSON *summary = NULL;

    if (store && local_app_store_summary(store, &summary) == ESP_OK) {
        const cJSON *count = cJSON_GetObjectItemCaseSensitive(summary, "count");
        size_t n = cJSON_IsNumber(count) ? (size_t)count->valuedouble : 0;
        for (size_t i = 0; i < n && i < 64; i++) {
            cJSON *item = NULL;
            hv_app_t app = {0};
            if (local_app_store_at(store, i, &item) != ESP_OK) {
                continue;
            }
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "app_id");
            const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
            const cJSON *dir = cJSON_GetObjectItemCaseSensitive(item, "directory");
            if (cJSON_IsString(id) && cJSON_IsString(title) && cJSON_IsString(dir)) {
                snprintf(app.id, sizeof(app.id), HOME_PACKAGE_PREFIX "%s", id->valuestring);
                strlcpy(app.title, title->valuestring, sizeof(app.title));
                hv_utf8_clip(app.title, sizeof(app.title) - 1);
                app.kind = HV_KIND_PACKAGE;
                app.group = HV_GROUP_MINE;
                app.ai = true;
                app.removable = true;
                app.renamable = true;
                app.color = home_apps_color(cJSON_GetObjectItemCaseSensitive(item, "frame_color"), 0x35C7A5);
                snprintf(app.script, sizeof(app.script), "%s/apps/%s/main.lua",
                         claw_paths_get(CLAW_PATH_DATA), dir->valuestring);
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "icon_unselected"))) {
                    snprintf(app.icon_path, sizeof(app.icon_path), "S:%s/apps/%s/icon_unselected.bin",
                             claw_paths_get(CLAW_PATH_DATA), dir->valuestring);
                }
                cb(&app, ctx);
            }
            cJSON_Delete(item);
        }
    }
    cJSON_Delete(summary);
    esp_err_t err = claw_skill_foreach_catalog_entry(home_apps_skill_cb, &ec);
    if (err == ESP_ERR_INVALID_STATE) {
        /* First boot pass: app_claw reloads the home once the registry is up. */
        ESP_LOGI(SYSTEM_UI_TAG, "home: installed skills deferred, skill registry not ready");
    }
}

/* ------------------------------------------------------------------- launch */

esp_err_t hv_plat_launch_lua(const hv_app_t *app, const char *args_json)
{
    system_ui_work_event_t *event = calloc(1, sizeof(*event));
    const char *name;
    esp_err_t err;

    if (!event) {
        return ESP_ERR_NO_MEM;
    }
    event->type = SYSTEM_UI_WORK_EVENT_LAUNCHER_SELECT;
    event->generation = s_ui.generation;
    if (strlcpy(event->launcher_action, app->script, sizeof(event->launcher_action)) >=
            sizeof(event->launcher_action)) {
        free(event);
        return ESP_ERR_INVALID_SIZE;
    }
    if (app->kind == HV_KIND_SKILL || app->kind == HV_KIND_PACKAGE) {
        home_apps_job_name(app->id, event->launcher_id, sizeof(event->launcher_id));
    } else if (app->kind == HV_KIND_GARDEN) {
        strlcpy(event->launcher_id, "my_garden", sizeof(event->launcher_id));
    } else {
        /* Built-in scripts: the file name, so a second launch replaces it. */
        name = strrchr(app->script, '/');
        strlcpy(event->launcher_id, name ? name + 1 : app->script, sizeof(event->launcher_id));
    }
    strlcpy(event->launcher_title, app->title, sizeof(event->launcher_title));
    if (args_json && args_json[0]) {
        strlcpy(event->launcher_args_json, args_json, sizeof(event->launcher_args_json));
    } else if (app->args[0]) {
        strlcpy(event->launcher_args_json, app->args, sizeof(event->launcher_args_json));
    }
    err = event->launcher_id[0] ? system_ui_post_work_event(event, 0) : ESP_ERR_INVALID_ARG;
    ESP_LOGI(SYSTEM_UI_TAG, "HV_LAUNCH %s job=%s err=%s", app->id, event->launcher_id, esp_err_to_name(err));
    free(event);
    return err;
}

/* ---------------------------------------------------------------- uninstall */

esp_err_t hv_plat_request(int type, const char *id, const char *title)
{
    system_ui_work_event_t *event = calloc(1, sizeof(*event));
    esp_err_t err;

    if (!event) {
        return ESP_ERR_NO_MEM;
    }
    event->type = (system_ui_work_event_type_t)type;
    event->generation = s_ui.generation;
    if (strlen(id) >= sizeof(event->app_uninstall.app_id) ||
            (title && strlen(title) >= sizeof(event->app_uninstall.title))) {
        free(event);
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(event->app_uninstall.app_id, id, sizeof(event->app_uninstall.app_id));
    if (title) {
        strlcpy(event->app_uninstall.title, title, sizeof(event->app_uninstall.title));
    }
    err = system_ui_post_work_event(event, 0);
    free(event);
    return err;
}

/* Copy the name of the first entry in @p path other than "." / ".." into @p name.
 * Re-opening the directory for every entry keeps the delete independent of how
 * the filesystem's readdir copes with entries vanishing underneath it. */
static esp_err_t home_apps_first_entry(const char *path, char *name, size_t name_size, bool *found)
{
    DIR *dir = opendir(path);
    struct dirent *de;
    esp_err_t ret = ESP_OK;

    *found = false;
    if (dir == NULL) {
        return ESP_FAIL;
    }
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (strlcpy(name, de->d_name, name_size) >= name_size) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            *found = true;
        }
        break;
    }
    closedir(dir);
    return ret;
}

/* Delete the directory @p path and everything below it. @p path is a buffer of
 * @p cap bytes, extended in place for each entry and restored before returning. */
static esp_err_t home_apps_remove_tree(char *path, size_t cap, int depth)
{
    size_t len = strlen(path);
    char *name;
    esp_err_t ret = ESP_OK;
    bool found = false;

    if (depth > HOME_APPS_MAX_DEPTH) {
        return ESP_ERR_INVALID_STATE;
    }
    name = malloc(HOME_APPS_NAME_LEN);
    if (name == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < HOME_APPS_MAX_ENTRIES; i++) {
        struct stat st;

        ret = home_apps_first_entry(path, name, HOME_APPS_NAME_LEN, &found);
        if (ret != ESP_OK || !found) {
            break;
        }
        if (snprintf(path + len, cap - len, "/%s", name) >= (int)(cap - len)) {
            ret = ESP_ERR_INVALID_SIZE;
        } else if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            ret = home_apps_remove_tree(path, cap, depth + 1);
        } else if (unlink(path) != 0) {
            ESP_LOGW(SYSTEM_UI_TAG, "home: delete %s failed: errno %d", path, errno);
            ret = ESP_FAIL;
        }
        path[len] = '\0';
        if (ret != ESP_OK) {
            break;
        }
    }
    free(name);
    if (ret == ESP_OK && found) {
        ret = ESP_ERR_INVALID_STATE;            /* gave up after HOME_APPS_MAX_ENTRIES */
    }
    if (ret == ESP_OK && rmdir(path) != 0) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: remove %s failed: errno %d", path, errno);
        ret = ESP_FAIL;
    }
    return ret;
}

/* Stop the app, delete it and reload the registry. Runs in the system UI event
 * task, without the LVGL lock. */
esp_err_t hv_plat_uninstall(const hv_app_t *app)
{
    char job_name[SYSTEM_UI_LAUNCHER_ID_LEN];
    char *output = NULL;
    char *dir = NULL;
    esp_err_t err;
    claw_skill_catalog_entry_t entry;
    const char *skill_id;

    home_apps_job_name(app->id, job_name, sizeof(job_name));
    if (!job_name[0]) {
        return ESP_FAIL;
    }
    output = calloc(1, HOME_APPS_STOP_OUTPUT_LEN);
    if (!output) {
        return ESP_ERR_NO_MEM;
    }
    err = cap_lua_stop_job(job_name, HOME_APPS_STOP_WAIT_MS, output, HOME_APPS_STOP_OUTPUT_LEN);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: uninstall %s: could not stop it: %s", app->id, output);
        free(output);
        return err;
    }
    free(output);

    if (app->kind == HV_KIND_PACKAGE) {
        if (display_service_has_exclusive_session()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (system_ui_lock() != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        local_app_store_handle_t store = home_apps_store();
        err = store ? local_app_store_remove(store, app->id + strlen(HOME_PACKAGE_PREFIX)) : ESP_ERR_INVALID_STATE;
        system_ui_unlock();
        return err;
    }
    if (app->kind != HV_KIND_SKILL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    skill_id = app->id + strlen(HOME_SKILL_PREFIX);
    ESP_RETURN_ON_ERROR(claw_skill_get_catalog_entry(skill_id, &entry), SYSTEM_UI_TAG,
                        "home: uninstall %s: not in the skill registry", skill_id);
    ESP_RETURN_ON_FALSE(home_apps_dir_is_installed(entry.skill_dir), ESP_ERR_NOT_SUPPORTED,
                        SYSTEM_UI_TAG, "home: uninstall %s: not an installed app", skill_id);
    /* The entry points into the registry, which the reload below replaces. */
    dir = malloc(SYSTEM_UI_PATH_MAX);
    if (!dir) {
        return ESP_ERR_NO_MEM;
    }
    if (strlcpy(dir, entry.skill_dir, SYSTEM_UI_PATH_MAX) >= SYSTEM_UI_PATH_MAX) {
        free(dir);
        return ESP_ERR_INVALID_SIZE;
    }
    err = home_apps_remove_tree(dir, SYSTEM_UI_PATH_MAX, 0);
    if (err != ESP_OK) {
        ESP_LOGE(SYSTEM_UI_TAG, "home: uninstall %s: deleting %s failed: %s",
                 skill_id, dir, esp_err_to_name(err));
    }
    /* Reload even after a partial delete, so the registry matches the disk. */
    if (claw_skill_reload_registry() != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: uninstall %s: skill registry reload failed", skill_id);
    }
    if (err == ESP_OK) {
        ESP_LOGI(SYSTEM_UI_TAG, "home: uninstalled %s (%s)", skill_id, dir);
    }
    free(dir);
    return err;
}

esp_err_t hv_plat_rename(const hv_app_t *app, const char *title)
{
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

    if (!title || !title[0] || strlen(title) > 95) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app->kind == HV_KIND_PACKAGE) {
        if (system_ui_lock() != ESP_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        local_app_store_handle_t store = home_apps_store();
        err = store ? local_app_store_rename(store, app->id + strlen(HOME_PACKAGE_PREFIX), title)
                    : ESP_ERR_INVALID_STATE;
        system_ui_unlock();
        return err;
    }
    if (app->kind == HV_KIND_SKILL) {
        claw_skill_catalog_entry_t entry;
        char path[SYSTEM_UI_PATH_MAX], tmp[SYSTEM_UI_PATH_MAX];
        cJSON *manifest;
        char *text;
        FILE *f;

        err = claw_skill_get_catalog_entry(app->id + strlen(HOME_SKILL_PREFIX), &entry);
        if (err != ESP_OK || !home_apps_dir_is_installed(entry.skill_dir)) {
            return err != ESP_OK ? err : ESP_ERR_NOT_SUPPORTED;
        }
        snprintf(path, sizeof(path), "%s/manifest.json", entry.skill_dir);
        snprintf(tmp, sizeof(tmp), "%s/manifest.json.tmp", entry.skill_dir);
        manifest = home_apps_read_json(path);
        if (!manifest) {
            manifest = cJSON_CreateObject();
        }
        if (!manifest) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(manifest, "title");
        cJSON_AddStringToObject(manifest, "title", title);
        text = cJSON_PrintUnformatted(manifest);
        cJSON_Delete(manifest);
        if (!text) {
            return ESP_ERR_NO_MEM;
        }
        f = fopen(tmp, "wb");
        err = ESP_FAIL;
        if (f) {
            bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
            ok = (fclose(f) == 0) && ok;
            if (ok) {
                unlink(path);
                err = rename(tmp, path) == 0 ? ESP_OK : ESP_FAIL;
            }
            if (err != ESP_OK) {
                unlink(tmp);
            }
        }
        cJSON_free(text);
    }
    return err;
}

/* ------------------------------------------------------- work events */

static void home_apps_after_change(const char *toast)
{
    if (system_ui_lock() != ESP_OK) {
        return;
    }
    system_ui_home_apps_register_locked();
    /* 底栏里引用的应用若已卸载，顺手移出 */
    const hv_layout_t *cur = hv_layout();
    hv_layout_t l = *cur;
    int k = 0;
    hv_catalog_refresh();
    for (int i = 0; i < l.dock_n; i++) {
        if (hv_catalog_find(l.dock[i])) {
            strlcpy(l.dock[k++], l.dock[i], sizeof(l.dock[0]));
        }
    }
    if (k != l.dock_n) {
        l.dock_n = (uint8_t)k;
        (void)hv_apply_layout(&l, true);
    }
    hv_reload_apps();
    if (toast) {
        hv_toast(toast);
    }
    system_ui_unlock();
}

void system_ui_home_apps_handle_uninstall(const system_ui_work_event_t *event)
{
    char toast[160];
    char title[HV_TITLE_LEN] = {0};
    esp_err_t err;

    if (system_ui_lock() == ESP_OK) {
        const hv_app_t *app = hv_catalog_find(event->app_uninstall.app_id);
        if (app) {
            strlcpy(title, app->title, sizeof(title));
        }
        system_ui_unlock();
    }
    err = hv_catalog_uninstall_now(event->app_uninstall.app_id);
    if (err == ESP_OK) {
        snprintf(toast, sizeof(toast), "已卸载「%s」", title[0] ? title : event->app_uninstall.app_id);
    } else if (err == ESP_ERR_INVALID_STATE) {
        strlcpy(toast, "应用正在运行，先退出再卸载", sizeof(toast));
    } else {
        strlcpy(toast, "卸载失败", sizeof(toast));
    }
    ESP_LOGI(SYSTEM_UI_TAG, "HV_UNINSTALL %s err=%s", event->app_uninstall.app_id, esp_err_to_name(err));
    home_apps_after_change(toast);
}

void system_ui_home_apps_handle_rename(const system_ui_work_event_t *event)
{
    esp_err_t err = hv_catalog_rename_now(event->app_uninstall.app_id, event->app_uninstall.title);
    ESP_LOGI(SYSTEM_UI_TAG, "HV_RENAME %s err=%s", event->app_uninstall.app_id, esp_err_to_name(err));
    home_apps_after_change(err == ESP_OK ? "已保存名称" : "重命名失败，名称最多 95 字节");
}

/* Kept for callers outside the home (BLE commit): refresh when the home is
 * foreground and no app owns the display. */
esp_err_t system_ui_schedule_home_reload(void)
{
    if (!s_ui.started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = system_ui_lock();
    if (err != ESP_OK) {
        return err;
    }
    s_reload_requested = true;
    system_ui_unlock();
    return ESP_OK;
}

void system_ui_home_apps_process_reload(void)
{
    if (!s_ui.started || system_ui_lock() != ESP_OK) {
        return;
    }
    if (s_reload_requested && !display_service_has_exclusive_session()) {
        s_reload_requested = false;
        system_ui_home_apps_register_locked();
        hv_reload_apps();
        ESP_LOGI(SYSTEM_UI_TAG, "home apps reloaded (%u)", (unsigned)hv_catalog_count());
    }
    system_ui_unlock();
}

void system_ui_home_apps_request_uninstall(uint32_t page, const char *app_id)
{
    (void)page;
    (void)hv_catalog_request_uninstall(app_id);
}

void system_ui_home_apps_request_rename(uint32_t page, const char *id, const char *title)
{
    (void)page;
    (void)hv_catalog_request_rename(id, title);
}

bool system_ui_home_apps_launch(const char *app_id)
{
    if (system_ui_lock() != ESP_OK) {
        return false;
    }
    esp_err_t err = hv_catalog_launch(app_id, NULL);
    system_ui_unlock();
    return err == ESP_OK;
}
