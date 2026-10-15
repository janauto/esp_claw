/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 1.0 新操作：BLE（手机 App / 电脑端）与 UART（clip_cmd，自动验收）共用。
 * 手机能做的应用管理与首页布局，在背夹上也都能做，反之亦然：两边都走同一份
 * 应用目录（home_catalog.c）和同一份布局（home_layout.c）。
 */
#include "home_priv.h"
#include "system_ui_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "claw_paths.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "display_service.h"
#include "esp_log.h"
#include "voice_cloud.h"
#include "esp_timer.h"

esp_err_t hv_voice_ask_text(const char *text);
void hv_touch_inject_start(int type, int32_t x, int32_t y, int32_t x2, int32_t y2, uint32_t ms);
void system_ui_home_key_post(hv_key_t key);

/* 由 lua_module_button 提供（固件里链接了按键模块时解析到它，否则为 NULL） */
esp_err_t lua_module_button_inject(const char *label, const char *event) __attribute__((weak));

/* UART ui_key 在有 Lua 应用独占显示时，也像实体键一样送给它（以前只走首页按键路径，Lua 应用收不到） */
static char s_lua_key_up[16];

static void hv_lua_key_up_cb(void *arg)
{
    (void)arg;
    if (lua_module_button_inject && s_lua_key_up[0]) {
        (void)lua_module_button_inject(s_lua_key_up, "press_up");
        (void)lua_module_button_inject(s_lua_key_up, "long_press_up");
    }
}

static void hv_lua_key_inject(const char *label, const char *action, uint32_t ms)
{
    static esp_timer_handle_t s_up_timer;
    if (!lua_module_button_inject || !label || !display_service_has_exclusive_session()) {
        return;
    }
    if (!action || !strcmp(action, "click")) {
        (void)lua_module_button_inject(label, "press_down");
        (void)lua_module_button_inject(label, "press_up");
        (void)lua_module_button_inject(label, "single_click");
    } else if (!strcmp(action, "press")) {
        (void)lua_module_button_inject(label, "press_down");
    } else if (!strcmp(action, "release")) {
        (void)lua_module_button_inject(label, "press_up");
    } else if (!strcmp(action, "long")) {
        (void)lua_module_button_inject(label, "press_down");
        strlcpy(s_lua_key_up, label, sizeof(s_lua_key_up));
        if (!s_up_timer) {
            const esp_timer_create_args_t args = {.callback = hv_lua_key_up_cb, .name = "hv_lua_key"};
            if (esp_timer_create(&args, &s_up_timer) != ESP_OK) {
                return;
            }
        }
        (void)esp_timer_stop(s_up_timer);
        (void)esp_timer_start_once(s_up_timer, (uint64_t)(ms ? ms : 900) * 1000);
    }
}
void system_ui_home_key_post_delayed(hv_key_t key, uint32_t ms);

static const char *s_ops_json =
    "\"mode_get\",\"mode_set\",\"ai_config_get\",\"ai_config_set\",\"apps_all\",\"app_launch\","
    "\"home_get\",\"home_set\",\"assistant_show\",\"voice_intent\",\"garden_put\",\"garden_list\","
    "\"garden_remove\"";

const char *system_ui_home_ops_json(void)
{
    return s_ops_json;
}

