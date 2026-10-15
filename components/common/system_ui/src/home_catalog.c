/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 统一应用目录：系统原生页面（sys:）、固件内置 Lua（sys:）、我的花园（garden）、
 * DATA/skills 旧格式应用（skill:）、DATA/apps 应用包（app:）。
 * 总览、底部应用栏、BLE/UART 的 apps_all 都从这里取，保证三处看到的是同一份列表。
 */
#include "home_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define HV_CATALOG_MAX 96

typedef struct {
    const char *id;
    const char *title;
    hv_kind_t kind;
    hv_group_t group;
    bool ai;
    uint32_t color;
    const char *glyph;
    bool symbol;
    const char *script;         /* 相对 SYSTEM 根；原生页面为 NULL */
} hv_builtin_t;

/* 顺序即总览里的顺序。原生页面没有 AI 角标；固件自带的 Lua 属于“官方应用”。 */
static const hv_builtin_t s_builtins[] = {
    {"sys:meeting",   "会议录音",   HV_KIND_SYSTEM, HV_GROUP_BUILTIN, false, 0xE5484D, "录", false, NULL},
    {"sys:ir",        "遥控器",     HV_KIND_SYSTEM, HV_GROUP_BUILTIN, false, 0x3B82F6, LV_SYMBOL_WIFI, true, NULL},
    {"sys:card",      "名片分享",   HV_KIND_SYSTEM, HV_GROUP_BUILTIN, false, 0x2FBF71, LV_SYMBOL_FILE, true, NULL},
    {"sys:workbuddy", "WorkBuddy",  HV_KIND_SYSTEM, HV_GROUP_BUILTIN, false, 0x8B5CF6, LV_SYMBOL_BELL, true, NULL},
    {"garden",        "我的·小磁",  HV_KIND_GARDEN, HV_GROUP_BUILTIN, false, 0xF0C430, "磁", false,
        "skills/my_garden/scripts/main.lua"},
    {"sys:settings",  "控制中心",   HV_KIND_SYSTEM, HV_GROUP_BUILTIN, false, 0x6B7280, LV_SYMBOL_SETTINGS, true, NULL},
    {"sys:presenter", "会议翻页笔", HV_KIND_BUILTIN_LUA, HV_GROUP_OFFICIAL, true, 0xF59E0B, LV_SYMBOL_IMAGE, true,
        "skills/ble_hid/scripts/meeting_presenter.lua"},
    {"sys:snake",     "贪吃蛇",     HV_KIND_BUILTIN_LUA, HV_GROUP_OFFICIAL, true, 0x18A89F, LV_SYMBOL_SHUFFLE, true,
        "skills/snake_game/scripts/snake_game.lua"},
    {"sys:2048",      "2048",       HV_KIND_BUILTIN_LUA, HV_GROUP_OFFICIAL, true, 0xEDC22E, "2048", false,
        "skills/game_2048/scripts/game_2048.lua"},
    {"sys:dino",      "小恐龙",     HV_KIND_BUILTIN_LUA, HV_GROUP_OFFICIAL, true, 0x64748B, LV_SYMBOL_UP, true,
        "skills/dino/scripts/dino.lua"},
};

static hv_app_t *s_apps;
static size_t s_count;

static const char *const s_group_names[HV_GROUP_COUNT] = {"内置应用", "官方应用", "我的创作"};

const char *hv_group_name(hv_group_t g)
{
    return g < HV_GROUP_COUNT ? s_group_names[g] : "其他";
}

const char *hv_kind_name(hv_kind_t k)
{
    switch (k) {
    case HV_KIND_SYSTEM: return "system";
    case HV_KIND_BUILTIN_LUA: return "builtin_lua";
    case HV_KIND_SKILL: return "skill";
    case HV_KIND_PACKAGE: return "package";
    case HV_KIND_GARDEN: return "garden";
    default: return "unknown";
    }
}

static bool hv_catalog_add(const hv_app_t *app, void *ctx)
{
    (void)ctx;
    if (s_count >= HV_CATALOG_MAX || !app || !app->id[0]) {
        return false;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, app->id) == 0) {
            return true;
        }
    }
    s_apps[s_count++] = *app;
    return true;
}

