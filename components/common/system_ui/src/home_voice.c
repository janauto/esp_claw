/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 按住说话：首页长按 M（或长按「我的」/「语音助手」）开始收音，松开发送。
 *
 * 路由按控制中心的模式决定：
 *   背夹 · 手机：Opus 经 BLE 语音特征送手机，手机识别 + DeepSeek 对话后用
 *                assistant_show（或旧版 workbuddy_ui）把“识别中/你说/回复”推回来；
 *                手机没连上但 Wi-Fi 与底座 AI 配置可用时，自动改用底座直连。
 *   底座 · Wi-Fi：背夹自己把 Opus 封装成 Ogg 直连云端识别，再调对话模型；
 *                Wi-Fi 不可用而手机在线时改走手机。
 * 卡片显示：正在听 → 识别中 → 你说 → 思考中 → 回复；失败给出原因和下一步。
 */
#include "home_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

void hv_view_set(hv_view_t v);
lv_obj_t *hv_screen(void);

#define HV_VOICE_MAX_LISTEN_MS  30000
#define HV_VOICE_WAIT_MS        35000
#define HV_VOICE_REPLY_HIDE_MS  25000
#define HV_VOICE_ERROR_HIDE_MS  9000

typedef enum { ROUTE_NONE = 0, ROUTE_PHONE, ROUTE_WIFI } hv_route_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *pet;
    lv_obj_t *title;
    lv_obj_t *sub;
    lv_obj_t *body;
    lv_obj_t *route;
    lv_obj_t *bars[9];
    lv_timer_t *wave;
    lv_timer_t *timer;
    hv_voice_state_t state;
    hv_route_t route_kind;
    uint32_t state_ms;
    hv_view_t prev_view;
    char heard[160];
    char text[320];
} hv_voice_ui_t;

static hv_voice_ui_t *s_v;
#define V (*s_v)

void hv_voice_mem_init(void)
{
    if (!s_v) s_v = hv_psram_calloc(sizeof(*s_v));
}

static char s_deferred_launch[HV_APP_ID_LEN];
static uint32_t s_deferred_since;

void hv_voice_forget(void)
{
    memset(&V, 0, sizeof(V));
    s_deferred_launch[0] = 0;
}

/* 语音回复出现后再打开应用（来自手机的 app_launch 或底座工具）。 */
void hv_voice_defer_launch(const char *id)
{
    strlcpy(s_deferred_launch, id, sizeof(s_deferred_launch));
    s_deferred_since = lv_tick_get();
}

static void hv_voice_run_deferred(void)
{
    char id[HV_APP_ID_LEN];
    if (!s_deferred_launch[0]) {
        return;
    }
    strlcpy(id, s_deferred_launch, sizeof(id));
    s_deferred_launch[0] = 0;
    hv_voice_hide();
    (void)hv_catalog_launch(id, NULL);
}

hv_voice_state_t hv_voice_state(void)
{
    return V.state;
}

const char *hv_voice_state_name(hv_voice_state_t st)
{
    switch (st) {
    case HV_VOICE_IDLE: return "idle";
    case HV_VOICE_LISTENING: return "listening";
    case HV_VOICE_RECOGNIZING: return "recognizing";
    case HV_VOICE_THINKING: return "thinking";
    case HV_VOICE_REPLY: return "reply";
    case HV_VOICE_ERROR: return "error";
    default: return "?";
    }
}

const char *hv_voice_route_name(void)
{
    if (V.route_kind == ROUTE_WIFI) {
        return "底座直连";
    }
    if (V.route_kind == ROUTE_PHONE) {
        return "经手机";
    }
    return hv_plat_mode_get() == HV_MODE_DOCK ? "底座直连" : "经手机";
}

bool hv_voice_visible(void)
{
    return V.card && !lv_obj_has_flag(V.card, LV_OBJ_FLAG_HIDDEN);
}

static void hv_wave_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t seed = 0x1234567;
    for (int i = 0; i < 9; i++) {
        seed = seed * 1103515245u + 12345u;
        int32_t h = V.state == HV_VOICE_LISTENING ? 8 + (int32_t)((seed >> 16) % 30) : 6;
        if (V.bars[i]) {
            lv_obj_set_height(V.bars[i], h);
            lv_obj_set_y(V.bars[i], 20 - h / 2);
        }
    }
}

