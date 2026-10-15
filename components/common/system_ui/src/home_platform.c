/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 首页的固件侧实现：模式持久化（NVS）、Wi-Fi / 蓝牙状态、音量、实时数据、
 * 字体，以及按住说话的两条链路（BLE 交给手机 / 设备经 Wi-Fi 直连 voice_cloud）。
 */
#include "home_priv.h"
#include "system_ui_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "claw_paths.h"
#include "dev_audio_codec.h"
#include "esp_board_manager.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "voice_cloud.h"
#include "wifi_manager.h"

#define HV_NVS_NS "backclip"

static system_ui_home_providers_t s_prov;
static int s_mode = -1;
static int s_volume = -1;
static int s_vibration = -1;

esp_err_t system_ui_home_set_providers(const system_ui_home_providers_t *providers)
{
    if (providers) {
        s_prov = *providers;
    } else {
        memset(&s_prov, 0, sizeof(s_prov));
    }
    return ESP_OK;
}

static int hv_nvs_get_i32(const char *key, int def)
{
    nvs_handle_t h;
    int32_t v = def;
    if (nvs_open(HV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i32(h, key, &v);
        nvs_close(h);
    }
    return (int)v;
}

static esp_err_t hv_nvs_set_i32(const char *key, int v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(HV_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, key, v);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

/* ------------------------------------------------------------- 模式 */

hv_mode_t hv_plat_mode_get(void)
{
    if (s_mode < 0) {
        s_mode = hv_nvs_get_i32("mode", HV_MODE_CLIP) == HV_MODE_DOCK ? HV_MODE_DOCK : HV_MODE_CLIP;
    }
    return (hv_mode_t)s_mode;
}

esp_err_t hv_plat_mode_set(hv_mode_t mode)
{
    esp_err_t err = hv_nvs_set_i32("mode", (int)mode);
    if (err == ESP_OK) {
        s_mode = (int)mode;
        ESP_LOGI(SYSTEM_UI_TAG, "HV_MODE %s", mode == HV_MODE_DOCK ? "dock" : "clip");
    }
    return err;
}

const char *system_ui_home_mode_name(void)
{
    return hv_plat_mode_get() == HV_MODE_DOCK ? "dock" : "clip";
}

/* ------------------------------------------------------------- 连接 */

bool hv_plat_phone_linked(void)
{
    return s_prov.phone_linked ? s_prov.phone_linked() : false;
}

bool hv_plat_ble_connected(void)
{
    return s_prov.ble_connected ? s_prov.ble_connected() : false;
}

bool hv_plat_wifi(char *ssid, size_t ssid_size)
{
    wifi_manager_status_t st = {0};
    wifi_ap_record_t ap = {0};

    wifi_manager_get_status(&st);
    if (ssid && ssid_size) {
        ssid[0] = 0;
    }
    if (!st.sta_connected) {
        return false;
    }
    if (ssid && ssid_size && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(ssid, (const char *)ap.ssid, ssid_size);
    }
    return true;
}

bool hv_plat_ai_ready(void)
{
    return voice_cloud_asr_ready() && voice_cloud_chat_ready();
}

char *hv_plat_feed_read(const char *id, int64_t *age_ms)
{
    return s_prov.feed_read ? s_prov.feed_read(id, age_ms) : NULL;
}

/* ------------------------------------------------------------- 文件 */

const char *hv_plat_data_root(void)
{
    const char *root = claw_paths_get(CLAW_PATH_DATA);
    return root ? root : "/sdcard";
}

const char *hv_plat_system_root(void)
{
    const char *root = claw_paths_get(CLAW_PATH_SYSTEM);
    return root ? root : "/system";
}

bool hv_plat_file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

char *hv_plat_read_text(const char *path, size_t max)
{
    struct stat st;
    FILE *f;
    char *text;

    if (!path || stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > max) {
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    text = calloc((size_t)st.st_size + 1, 1);
    if (text && fread(text, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

bool hv_plat_now(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
    return out->tm_year + 1900 >= 2025;
}

/* ------------------------------------------------------------- 字体 */

lv_font_t *hv_plat_font(int px)
{
#if LV_USE_TINY_TTF
    if (s_ui.font_data && s_ui.font_data_size) {
        return lv_tiny_ttf_create_data_ex(s_ui.font_data, s_ui.font_data_size, px,
                                          LV_FONT_KERNING_NORMAL, LV_TINY_TTF_CACHE_GLYPH_CNT);
    }
#endif
    (void)px;
    return NULL;
}

void hv_plat_font_free(lv_font_t *f)
{
#if LV_USE_TINY_TTF
    if (f) {
        lv_tiny_ttf_destroy(f);
    }
#endif
}

/* ------------------------------------------------------- 音量与震动 */

static esp_codec_dev_handle_t hv_codec(void)
{
    void *handle = NULL;
    if (esp_board_manager_get_device_handle("audio_dac", &handle) != ESP_OK || !handle) {
        return NULL;
    }
    return ((dev_audio_codec_handles_t *)handle)->codec_dev;
}

int hv_plat_volume_get(void)
{
    if (s_volume < 0) {
        s_volume = hv_nvs_get_i32("volume", 60);
    }
    return s_volume;
}

esp_err_t hv_plat_volume_set(int percent)
{
    esp_codec_dev_handle_t codec = hv_codec();
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s_volume = percent;
    (void)hv_nvs_set_i32("volume", percent);
    if (codec && esp_codec_dev_set_out_vol(codec, percent) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return codec ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool system_ui_vibration_available(void);
void system_ui_vibration_set_user_enabled(bool on);

bool hv_plat_vibration_get(void)
{
    if (s_vibration < 0) {
        s_vibration = hv_nvs_get_i32("vibrate", 1);
        system_ui_vibration_set_user_enabled(s_vibration != 0);
    }
    return s_vibration != 0;
}

void hv_plat_vibration_set(bool on)
{
    s_vibration = on ? 1 : 0;
    (void)hv_nvs_set_i32("vibrate", s_vibration);
    system_ui_vibration_set_user_enabled(on);
    if (!system_ui_vibration_available()) {
        hv_toast(on ? "已开启（这块样机没有震动马达）" : "已关闭");
    }
}

/* ------------------------------------------------------------- 语音 */

static char s_pending_launch[HV_APP_ID_LEN];

static void hv_pending_launch_cb(lv_timer_t *t)
{
    char id[HV_APP_ID_LEN];
    strlcpy(id, s_pending_launch, sizeof(id));
    s_pending_launch[0] = 0;
    (void)t;
    if (id[0]) {
        hv_voice_hide();
        (void)hv_catalog_launch(id, NULL);
    }
}

/* 在 system_ui 事件任务里执行（内部 RAM 栈）：voice_cloud 工作任务的栈在 PSRAM，
 * 它只负责 HTTP/JSON，界面更新与 NVS 写入都投递到这里。 */
static esp_err_t hv_post_call(void (*fn)(int, void *), int arg, void *payload)
{
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_HOME_CALL,
        .generation = s_ui.generation,
    };
    event.home_call.fn = fn;
    event.home_call.arg = arg;
    event.home_call.payload = payload;
    return system_ui_post_work_event(&event, pdMS_TO_TICKS(500));
}

typedef struct {
    char *text;
} hv_voice_msg_t;

static void hv_voice_apply(int evt, void *payload)
{
    hv_voice_msg_t *m = payload;
    const char *text = m ? m->text : NULL;

    if (system_ui_lock() == ESP_OK) {
        switch ((voice_cloud_event_t)evt) {
        case VOICE_CLOUD_EVT_RECOGNIZING:
            hv_voice_show(HV_VOICE_RECOGNIZING, NULL, NULL);
            break;
        case VOICE_CLOUD_EVT_HEARD:
            hv_voice_show(HV_VOICE_THINKING, text, NULL);
            break;
        case VOICE_CLOUD_EVT_THINKING:
            if (hv_voice_state() != HV_VOICE_THINKING) {
                hv_voice_show(HV_VOICE_THINKING, NULL, NULL);
            }
            break;
        case VOICE_CLOUD_EVT_TOOL:
            ESP_LOGI(SYSTEM_UI_TAG, "HV_TOOL %s", text ? text : "");
            break;
        case VOICE_CLOUD_EVT_REPLY:
            hv_voice_show(HV_VOICE_REPLY, NULL, text);
            if (s_pending_launch[0]) {
                lv_timer_t *t = lv_timer_create(hv_pending_launch_cb, 1800, NULL);
                lv_timer_set_repeat_count(t, 1);
            }
            break;
        case VOICE_CLOUD_EVT_ERROR:
            s_pending_launch[0] = 0;
            hv_voice_show(HV_VOICE_ERROR, NULL, text ? text : "出错了，请再试一次");
            break;
        }
        system_ui_unlock();
    } else {
        ESP_LOGW(SYSTEM_UI_TAG, "HV_VOICE event %d dropped: UI lock busy", evt);
    }
    if (m) {
        free(m->text);
        free(m);
    }
}

static void hv_cloud_event_cb(voice_cloud_event_t evt, const char *text, const char *detail, void *ctx)
{
    hv_voice_msg_t *m = calloc(1, sizeof(*m));
    (void)ctx;
    (void)detail;
    if (m && text) {
        m->text = strdup(text);
    }
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_VOICE_EVENT,
        .generation = s_ui.generation,
    };
    event.home_call.fn = hv_voice_apply;
    event.home_call.arg = (int)evt;
    event.home_call.payload = m;
    /* 终态（回复/错误）不能丢：队列满时多等一会儿 */
    TickType_t wait = (evt == VOICE_CLOUD_EVT_REPLY || evt == VOICE_CLOUD_EVT_ERROR) ? pdMS_TO_TICKS(3000) : 0;
    if (system_ui_post_work_event(&event, wait) != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "HV_VOICE event %d not queued", (int)evt);
        if (m) {
            free(m->text);
            free(m);
        }
    }
}

void system_ui_workbuddy_set_local_sink(esp_err_t (*sink)(const uint8_t *, size_t));

static esp_err_t hv_local_sink(const uint8_t *opus, size_t size)
{
    return voice_cloud_push_opus(opus, size);
}

esp_err_t hv_plat_voice_begin(bool local)
{
    esp_err_t err;

    if (local) {
        err = voice_cloud_begin();
        if (err != ESP_OK) {
            return err;
        }
        system_ui_workbuddy_set_local_sink(hv_local_sink);
    } else {
        system_ui_workbuddy_set_local_sink(NULL);
    }
    err = system_ui_workbuddy_start_capture_background(local ? "home-dock" : "home");
    if (err != ESP_OK) {
        system_ui_workbuddy_set_local_sink(NULL);
        if (local) {
            voice_cloud_cancel();
        }
    }
    return err;
}

/* 在 UI 事件任务里提交（栈够、和界面同一线程）；未配置时的同步错误回调也在这里。 */
void hv_plat_voice_finish_now(void)
{
    system_ui_workbuddy_set_local_sink(NULL);
    ESP_LOGI(SYSTEM_UI_TAG, "HV_VOICE dock packets=%u", (unsigned)voice_cloud_packet_count());
    if (voice_cloud_finish(hv_cloud_event_cb, NULL) != ESP_OK) {
        voice_cloud_cancel();       /* 释放录音缓存 */
        hv_cloud_event_cb(VOICE_CLOUD_EVT_ERROR, "提交失败，请再试一次", "finish", NULL);
    }
}

/* 松手后录音脚本还要几十毫秒才停下：再收 300 ms 尾音后提交，避免句尾被截。 */
static void hv_dock_finish_cb(void *arg)
{
    (void)arg;
    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_VOICE_FINISH,
        .generation = s_ui.generation,
    };
    if (system_ui_post_work_event(&event, pdMS_TO_TICKS(200)) != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "HV_VOICE finish event dropped");
    }
}

esp_err_t hv_plat_voice_end(bool local, bool cancel)
{
    static esp_timer_handle_t finish_timer;

    (void)system_ui_workbuddy_stop_audio_session_from(cancel ? "cancel" : "button");
    if (!local) {
        return ESP_OK;
    }
    if (cancel) {
        system_ui_workbuddy_set_local_sink(NULL);
        voice_cloud_cancel();
        return ESP_OK;
    }
    if (!finish_timer) {
        const esp_timer_create_args_t args = {.callback = hv_dock_finish_cb, .name = "hv_finish"};
        if (esp_timer_create(&args, &finish_timer) != ESP_OK) {
            hv_dock_finish_cb(NULL);
            return ESP_OK;
        }
    }
    (void)esp_timer_stop(finish_timer);
    return esp_timer_start_once(finish_timer, 300 * 1000);
}

esp_err_t hv_plat_voice_ask_text(const char *text)
{
    return voice_cloud_ask_text(text, hv_cloud_event_cb, NULL);
}

/* ---------------------------------------------------- 对话里的设备工具 */

/* 工具定义用 voice_cloud 的默认版本：描述经真实对话核验（只在明确要求时才操作设备）。 */

typedef struct {
    char name[24];
    char arg[64];
    int num;
} hv_tool_job_t;

void hv_control_set_brightness(int percent);

/* 工具的实际动作：在 system_ui 事件任务里执行（会写 NVS）。 */
static void hv_tool_volume(int v, void *payload)
{
    (void)payload;
    (void)hv_plat_volume_set(v);
    if (system_ui_lock() == ESP_OK) {
        hv_control_refresh();
        system_ui_unlock();
    }
}

static void hv_tool_brightness(int v, void *payload)
{
    (void)payload;
    if (system_ui_lock() == ESP_OK) {
        hv_control_set_brightness(v < 10 ? 10 : v);
        system_ui_unlock();
    }
}

static void hv_tool_mode(int dock, void *payload)
{
    (void)payload;
    (void)hv_plat_mode_set(dock ? HV_MODE_DOCK : HV_MODE_CLIP);
    if (system_ui_lock() == ESP_OK) {
        hv_refresh_status();
        hv_control_refresh();
        system_ui_unlock();
    }
}

/* 工具在 UI 事件任务外执行：这里只做必须拿锁的界面动作。 */
static esp_err_t hv_tool_cb(const char *name, const cJSON *args, char *out, size_t out_size, void *ctx)
{
    (void)ctx;
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(name, "open_app") == 0) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(args, "name");
        const hv_app_t *best = NULL;
        if (!cJSON_IsString(n)) {
            snprintf(out, out_size, "{\"ok\":false,\"error\":\"缺少应用名称\"}");
            return ESP_OK;
        }
        if (system_ui_lock() != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        for (size_t i = 0; i < hv_catalog_count(); i++) {
            const hv_app_t *app = hv_catalog_at(i);
            if (strstr(app->title, n->valuestring) || strstr(n->valuestring, app->title)) {
                best = app;
                break;
            }
        }
        if (!best && (strstr(n->valuestring, "花园") || strstr(n->valuestring, "我的") || strstr(n->valuestring, "小磁"))) {
            best = hv_catalog_find("garden");
        }
        if (!best && strstr(n->valuestring, "天气")) {
            best = hv_catalog_find("skill:live_weather");
        }
        if (!best && strstr(n->valuestring, "股")) {
            best = hv_catalog_find("skill:live_stocks");
        }
        if (!best && (strstr(n->valuestring, "热") || strstr(n->valuestring, "新闻"))) {
            best = hv_catalog_find("skill:live_aihot");
        }
        if (best) {
            /* 先把回复显示出来，回复到达后再打开应用（见 hv_cloud_event_cb）。 */
            strlcpy(s_pending_launch, best->id, sizeof(s_pending_launch));
            snprintf(out, out_size, "{\"ok\":true,\"opened\":\"%s\"}", best->title);
        } else {
            snprintf(out, out_size, "{\"ok\":false,\"error\":\"背夹上没有这个应用\"}");
        }
        system_ui_unlock();
        return ESP_OK;
    }
    if (strcmp(name, "set_volume") == 0 || strcmp(name, "set_brightness") == 0) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "percent");
        int v = cJSON_IsNumber(p) ? p->valueint : -1;
        if (v < 0 || v > 100) {
            snprintf(out, out_size, "{\"ok\":false,\"error\":\"百分比需在 0-100\"}");
            return ESP_OK;
        }
        if (hv_post_call(name[4] == 'v' ? hv_tool_volume : hv_tool_brightness, v, NULL) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        snprintf(out, out_size, "{\"ok\":true,\"percent\":%d}", v);
        return ESP_OK;
    }
    if (strcmp(name, "set_mode") == 0) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(args, "mode");
        bool dock = cJSON_IsString(m) && strcmp(m->valuestring, "dock") == 0;
        if (hv_post_call(hv_tool_mode, dock ? 1 : 0, NULL) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        snprintf(out, out_size, "{\"ok\":true,\"mode\":\"%s\"}", dock ? "dock" : "clip");
        return ESP_OK;
    }
    snprintf(out, out_size, "{\"ok\":false,\"error\":\"未知工具\"}");
    return ESP_ERR_NOT_SUPPORTED;
}

void hv_platform_init(void)
{
    (void)voice_cloud_init();
    (void)voice_cloud_set_tools(voice_cloud_default_tools_json(), hv_tool_cb, NULL);
    (void)hv_plat_mode_get();
    (void)hv_plat_vibration_get();
    int vol = hv_plat_volume_get();
    esp_codec_dev_handle_t codec = hv_codec();
    if (codec) {
        (void)esp_codec_dev_set_out_vol(codec, vol);
    }
}