static const char *str_field(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ---------------------------------------------- 「我的」花园：用户 / AI 植物与主题
 * 文件放进 <DATA>/garden/plants|themes/<id>.lua，花园每 3 秒看一眼、几秒内热重载（PLANT_API.md）。
 * 这里只做文件层面的检查（名字、大小、不是预编译字节码），接口是否写对由花园的沙箱判断，
 * 载入失败会在花园「一句话生成」页列出原因，不影响别的植物。 */
#define HV_GARDEN_SRC_MAX (16 * 1024)

static bool hv_garden_id_ok(const char *id)
{
    if (!id || !(id[0] >= 'a' && id[0] <= 'z') || strlen(id) > 32) {
        return false;
    }
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return true;
}

static const char *hv_garden_subdir(const char *kind)
{
    return kind && !strcmp(kind, "theme") ? "themes" : (!kind || !strcmp(kind, "plant")) ? "plants" : NULL;
}

static esp_err_t hv_garden_path(const char *kind, const char *id, char *out, size_t size)
{
    const char *sub = hv_garden_subdir(kind);
    if (!sub || !hv_garden_id_ok(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, size, "%s/garden/%s/%s.lua", claw_paths_get(CLAW_PATH_DATA), sub, id);
    return n > 0 && (size_t)n < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t hv_garden_put(const char *kind, const char *id, const char *src, char *why, size_t why_size)
{
    char path[160], tmp[168], dir[128];
    size_t len = src ? strlen(src) : 0;

    if (hv_garden_path(kind, id, path, sizeof(path)) != ESP_OK) {
        snprintf(why, why_size, "名字只能用小写字母开头，后面是小写字母、数字或下划线（最多 32 个）");
        return ESP_ERR_INVALID_ARG;
    }
    if (!len || len > HV_GARDEN_SRC_MAX) {
        snprintf(why, why_size, "源码为空或超过 16 KB");
        return ESP_ERR_INVALID_SIZE;
    }
    if ((unsigned char)src[0] == 0x1B) {
        snprintf(why, why_size, "只能装 Lua 源码，不能是预编译字节码");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(dir, sizeof(dir), "%s/garden", claw_paths_get(CLAW_PATH_DATA));
    (void)mkdir(dir, 0775);
    snprintf(dir, sizeof(dir), "%s/garden/%s", claw_paths_get(CLAW_PATH_DATA), hv_garden_subdir(kind));
    (void)mkdir(dir, 0775);
    /* 先写 .tmp，再删旧文件、改名（FAT 不能覆盖式改名）；花园扫描只认 .lua */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(why, why_size, "写不进背夹存储（%d）", errno);
        return ESP_FAIL;
    }
    bool ok = fwrite(src, 1, len, f) == len;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)unlink(tmp);
        snprintf(why, why_size, "写入不完整（%d）", errno);
        return ESP_FAIL;
    }
    (void)unlink(path);
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        snprintf(why, why_size, "改名失败（%d）", errno);
        return ESP_FAIL;
    }
    ESP_LOGI(SYSTEM_UI_TAG, "HV_GARDEN put %s %s (%u bytes)", hv_garden_subdir(kind), id, (unsigned)len);
    return ESP_OK;
}

static cJSON *hv_garden_list_json(void)
{
    static const char *const kinds[2] = {"plant", "theme"};
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    for (int k = 0; k < 2; k++) {
        char dir[128];
        cJSON *arr = cJSON_AddArrayToObject(o, k ? "themes" : "plants");
        snprintf(dir, sizeof(dir), "%s/garden/%s", claw_paths_get(CLAW_PATH_DATA), hv_garden_subdir(kinds[k]));
        DIR *d = opendir(dir);
        struct dirent *de;
        while (d && arr && (de = readdir(d))) {
            size_t n = strlen(de->d_name);
            if (n > 4 && !strcmp(de->d_name + n - 4, ".lua")) {
                char id[40];
                snprintf(id, sizeof(id), "%.*s", (int)(n - 4), de->d_name);
                cJSON_AddItemToArray(arr, cJSON_CreateString(id));
            }
        }
        if (d) {
            closedir(d);
        }
    }
    return o;
}

static cJSON *hv_err_obj(const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "error", msg);
    }
    return o;
}