static void hv_voice_timer_cb(lv_timer_t *t)
{
    (void)t;
    V.state_ms += 500;
    switch (V.state) {
    case HV_VOICE_LISTENING:
        if (V.state_ms >= HV_VOICE_MAX_LISTEN_MS) {
            hv_toast("最长 30 秒，已自动发送");
            hv_voice_release();
        }
        break;
    case HV_VOICE_RECOGNIZING:
    case HV_VOICE_THINKING:
        if (s_deferred_launch[0] && lv_tick_elaps(s_deferred_since) > 6000) {
            hv_voice_run_deferred();        /* 等了 6 s 还没回复：不再拦着应用 */
            break;
        }
        if (V.state_ms >= HV_VOICE_WAIT_MS) {
            hv_voice_show(HV_VOICE_ERROR, NULL,
                          V.route_kind == ROUTE_PHONE ? "手机没有回应：请确认手机 App 打开并已连接背夹"
                                                      : "云端没有回应：请检查网络后再试");
        }
        break;
    case HV_VOICE_REPLY:
        if (s_deferred_launch[0] && V.state_ms >= 1800) {
            hv_voice_run_deferred();
        } else if (V.state_ms >= HV_VOICE_REPLY_HIDE_MS) {
            hv_voice_hide();
        }
        break;
    case HV_VOICE_ERROR:
        if (V.state_ms >= HV_VOICE_ERROR_HIDE_MS) {
            hv_voice_hide();
        }
        break;
    default:
        break;
    }
}

static void hv_voice_card_cb(lv_event_t *e)
{
    (void)e;
    /* 点卡片：听的时候不处理（松开 M 才发送），其他状态收起 */
    if (V.state != HV_VOICE_LISTENING) {
        hv_voice_hide();
    }
}

