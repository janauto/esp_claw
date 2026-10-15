/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 首页小组件。按《小组件规范 v0.1》：应用只交数据、系统按模板画；底面由内容决定
 * （天气天空蓝、股票深石板、热榜暖色、语音深色带黄），「我的」和时钟没有底面。
 *
 * 内置：mine 我的 / clock 时钟 / weather 天气 / stocks 股票 / aihot 热榜 /
 *       voice 语音助手 / system 设备状态。
 * 应用声明：任何应用把 <DATA>/widgets/<应用ID>.json 写成
 *   {"v":1,"title":"喝水","value":"3/8","unit":"杯","sub":"目标 8 杯","color":"#2E7DD7",
 *    "app":"skill:water","sizes":["S","M"]}
 * 就会出现在「添加小组件」里（ID 为 "x:<文件名>"），按 metric 模板绘制。
 */
#include "home_priv.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#define HV_APP_WIDGETS_MAX 12

static hv_widget_desc_t s_builtin_widgets[] = {
    {"mine",    "我的",     "小磁与你的花园，点开进入「我的」", HV_SZ_BIT(HV_SZ_S) | HV_SZ_BIT(HV_SZ_M), "garden", true, false},
    {"clock",   "时钟",     "表盘时钟；M 尺寸为数字时钟与日期", HV_SZ_BIT(HV_SZ_S) | HV_SZ_BIT(HV_SZ_M), "", false, false},
    {"weather", "天气",     "实时天气与今日温度范围",           HV_SZ_BIT(HV_SZ_S) | HV_SZ_BIT(HV_SZ_M), "skill:live_weather", false, true},
    {"stocks",  "股票",     "自选股行情，红涨绿跌",             HV_SZ_BIT(HV_SZ_S) | HV_SZ_BIT(HV_SZ_M), "skill:live_stocks", false, true},
    {"aihot",   "AI 热榜",  "AIHOT 实时热点标题",               HV_SZ_BIT(HV_SZ_M) | HV_SZ_BIT(HV_SZ_L), "skill:live_aihot", false, true},
    {"voice",   "语音助手", "按住 M 说话；显示最近一次对话",     HV_SZ_BIT(HV_SZ_S) | HV_SZ_BIT(HV_SZ_M), "", false, false},
    {"system",  "设备状态", "当前模式、Wi-Fi 与手机连接",       HV_SZ_BIT(HV_SZ_S) | HV_SZ_BIT(HV_SZ_M), "sys:settings", false, false},
};
#define HV_BUILTIN_WIDGETS (sizeof(s_builtin_widgets) / sizeof(s_builtin_widgets[0]))

static hv_widget_desc_t s_app_widgets[HV_APP_WIDGETS_MAX];
static size_t s_app_widget_count;

static char s_voice_heard[160];
static char s_voice_reply[320];

size_t hv_widgets_count(void)
{
    return HV_BUILTIN_WIDGETS + s_app_widget_count;
}

const hv_widget_desc_t *hv_widgets_at(size_t i)
{
    if (i < HV_BUILTIN_WIDGETS) {
        return &s_builtin_widgets[i];
    }
    i -= HV_BUILTIN_WIDGETS;
    return i < s_app_widget_count ? &s_app_widgets[i] : NULL;
}

const hv_widget_desc_t *hv_widgets_find(const char *id)
{
    for (size_t i = 0; id && i < hv_widgets_count(); i++) {
        const hv_widget_desc_t *d = hv_widgets_at(i);
        if (strcmp(d->id, id) == 0) {
            return d;
        }
    }
    return NULL;
}

static cJSON *hv_read_json(const char *path, size_t max)
{
    char *text = hv_plat_read_text(path, max);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    free(text);
    return json;
}

void hv_widgets_rescan(void)
{
    char dir_path[SYSTEM_UI_PATH_MAX];
    DIR *dir;
    struct dirent *de;

    s_app_widget_count = 0;
    snprintf(dir_path, sizeof(dir_path), "%s/widgets", hv_plat_data_root());
    dir = opendir(dir_path);
    if (!dir) {
        return;
    }
    while ((de = readdir(dir)) != NULL && s_app_widget_count < HV_APP_WIDGETS_MAX) {
        size_t len = strlen(de->d_name);
        char path[SYSTEM_UI_PATH_MAX];
        hv_widget_desc_t *d = &s_app_widgets[s_app_widget_count];
        cJSON *json;
        const cJSON *title, *sizes, *app;

        if (len < 6 || len > 30 || strcmp(de->d_name + len - 5, ".json") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name);
        json = hv_read_json(path, 2048);
        title = cJSON_GetObjectItemCaseSensitive(json, "title");
        if (!cJSON_IsString(title) || !title->valuestring[0]) {
            cJSON_Delete(json);
            continue;
        }
        memset(d, 0, sizeof(*d));
        snprintf(d->id, sizeof(d->id), "x:%.*s", (int)(len - 5), de->d_name);
        strlcpy(d->name, title->valuestring, sizeof(d->name));
        hv_utf8_clip(d->name, 24);
        strlcpy(d->desc, "应用声明的小组件", sizeof(d->desc));
        sizes = cJSON_GetObjectItemCaseSensitive(json, "sizes");
        if (cJSON_IsArray(sizes)) {
            const cJSON *s;
            cJSON_ArrayForEach(s, sizes) {
                int sz = cJSON_IsString(s) ? hv_size_from_name(s->valuestring) : -1;
                if (sz == HV_SZ_S || sz == HV_SZ_M) {
                    d->sizes |= (uint8_t)HV_SZ_BIT(sz);
                }
            }
        }
        if (!d->sizes) {
            d->sizes = HV_SZ_BIT(HV_SZ_S) | HV_SZ_BIT(HV_SZ_M);
        }
        app = cJSON_GetObjectItemCaseSensitive(json, "app");
        if (cJSON_IsString(app)) {
            strlcpy(d->app, app->valuestring, sizeof(d->app));
        }
        d->ai = true;
        s_app_widget_count++;
        cJSON_Delete(json);
    }
    closedir(dir);
}

