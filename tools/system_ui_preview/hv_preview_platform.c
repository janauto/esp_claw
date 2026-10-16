/*
 * 1.0 首页宿主预览的平台桩：模拟模式、Wi-Fi、手机、实时数据与已安装应用，
 * 让 home_*.c 的真实界面代码能在电脑上跑并截图。数据都是演示值。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "home_priv.h"

static hv_mode_t s_mode = HV_MODE_CLIP;
static int s_volume = 60;
static bool s_vib = true;

static const char *env_or(const char *k, const char *def)
{
    const char *v = getenv(k);
    return v ? v : def;
}

hv_mode_t hv_plat_mode_get(void) { return s_mode; }
esp_err_t hv_plat_mode_set(hv_mode_t m) { s_mode = m; fprintf(stderr, "PREVIEW_MODE %d\n", m); return ESP_OK; }
bool hv_plat_phone_linked(void) { return atoi(env_or("PREVIEW_PHONE", "1")) != 0; }
bool hv_plat_ble_connected(void) { return hv_plat_phone_linked(); }
bool hv_plat_wifi(char *ssid, size_t n)
{
    bool on = atoi(env_or("PREVIEW_WIFI", "1")) != 0;
    if (ssid && n) {
        strlcpy(ssid, on ? "MyWiFi" : "", n);
    }
    return on;
}
bool hv_plat_ai_ready(void) { return atoi(env_or("PREVIEW_AI", "1")) != 0; }

const char *hv_plat_data_root(void) { return env_or("PREVIEW_DATA", "/tmp/hv-preview-data"); }
const char *hv_plat_system_root(void) { return "/preview-system"; }
bool hv_plat_file_exists(const char *path) { (void)path; return true; }

char *hv_plat_read_text(const char *path, size_t max)
{
    struct stat st;
    FILE *f;
    char *t;
    if (!path || stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > max) {
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    t = calloc((size_t)st.st_size + 1, 1);
    if (t && fread(t, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        free(t);
        t = NULL;
    }
    fclose(f);
    return t;
}

char *hv_plat_feed_read(const char *id, int64_t *age_ms)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/clip_feeds/%s.json", hv_plat_data_root(), id);
    *age_ms = 7 * 60000;
    return hv_plat_read_text(path, 120000);
}

bool hv_plat_now(struct tm *out)
{
    time_t t = 1792077000;      /* 2026-10-16 10:30 左右，固定便于对比截图 */
    gmtime_r(&t, out);
    out->tm_hour = (out->tm_hour + 8) % 24;
    return true;
}

int hv_plat_volume_get(void) { return s_volume; }
esp_err_t hv_plat_volume_set(int p) { s_volume = p; return ESP_OK; }
bool hv_plat_vibration_get(void) { return s_vib; }
void hv_plat_vibration_set(bool on) { s_vib = on; }

esp_err_t hv_plat_voice_begin(bool local) { fprintf(stderr, "PREVIEW_VOICE begin local=%d\n", local); return ESP_OK; }
esp_err_t hv_plat_voice_end(bool local, bool cancel)
{
    fprintf(stderr, "PREVIEW_VOICE end local=%d cancel=%d\n", local, cancel);
    return ESP_OK;
}
esp_err_t hv_plat_voice_ask_text(const char *text) { fprintf(stderr, "PREVIEW_VOICE ask %s\n", text); return ESP_OK; }

lv_font_t *hv_plat_font(int px)
{
#ifdef PREVIEW_FONT_PATH
    return lv_tiny_ttf_create_file("S:" PREVIEW_FONT_PATH, px);
#else
    (void)px;
    return NULL;
#endif
}

void hv_plat_font_free(lv_font_t *f)
{
    if (f) {
        lv_tiny_ttf_destroy(f);
    }
}

typedef bool (*hv_plat_app_cb_t)(const hv_app_t *app, void *ctx);

static void add_app(hv_plat_app_cb_t cb, void *ctx, const char *id, const char *title, hv_kind_t kind,
                    hv_group_t group, uint32_t color)
{
    hv_app_t a = {0};
    strlcpy(a.id, id, sizeof(a.id));
    strlcpy(a.title, title, sizeof(a.title));
    a.kind = kind;
    a.group = group;
    a.ai = true;
    a.removable = true;
    a.renamable = true;
    a.color = color;
    snprintf(a.script, sizeof(a.script), "/sdcard/skills/%s/main.lua", id);
    cb(&a, ctx);
}

void hv_plat_enum_installed(hv_plat_app_cb_t cb, void *ctx)
{
    add_app(cb, ctx, "skill:live_weather", "天气", HV_KIND_SKILL, HV_GROUP_OFFICIAL, 0x4C8BE0);
    add_app(cb, ctx, "skill:live_aihot", "AIHOT AI资讯", HV_KIND_SKILL, HV_GROUP_OFFICIAL, 0xF08A24);
    add_app(cb, ctx, "skill:live_stocks", "股票自选", HV_KIND_SKILL, HV_GROUP_OFFICIAL, 0xE5484D);
    add_app(cb, ctx, "skill:official_jump", "跳跃游戏", HV_KIND_SKILL, HV_GROUP_OFFICIAL, 0x8860C7);
    add_app(cb, ctx, "skill:official_snake", "贪吃蛇 2", HV_KIND_SKILL, HV_GROUP_OFFICIAL, 0x18A89F);
    add_app(cb, ctx, "skill:demo_timer", "专注计时器", HV_KIND_SKILL, HV_GROUP_OFFICIAL, 0x18A89F);
    if (!getenv("PREVIEW_NO_MINE")) {
        add_app(cb, ctx, "app:water", "喝水提醒", HV_KIND_PACKAGE, HV_GROUP_MINE, 0x2E7DD7);
        add_app(cb, ctx, "skill:counter", "灵感计数器", HV_KIND_SKILL, HV_GROUP_MINE, 0xC7EF38);
    }
}

esp_err_t hv_plat_launch_lua(const hv_app_t *app, const char *args)
{
    fprintf(stderr, "PREVIEW_LAUNCH %s script=%s args=%s\n", app->id, app->script, args ? args : "");
    return ESP_OK;
}

esp_err_t hv_plat_uninstall(const hv_app_t *app)
{
    fprintf(stderr, "PREVIEW_UNINSTALL %s\n", app->id);
    return ESP_OK;
}

esp_err_t hv_plat_rename(const hv_app_t *app, const char *title)
{
    fprintf(stderr, "PREVIEW_RENAME %s -> %s\n", app->id, title);
    return ESP_OK;
}

esp_err_t hv_plat_request(int type, const char *id, const char *title)
{
    fprintf(stderr, "PREVIEW_REQUEST type=%d id=%s title=%s\n", type, id, title ? title : "");
    return ESP_OK;
}

/* system_ui 里首页用到、但预览不编译的函数 */
void system_ui_touch_inject_sample(int32_t x, int32_t y, bool pressed) { (void)x; (void)y; (void)pressed; }
void system_ui_jobs_set_visible_locked(bool visible) { fprintf(stderr, "PREVIEW_JOBS %d\n", visible); }
void system_ui_click_feedback(void) {}
esp_err_t system_ui_post_work_event(const system_ui_work_event_t *event, TickType_t wait)
{
    (void)wait;
    fprintf(stderr, "PREVIEW_EVENT %d\n", event->type);
    return ESP_OK;
}

bool system_ui_gesture_recent(uint32_t window_ms) { (void)window_ms; return false; }

void hv_platform_mem_init(void)
{
}