static void hv_voice_build(void)
{
    lv_obj_t *scr = hv_screen();
    int32_t w = (int32_t)s_ui.width;

    if (V.card || !scr) {
        return;
    }
    V.card = hv_box(scr, 12, 0, w - 24, 230, 0x15161A, 30);
    lv_obj_set_style_border_width(V.card, 1, 0);
    lv_obj_set_style_border_color(V.card, lv_color_hex(0x3A3420), 0);
    lv_obj_set_style_shadow_width(V.card, 40, 0);
    lv_obj_set_style_shadow_color(V.card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(V.card, LV_OPA_70, 0);
    lv_obj_add_flag(V.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(V.card, hv_voice_card_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(V.card, LV_ALIGN_BOTTOM_MID, 0, -(HV_SOFTKEY_H + 14));

    /* 头像圈里的小磁 */
    lv_obj_t *ring = hv_box(V.card, 22, 22, 96, 96, 0x1F1B0C, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(HV_C_ACCENT), 0);
    V.pet = hv_pet_create(ring, 86);
    lv_obj_center(V.pet);

    V.title = hv_text(V.card, g_hvf.f28, HV_C_TEXT, "");
    lv_obj_set_pos(V.title, 138, 26);
    V.sub = hv_text(V.card, g_hvf.f16, HV_C_MUTED, "");
    lv_obj_set_pos(V.sub, 140, 66);
    V.route = hv_text(V.card, g_hvf.f16, HV_C_ACCENT, "");
    lv_obj_align(V.route, LV_ALIGN_TOP_RIGHT, -20, 30);

    lv_obj_t *wave = hv_box(V.card, 140, 90, 200, 40, 0, 0);
    lv_obj_set_style_bg_opa(wave, LV_OPA_TRANSP, 0);
    for (int i = 0; i < 9; i++) {
        V.bars[i] = hv_box(wave, i * 20, 14, 8, 12, HV_C_ACCENT, 4);
    }
    V.body = hv_text(V.card, g_hvf.f20, HV_C_TEXT, "");
    lv_obj_set_width(V.body, w - 24 - 44);
    lv_obj_set_height(V.body, 84);
    lv_label_set_long_mode(V.body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(V.body, 22, 134);

    V.wave = lv_timer_create(hv_wave_cb, 120, NULL);
    V.timer = lv_timer_create(hv_voice_timer_cb, 500, NULL);
    lv_obj_add_flag(V.card, LV_OBJ_FLAG_HIDDEN);
}

static void hv_voice_softkeys(void)
{
    switch (V.state) {
    case HV_VOICE_LISTENING:
        hv_softkeys_set("", "松开发送", "", "取消");
        break;
    case HV_VOICE_RECOGNIZING:
    case HV_VOICE_THINKING:
        hv_softkeys_set("", "收起", "", "取消");
        break;
    default:
        hv_softkeys_set("", "收起", "", "关闭");
        break;
    }
}

static void hv_voice_render(void)
{
    char sub[96];
    char body[512];

    hv_voice_build();
    if (!V.card) {
        return;
    }
    const char *title = "";
    sub[0] = 0;
    body[0] = 0;
    switch (V.state) {
    case HV_VOICE_LISTENING:
        title = "正在听…";
        strlcpy(sub, "松开 M 发送   SET 取消", sizeof(sub));
        break;
    case HV_VOICE_RECOGNIZING:
        title = "识别中…";
        strlcpy(sub, V.route_kind == ROUTE_PHONE ? "手机正在识别" : "背夹正在联网识别", sizeof(sub));
        break;
    case HV_VOICE_THINKING:
        title = "思考中…";
        strlcpy(sub, V.route_kind == ROUTE_PHONE ? "手机 AI 正在回答" : "背夹 AI 正在回答", sizeof(sub));
        break;
    case HV_VOICE_REPLY:
        title = "回复";
        strlcpy(sub, "按 M 或点卡片收起", sizeof(sub));
        break;
    case HV_VOICE_ERROR:
        if (V.route_kind == ROUTE_NONE) {
            title = "语音暂时不可用";
            strlcpy(sub, "按提示处理后再按住 M", sizeof(sub));
        } else if (strstr(V.text, "识别") || strstr(V.text, "没听") || strstr(V.text, "太短")) {
            title = "没听清楚";
            strlcpy(sub, "按住 M 再说一次", sizeof(sub));
        } else {
            title = "出了点问题";
            strlcpy(sub, "按住 M 再试一次", sizeof(sub));
        }
        break;
    default:
        break;
    }
    if (V.heard[0]) {
        snprintf(body, sizeof(body), "你说：%s%s%s", V.heard, V.text[0] ? "\n" : "", V.text);
    } else {
        strlcpy(body, V.text, sizeof(body));
    }
    lv_label_set_text(V.title, title);
    lv_label_set_text(V.sub, sub);
    lv_label_set_text(V.body, body);
    lv_label_set_text(V.route, V.route_kind == ROUTE_NONE && V.state == HV_VOICE_ERROR ? "" : hv_voice_route_name());
    lv_obj_set_style_text_color(V.title, lv_color_hex(V.state == HV_VOICE_ERROR ? 0xFF8A80 : HV_C_TEXT), 0);
    for (int i = 0; i < 9; i++) {
        if (V.state == HV_VOICE_LISTENING || V.state == HV_VOICE_RECOGNIZING || V.state == HV_VOICE_THINKING) {
            lv_obj_remove_flag(V.bars[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(V.bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    hv_pet_set_mood(V.pet, V.state == HV_VOICE_REPLY ? 2 : V.state == HV_VOICE_LISTENING ? 1 : 0);
    lv_obj_remove_flag(V.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(V.card);
    if (hv_view() != HV_VIEW_VOICE) {
        V.prev_view = hv_view();
        hv_view_set(HV_VIEW_VOICE);
    }
    hv_voice_softkeys();
    hv_refresh_status();
    ESP_LOGI(SYSTEM_UI_TAG, "HV_VOICE %s route=%s heard_len=%u text_len=%u", hv_voice_state_name(V.state),
             hv_voice_route_name(), (unsigned)strlen(V.heard), (unsigned)strlen(V.text));
}

void hv_voice_show(hv_voice_state_t st, const char *heard, const char *text)
{
    if (st == HV_VOICE_IDLE) {
        hv_voice_hide();
        return;
    }
    /* 设备没在等这一轮，但手机推来了回复（例如在手机上直接说）：照样显示 */
    if (V.route_kind == ROUTE_NONE && st != HV_VOICE_LISTENING) {
        V.route_kind = ROUTE_PHONE;
    }
    if (heard) {
        strlcpy(V.heard, heard, sizeof(V.heard));
    }
    if (text) {
        strlcpy(V.text, text, sizeof(V.text));
    } else if (st == HV_VOICE_RECOGNIZING || st == HV_VOICE_THINKING) {
        V.text[0] = 0;
    }
    V.state = st;
    V.state_ms = 0;
    if (st == HV_VOICE_REPLY) {
        extern void hv_refresh_widget_kind(const char *wid);
        hv_widgets_note_voice(V.heard, V.text);
        hv_refresh_widget_kind("voice");
    }
    hv_voice_render();
}

void hv_voice_hide(void)
{
    if (V.card) {
        lv_obj_add_flag(V.card, LV_OBJ_FLAG_HIDDEN);
    }
    if (V.state == HV_VOICE_LISTENING) {
        (void)hv_plat_voice_end(V.route_kind == ROUTE_WIFI, true);
    }
    if (V.state != HV_VOICE_RECOGNIZING && V.state != HV_VOICE_THINKING) {
        V.state = HV_VOICE_IDLE;
        V.route_kind = ROUTE_NONE;
    }
    if (hv_view() == HV_VIEW_VOICE) {
        hv_view_t back = V.prev_view == HV_VIEW_VOICE ? HV_VIEW_HOME : V.prev_view;
        hv_view_set(back == HV_VIEW_EDIT || back == HV_VIEW_HOME ? back : HV_VIEW_HOME);
    }
    hv_refresh_status();
    /* 语音助手小组件跟着更新 */
    hv_on_show();
}

static hv_route_t hv_pick_route(const char **why)
{
    char ssid[40];
    bool wifi = hv_plat_wifi(ssid, sizeof(ssid));
    bool ai = hv_plat_ai_ready();
    bool phone = hv_plat_phone_linked();

    *why = NULL;
    if (hv_plat_mode_get() == HV_MODE_DOCK) {
        if (wifi && ai) {
            return ROUTE_WIFI;
        }
        if (phone) {
            *why = wifi ? "底座 AI 未配置，这次改用手机" : "Wi-Fi 未连接，这次改用手机";
            return ROUTE_PHONE;
        }
        *why = !wifi ? "Wi-Fi 未连接：底座模式需要联网" : "底座 AI 服务未配置：请在手机 App 推送配置";
        return ROUTE_NONE;
    }
    if (phone) {
        return ROUTE_PHONE;
    }
    if (wifi && ai) {
        *why = "手机未连接，这次改用底座直连";
        return ROUTE_WIFI;
    }
    *why = "手机未连接：打开手机 App 连接背夹，或在控制中心切到底座模式";
    return ROUTE_NONE;
}

void hv_voice_press(void)
{
    const char *why = NULL;
    hv_route_t route;

    if (V.state == HV_VOICE_LISTENING) {
        return;
    }
    if (V.state == HV_VOICE_RECOGNIZING || V.state == HV_VOICE_THINKING) {
        hv_toast("上一句还在处理，稍等");
        return;
    }
    route = hv_pick_route(&why);
    V.heard[0] = 0;
    V.text[0] = 0;
    if (route == ROUTE_NONE) {
        V.route_kind = ROUTE_NONE;
        hv_voice_show(HV_VOICE_ERROR, NULL, why);
        return;
    }
    V.route_kind = route;
    if (hv_plat_voice_begin(route == ROUTE_WIFI) != ESP_OK) {
        hv_voice_show(HV_VOICE_ERROR, NULL, "麦克风启动失败，请稍后再试");
        return;
    }
    system_ui_click_feedback();
    if (why) {
        hv_toast(why);
    }
    hv_voice_show(HV_VOICE_LISTENING, NULL, NULL);
}

void hv_voice_release(void)
{
    if (V.state != HV_VOICE_LISTENING) {
        return;
    }
    if (V.state_ms < 500) {
        /* 太短：多半是误触 */
        (void)hv_plat_voice_end(V.route_kind == ROUTE_WIFI, true);
        hv_voice_show(HV_VOICE_ERROR, NULL, "说话时间太短：按住 M 说完再松开");
        return;
    }
    if (hv_plat_voice_end(V.route_kind == ROUTE_WIFI, false) != ESP_OK) {
        hv_voice_show(HV_VOICE_ERROR, NULL, "发送失败，请再试一次");
        return;
    }
    hv_voice_show(HV_VOICE_RECOGNIZING, NULL, NULL);
}

void hv_voice_cancel(void)
{
    if (V.state == HV_VOICE_LISTENING) {
        (void)hv_plat_voice_end(V.route_kind == ROUTE_WIFI, true);
    }
    V.state = HV_VOICE_IDLE;
    hv_voice_hide();
    hv_toast("已取消");
}

void hv_voice_key(hv_key_t key)
{
    switch (key) {
    case HV_KEY_M_LONG_END:
        hv_voice_release();
        break;
    case HV_KEY_M_LONG_START:
        hv_voice_press();
        break;
    case HV_KEY_M:
        if (V.state != HV_VOICE_LISTENING) {
            hv_voice_hide();
        }
        break;
    case HV_KEY_SET:
    case HV_KEY_SET_LONG:
        if (V.state == HV_VOICE_LISTENING || V.state == HV_VOICE_RECOGNIZING || V.state == HV_VOICE_THINKING) {
            hv_voice_cancel();
        } else {
            hv_voice_hide();
        }
        break;
    default:
        break;
    }
}

/* 文字直达（UART voice_text / 调试）：跳过录音与识别。 */
esp_err_t hv_voice_ask_text(const char *text)
{
    const char *why = NULL;
    hv_route_t route = hv_pick_route(&why);

    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (route != ROUTE_WIFI) {
        hv_voice_show(HV_VOICE_ERROR, text, why ? why : "文字直达只在底座直连可用");
        return ESP_ERR_INVALID_STATE;
    }
    V.route_kind = ROUTE_WIFI;
    V.heard[0] = 0;
    V.text[0] = 0;
    hv_voice_show(HV_VOICE_THINKING, text, NULL);
    return hv_plat_voice_ask_text(text);
}
