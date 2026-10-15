/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Installed apps on the home carousel.
 *
 * Skills on the writable data partition that declare a visible execution entry
 * (DATA/skills/<id>/SKILL.md, e.g. installed by the Web Lua Studio) are added to
 * the carousel after the built-in pages. They start through the same launcher
 * path as launcher tiles, and the long-press menu can uninstall them: the
 * running instance is stopped, the skill directory deleted and the registry and
 * carousel rebuilt. Firmware skills under the read-only system root are never
 * listed here, so they can never be deleted.
 */
#include "system_ui_private.h"

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
#include "esp_check.h"
#include "esp_log.h"
#include "ui_squareline.h"

#define HOME_APPS_SKILLS_SUBDIR     "skills"
#define HOME_APPS_STOP_WAIT_MS      3000
#define HOME_APPS_STOP_OUTPUT_LEN   192
#define HOME_APPS_MAX_DEPTH         6
#define HOME_APPS_MAX_ENTRIES       512     /* per directory, guards a looping delete */
#define HOME_APPS_NAME_LEN          256     /* FAT long names are at most 255 bytes   */
#define HOME_APPS_TOAST_LEN         (SYSTEM_UI_LAUNCHER_TITLE_LEN + 24)

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
    if (strncmp(app_id, "app:", 4) == 0) {
        if (local_app_store_job_name(app_id + 4, out, out_size) != ESP_OK) out[0] = 0;
        return;
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

/* Package apps have a distinct UI namespace, preserving old skill and system IDs. */
#define HOME_PACKAGE_PREFIX "app:"
static local_app_store_handle_t s_package_store;
static bool s_reload_requested;
static struct {
    char title[96], id[40], location[SYSTEM_UI_PATH_MAX];
    char selected[SYSTEM_UI_PATH_MAX + 32], unselected[SYSTEM_UI_PATH_MAX + 32];
} s_package_text;
static bool home_package_at(uint32_t index, ui_squareline_app_t *app, void *ctx)
{
    cJSON *item = NULL; local_app_store_handle_t store = ctx;
    if (local_app_store_at(store, index, &item) != ESP_OK) return false;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "app_id");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
    const cJSON *dir = cJSON_GetObjectItemCaseSensitive(item, "directory");
    bool ok = cJSON_IsString(id) && cJSON_IsString(title) && cJSON_IsString(dir);
    if (ok) {
        strlcpy(s_package_text.title, title->valuestring, sizeof(s_package_text.title));
        snprintf(s_package_text.id, sizeof(s_package_text.id), HOME_PACKAGE_PREFIX "%s", id->valuestring);
        snprintf(s_package_text.location, sizeof(s_package_text.location), "%s/apps/%s", claw_paths_get(CLAW_PATH_DATA), dir->valuestring);
        snprintf(s_package_text.selected, sizeof(s_package_text.selected), "S:%s/icon_selected.bin", s_package_text.location);
        snprintf(s_package_text.unselected, sizeof(s_package_text.unselected), "S:%s/icon_unselected.bin", s_package_text.location);
        *app = (ui_squareline_app_t){.title = s_package_text.title, .app_id = s_package_text.id,
            .location = s_package_text.location, .renameable = true,
            .icon_selected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "icon_selected")) ? s_package_text.selected : NULL,
            .icon_unselected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "icon_unselected")) ? s_package_text.unselected : NULL,
            .frame_color = (uint32_t)cJSON_GetObjectItemCaseSensitive(item, "frame_color")->valuedouble,
            .pill_color = (uint32_t)cJSON_GetObjectItemCaseSensitive(item, "pill_color")->valuedouble};
    }
    cJSON_Delete(item); return ok;
}
void system_ui_home_apps_close_locked(void)
{
    ui_squareline_set_app_provider(0, NULL, NULL);
    local_app_store_delete(s_package_store); s_package_store = NULL;
}
static void home_packages_register(void)
{
    char *root = malloc(SYSTEM_UI_PATH_MAX);
    if (!root) return;
    local_app_store_config_t config = {.root = root};
    esp_err_t err = claw_paths_join(CLAW_PATH_DATA, LOCAL_APP_DIRECTORY, root, SYSTEM_UI_PATH_MAX);
    if (err == ESP_OK) err = local_app_store_create(&config, &s_package_store);
    cJSON *summary = NULL;
    if (err == ESP_OK) err = local_app_store_summary(s_package_store, &summary);
    if (err == ESP_OK) {
        uint32_t count = (uint32_t)cJSON_GetObjectItemCaseSensitive(summary, "count")->valuedouble;
        ui_squareline_set_app_provider(count, home_package_at, s_package_store);
        ESP_LOGI(SYSTEM_UI_TAG, "home: %lu package app(s), lazy groups", (unsigned long)count);
    } else ESP_LOGW(SYSTEM_UI_TAG, "home package index unavailable: %s", esp_err_to_name(err));
    cJSON_Delete(summary); free(root);
}