static cJSON *hv_mode_json(void)
{
    cJSON *o = cJSON_CreateObject();
    char ssid[40] = {0};
    bool wifi = hv_plat_wifi(ssid, sizeof(ssid));
    bool dock = hv_plat_mode_get() == HV_MODE_DOCK;
    bool phone = hv_plat_phone_linked();
    bool ai = hv_plat_ai_ready();
    cJSON *w;

    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "mode", dock ? "dock" : "clip");
    /* 实际会走哪条：底座且可直连 → wifi；背夹且手机在线 → ble；否则按回退规则 */
    const char *route = dock ? ((wifi && ai) ? "wifi" : phone ? "ble" : "none")
                             : (phone ? "ble" : (wifi && ai) ? "wifi" : "none");
    cJSON_AddStringToObject(o, "voice_route", route);
    cJSON_AddBoolToObject(o, "phone_linked", phone);
    cJSON_AddBoolToObject(o, "ble_connected", hv_plat_ble_connected());
    w = cJSON_AddObjectToObject(o, "wifi");
    if (w) {
        cJSON_AddBoolToObject(w, "connected", wifi);
        cJSON_AddStringToObject(w, "ssid", ssid);
    }
    cJSON_AddBoolToObject(o, "ai_ready", ai);
    cJSON_AddStringToObject(o, "release", SYSTEM_UI_RELEASE);
    return o;
}

static cJSON *hv_apps_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = o ? cJSON_AddArrayToObject(o, "apps") : NULL;
    static const char *const groups[] = {"builtin", "official", "mine"};
    char *jobs = malloc(8192);

    if (!arr) {
        cJSON_Delete(o);
        free(jobs);
        return NULL;
    }
    if (jobs && cap_lua_list_jobs("running", jobs, 8192) != ESP_OK) {
        jobs[0] = 0;
    }
    hv_catalog_refresh();
    for (size_t i = 0; i < hv_catalog_count(); i++) {
        const hv_app_t *a = hv_catalog_at(i);
        cJSON *it = cJSON_CreateObject();
        if (!it) {
            break;
        }
        cJSON_AddStringToObject(it, "id", a->id);
        cJSON_AddStringToObject(it, "title", a->title);
        cJSON_AddStringToObject(it, "kind", hv_kind_name(a->kind));
        cJSON_AddStringToObject(it, "group", groups[a->group < 3 ? a->group : 2]);
        cJSON_AddBoolToObject(it, "ai", a->ai);
        cJSON_AddBoolToObject(it, "removable", a->removable);
        cJSON_AddBoolToObject(it, "renamable", a->renamable);
        cJSON_AddBoolToObject(it, "in_dock", hv_layout_dock_has(hv_layout(), a->id));
        bool running = false;
        if (a->kind == HV_KIND_SYSTEM) {
            running = (!strcmp(a->id, "sys:workbuddy") && s_ui.workbuddy_visible) ||
                      (!strcmp(a->id, "sys:meeting") && s_ui.meeting_minutes_visible) ||
                      (!strcmp(a->id, "sys:ir") && s_ui.infrared_remote_visible) ||
                      (!strcmp(a->id, "sys:card") && s_ui.businesscard_visible);
        } else if (jobs && jobs[0] && a->script[0]) {
            char needle[SYSTEM_UI_PATH_MAX + 8];
            snprintf(needle, sizeof(needle), "path=%s", a->script);
            running = strstr(jobs, needle) != NULL;
        }
        cJSON_AddBoolToObject(it, "running", running);
        cJSON_AddItemToArray(arr, it);
    }
    free(jobs);
    cJSON_AddNumberToObject(o, "count", (double)hv_catalog_count());
    return o;
}