/* 平台提供：枚举 DATA 里已安装的旧格式应用与应用包。 */
typedef bool (*hv_plat_app_cb_t)(const hv_app_t *app, void *ctx);
void hv_plat_enum_installed(hv_plat_app_cb_t cb, void *ctx);
esp_err_t hv_plat_launch_lua(const hv_app_t *app, const char *args_json);
esp_err_t hv_plat_uninstall(const hv_app_t *app);
esp_err_t hv_plat_rename(const hv_app_t *app, const char *title);
esp_err_t hv_plat_request(int type, const char *id, const char *title);

size_t hv_catalog_refresh(void)
{
    if (!s_apps) {
        s_apps = calloc(HV_CATALOG_MAX, sizeof(*s_apps));
        if (!s_apps) {
            s_count = 0;
            return 0;
        }
    }
    s_count = 0;
    for (size_t i = 0; i < sizeof(s_builtins) / sizeof(s_builtins[0]); i++) {
        const hv_builtin_t *b = &s_builtins[i];
        hv_app_t app = {0};

        if (b->script) {
            snprintf(app.script, sizeof(app.script), "%s/%s", hv_plat_system_root(), b->script);
            if (!hv_plat_file_exists(app.script)) {
                continue;       /* 这一版固件没带这个 Lua，就不列出来 */
            }
        }
        strlcpy(app.id, b->id, sizeof(app.id));
        strlcpy(app.title, b->title, sizeof(app.title));
        app.kind = b->kind;
        app.group = b->group;
        app.ai = b->ai;
        app.color = b->color;
        strlcpy(app.glyph, b->glyph, sizeof(app.glyph));
        app.glyph_symbol = b->symbol;
        (void)hv_catalog_add(&app, NULL);
    }
    hv_plat_enum_installed(hv_catalog_add, NULL);
    return s_count;
}

size_t hv_catalog_count(void)
{
    return s_count;
}

const hv_app_t *hv_catalog_at(size_t i)
{
    return i < s_count ? &s_apps[i] : NULL;
}

const hv_app_t *hv_catalog_find(const char *id)
{
    for (size_t i = 0; id && i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
            return &s_apps[i];
        }
    }
    /* 兼容 0.6.0 App：无前缀 ID 视为应用包。 */
    if (id && !strchr(id, ':') && strcmp(id, "garden") != 0) {
        char prefixed[HV_APP_ID_LEN];
        snprintf(prefixed, sizeof(prefixed), "app:%s", id);
        for (size_t i = 0; i < s_count; i++) {
            if (strcmp(s_apps[i].id, prefixed) == 0) {
                return &s_apps[i];
            }
        }
    }
    return NULL;
}

size_t hv_catalog_group_count(hv_group_t g)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count; i++) {
        n += s_apps[i].group == g;
    }
    return n;
}

esp_err_t hv_catalog_launch(const char *id, const char *args_json)
{
    const hv_app_t *app = hv_catalog_find(id);

    if (!app) {
        hv_toast("找不到这个应用");
        return ESP_ERR_NOT_FOUND;
    }
    system_ui_click_feedback();
    if (strcmp(app->id, "sys:meeting") == 0) {
        return system_ui_show_meeting_minutes();
    }
    if (strcmp(app->id, "sys:ir") == 0) {
        return system_ui_show_infrared_remote();
    }
    if (strcmp(app->id, "sys:card") == 0) {
        return system_ui_show_businesscard();
    }
    if (strcmp(app->id, "sys:workbuddy") == 0) {
        return system_ui_show_workbuddy();
    }
    if (strcmp(app->id, "sys:settings") == 0) {
        hv_open_control();
        return ESP_OK;
    }
    if (!app->script[0]) {
        hv_toast("这个应用还没有可运行的入口");
        return ESP_ERR_NOT_SUPPORTED;
    }
    hv_toast(app->kind == HV_KIND_GARDEN ? "正在进入我的花园" : "正在打开…");
    return hv_plat_launch_lua(app, args_json);
}

esp_err_t hv_catalog_request_uninstall(const char *id)
{
    const hv_app_t *app = hv_catalog_find(id);
    if (!app || !app->removable) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return hv_plat_request(SYSTEM_UI_WORK_EVENT_APP_UNINSTALL, app->id, NULL);
}