void hv_widgets_note_voice(const char *heard, const char *reply)
{
    if (heard) {
        strlcpy(s_voice_heard, heard, sizeof(s_voice_heard));
    }
    if (reply) {
        strlcpy(s_voice_reply, reply, sizeof(s_voice_reply));
    }
}

/* ------------------------------------------------------------ 实例 */

typedef struct {
    char wid[HV_WID_LEN];
    uint8_t sz;
    int32_t w, h;
    lv_obj_t *card;
    lv_obj_t *l[8];
    lv_obj_t *extra[4];
    /* 时钟指针 */
    lv_obj_t *hand[3];
    lv_point_precise_t pts[3][2];
    int32_t cx, cy, r;
} hv_wi_t;

static void hv_wi_delete_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static const lv_font_t *F(lv_font_t *f)
{
    return f ? f : s_ui.font;
}

static lv_obj_t *hv_card(lv_obj_t *parent, int32_t w, int32_t h, uint32_t top, uint32_t bottom)
{
    lv_obj_t *card = hv_box(parent, 0, 0, w, h, top, 26);
    if (bottom != top) {
        lv_obj_set_style_bg_grad_color(card, lv_color_hex(bottom), 0);
        lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    }
    lv_obj_set_style_pad_all(card, 18, 0);
    return card;
}

static lv_obj_t *hv_label_at(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                             const char *text, lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *l = hv_text(parent, font, color, text);
    lv_obj_align(l, align, x, y);
    return l;
}