/* ------------------------------------------------------------- carousel list */

static esp_err_t home_apps_add_entry(const claw_skill_catalog_entry_t *entry, void *user_ctx)
{
    size_t *count = (size_t *)user_ctx;
    ui_squareline_app_t app = {
        .title = entry ? entry->id : NULL,
        .app_id = entry ? entry->id : NULL,
        .location = entry ? entry->skill_dir : NULL,
    };

    if (!home_apps_entry_is_installed_app(entry)) {
        return ESP_OK;
    }
    /* Companion-installed apps already carry a user-facing title. Reading it
     * leaves the package intact; ui_squareline_add_app copies the string. */
    char *path = malloc(SYSTEM_UI_PATH_MAX);
    cJSON *manifest = NULL;
    if (path) {
        int n = snprintf(path, SYSTEM_UI_PATH_MAX, "%s/manifest.json", entry->skill_dir);
        FILE *file = n > 0 && n < SYSTEM_UI_PATH_MAX ? fopen(path, "rb") : NULL;
        if (file) {
            char *text = calloc(4097, 1);
            if (text) {
                size_t count = fread(text, 1, 4096, file);
                if (count < 4096 && !ferror(file)) manifest = cJSON_Parse(text);
                free(text);
            }
            fclose(file);
        }
        free(path);
    }
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(manifest, "title");
    if (cJSON_IsString(title) && title->valuestring[0] &&
        strlen(title->valuestring) < SYSTEM_UI_LAUNCHER_TITLE_LEN) app.title = title->valuestring;
    if (!ui_squareline_add_app(&app)) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: cannot add installed app %s (carousel full or name too long)",
                 entry->id);
        cJSON_Delete(manifest);
        return ESP_OK;
    }
    cJSON_Delete(manifest);
    (*count)++;
    return ESP_OK;
}

void system_ui_home_apps_register_locked(void)
{
    size_t count = 0;
    esp_err_t err;

    system_ui_home_apps_close_locked();
    ui_squareline_clear_apps();
    home_packages_register();
    err = claw_skill_foreach_catalog_entry(home_apps_add_entry, &count);
    if (err == ESP_ERR_INVALID_STATE) {
        /* First boot pass: app_claw rebuilds the home once the registry is up. */
        ESP_LOGI(SYSTEM_UI_TAG, "home: installed apps deferred, skill registry not ready");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: listing installed apps failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(SYSTEM_UI_TAG, "home: %u installed app(s) on the carousel", (unsigned)count);
}

/* ------------------------------------------------------------------- launch */

bool system_ui_home_apps_launch(const char *app_id)
{
    claw_skill_catalog_entry_t entry;
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_LAUNCHER_SELECT,
        .generation = s_ui.generation,
    };

    if (app_id && strncmp(app_id, HOME_PACKAGE_PREFIX, strlen(HOME_PACKAGE_PREFIX)) == 0) {
        char *relative = malloc(SYSTEM_UI_PATH_MAX);
        if (!relative) return true;
        cJSON *manifest = NULL;
        esp_err_t err = s_package_store ? local_app_store_get(s_package_store, app_id + strlen(HOME_PACKAGE_PREFIX), &manifest) : ESP_ERR_INVALID_STATE;
        if (err == ESP_OK) err = local_app_store_directory(s_package_store, app_id + strlen(HOME_PACKAGE_PREFIX), relative, SYSTEM_UI_PATH_MAX);
        if (err == ESP_OK) {
            int n = snprintf(event.launcher_action, sizeof(event.launcher_action), "%s/apps/%s/main.lua", claw_paths_get(CLAW_PATH_DATA), relative);
            if (n > 0 && n < sizeof(event.launcher_action)) {
                home_apps_job_name(app_id, event.launcher_id, sizeof(event.launcher_id));
                strlcpy(event.launcher_title, cJSON_GetObjectItemCaseSensitive(manifest, "title")->valuestring, sizeof(event.launcher_title));
                if (event.launcher_id[0]) (void)system_ui_post_work_event(&event, 0);
            }
        } else ESP_LOGW(SYSTEM_UI_TAG, "package launch failed: %s", esp_err_to_name(err));
        cJSON_Delete(manifest); free(relative); return true;
    }

    if (app_id == NULL || claw_skill_get_catalog_entry(app_id, &entry) != ESP_OK ||
            !home_apps_entry_is_installed_app(&entry)) {
        return false;
    }
    if (strlcpy(event.launcher_action, entry.execution->entry,
                sizeof(event.launcher_action)) >= sizeof(event.launcher_action) ||
            (entry.execution->args_json != NULL &&
             strlcpy(event.launcher_args_json, entry.execution->args_json,
                     sizeof(event.launcher_args_json)) >= sizeof(event.launcher_args_json))) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: installed app %s has an over-long entry or args", app_id);
        return true;        /* it is ours; there is just nothing safe to start */
    }
    home_apps_job_name(app_id, event.launcher_id, sizeof(event.launcher_id));
    strlcpy(event.launcher_title, app_id, sizeof(event.launcher_title));
    (void)system_ui_post_work_event(&event, 0);
    return true;
}