esp_err_t hv_catalog_request_rename(const char *id, const char *title)
{
    const hv_app_t *app = hv_catalog_find(id);
    if (!app || !app->renamable || !title || !title[0]) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return hv_plat_request(SYSTEM_UI_WORK_EVENT_APP_RENAME, app->id, title);
}

esp_err_t hv_catalog_uninstall_now(const char *id)
{
    hv_app_t copy;
    const hv_app_t *app;

    if (system_ui_lock() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    app = hv_catalog_find(id);
    if (app) {
        copy = *app;
    }
    system_ui_unlock();
    if (!app) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!copy.removable) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return hv_plat_uninstall(&copy);
}

esp_err_t hv_catalog_rename_now(const char *id, const char *title)
{
    hv_app_t copy;
    const hv_app_t *app;

    if (system_ui_lock() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    app = hv_catalog_find(id);
    if (app) {
        copy = *app;
    }
    system_ui_unlock();
    if (!app) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!copy.renamable) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return hv_plat_rename(&copy, title);
}

/* ------------------------------------------------------------- 图标 */

/* 文字图标取标题第一个字（UTF-8 完整字符）。 */
static void hv_first_char(const char *title, char *out, size_t size)
{
    size_t n = 1;
    unsigned char c = (unsigned char)title[0];

    if (c >= 0xF0) {
        n = 4;
    } else if (c >= 0xE0) {
        n = 3;
    } else if (c >= 0xC0) {
        n = 2;
    }
    if (n >= size || strlen(title) < n) {
        n = 0;
    }
    memcpy(out, title, n);
    out[n] = '\0';
}

static uint32_t hv_dim(uint32_t c, int percent)
{
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)percent / 100;
    uint32_t g = ((c >> 8) & 0xFF) * (uint32_t)percent / 100;
    uint32_t b = (c & 0xFF) * (uint32_t)percent / 100;
    return (r << 16) | (g << 8) | b;
}

lv_obj_t *hv_app_icon_create(lv_obj_t *parent, const hv_app_t *app, int32_t size, bool with_label)
{
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_t *tile;
    lv_obj_t *glyph;
    int32_t label_h = with_label ? 30 : 0;
    int32_t wrap_w = size + 32;

    hv_plain(wrap);
    lv_obj_set_size(wrap, wrap_w, size + label_h + 6);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(wrap, 18, 0);

    /* 应用图标：深色圆角方块 + 身份色符号，延续原轮播的黄绿身份色。 */
    tile = lv_obj_create(wrap);
    hv_plain(tile);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tile, size, size);
    lv_obj_align(tile, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_radius(tile, size * 28 / 100, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x23262E), 0);
    lv_obj_set_style_bg_grad_color(tile, lv_color_hex(0x14161B), 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(hv_dim(app->color, 45)), 0);

    if (app->icon_path[0]) {
        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, app->icon_path);
        /* 应用包图标是 256x256，缩到图标框内。 */
        lv_image_set_scale(img, (uint32_t)(size * 256 / 256 * 220 / 256));
        lv_obj_center(img);
    } else {
        char text[16];
        const lv_font_t *font;

        if (app->glyph[0]) {
            strlcpy(text, app->glyph, sizeof(text));
        } else {
            hv_first_char(app->title, text, sizeof(text));
        }
        if (app->glyph_symbol) {
            font = hv_symbol_font(size * 44 / 100);
        } else if (strlen(text) > 3) {
            font = g_hvf.f16 ? g_hvf.f16 : s_ui.font;      /* 如 "2048" */
        } else {
            font = g_hvf.f28 ? g_hvf.f28 : s_ui.font;
        }
        glyph = hv_text(tile, font, app->color, text);
        lv_obj_center(glyph);
    }

    if (app->ai) {
        lv_obj_t *badge = hv_box(wrap, 0, 0, 30, 18, HV_C_ACCENT, 9);
        lv_obj_t *t = hv_text(badge, &lv_font_montserrat_14, HV_C_ACCENT_INK, "AI");
        lv_obj_center(t);
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -4, -2);
    }
    if (with_label) {
        lv_obj_t *label = hv_text(wrap, g_hvf.f16 ? g_hvf.f16 : s_ui.font, HV_C_TEXT, app->title);
        lv_obj_set_width(label, wrap_w);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, size + 8);
    }
    return wrap;
}