static cJSON *hv_home_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *widgets;

    if (!o) {
        return NULL;
    }
    cJSON_AddItemToObject(o, "layout", hv_layout_to_json(hv_layout()));
    widgets = cJSON_AddArrayToObject(o, "widgets");
    hv_widgets_rescan();
    for (size_t i = 0; widgets && i < hv_widgets_count(); i++) {
        const hv_widget_desc_t *d = hv_widgets_at(i);
        cJSON *it = cJSON_CreateObject();
        cJSON *sizes = it ? cJSON_AddArrayToObject(it, "sizes") : NULL;
        if (!sizes) {
            cJSON_Delete(it);
            break;
        }
        cJSON_AddStringToObject(it, "w", d->id);
        cJSON_AddStringToObject(it, "name", d->name);
        cJSON_AddStringToObject(it, "desc", d->desc);
        for (int s = 0; s <= HV_SZ_L; s++) {
            if (d->sizes & HV_SZ_BIT(s)) {
                cJSON_AddItemToArray(sizes, cJSON_CreateString(hv_size_name((uint8_t)s)));
            }
        }
        cJSON_AddStringToObject(it, "app", d->app);
        cJSON_AddBoolToObject(it, "fixed", d->fixed);
        cJSON_AddBoolToObject(it, "ai", d->ai);
        cJSON_AddItemToArray(widgets, it);
    }
    cJSON_AddNumberToObject(o, "dock_max", HV_DOCK_MAX);
    cJSON_AddNumberToObject(o, "pages_max", HV_MAX_PAGES);
    return o;
}

static hv_voice_state_t hv_state_from_name(const char *s)
{
    if (!s) return HV_VOICE_IDLE;
    if (!strcmp(s, "listening")) return HV_VOICE_LISTENING;
    if (!strcmp(s, "recognizing")) return HV_VOICE_RECOGNIZING;
    if (!strcmp(s, "thinking")) return HV_VOICE_THINKING;
    if (!strcmp(s, "reply")) return HV_VOICE_REPLY;
    if (!strcmp(s, "error")) return HV_VOICE_ERROR;
    return HV_VOICE_IDLE;
}