/* ---------------------------------------------------------------- uninstall */

void system_ui_home_apps_request_uninstall(uint32_t page, const char *app_id)
{
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_APP_UNINSTALL,
        .generation = s_ui.generation,
    };

    if (app_id == NULL ||
            strlcpy(event.app_uninstall.app_id, app_id, sizeof(event.app_uninstall.app_id)) >=
            sizeof(event.app_uninstall.app_id)) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: uninstall request without a usable app id");
        return;
    }
    event.app_uninstall.page = page;
    if (system_ui_post_work_event(&event, 0) != ESP_OK) {
        ui_squareline_show_toast("卸载失败");
    }
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

/* Stop the app, delete its skill directory and reload the skill registry. Runs
 * in the system UI event task, without the LVGL lock. */
static esp_err_t home_apps_uninstall(const char *app_id)
{
    claw_skill_catalog_entry_t entry;
    char job_name[SYSTEM_UI_LAUNCHER_ID_LEN];
    char *dir = NULL;
    char *output = NULL;
    esp_err_t err;

    if (app_id && strncmp(app_id, HOME_PACKAGE_PREFIX, strlen(HOME_PACKAGE_PREFIX)) == 0) {
        home_apps_job_name(app_id, job_name, sizeof(job_name));
        if (!job_name[0]) return ESP_FAIL;
        char *reply = calloc(1, HOME_APPS_STOP_OUTPUT_LEN);
        if (!reply) return ESP_ERR_NO_MEM;
        err = cap_lua_stop_job(job_name, HOME_APPS_STOP_WAIT_MS, reply, HOME_APPS_STOP_OUTPUT_LEN); free(reply);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
        if (display_service_has_exclusive_session()) return ESP_ERR_INVALID_STATE;
        if (system_ui_lock() != ESP_OK) return ESP_ERR_INVALID_STATE;
        err = s_package_store ? local_app_store_remove(s_package_store, app_id + strlen(HOME_PACKAGE_PREFIX)) : ESP_ERR_INVALID_STATE;
        system_ui_unlock(); return err;
    }

    ESP_RETURN_ON_ERROR(claw_skill_get_catalog_entry(app_id, &entry), SYSTEM_UI_TAG,
                        "home: uninstall %s: not in the skill registry", app_id);
    ESP_RETURN_ON_FALSE(home_apps_dir_is_installed(entry.skill_dir), ESP_ERR_NOT_SUPPORTED,
                        SYSTEM_UI_TAG, "home: uninstall %s: %s is not an installed app",
                        app_id, entry.skill_dir ? entry.skill_dir : "(no dir)");

    /* The entry points into the registry, which the reload below replaces. */
    dir = malloc(SYSTEM_UI_PATH_MAX);
    output = calloc(1, HOME_APPS_STOP_OUTPUT_LEN);
    if (dir == NULL || output == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (strlcpy(dir, entry.skill_dir, SYSTEM_UI_PATH_MAX) >= SYSTEM_UI_PATH_MAX) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    /* Launched from the carousel the app runs as a Lua job named after it. */
    home_apps_job_name(app_id, job_name, sizeof(job_name));
    err = cap_lua_stop_job(job_name, HOME_APPS_STOP_WAIT_MS, output, HOME_APPS_STOP_OUTPUT_LEN);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: uninstall %s: could not stop it: %s", app_id, output);
        goto done;
    }

    err = home_apps_remove_tree(dir, SYSTEM_UI_PATH_MAX, 0);
    if (err != ESP_OK) {
        ESP_LOGE(SYSTEM_UI_TAG, "home: uninstall %s: deleting %s failed: %s",
                 app_id, dir, esp_err_to_name(err));
    }
    /* Reload even after a partial delete, so the registry matches the disk. */
    if (claw_skill_reload_registry() != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: uninstall %s: skill registry reload failed", app_id);
    }
    if (err == ESP_OK) {
        ESP_LOGI(SYSTEM_UI_TAG, "home: uninstalled %s (%s)", app_id, dir);
    }

done:
    free(output);
    free(dir);
    return err;
}