/* 设备字库没有 “°”，用小圆环代替。 */
static lv_obj_t *hv_degree(lv_obj_t *parent, int32_t d, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    hv_plain(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, d >= 12 ? 3 : 2, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    return o;
}

static void hv_set_text(lv_obj_t *l, const char *text)
{
    if (l && text && strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static void hv_age_text(int64_t age_ms, char *out, size_t size)
{
    if (age_ms < 0) {
        strlcpy(out, "缓存", size);
    } else if (age_ms < 120000) {
        strlcpy(out, "刚刚", size);
    } else if (age_ms < 3600000) {
        snprintf(out, size, "%d 分钟前", (int)(age_ms / 60000));
    } else {
        snprintf(out, size, "%d 小时前", (int)(age_ms / 3600000));
    }
}

/* ---------------------------------------------------------------- 我的 */

static void hv_mine_create(hv_wi_t *wi, lv_obj_t *root)
{
    int32_t pet = wi->sz == HV_SZ_S ? wi->h - 30 : wi->h - 16;
    lv_obj_t *p = hv_pet_create(root, pet);
    lv_obj_align(p, wi->sz == HV_SZ_S ? LV_ALIGN_TOP_MID : LV_ALIGN_LEFT_MID, 0, 0);
    wi->extra[0] = p;
    if (wi->sz == HV_SZ_S) {
        /* 小尺寸也要看得出花园里的照料结果：心情与能量胶囊 */
        lv_obj_t *pill = hv_box(root, 0, 0, wi->w - 40, 30, 0x2A2410, 15);
        lv_obj_set_style_border_width(pill, 1, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(0x6B5A1A), 0);
        lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -2);
        wi->l[2] = hv_label_at(pill, F(g_hvf.f16), HV_C_ACCENT, "", LV_ALIGN_CENTER, 0, 0);
    }
    if (wi->sz == HV_SZ_M) {
        lv_obj_t *bubble = hv_box(root, 0, 0, wi->w - pet - 12, 96, 0xFFF8E1, 20);
        lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, 0, -12);
        lv_obj_set_style_pad_all(bubble, 12, 0);
        wi->l[0] = hv_label_at(bubble, F(g_hvf.f20), 0x2A2310, "", LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_width(wi->l[0], wi->w - pet - 36);
        lv_label_set_long_mode(wi->l[0], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(wi->l[0], 52);
        wi->l[1] = hv_label_at(root, F(g_hvf.f16), HV_C_MUTED, "", LV_ALIGN_BOTTOM_RIGHT, -8, 0);
    }
}

static void hv_mine_refresh(hv_wi_t *wi)
{
    char path[SYSTEM_UI_PATH_MAX];
    cJSON *snap;
    const cJSON *say, *mood, *name, *theme;
    char line[128];

    snprintf(path, sizeof(path), "%s/garden/snapshot.json", hv_plat_data_root());
    if (wi->sz == HV_SZ_S) {
        cJSON *s = hv_read_json(path, 2048);
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(s, "mood");
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(s, "energy");
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(s, "pet_name");
        if (cJSON_IsString(m)) {
            snprintf(line, sizeof(line), "%s %s  能量%d", cJSON_IsString(n) ? n->valuestring : "小磁",
                     m->valuestring, cJSON_IsNumber(e) ? e->valueint : 0);
        } else {
            strlcpy(line, "点我进入花园", sizeof(line));
        }
        hv_set_text(wi->l[2], line);
        cJSON_Delete(s);
        return;
    }
    snap = hv_read_json(path, 2048);
    say = cJSON_GetObjectItemCaseSensitive(snap, "say");
    mood = cJSON_GetObjectItemCaseSensitive(snap, "mood");
    name = cJSON_GetObjectItemCaseSensitive(snap, "pet_name");
    theme = cJSON_GetObjectItemCaseSensitive(snap, "theme_name");
    hv_set_text(wi->l[0], cJSON_IsString(say) && say->valuestring[0] ? say->valuestring
                                                                       : "欢迎回家，点我进入我的花园");
    snprintf(line, sizeof(line), "%s  %s  %s",
             cJSON_IsString(name) ? name->valuestring : "小磁",
             cJSON_IsString(mood) ? mood->valuestring : "开心",
             cJSON_IsString(theme) ? theme->valuestring : "小家");
    hv_set_text(wi->l[1], line);
    cJSON_Delete(snap);
}

/* ---------------------------------------------------------------- 时钟 */

static void hv_clock_hand(hv_wi_t *wi, int i, double turns, int32_t len, int32_t width, uint32_t color)
{
    double a = turns * 2.0 * M_PI - M_PI / 2.0;
    wi->pts[i][0].x = wi->cx;
    wi->pts[i][0].y = wi->cy;
    wi->pts[i][1].x = wi->cx + (int32_t)(cos(a) * len);
    wi->pts[i][1].y = wi->cy + (int32_t)(sin(a) * len);
    if (!wi->hand[i]) {
        wi->hand[i] = lv_line_create(wi->card);
        lv_obj_set_style_line_width(wi->hand[i], width, 0);
        lv_obj_set_style_line_color(wi->hand[i], lv_color_hex(color), 0);
        lv_obj_set_style_line_rounded(wi->hand[i], true, 0);
        lv_obj_set_pos(wi->hand[i], 0, 0);
    }
    lv_line_set_points(wi->hand[i], wi->pts[i], 2);
}

static void hv_clock_create(hv_wi_t *wi, lv_obj_t *root)
{
    if (wi->sz == HV_SZ_S) {
        int32_t d = (wi->h < wi->w ? wi->h : wi->w) - 4;
        lv_obj_t *face = hv_box(root, 0, 0, d, d, 0x0E0F12, LV_RADIUS_CIRCLE);
        lv_obj_align(face, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_border_width(face, 3, 0);
        lv_obj_set_style_border_color(face, lv_color_hex(0x2B2E36), 0);
        lv_obj_set_style_shadow_width(face, 18, 0);
        lv_obj_set_style_shadow_opa(face, LV_OPA_40, 0);
        wi->card = face;
        wi->cx = d / 2;
        wi->cy = d / 2;
        wi->r = d / 2;
        /* 12 个刻度：整点长、其他短 */
        for (int i = 0; i < 12; i++) {
            double a = i * M_PI / 6.0;
            int32_t len = (i % 3 == 0) ? 12 : 7;
            int32_t rr = wi->r - 10;
            lv_obj_t *tick = hv_box(face, 0, 0, i % 3 == 0 ? 4 : 3, len, i % 3 == 0 ? 0xE8EAEE : 0x8C929C, 2);
            lv_obj_set_pos(tick, wi->cx + (int32_t)(sin(a) * (rr - len / 2)) - 2,
                           wi->cy - (int32_t)(cos(a) * (rr - len / 2)) - len / 2);
            lv_obj_set_style_transform_rotation(tick, i * 300, 0);
            lv_obj_set_style_transform_pivot_x(tick, 2, 0);
            lv_obj_set_style_transform_pivot_y(tick, len / 2, 0);
        }
        hv_clock_hand(wi, 0, 0, wi->r * 50 / 100, 6, 0xF2F4F8);
        hv_clock_hand(wi, 1, 0, wi->r * 72 / 100, 4, 0xF2F4F8);
        hv_clock_hand(wi, 2, 0, wi->r * 80 / 100, 2, HV_C_ACCENT);
        lv_obj_t *hub = hv_box(face, wi->cx - 5, wi->cy - 5, 10, 10, HV_C_ACCENT, LV_RADIUS_CIRCLE);
        (void)hub;
    } else {
        lv_obj_t *card = hv_card(root, wi->w, wi->h, 0x101216, 0x101216);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(HV_C_LINE), 0);
        wi->card = card;
        wi->l[0] = hv_label_at(card, F(g_hvf.f64), HV_C_TEXT, "--:--", LV_ALIGN_LEFT_MID, 4, -8);
        wi->l[1] = hv_label_at(card, F(g_hvf.f20), HV_C_MUTED, "", LV_ALIGN_TOP_RIGHT, -4, 6);
        wi->l[2] = hv_label_at(card, F(g_hvf.f20), HV_C_ACCENT, "", LV_ALIGN_BOTTOM_RIGHT, -4, -6);
    }
}

static void hv_clock_refresh(hv_wi_t *wi)
{
    struct tm now;
    bool ok = hv_plat_now(&now);
    static const char *const wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

    if (wi->sz == HV_SZ_S) {
        double h = ok ? (now.tm_hour % 12 + now.tm_min / 60.0) / 12.0 : 10.0 / 12.0;
        double m = ok ? (now.tm_min + now.tm_sec / 60.0) / 60.0 : 10.0 / 60.0;
        double s = ok ? now.tm_sec / 60.0 : 0;
        hv_clock_hand(wi, 0, h, wi->r * 50 / 100, 6, 0xF2F4F8);
        hv_clock_hand(wi, 1, m, wi->r * 72 / 100, 4, 0xF2F4F8);
        hv_clock_hand(wi, 2, s, wi->r * 80 / 100, 2, HV_C_ACCENT);
    } else {
        char t[16], d[32];
        if (ok) {
            snprintf(t, sizeof(t), "%02d:%02d", now.tm_hour, now.tm_min);
            snprintf(d, sizeof(d), "%d月%d日", now.tm_mon + 1, now.tm_mday);
        } else {
            strlcpy(t, "--:--", sizeof(t));
            strlcpy(d, "等待对时", sizeof(d));
        }
        hv_set_text(wi->l[0], t);
        hv_set_text(wi->l[1], d);
        hv_set_text(wi->l[2], ok ? wd[now.tm_wday % 7] : "");
    }
}

/* ---------------------------------------------------------------- 天气 */

static void hv_weather_create(hv_wi_t *wi, lv_obj_t *root)
{
    lv_obj_t *card = hv_card(root, wi->w, wi->h, 0x4C8BE0, 0x24508F);
    wi->card = card;
    wi->l[0] = hv_label_at(card, F(g_hvf.f16), 0xDCE8FF, "实时天气", LV_ALIGN_TOP_LEFT, 0, 0);
    wi->l[1] = hv_label_at(card, F(g_hvf.f64), 0xFFFFFF, "--", LV_ALIGN_TOP_LEFT, 0, 18);
    wi->extra[0] = hv_degree(card, 14, 0xFFFFFF);
    wi->l[2] = hv_label_at(card, F(g_hvf.f20), 0xFFFFFF, "", LV_ALIGN_BOTTOM_LEFT, 0, -24);
    wi->l[3] = hv_label_at(card, F(g_hvf.f16), 0xDCE8FF, "", LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_width(wi->l[2], wi->w - 36);
    lv_label_set_long_mode(wi->l[2], LV_LABEL_LONG_MODE_DOTS);
    if (wi->sz == HV_SZ_M) {
        wi->l[4] = hv_label_at(card, F(g_hvf.f16), 0xDCE8FF, "", LV_ALIGN_TOP_RIGHT, 0, 4);
        wi->l[5] = hv_label_at(card, F(g_hvf.f20), 0xFFFFFF, "", LV_ALIGN_RIGHT_MID, 0, 6);
        lv_obj_set_style_text_align(wi->l[5], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static const char *hv_weather_text(int code)
{
    if (code == 0) return "晴";
    if (code >= 1 && code <= 3) return "多云";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "毛毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code == 85 || code == 86) return "阵雪";
    if (code >= 95 && code <= 99) return "雷雨";
    return "天气";
}

/* 首页标题栏也要用天气摘要。天气数据半小时才更新一次：缓存 60 s，避免每 5 s 解析整份 JSON。 */
bool hv_weather_summary(char *out, size_t size, int *temp_out)
{
    static uint32_t cached_at;
    static bool cached_ok;
    static int cached_temp;
    static char cached_cond[24];
    uint32_t now = lv_tick_get();

    if (cached_at && now - cached_at < 60000) {
        if (cached_ok) {
            *temp_out = cached_temp;
            strlcpy(out, cached_cond, size);
        }
        return cached_ok;
    }
    cached_at = now ? now : 1;
    cached_ok = false;
    int64_t age = 0;
    char *text = hv_plat_feed_read("weather", &age);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(json, "current");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
    bool ok = cJSON_IsNumber(t);

    free(text);
    if (ok) {
        *temp_out = (int)lround(t->valuedouble);
        strlcpy(out, cJSON_IsNumber(code) ? hv_weather_text(code->valueint) : "", size);
        cached_ok = true;
        cached_temp = *temp_out;
        strlcpy(cached_cond, out, sizeof(cached_cond));
    }
    cJSON_Delete(json);
    return ok;
}

/* 给语音助手的天气上下文：城市、当前温度与天气、今天最低/最高。 */
bool hv_weather_context(char *out, size_t size)
{
    int64_t age = 0;
    char *text = hv_plat_feed_read("weather", &age);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(json, "city");
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(json, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(json, "daily");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
    const cJSON *hi = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"), 0);
    const cJSON *lo = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"), 0);
    const cJSON *hi2 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"), 1);
    const cJSON *lo2 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"), 1);
    bool ok = cJSON_IsNumber(t);

    free(text);
    if (ok) {
        snprintf(out, size, "%s现在%d度%s，今天%d到%d度%s", cJSON_IsString(city) ? city->valuestring : "本地",
                 (int)lround(t->valuedouble), cJSON_IsNumber(code) ? hv_weather_text(code->valueint) : "",
                 cJSON_IsNumber(lo) ? (int)lround(lo->valuedouble) : 0,
                 cJSON_IsNumber(hi) ? (int)lround(hi->valuedouble) : 0,
                 (cJSON_IsNumber(lo2) && cJSON_IsNumber(hi2)) ? "" : "");
        if (cJSON_IsNumber(lo2) && cJSON_IsNumber(hi2)) {
            size_t used = strlen(out);
            snprintf(out + used, size - used, "，明天%d到%d度", (int)lround(lo2->valuedouble), (int)lround(hi2->valuedouble));
        }
    }
    cJSON_Delete(json);
    return ok;
}

static void hv_weather_refresh(hv_wi_t *wi)
{
    int64_t age = 0;
    char *text = hv_plat_feed_read("weather", &age);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(json, "city");
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(json, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(json, "daily");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
    const cJSON *hi = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"), 0);
    const cJSON *lo = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"), 0);
    char buf[96];

    free(text);
    if (!cJSON_IsNumber(t)) {
        hv_set_text(wi->l[1], "--");
        hv_set_text(wi->l[2], "暂无数据");
        hv_set_text(wi->l[3], "联网后自动更新");
        lv_obj_align_to(wi->extra[0], wi->l[1], LV_ALIGN_OUT_RIGHT_TOP, 4, 10);
        cJSON_Delete(json);
        return;
    }
    snprintf(buf, sizeof(buf), "%d", (int)lround(t->valuedouble));
    hv_set_text(wi->l[1], buf);
    lv_obj_update_layout(wi->l[1]);
    lv_obj_align_to(wi->extra[0], wi->l[1], LV_ALIGN_OUT_RIGHT_TOP, 4, 10);
    snprintf(buf, sizeof(buf), "%s %s", cJSON_IsString(city) ? city->valuestring : "",
             cJSON_IsNumber(code) ? hv_weather_text(code->valueint) : "");
    hv_set_text(wi->l[2], buf);
    if (cJSON_IsNumber(hi) && cJSON_IsNumber(lo)) {
        snprintf(buf, sizeof(buf), "%d / %d", (int)lround(lo->valuedouble), (int)lround(hi->valuedouble));
    } else {
        buf[0] = 0;
    }
    hv_set_text(wi->l[3], buf);
    if (wi->sz == HV_SZ_M) {
        char age_text[24];
        const cJSON *dates = cJSON_GetObjectItemCaseSensitive(daily, "time");
        char lines[128] = {0};
        hv_age_text(age, age_text, sizeof(age_text));
        hv_set_text(wi->l[4], age_text);
        for (int i = 1; i < 3 && i < cJSON_GetArraySize(dates); i++) {
            const cJSON *d = cJSON_GetArrayItem(dates, i);
            const cJSON *h2 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max"), i);
            const cJSON *l2 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min"), i);
            if (cJSON_IsString(d) && strlen(d->valuestring) >= 10 && cJSON_IsNumber(h2) && cJSON_IsNumber(l2)) {
                char row[48];
                snprintf(row, sizeof(row), "%s%.5s  %d / %d", lines[0] ? "\n" : "", d->valuestring + 5,
                         (int)lround(l2->valuedouble), (int)lround(h2->valuedouble));
                strlcat(lines, row, sizeof(lines));
            }
        }
        hv_set_text(wi->l[5], lines);
    }
    cJSON_Delete(json);
}

/* ---------------------------------------------------------------- 股票 */

static void hv_stocks_create(hv_wi_t *wi, lv_obj_t *root)
{
    lv_obj_t *card = hv_card(root, wi->w, wi->h, 0x1C2230, 0x121621);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3344), 0);
    wi->card = card;
    wi->l[0] = hv_label_at(card, F(g_hvf.f16), HV_C_MUTED, "自选股", LV_ALIGN_TOP_LEFT, 0, 0);
    wi->l[1] = hv_label_at(card, F(g_hvf.f16), HV_C_FAINT, "", LV_ALIGN_TOP_RIGHT, 0, 0);
    int rows = wi->sz == HV_SZ_S ? 3 : 3;
    for (int i = 0; i < rows; i++) {
        int32_t y = 30 + i * 40;
        wi->extra[i] = hv_label_at(card, F(g_hvf.f20), HV_C_TEXT, "", LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_set_width(wi->extra[i], wi->sz == HV_SZ_S ? 96 : 200);
        lv_label_set_long_mode(wi->extra[i], LV_LABEL_LONG_MODE_DOTS);
        wi->l[2 + i] = hv_label_at(card, F(g_hvf.f20), HV_C_TEXT, "", LV_ALIGN_TOP_RIGHT, 0, y);
    }
}

static void hv_stocks_refresh(hv_wi_t *wi)
{
    int64_t age = 0;
    char *text = hv_plat_feed_read("stocks", &age);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(json, "cards");
    int shown = 0;
    char age_text[24];

    free(text);
    hv_age_text(age, age_text, sizeof(age_text));
    hv_set_text(wi->l[1], json ? age_text : "");
    for (int i = 0; i < 3; i++) {
        const cJSON *c = cJSON_GetArrayItem(cards, i);
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(c, "name");
        const cJSON *price = cJSON_GetObjectItemCaseSensitive(c, "price");
        const cJSON *pct = cJSON_GetObjectItemCaseSensitive(c, "percent");
        char buf[48];

        if (!cJSON_IsString(name) || !cJSON_IsNumber(pct)) {
            hv_set_text(wi->extra[i], i == 0 && !json ? "暂无数据" : "");
            hv_set_text(wi->l[2 + i], "");
            continue;
        }
        hv_set_text(wi->extra[i], name->valuestring);
        if (wi->sz == HV_SZ_M && cJSON_IsNumber(price)) {
            snprintf(buf, sizeof(buf), "%.2f  %+.2f%%", price->valuedouble, pct->valuedouble);
        } else {
            snprintf(buf, sizeof(buf), "%+.2f%%", pct->valuedouble);
        }
        hv_set_text(wi->l[2 + i], buf);
        /* 中国习惯：红涨绿跌；过期变灰 */
        uint32_t color = pct->valuedouble >= 0 ? 0xFF5A5F : 0x2FBF71;
        if (age > 1800000) {
            color = HV_C_FAINT;
        }
        lv_obj_set_style_text_color(wi->l[2 + i], lv_color_hex(color), 0);
        shown++;
    }
    (void)shown;
    cJSON_Delete(json);
}

/* ---------------------------------------------------------------- 热榜 */

static void hv_aihot_create(hv_wi_t *wi, lv_obj_t *root)
{
    lv_obj_t *card = hv_card(root, wi->w, wi->h, 0x3A2416, 0x1E140D);
    wi->card = card;
    wi->l[0] = hv_label_at(card, F(g_hvf.f16), 0xFFB27A, "AIHOT 热榜", LV_ALIGN_TOP_LEFT, 0, 0);
    wi->l[1] = hv_label_at(card, F(g_hvf.f16), 0xA08470, "", LV_ALIGN_TOP_RIGHT, 0, 0);
    int rows = wi->sz == HV_SZ_L ? 6 : 2;
    int32_t line_h = wi->sz == HV_SZ_L ? 56 : 52;
    for (int i = 0; i < rows && i < 6; i++) {
        lv_obj_t *l = hv_label_at(card, F(g_hvf.f20), 0xFFF3EA, "", LV_ALIGN_TOP_LEFT, 0, 30 + i * line_h);
        lv_obj_set_width(l, wi->w - 36);
        lv_obj_set_height(l, line_h - 6);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        if (i < 6) {
            if (i < 4) {
                wi->extra[i] = l;
            } else {
                wi->l[2 + i] = l;
            }
        }
    }
}

static lv_obj_t *hv_aihot_row(hv_wi_t *wi, int i)
{
    return i < 4 ? wi->extra[i] : wi->l[2 + i];
}

static void hv_aihot_refresh(hv_wi_t *wi)
{
    int64_t age = 0;
    char *text = hv_plat_feed_read("aihot", &age);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(json, "cards");
    int rows = wi->sz == HV_SZ_L ? 6 : 2;
    char age_text[24];

    free(text);
    hv_age_text(age, age_text, sizeof(age_text));
    hv_set_text(wi->l[1], json ? age_text : "");
    for (int i = 0; i < rows; i++) {
        const cJSON *c = cJSON_GetArrayItem(cards, i);
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(c, "value");
        char buf[200];
        if (cJSON_IsString(v)) {
            snprintf(buf, sizeof(buf), "%d  %s", i + 1, v->valuestring);
        } else {
            strlcpy(buf, i == 0 && !json ? "暂无数据，联网后自动更新" : "", sizeof(buf));
        }
        hv_set_text(hv_aihot_row(wi, i), buf);
    }
    cJSON_Delete(json);
}

/* -------------------------------------------------------------- 语音助手 */

static void hv_voice_w_create(hv_wi_t *wi, lv_obj_t *root)
{
    lv_obj_t *card = hv_card(root, wi->w, wi->h, 0x1D1A10, 0x111009);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x4A3F14), 0);
    wi->card = card;
    wi->l[0] = hv_label_at(card, F(g_hvf.f16), HV_C_ACCENT, "语音助手", LV_ALIGN_TOP_LEFT, 0, 0);
    wi->l[1] = hv_label_at(card, F(g_hvf.f16), HV_C_MUTED, "", LV_ALIGN_TOP_RIGHT, 0, 0);
    wi->l[2] = hv_label_at(card, F(g_hvf.f20), HV_C_TEXT, "", LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_width(wi->l[2], wi->w - 36);
    lv_obj_set_height(wi->l[2], wi->sz == HV_SZ_S ? 58 : wi->h - 100);
    lv_label_set_long_mode(wi->l[2], LV_LABEL_LONG_MODE_DOTS);
    /* 底部麦克风胶囊：按住 M 说话 */
    lv_obj_t *pill = hv_box(card, 0, 0, wi->w - 36, 36, HV_C_ACCENT, 18);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, 0);
    wi->l[3] = hv_label_at(pill, F(g_hvf.f16), HV_C_ACCENT_INK, "按住 M 说话", LV_ALIGN_CENTER, 0, 0);
}

static void hv_voice_w_refresh(hv_wi_t *wi)
{
    char buf[400];
    hv_set_text(wi->l[1], hv_voice_route_name());
    if (s_voice_reply[0]) {
        if (wi->sz == HV_SZ_M && s_voice_heard[0]) {
            snprintf(buf, sizeof(buf), "你：%s\n%s", s_voice_heard, s_voice_reply);
        } else {
            strlcpy(buf, s_voice_reply, sizeof(buf));
        }
    } else {
        strlcpy(buf, wi->sz == HV_SZ_M ? "试试说：今天天气怎么样？把音量调到一半。打开贪吃蛇。"
                                        : "试试说：今天天气怎么样？", sizeof(buf));
    }
    hv_set_text(wi->l[2], buf);
}

/* -------------------------------------------------------------- 设备状态 */

static void hv_system_create(hv_wi_t *wi, lv_obj_t *root)
{
    lv_obj_t *card = hv_card(root, wi->w, wi->h, 0x1B1E25, 0x121419);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(HV_C_LINE), 0);
    wi->card = card;
    wi->l[0] = hv_label_at(card, F(g_hvf.f16), HV_C_MUTED, "当前模式", LV_ALIGN_TOP_LEFT, 0, 0);
    wi->l[1] = hv_label_at(card, F(g_hvf.f28), HV_C_ACCENT, "", LV_ALIGN_TOP_LEFT, 0, 22);
    wi->l[2] = hv_label_at(card, F(g_hvf.f16), HV_C_TEXT, "", LV_ALIGN_BOTTOM_LEFT, 0, -26);
    wi->l[3] = hv_label_at(card, F(g_hvf.f16), HV_C_TEXT, "", LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_width(wi->l[2], wi->w - 36);
    lv_obj_set_width(wi->l[3], wi->w - 36);
    lv_label_set_long_mode(wi->l[2], LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(wi->l[3], LV_LABEL_LONG_MODE_DOTS);
}

static void hv_system_refresh(hv_wi_t *wi)
{
    char ssid[40] = {0}, buf[96];
    bool wifi = hv_plat_wifi(ssid, sizeof(ssid));
    bool dock = hv_plat_mode_get() == HV_MODE_DOCK;

    hv_set_text(wi->l[1], dock ? "底座 · Wi-Fi" : "背夹 · 手机");
    snprintf(buf, sizeof(buf), "Wi-Fi  %s", wifi ? ssid : "未连接");
    hv_set_text(wi->l[2], buf);
    snprintf(buf, sizeof(buf), "手机  %s", hv_plat_ble_connected() ? "已连接" : "未连接");
    hv_set_text(wi->l[3], buf);
}

/* ------------------------------------------------------------ 应用声明 */

static uint32_t hv_parse_color(const cJSON *c, uint32_t def)
{
    if (cJSON_IsString(c) && c->valuestring[0] == '#' && strlen(c->valuestring) == 7) {
        return (uint32_t)strtoul(c->valuestring + 1, NULL, 16);
    }
    return def;
}

static void hv_appw_create(hv_wi_t *wi, lv_obj_t *root)
{
    lv_obj_t *card = hv_card(root, wi->w, wi->h, 0x20242C, 0x20242C);
    wi->card = card;
    wi->l[0] = hv_label_at(card, F(g_hvf.f16), 0xFFFFFF, "", LV_ALIGN_TOP_LEFT, 0, 0);
    wi->l[1] = hv_label_at(card, F(g_hvf.f40), 0xFFFFFF, "", LV_ALIGN_LEFT_MID, 0, -4);
    wi->l[2] = hv_label_at(card, F(g_hvf.f20), 0xFFFFFF, "", LV_ALIGN_BOTTOM_LEFT, 0, 0);
    wi->l[3] = hv_label_at(card, F(g_hvf.f16), 0xFFFFFF, "", LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_width(wi->l[2], wi->w - 36);
    lv_label_set_long_mode(wi->l[2], LV_LABEL_LONG_MODE_DOTS);
}

static void hv_appw_refresh(hv_wi_t *wi)
{
    char path[SYSTEM_UI_PATH_MAX];
    cJSON *json;
    const cJSON *title, *value, *unit, *sub;
    char buf[96];

    snprintf(path, sizeof(path), "%s/widgets/%s.json", hv_plat_data_root(), wi->wid + 2);
    json = hv_read_json(path, 2048);
    title = cJSON_GetObjectItemCaseSensitive(json, "title");
    value = cJSON_GetObjectItemCaseSensitive(json, "value");
    unit = cJSON_GetObjectItemCaseSensitive(json, "unit");
    sub = cJSON_GetObjectItemCaseSensitive(json, "sub");
    uint32_t color = hv_parse_color(cJSON_GetObjectItemCaseSensitive(json, "color"), 0x2E7DD7);
    lv_obj_set_style_bg_color(wi->card, lv_color_hex(color), 0);
    lv_obj_set_style_bg_grad_color(wi->card, lv_color_hex(color), 0);
    hv_set_text(wi->l[0], cJSON_IsString(title) ? title->valuestring : "小组件");
    snprintf(buf, sizeof(buf), "%s%s%s", cJSON_IsString(value) ? value->valuestring : "--",
             cJSON_IsString(unit) && unit->valuestring[0] ? " " : "",
             cJSON_IsString(unit) ? unit->valuestring : "");
    hv_set_text(wi->l[1], buf);
    hv_set_text(wi->l[2], cJSON_IsString(sub) ? sub->valuestring : (json ? "" : "暂无数据"));
    hv_set_text(wi->l[3], "AI");
    cJSON_Delete(json);
}

/* -------------------------------------------------------------- 公共入口 */

lv_obj_t *hv_widget_create(lv_obj_t *parent, const char *wid, uint8_t sz, int32_t w, int32_t h)
{
    lv_obj_t *root = lv_obj_create(parent);
    hv_wi_t *wi = calloc(1, sizeof(*wi));

    hv_plain(root);
    lv_obj_set_size(root, w, h);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    if (!wi) {
        return root;
    }
    strlcpy(wi->wid, wid, sizeof(wi->wid));
    wi->sz = sz;
    wi->w = w;
    wi->h = h;
    lv_obj_set_user_data(root, wi);
    lv_obj_add_event_cb(root, hv_wi_delete_cb, LV_EVENT_DELETE, wi);

    if (strcmp(wid, "mine") == 0) {
        hv_mine_create(wi, root);
    } else if (strcmp(wid, "clock") == 0) {
        hv_clock_create(wi, root);
    } else if (strcmp(wid, "weather") == 0) {
        hv_weather_create(wi, root);
    } else if (strcmp(wid, "stocks") == 0) {
        hv_stocks_create(wi, root);
    } else if (strcmp(wid, "aihot") == 0) {
        hv_aihot_create(wi, root);
    } else if (strcmp(wid, "voice") == 0) {
        hv_voice_w_create(wi, root);
    } else if (strcmp(wid, "system") == 0) {
        hv_system_create(wi, root);
    } else if (strncmp(wid, "x:", 2) == 0) {
        hv_appw_create(wi, root);
    }
    /* 卡片内部不要截走点击：点按与长按都交给外层槽位处理。 */
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_remove_flag(lv_obj_get_child(root, (int32_t)i), LV_OBJ_FLAG_CLICKABLE);
    }
    hv_widget_refresh(root);
    return root;
}

void hv_widget_refresh(lv_obj_t *root)
{
    hv_wi_t *wi = root ? lv_obj_get_user_data(root) : NULL;

    if (!wi) {
        return;
    }
    if (strcmp(wi->wid, "mine") == 0) {
        hv_mine_refresh(wi);
    } else if (strcmp(wi->wid, "clock") == 0) {
        hv_clock_refresh(wi);
    } else if (strcmp(wi->wid, "weather") == 0) {
        hv_weather_refresh(wi);
    } else if (strcmp(wi->wid, "stocks") == 0) {
        hv_stocks_refresh(wi);
    } else if (strcmp(wi->wid, "aihot") == 0) {
        hv_aihot_refresh(wi);
    } else if (strcmp(wi->wid, "voice") == 0) {
        hv_voice_w_refresh(wi);
    } else if (strcmp(wi->wid, "system") == 0) {
        hv_system_refresh(wi);
    } else if (strncmp(wi->wid, "x:", 2) == 0) {
        hv_appw_refresh(wi);
    }
}

void hv_widget_tick_second(lv_obj_t *root)
{
    hv_wi_t *wi = root ? lv_obj_get_user_data(root) : NULL;
    if (wi && strcmp(wi->wid, "clock") == 0) {
        hv_clock_refresh(wi);
    }
}