esp_err_t system_ui_home_assistant_show(const char *state, const char *heard, const char *text)
{
    hv_voice_state_t st = hv_state_from_name(state);
    char *h = NULL, *t = NULL;

    if (state && strcmp(state, "hide") != 0 && st == HV_VOICE_IDLE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (system_ui_lock() != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    if (heard) {
        h = strdup(heard);
        if (h) hv_utf8_clip(h, 150);
    }
    if (text) {
        t = strdup(text);
        if (t) hv_utf8_clip(t, 300);
    }
    hv_voice_show(st, h, t);
    system_ui_unlock();
    free(h);
    free(t);
    return ESP_OK;
}

/* 旧版 App 只会发 workbuddy_ui：reply_status 是“识别中/你说：/提醒：”这类小标题。 */
void system_ui_home_note_workbuddy(const char *status, const char *reply_status, const char *reply_text)
{
    hv_voice_state_t st = hv_voice_state();

    (void)status;
    if (!reply_text && !reply_status) {
        return;
    }
    if (st != HV_VOICE_RECOGNIZING && st != HV_VOICE_THINKING && st != HV_VOICE_REPLY) {
        return;     /* 首页没在等语音：WorkBuddy 页自己显示 */
    }
    if (reply_status && strstr(reply_status, "你说")) {
        (void)system_ui_home_assistant_show("thinking", reply_text, NULL);
    } else if (reply_status && strstr(reply_status, "识别")) {
        (void)system_ui_home_assistant_show("recognizing", NULL, NULL);
    } else if (reply_text && reply_text[0]) {
        (void)system_ui_home_assistant_show("reply", NULL, reply_text);
    }
}

static hv_key_t hv_key_from(const char *key, const char *action, bool *ok, bool *hold)
{
    *ok = true;
    *hold = false;
    if (!key) {
        *ok = false;
        return HV_KEY_M;
    }
    bool longp = action && !strcmp(action, "long");
    bool press = action && !strcmp(action, "press");
    bool release = action && !strcmp(action, "release");
    if (!strcmp(key, "VOLUME_DOWN") || !strcmp(key, "PREV")) return HV_KEY_PREV;
    if (!strcmp(key, "VOLUME_UP") || !strcmp(key, "NEXT")) return HV_KEY_NEXT;
    if (!strcmp(key, "MODE") || !strcmp(key, "M")) {
        if (longp) {
            *hold = true;
            return HV_KEY_M_LONG_START;
        }
        if (press) return HV_KEY_M_LONG_START;
        if (release) return HV_KEY_M_LONG_END;
        return HV_KEY_M;
    }
    if (!strcmp(key, "SET")) {
        return longp ? HV_KEY_SET_LONG : HV_KEY_SET;
    }
    *ok = false;
    return HV_KEY_M;
}

esp_err_t system_ui_home_handle_op(const char *op, const cJSON *req, bool uart, cJSON **out)
{
    esp_err_t err = ESP_OK;
    const char *id;

    *out = NULL;
    if (!op) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!strcmp(op, "mode_get")) {
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        *out = hv_mode_json();
        system_ui_unlock();
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!strcmp(op, "mode_set")) {
        const char *m = str_field(req, "mode");
        if (!m || (strcmp(m, "clip") && strcmp(m, "dock"))) {
            *out = hv_err_obj("mode 只能是 clip 或 dock");
            return ESP_ERR_INVALID_ARG;
        }
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        err = hv_plat_mode_set(!strcmp(m, "dock") ? HV_MODE_DOCK : HV_MODE_CLIP);
        hv_refresh_status();
        hv_control_refresh();
        hv_toast(!strcmp(m, "dock") ? "已切到底座模式" : "已切到背夹模式");
        *out = hv_mode_json();
        system_ui_unlock();
        return err;
    }
    if (!strcmp(op, "ai_config_get")) {
        *out = voice_cloud_config_public();
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!strcmp(op, "ai_config_set")) {
        /* 只接受这几个字段；密钥永不回显、永不写日志 */
        static const char *const keys[] = {"asr_base", "asr_key", "asr_model", "chat_base", "chat_key", "chat_model"};
        cJSON *fields = cJSON_CreateObject();
        if (!fields) return ESP_ERR_NO_MEM;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const char *v = str_field(req, keys[i]);
            if (v) {
                cJSON_AddStringToObject(fields, keys[i], v);
            }
        }
        err = voice_cloud_config_set(fields);
        cJSON_Delete(fields);
        *out = voice_cloud_config_public();
        if (err == ESP_OK && system_ui_lock() == ESP_OK) {
            hv_control_refresh();
            hv_toast(hv_plat_ai_ready() ? "底座 AI 服务已配置" : "底座 AI 配置已更新");
            system_ui_unlock();
        }
        ESP_LOGI(SYSTEM_UI_TAG, "HV_AI_CONFIG set err=%s", esp_err_to_name(err));
        return err;
    }
    if (!strcmp(op, "apps_all")) {
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        *out = hv_apps_json();
        system_ui_unlock();
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!strcmp(op, "app_launch")) {
        id = str_field(req, "id");
        if (!id) {
            id = str_field(req, "app_id");
        }
        if (!id) {
            *out = hv_err_obj("缺少 id");
            return ESP_ERR_INVALID_ARG;
        }
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        const hv_app_t *app = hv_catalog_find(id);
        if (!app) {
            hv_catalog_refresh();
            app = hv_catalog_find(id);
        }
        if (!app) {
            err = ESP_ERR_NOT_FOUND;
            *out = hv_err_obj("背夹上没有这个应用");
        } else if (app->kind == HV_KIND_SYSTEM && display_service_has_exclusive_session()) {
            err = ESP_ERR_INVALID_STATE;
            *out = hv_err_obj("背夹正在运行其他应用，请先退出");
        } else if (hv_voice_state() == HV_VOICE_RECOGNIZING || hv_voice_state() == HV_VOICE_THINKING ||
                   hv_voice_state() == HV_VOICE_REPLY) {
            /* 语音这一轮还没让用户看清回复：先显示回复，约 1.8 s 后再打开应用 */
            extern void hv_voice_defer_launch(const char *id);
            hv_voice_defer_launch(app->id);
            *out = cJSON_CreateObject();
            if (*out) {
                cJSON_AddBoolToObject(*out, "launched", true);
                cJSON_AddBoolToObject(*out, "deferred", true);
                cJSON_AddStringToObject(*out, "id", app->id);
            }
        } else {
            hv_go_home();
            err = hv_catalog_launch(app->id, str_field(req, "args"));
            *out = cJSON_CreateObject();
            if (*out) {
                cJSON_AddBoolToObject(*out, "launched", err == ESP_OK);
                cJSON_AddStringToObject(*out, "id", app->id);
            }
        }
        system_ui_unlock();
        return err;
    }
    /* 带前缀 skill: 的卸载/改名由首页处理；app:/无前缀交回应用包处理器 */
    if ((!strcmp(op, "app_remove") || !strcmp(op, "app_rename")) &&
            ((id = str_field(req, "app_id")) != NULL || (id = str_field(req, "id")) != NULL) &&
            !strncmp(id, "skill:", 6)) {
        if (!strcmp(op, "app_remove")) {
            err = hv_catalog_uninstall_now(id);
        } else {
            const char *title = str_field(req, "title");
            err = title ? hv_catalog_rename_now(id, title) : ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK) {
            (void)system_ui_schedule_home_reload();
        }
        *out = cJSON_CreateObject();
        if (*out) {
            cJSON_AddStringToObject(*out, "id", id);
            cJSON_AddBoolToObject(*out, !strcmp(op, "app_remove") ? "removed" : "renamed", err == ESP_OK);
        }
        return err;
    }
    if (!strcmp(op, "home_get")) {
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        *out = hv_home_json();
        system_ui_unlock();
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!strcmp(op, "home_set")) {
        const cJSON *layout = cJSON_GetObjectItemCaseSensitive(req, "layout");
        hv_layout_t l;
        char why[64] = {0};
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        hv_widgets_rescan();
        err = hv_layout_from_json(layout, &l, why, sizeof(why));
        if (err == ESP_OK) {
            if (hv_view() == HV_VIEW_EDIT) {
                hv_exit_edit();
            }
            err = hv_apply_layout(&l, true);
            hv_toast("首页布局已按手机同步");
            *out = cJSON_CreateObject();
            if (*out) {
                cJSON_AddItemToObject(*out, "layout", hv_layout_to_json(hv_layout()));
            }
        } else {
            *out = hv_err_obj(why[0] ? why : "布局无效");
        }
        system_ui_unlock();
        return err;
    }
    if (!strcmp(op, "assistant_show")) {
        err = system_ui_home_assistant_show(str_field(req, "state"), str_field(req, "heard"), str_field(req, "text"));
        *out = cJSON_CreateObject();
        if (err != ESP_OK) {
            cJSON_Delete(*out);
            *out = hv_err_obj("state 无效");
        }
        return err;
    }
    if (!strcmp(op, "garden_put")) {
        /* 手机「种到花园」：把一份植物 / 主题 Lua 放进花园，几秒内出现在种子库或主题卡里 */
        char why[96] = "";
        err = hv_garden_put(str_field(req, "kind"), str_field(req, "id"), str_field(req, "source"), why, sizeof(why));
        if (err != ESP_OK) {
            *out = hv_err_obj(why[0] ? why : "放不进花园");
            return err;
        }
        *out = cJSON_CreateObject();
        if (*out) {
            cJSON_AddStringToObject(*out, "kind", str_field(req, "kind") ? str_field(req, "kind") : "plant");
            cJSON_AddStringToObject(*out, "id", str_field(req, "id"));
            cJSON_AddBoolToObject(*out, "saved", true);
        }
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!strcmp(op, "garden_list")) {
        *out = hv_garden_list_json();
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!strcmp(op, "garden_remove")) {
        char path[160];
        if (hv_garden_path(str_field(req, "kind"), str_field(req, "id"), path, sizeof(path)) != ESP_OK) {
            *out = hv_err_obj("名字无效");
            return ESP_ERR_INVALID_ARG;
        }
        err = unlink(path) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
        *out = err == ESP_OK ? cJSON_CreateObject() : hv_err_obj("花园里没有这个文件");
        return err;
    }
    if (!strcmp(op, "voice_intent")) {
        /* 背夹·手机模式：手机识别出文字后先问背夹能不能直接执行（调音量/亮度、切模式、打开应用），
         * 能执行就不再请求对话模型；和底座模式用同一套规则 */
        const char *text = str_field(req, "text");
        char reply[160];
        bool handled = text && hv_intent_run(text, reply, sizeof(reply), true);
        *out = cJSON_CreateObject();
        if (!*out) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(*out, "handled", handled);
        if (handled) {
            cJSON_AddStringToObject(*out, "reply", reply);
        }
        return ESP_OK;
    }
    if (!uart) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* ---------------------------------------------- 只给 UART 自动验收用 */
    if (!strcmp(op, "home_reset")) {
        /* 自动验收：每个场景从干净的首页开始（不退出正在运行的应用，应用用 app_stop） */
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        hv_go_home();
        hv_set_page(0, false);
        hv_focus_ring(NULL, false);
        system_ui_jobs_set_visible_locked(false);
        *out = hv_state_json();
        system_ui_unlock();
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!strcmp(op, "home_state")) {
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        *out = hv_state_json();
        system_ui_unlock();
        if (*out) {
            cJSON_AddBoolToObject(*out, "exclusive_app", display_service_has_exclusive_session());
        }
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (!strcmp(op, "ui_key")) {
        bool ok, hold;
        const cJSON *ms = cJSON_GetObjectItemCaseSensitive(req, "ms");
        hv_key_t key = hv_key_from(str_field(req, "key"), str_field(req, "action"), &ok, &hold);
        if (!ok) {
            *out = hv_err_obj("key 需为 MODE/SET/VOLUME_UP/VOLUME_DOWN");
            return ESP_ERR_INVALID_ARG;
        }
        hv_lua_key_inject(str_field(req, "key"), str_field(req, "action"),
                          cJSON_IsNumber(ms) ? (uint32_t)ms->valuedouble : 900);
        system_ui_home_key_post(key);
        if (hold) {
            system_ui_home_key_post_delayed(HV_KEY_M_LONG_END, cJSON_IsNumber(ms) ? (uint32_t)ms->valuedouble : 2500);
        }
        *out = cJSON_CreateObject();
        return ESP_OK;
    }
    if (!strcmp(op, "ui_touch")) {
        const char *type = str_field(req, "type");
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(req, "x"), *y = cJSON_GetObjectItemCaseSensitive(req, "y");
        const cJSON *x2 = cJSON_GetObjectItemCaseSensitive(req, "x2"), *y2 = cJSON_GetObjectItemCaseSensitive(req, "y2");
        const cJSON *ms = cJSON_GetObjectItemCaseSensitive(req, "ms");
        int t = type && !strcmp(type, "long") ? 1 : type && !strcmp(type, "drag") ? 2 : 0;
        if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y)) {
            *out = hv_err_obj("需要 x、y");
            return ESP_ERR_INVALID_ARG;
        }
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        hv_touch_inject_start(t, x->valueint, y->valueint,
                              cJSON_IsNumber(x2) ? x2->valueint : x->valueint,
                              cJSON_IsNumber(y2) ? y2->valueint : y->valueint,
                              cJSON_IsNumber(ms) ? (uint32_t)ms->valuedouble : (t == 1 ? 900 : t == 2 ? 400 : 90));
        system_ui_unlock();
        *out = cJSON_CreateObject();
        return ESP_OK;
    }
    if (!strcmp(op, "voice_text")) {
        const char *text = str_field(req, "text");
        if (system_ui_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
        err = hv_voice_ask_text(text);
        system_ui_unlock();
        *out = err == ESP_OK ? cJSON_CreateObject() : hv_err_obj("当前无法走底座直连（看 home_state.voice）");
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