void system_ui_home_apps_handle_uninstall(const system_ui_work_event_t *event)
{
    const char *app_id = event->app_uninstall.app_id;
    char toast[HOME_APPS_TOAST_LEN];
    esp_err_t err = home_apps_uninstall(app_id);
    esp_err_t reload;

    if (err == ESP_OK) {
        snprintf(toast, sizeof(toast), "已卸载「%s」", app_id);
    } else {
        strlcpy(toast, "卸载失败", sizeof(toast));
    }

    /* Rebuild the carousel from the registry, then land next to where the app
     * was and say how it went. */
    reload = system_ui_reload_home();
    if (reload != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "home: rebuild after uninstall failed: %s", esp_err_to_name(reload));
        return;
    }
    if (system_ui_lock() == ESP_OK) {
        uint32_t count = ui_squareline_page_count();
        uint32_t page = event->app_uninstall.page;

        ui_squareline_show_page(page < count ? page : (uint32_t)(count - 1));
        ui_squareline_show_toast(toast);
        system_ui_unlock();
    }
}

/* Polling on the existing UI task coalesces refreshes and never destroys an
 * active native/Lua screen. The same display mutex protects store readers. */
esp_err_t system_ui_schedule_home_reload(void)
{
    if (!s_ui.started) return ESP_ERR_INVALID_STATE;
    esp_err_t err = system_ui_lock(); if (err != ESP_OK) return err;
    s_reload_requested = true; system_ui_unlock(); return ESP_OK;
}
void system_ui_home_apps_process_reload(void)
{
    if (!s_ui.started || system_ui_lock() != ESP_OK) return;
    if (s_reload_requested && !display_service_has_exclusive_session() && lv_screen_active() == s_ui.home_screen) {
        s_reload_requested = false;
        uint32_t page = ui_squareline_current_page();
        system_ui_delete_home_locked();
        esp_err_t err = system_ui_create_home_locked();
        if (err == ESP_OK) {
            display_service_set_default_screen_locked(s_ui.home_screen);
            uint32_t count = ui_squareline_page_count();
            ui_squareline_show_page(page < count ? page : count - 1);
        } else {
            display_service_set_default_screen_locked(NULL);
            ESP_LOGE(SYSTEM_UI_TAG, "home refresh failed: %s", esp_err_to_name(err));
        }
    }
    system_ui_unlock();
}
void system_ui_home_apps_request_rename(uint32_t page, const char *id, const char *title)
{
    if (!id || !title) return;
    system_ui_work_event_t *event = calloc(1, sizeof(*event));
    if (!event) return;
    event->type = SYSTEM_UI_WORK_EVENT_APP_RENAME; event->generation = s_ui.generation;
    if (strlen(id) >= sizeof(event->app_uninstall.app_id) || strlen(title) >= sizeof(event->app_uninstall.title)) { free(event); return; }
    event->app_uninstall.page = page;
    strlcpy(event->app_uninstall.app_id, id, sizeof(event->app_uninstall.app_id));
    strlcpy(event->app_uninstall.title, title, sizeof(event->app_uninstall.title));
    if (system_ui_post_work_event(event, 0) != ESP_OK) ui_squareline_show_toast("重命名请求失败");
    free(event);
}
void system_ui_home_apps_handle_rename(const system_ui_work_event_t *event)
{
    const char *id = event->app_uninstall.app_id;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (strncmp(id, HOME_PACKAGE_PREFIX, strlen(HOME_PACKAGE_PREFIX)) == 0 && system_ui_lock() == ESP_OK) {
        err = s_package_store ? local_app_store_rename(s_package_store, id + strlen(HOME_PACKAGE_PREFIX), event->app_uninstall.title) : ESP_ERR_INVALID_STATE;
        system_ui_unlock();
    }
    if (err == ESP_OK) (void)system_ui_schedule_home_reload();
    if (system_ui_lock() == ESP_OK) { ui_squareline_show_toast(err == ESP_OK ? "已保存名称" : "重命名失败，名称最多95字节"); system_ui_unlock(); }
}
