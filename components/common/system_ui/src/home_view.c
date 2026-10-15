/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 1.0 系统首页主体：状态栏（系统外框）、标题行、小组件页（左右滑/按键翻页）、
 * 第 1 页底部应用栏、分页点、软键栏、焦点框与提示条。
 *
 * 按键语义（与《首页设计原型 v0.1》一致）：
 *   VOL− ◀ 上一个 / VOL+ ▶ 下一个（走到页尾自动翻页）
 *   MODE 短按打开；按住 0.6 s 说话，松开发送
 *   SET 短按 = 应用总览（其他界面 = 返回上一级）；按住 0.6 s = 控制中心
 * 手势：顶部下拉 = 控制中心；底部上滑 = 应用总览；长按小组件/空白 = 编辑首页；
 *       长按「我的」或「语音助手」= 按住说话。
 */
#include "home_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

hv_fonts_t g_hvf;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *glow[2];
    lv_obj_t *status_bar;
    /* 状态栏 */
    lv_obj_t *time_l;
    lv_obj_t *capsule, *capsule_dot, *capsule_l;
    lv_obj_t *mode_chip, *mode_chip_l;
    lv_obj_t *bt_l, *wifi_l, *pwr_l;
    /* 标题行 / 编辑栏 */
    lv_obj_t *header, *hdr_date, *hdr_weather, *hdr_deg;
    lv_obj_t *edit_bar, *edit_add, *edit_done;
    /* 页面 */
    lv_obj_t *pager;
    lv_obj_t *pg[HV_MAX_PAGES];
    lv_obj_t *slot[HV_MAX_PAGES][HV_MAX_PER_PAGE];
    lv_obj_t *wroot[HV_MAX_PAGES][HV_MAX_PER_PAGE];
    lv_obj_t *dock_slot[HV_DOCK_MAX];
    lv_obj_t *dots, *dot[HV_MAX_PAGES];
    /* 底部 */
    lv_obj_t *indicator;
    lv_obj_t *softkeys, *sk_btn[4], *sk_label[4];
    lv_obj_t *toast, *toast_l;
    lv_timer_t *toast_timer;
    lv_timer_t *tick;
    hv_layout_t layout;
    hv_view_t view;
    int page;
    int focus;
    bool focus_visible;
    bool edit;
    int picked;             /* 编辑：拿起的小组件下标，-1 = 无 */
    /* 编辑：拖动 */
    bool dragging;
    int drag_page, drag_index;
    lv_point_t drag_start, drag_origin;
    uint32_t seconds;
    lv_obj_t *focus_obj;
} hv_state_t;

static hv_state_t *s_hv;
#define H (*s_hv)

void *hv_psram_calloc(size_t size)
{
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, size);
}

void hv_overview_mem_init(void);
void hv_control_mem_init(void);
void hv_voice_mem_init(void);
void hv_edit_mem_init(void);
void hv_widgets_mem_init(void);
void hv_platform_mem_init(void);

void hv_mem_init(void)
{
    if (!s_hv) {
        s_hv = hv_psram_calloc(sizeof(*s_hv));
    }
    hv_overview_mem_init();
    hv_control_mem_init();
    hv_voice_mem_init();
    hv_edit_mem_init();
    hv_widgets_mem_init();
    hv_platform_mem_init();
}

/* ------------------------------------------------------------ 小工具 */

void hv_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
}

lv_obj_t *hv_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    hv_plain(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

lv_obj_t *hv_text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font ? font : s_ui.font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

void hv_focus_ring(lv_obj_t *obj, bool on)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_outline_width(obj, on ? 3 : 0, 0);
    lv_obj_set_style_outline_color(obj, lv_color_hex(HV_C_ACCENT), 0);
    lv_obj_set_style_outline_pad(obj, 4, 0);
    lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, 0);
}

void hv_utf8_clip(char *s, size_t max_bytes)
{
    size_t len = strlen(s);
    if (len <= max_bytes) {
        return;
    }
    len = max_bytes;
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) {
        len--;
    }
    s[len] = '\0';
}

/* ------------------------------------------------------------- 字体 */

lv_font_t *hv_plat_font(int px);

void hv_fonts_init(void)
{
    if (g_hvf.f24) {
        return;
    }
    g_hvf.f24 = s_ui.font;
    g_hvf.f16 = hv_plat_font(17);
    g_hvf.f20 = hv_plat_font(20);
    g_hvf.f28 = hv_plat_font(28);
    g_hvf.f40 = hv_plat_font(40);
    g_hvf.f64 = hv_plat_font(60);
}

void hv_plat_font_free(lv_font_t *f);

void hv_fonts_deinit(void)
{
    lv_font_t *fonts[] = {g_hvf.f16, g_hvf.f20, g_hvf.f28, g_hvf.f40, g_hvf.f64};
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        if (fonts[i]) {
            hv_plat_font_free(fonts[i]);
        }
    }
    memset(&g_hvf, 0, sizeof(g_hvf));
}

const lv_font_t *hv_symbol_font(int px)
{
#if LV_FONT_MONTSERRAT_28
    if (px >= 24) {
        return &lv_font_montserrat_28;
    }
#endif
#if LV_FONT_MONTSERRAT_20
    if (px >= 18) {
        return &lv_font_montserrat_20;
    }
#endif
    (void)px;
    return &lv_font_montserrat_14;
}

/* -------------------------------------------------------------- 提示条 */

static void hv_toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (H.toast) {
        lv_obj_add_flag(H.toast, LV_OBJ_FLAG_HIDDEN);
    }
    if (H.toast_timer) {
        lv_timer_pause(H.toast_timer);
    }
}

void hv_toast(const char *text)
{
    if (!H.scr || !text) {
        return;
    }
    if (!H.toast) {
        H.toast = hv_box(H.scr, 0, 0, 10, 48, 0x2A2D35, 24);
        lv_obj_set_style_border_width(H.toast, 1, 0);
        lv_obj_set_style_border_color(H.toast, lv_color_hex(0x434853), 0);
        lv_obj_set_style_pad_hor(H.toast, 22, 0);
        lv_obj_set_width(H.toast, LV_SIZE_CONTENT);
        H.toast_l = hv_text(H.toast, g_hvf.f20, HV_C_TEXT, "");
        lv_obj_center(H.toast_l);
        H.toast_timer = lv_timer_create(hv_toast_hide_cb, 2200, NULL);
    }
    lv_label_set_text(H.toast_l, text);
    lv_obj_remove_flag(H.toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(H.toast);
    lv_obj_align(H.toast, LV_ALIGN_BOTTOM_MID, 0, -(HV_SOFTKEY_H + 70));
    lv_timer_reset(H.toast_timer);
    lv_timer_resume(H.toast_timer);
    ESP_LOGI(SYSTEM_UI_TAG, "HV_TOAST %s", text);
}

/* ---------------------------------------------------------------- 软键栏 */

static const char *const s_sk_caps[4] = {LV_SYMBOL_LEFT, "M", LV_SYMBOL_RIGHT, "SET"};

static void hv_sk_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        static const hv_key_t keys[4] = {HV_KEY_PREV, HV_KEY_M, HV_KEY_NEXT, HV_KEY_SET};
        system_ui_click_feedback();
        hv_key(keys[i]);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (i == 1) {
            hv_key(HV_KEY_M_LONG_START);
        } else if (i == 3) {
            hv_key(HV_KEY_SET_LONG);
        }
    } else if (code == LV_EVENT_RELEASED && i == 1 && hv_voice_state() == HV_VOICE_LISTENING) {
        hv_key(HV_KEY_M_LONG_END);
    }
}

static void hv_softkeys_build(void)
{
    int32_t w = (int32_t)s_ui.width;
    int32_t seg = w / 4;

    H.softkeys = hv_box(H.scr, 0, (int32_t)s_ui.height - HV_SOFTKEY_H, w, HV_SOFTKEY_H, 0x07080A, 0);
    lv_obj_set_style_border_side(H.softkeys, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(H.softkeys, 1, 0);
    lv_obj_set_style_border_color(H.softkeys, lv_color_hex(0x1C1F25), 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_obj_create(H.softkeys);
        lv_obj_t *cap;
        lv_obj_t *cap_l;

        hv_plain(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(btn, seg, HV_SOFTKEY_H);
        lv_obj_set_pos(btn, i * seg, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1D23), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, hv_sk_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        H.sk_btn[i] = btn;

        cap = hv_box(btn, 0, 0, i == 3 ? 38 : 24, 22, 0x07080A, 5);
        lv_obj_set_style_border_width(cap, 1, 0);
        lv_obj_set_style_border_color(cap, lv_color_hex(i == 1 ? HV_C_ACCENT : 0x6B7280), 0);
        cap_l = hv_text(cap, (i == 0 || i == 2) ? &lv_font_montserrat_14 : &lv_font_montserrat_14,
                        i == 1 ? HV_C_ACCENT : 0xC8CDD5, s_sk_caps[i]);
        lv_obj_center(cap_l);
        H.sk_label[i] = hv_text(btn, g_hvf.f16, i == 1 ? HV_C_ACCENT : 0xC8CDD5, "");
        if (i == 2) {
            lv_obj_align(cap, LV_ALIGN_RIGHT_MID, -14, 0);
            lv_obj_align_to(H.sk_label[i], cap, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        } else {
            lv_obj_align(cap, LV_ALIGN_LEFT_MID, i == 0 ? 14 : 10, 0);
        }
    }
}

void hv_softkeys_set(const char *k1, const char *k2, const char *k3, const char *k4)
{
    const char *t[4] = {k1, k2, k3, k4};
    if (!H.softkeys) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        lv_label_set_text(H.sk_label[i], t[i] ? t[i] : "");
        if (i == 2) {
            lv_obj_update_layout(H.sk_label[i]);
            lv_obj_t *cap = lv_obj_get_child(H.sk_btn[i], 0);
            lv_obj_align_to(H.sk_label[i], cap, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        } else {
            lv_obj_t *cap = lv_obj_get_child(H.sk_btn[i], 0);
            lv_obj_align_to(H.sk_label[i], cap, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
        }
    }
}

static void hv_softkeys_for_view(void)
{
    switch (H.view) {
    case HV_VIEW_HOME:
        hv_softkeys_set("上一个", "打开", "下一个", "总览");
        break;
    case HV_VIEW_EDIT:
        hv_softkeys_set("上一个", H.picked >= 0 ? "换尺寸" : "拿起", "下一个", H.picked >= 0 ? "放下" : "完成");
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- 状态栏 */

static void hv_status_build(void)
{
    int32_t w = (int32_t)s_ui.width;
    lv_obj_t *bar = hv_box(H.scr, 0, 0, w, HV_STATUS_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    /* 吸收按压：从状态栏下拉时，手指滑进页面也不会被当成长按页面 */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    H.status_bar = bar;

    /* 系统外框身份：左侧黄色标记条 */
    hv_box(bar, 0, 12, 5, 20, HV_C_ACCENT, 2);
    H.time_l = hv_text(bar, g_hvf.f24, HV_C_TEXT, "--:--");
    lv_obj_align(H.time_l, LV_ALIGN_LEFT_MID, 18, 0);

    /* 中间胶囊：录音/语音进行中 */
    H.capsule = hv_box(bar, 0, 0, 120, 30, 0x1E2026, 15);
    lv_obj_align(H.capsule, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_width(H.capsule, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(H.capsule, 12, 0);
    H.capsule_dot = hv_box(H.capsule, 0, 0, 8, 8, HV_C_RED, 4);
    lv_obj_align(H.capsule_dot, LV_ALIGN_LEFT_MID, 0, 0);
    H.capsule_l = hv_text(H.capsule, g_hvf.f16, HV_C_TEXT, "");
    lv_obj_align(H.capsule_l, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_add_flag(H.capsule, LV_OBJ_FLAG_HIDDEN);

    /* 右侧：模式小胶囊、蓝牙、Wi-Fi、电源 */
    H.pwr_l = hv_text(bar, &lv_font_montserrat_14, HV_C_ACCENT, LV_SYMBOL_CHARGE);
    lv_obj_align(H.pwr_l, LV_ALIGN_RIGHT_MID, -14, 0);
    H.wifi_l = hv_text(bar, hv_symbol_font(20), HV_C_TEXT, LV_SYMBOL_WIFI);
    lv_obj_align_to(H.wifi_l, H.pwr_l, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    H.bt_l = hv_text(bar, hv_symbol_font(20), HV_C_TEXT, LV_SYMBOL_BLUETOOTH);
    lv_obj_align_to(H.bt_l, H.wifi_l, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    H.mode_chip = hv_box(bar, 0, 0, 64, 26, 0x2A2410, 13);
    lv_obj_set_style_border_width(H.mode_chip, 1, 0);
    lv_obj_set_style_border_color(H.mode_chip, lv_color_hex(0x6B5A1A), 0);
    H.mode_chip_l = hv_text(H.mode_chip, g_hvf.f16, HV_C_ACCENT, "背夹");
    lv_obj_center(H.mode_chip_l);
    lv_obj_align_to(H.mode_chip, H.bt_l, LV_ALIGN_OUT_LEFT_MID, -12, 0);
}

static int hv_deg_text(lv_obj_t *label, lv_obj_t *deg, int temp, const char *cond)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%d   %s", temp, cond);
    lv_label_set_text(label, buf);
    return 0;
}

bool hv_weather_summary(char *out, size_t size, int *temp_out);

void hv_refresh_status(void)
{
    struct tm now;
    char buf[48];
    char ssid[40] = {0};
    bool wifi;
    static const char *const wd[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

    if (!H.scr) {
        return;
    }
    if (hv_plat_now(&now)) {
        snprintf(buf, sizeof(buf), "%02d:%02d", now.tm_hour, now.tm_min);
        lv_label_set_text(H.time_l, buf);
        if (H.hdr_date) {
            snprintf(buf, sizeof(buf), "%d月%d日 %s", now.tm_mon + 1, now.tm_mday, wd[now.tm_wday % 7]);
            lv_label_set_text(H.hdr_date, buf);
        }
    } else {
        lv_label_set_text(H.time_l, "--:--");
        if (H.hdr_date) {
            lv_label_set_text(H.hdr_date, "等待联网对时");
        }
    }
    wifi = hv_plat_wifi(ssid, sizeof(ssid));
    lv_obj_set_style_text_opa(H.wifi_l, wifi ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_text_color(H.bt_l, lv_color_hex(hv_plat_ble_connected() ? 0x60A5FA : HV_C_TEXT), 0);
    lv_obj_set_style_text_opa(H.bt_l, hv_plat_ble_connected() ? LV_OPA_COVER : LV_OPA_50, 0);
    bool dock = hv_plat_mode_get() == HV_MODE_DOCK;
    static int last_mode = -1;
    if (last_mode != (int)dock) {
        /* 模式变了：语音助手与设备状态小组件上的线路/模式文字跟着变 */
        if (last_mode >= 0) {
            for (int p = 0; p < H.layout.pages; p++) {
                for (int i = 0; i < H.layout.n[p]; i++) {
                    if (H.wroot[p][i] && (!strcmp(H.layout.it[p][i].w, "voice") || !strcmp(H.layout.it[p][i].w, "system"))) {
                        hv_widget_refresh(H.wroot[p][i]);
                    }
                }
            }
        }
        last_mode = dock;
    }
    lv_label_set_text(H.mode_chip_l, dock ? "底座" : "背夹");
    lv_obj_set_style_bg_color(H.mode_chip, lv_color_hex(dock ? 0x10241A : 0x2A2410), 0);
    lv_obj_set_style_border_color(H.mode_chip, lv_color_hex(dock ? 0x1F6B43 : 0x6B5A1A), 0);
    lv_obj_set_style_text_color(H.mode_chip_l, lv_color_hex(dock ? 0x4ADE80 : HV_C_ACCENT), 0);

    /* 语音进行中：状态栏胶囊（离开语音卡片后仍可见） */
    hv_voice_state_t vs = hv_voice_state();
    if (vs != HV_VOICE_IDLE && vs != HV_VOICE_ERROR && !(H.view == HV_VIEW_VOICE)) {
        lv_obj_remove_flag(H.capsule, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(H.capsule_l, vs == HV_VOICE_LISTENING ? "正在听" :
                          vs == HV_VOICE_REPLY ? "有新回复" : "思考中");
    } else {
        lv_obj_add_flag(H.capsule, LV_OBJ_FLAG_HIDDEN);
    }

    if (H.hdr_weather) {
        int temp = 0;
        char cond[24];
        if (hv_weather_summary(cond, sizeof(cond), &temp)) {
            hv_deg_text(H.hdr_weather, H.hdr_deg, temp, cond);
            lv_obj_remove_flag(H.hdr_deg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(H.hdr_weather);
            /* 小圆环放在数字后面 */
            snprintf(buf, sizeof(buf), "%d", temp);
            lv_point_t p;
            lv_text_get_size(&p, buf, lv_obj_get_style_text_font(H.hdr_weather, 0), 0, 0, LV_COORD_MAX, 0);
            lv_obj_align_to(H.hdr_deg, H.hdr_weather, LV_ALIGN_TOP_LEFT, p.x + 3, 2);
        } else {
            lv_label_set_text(H.hdr_weather, "");
            lv_obj_add_flag(H.hdr_deg, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* -------------------------------------------------------------- 标题行 */

static void hv_edit_bar_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    system_ui_click_feedback();
    if (which == 0) {
        hv_add_open(false);
    } else {
        hv_exit_edit();
    }
}

static void hv_header_build(void)
{
    int32_t w = (int32_t)s_ui.width;

    H.header = hv_box(H.scr, 0, HV_STATUS_H, w, HV_PAGER_Y - HV_STATUS_H, 0, 0);
    lv_obj_set_style_bg_opa(H.header, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(H.header, LV_OBJ_FLAG_CLICKABLE);
    hv_box(H.header, HV_PAD, 19, 8, 8, HV_C_ACCENT, 2);
    lv_obj_t *t = hv_text(H.header, g_hvf.f20, HV_C_TEXT, "系统首页");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, HV_PAD + 16, 0);
    H.hdr_date = hv_text(H.header, g_hvf.f16, HV_C_MUTED, "");
    lv_obj_align_to(H.hdr_date, t, LV_ALIGN_OUT_RIGHT_MID, 12, 1);
    H.hdr_weather = hv_text(H.header, g_hvf.f20, HV_C_TEXT, "");
    lv_obj_align(H.hdr_weather, LV_ALIGN_RIGHT_MID, -HV_PAD - 4, 0);
    lv_obj_set_style_text_align(H.hdr_weather, LV_TEXT_ALIGN_LEFT, 0);
    H.hdr_deg = lv_obj_create(H.header);
    hv_plain(H.hdr_deg);
    lv_obj_set_size(H.hdr_deg, 9, 9);
    lv_obj_set_style_radius(H.hdr_deg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(H.hdr_deg, 2, 0);
    lv_obj_set_style_border_color(H.hdr_deg, lv_color_hex(HV_C_TEXT), 0);
    lv_obj_add_flag(H.hdr_deg, LV_OBJ_FLAG_HIDDEN);

    /* 编辑模式的标题栏：＋添加小组件 / 编辑首页 / 完成 */
    H.edit_bar = hv_box(H.scr, 0, 48, w, 42, 0, 0);
    lv_obj_set_style_bg_opa(H.edit_bar, LV_OPA_TRANSP, 0);
    H.edit_add = hv_box(H.edit_bar, HV_PAD - 6, 2, 156, 38, 0x24272E, 19);
    lv_obj_add_flag(H.edit_add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(H.edit_add, hv_edit_bar_cb, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *plus = hv_text(H.edit_add, &lv_font_montserrat_14, HV_C_TEXT, LV_SYMBOL_PLUS);
    lv_obj_align(plus, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t *add_l = hv_text(H.edit_add, g_hvf.f16, HV_C_TEXT, "添加小组件");
    lv_obj_align(add_l, LV_ALIGN_LEFT_MID, 34, 0);
    lv_obj_t *mid = hv_text(H.edit_bar, g_hvf.f20, HV_C_TEXT, "编辑首页");
    lv_obj_align(mid, LV_ALIGN_CENTER, 18, 0);
    H.edit_done = hv_box(H.edit_bar, w - HV_PAD - 84 + 6, 2, 84, 38, HV_C_ACCENT, 19);
    lv_obj_add_flag(H.edit_done, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(H.edit_done, hv_edit_bar_cb, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *done_l = hv_text(H.edit_done, g_hvf.f20, HV_C_ACCENT_INK, "完成");
    lv_obj_center(done_l);
    lv_obj_add_flag(H.edit_bar, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------ 焦点 */

/* 焦点表：编辑模式下第 0 个是「添加小组件」，然后是本页小组件，第 1 页再加底部应用。 */
static int hv_focus_count(void)
{
    int n = H.layout.n[H.page];
    if (H.page == 0) {
        n += H.edit ? HV_DOCK_MAX : H.layout.dock_n;
    }
    return n + (H.edit ? 1 : 0);
}

static lv_obj_t *hv_focus_obj(int idx)
{
    if (H.edit) {
        if (idx == 0) {
            return H.edit_add;
        }
        idx--;
    }
    if (idx < H.layout.n[H.page]) {
        return H.wroot[H.page][idx];
    }
    idx -= H.layout.n[H.page];
    if (H.page == 0 && idx < HV_DOCK_MAX) {
        return H.dock_slot[idx] ? lv_obj_get_child(H.dock_slot[idx], 0) : NULL;
    }
    return NULL;
}

static void hv_focus_apply(void)
{
    lv_obj_t *obj = H.focus_visible && (H.view == HV_VIEW_HOME || H.view == HV_VIEW_EDIT) ?
                    hv_focus_obj(H.focus) : NULL;
    if (H.focus_obj && H.focus_obj != obj) {
        hv_focus_ring(H.focus_obj, false);
    }
    H.focus_obj = obj;
    if (obj) {
        hv_focus_ring(obj, true);
    }
}

static void hv_focus_clear(void)
{
    H.focus_visible = false;
    hv_focus_apply();
}

/* ------------------------------------------------------------- 页面 */

static void hv_slot_geom(const hv_slot_t *s, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    *x = HV_PAD + s->col * (HV_CELL_W + HV_GAP);
    *y = HV_GRID_TOP + s->row * HV_ROW_H;
    *w = s->cols == 2 ? HV_CELL_W * 2 + HV_GAP : HV_CELL_W;
    *h = s->rows == 2 ? HV_ROW_H * 2 - HV_LABEL_H : HV_CELL_H;
}

static void hv_widget_event_cb(lv_event_t *e);
static void hv_dock_event_cb(lv_event_t *e);
static void hv_page_event_cb(lv_event_t *e);
static void hv_pager_scroll_cb(lv_event_t *e);
static void hv_dot_cb(lv_event_t *e);

static void hv_build_dock(lv_obj_t *page)
{
    int32_t w = (int32_t)s_ui.width;
    int32_t slot_w = (w - 2 * 12) / HV_DOCK_MAX;

    for (int i = 0; i < HV_DOCK_MAX; i++) {
        H.dock_slot[i] = NULL;
    }
    for (int i = 0; i < HV_DOCK_MAX; i++) {
        lv_obj_t *slot = lv_obj_create(page);
        hv_plain(slot);
        lv_obj_add_flag(slot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_size(slot, slot_w, HV_ICON + 40);
        lv_obj_set_pos(slot, 12 + i * slot_w, HV_DOCK_Y - HV_PAGER_Y);
        lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        H.dock_slot[i] = slot;
        if (i < H.layout.dock_n) {
            const hv_app_t *app = hv_catalog_find(H.layout.dock[i]);
            hv_app_t missing = {0};
            if (!app) {
                strlcpy(missing.id, H.layout.dock[i], sizeof(missing.id));
                strlcpy(missing.title, "已移除", sizeof(missing.title));
                missing.color = HV_C_FAINT;
                app = &missing;
            }
            lv_obj_t *icon = hv_app_icon_create(slot, app, HV_ICON, true);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_add_event_cb(icon, hv_dock_event_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
            if (H.edit) {
                lv_obj_t *minus = hv_box(icon, 0, 0, 28, 28, 0x4A4E57, 14);
                lv_obj_set_style_border_width(minus, 2, 0);
                lv_obj_set_style_border_color(minus, lv_color_hex(0x000000), 0);
                lv_obj_t *m = hv_text(minus, &lv_font_montserrat_14, HV_C_TEXT, LV_SYMBOL_MINUS);
                lv_obj_center(m);
                lv_obj_align(minus, LV_ALIGN_TOP_LEFT, 0, -4);
            }
        } else if (H.edit) {
            /* 空位：虚线 ＋ */
            lv_obj_t *plus = lv_obj_create(slot);
            hv_plain(plus);
            lv_obj_add_flag(plus, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(plus, HV_ICON, HV_ICON);
            lv_obj_align(plus, LV_ALIGN_TOP_MID, 0, 2);
            lv_obj_set_style_radius(plus, 18, 0);
            lv_obj_set_style_border_width(plus, 2, 0);
            lv_obj_set_style_border_color(plus, lv_color_hex(0x5C6370), 0);
            lv_obj_set_style_border_opa(plus, LV_OPA_COVER, 0);
            lv_obj_t *p = hv_text(plus, hv_symbol_font(20), HV_C_MUTED, LV_SYMBOL_PLUS);
            lv_obj_center(p);
            lv_obj_add_event_cb(plus, hv_dock_event_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        }
    }
}

void hv_edit_remove(int p, int i);
void hv_edit_cycle(int p, int i);

static void hv_badge_cb(lv_event_t *e)
{
    int key = (int)(intptr_t)lv_event_get_user_data(e);
    bool size = key >= 1000;
    key %= 1000;
    system_ui_click_feedback();
    if (size) {
        hv_edit_cycle(key / HV_MAX_PER_PAGE, key % HV_MAX_PER_PAGE);
    } else {
        hv_edit_remove(key / HV_MAX_PER_PAGE, key % HV_MAX_PER_PAGE);
    }
}

static void hv_decorate_slot(lv_obj_t *slot, const hv_item_t *it, int32_t w, int32_t h, int key)
{
    const hv_widget_desc_t *d = hv_widgets_find(it->w);
    int nsizes = 0;

    if (!d) {
        return;
    }
    for (int s = 0; s <= HV_SZ_L; s++) {
        nsizes += (d->sizes & HV_SZ_BIT(s)) ? 1 : 0;
    }
    if (d->fixed) {
        lv_obj_t *tag = hv_box(slot, 0, 0, 52, 26, 0x2C2F37, 13);
        lv_obj_t *t = hv_text(tag, g_hvf.f16, HV_C_MUTED, "固定");
        lv_obj_center(t);
        lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 0, 0);
    } else {
        lv_obj_t *minus = hv_box(slot, 0, 0, 30, 30, 0x4A4E57, 15);
        lv_obj_set_style_border_width(minus, 2, 0);
        lv_obj_set_style_border_color(minus, lv_color_hex(0x000000), 0);
        lv_obj_t *m = hv_text(minus, &lv_font_montserrat_14, HV_C_TEXT, LV_SYMBOL_MINUS);
        lv_obj_center(m);
        lv_obj_align(minus, LV_ALIGN_TOP_LEFT, 6, 6);
        lv_obj_add_flag(minus, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(minus, 8);
        lv_obj_add_event_cb(minus, hv_badge_cb, LV_EVENT_CLICKED, (void *)(intptr_t)key);
    }
    if (nsizes > 1) {
        lv_obj_t *sz = hv_box(slot, 0, 0, 36, 30, HV_C_ACCENT, 15);
        lv_obj_t *t = hv_text(sz, g_hvf.f16, HV_C_ACCENT_INK, hv_size_name(it->sz));
        lv_obj_center(t);
        lv_obj_set_pos(sz, w - 44, h - 38);
        lv_obj_add_flag(sz, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(sz, 8);
        lv_obj_add_event_cb(sz, hv_badge_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(1000 + key));
    }
}

static void hv_build_pages(void)
{
    int32_t w = (int32_t)s_ui.width;
    int32_t page_h = (int32_t)s_ui.height - HV_SOFTKEY_H - HV_PAGER_Y - 24;

    /* 重建时保持原来的层级：以前新建的分页容器总是排到最上层，改名/卸载后目录刷新时
     * 首页小组件和底栏会盖在应用总览、确认框上面（Codex 第 2 轮 N01） */
    int32_t pager_idx = H.pager ? (int32_t)lv_obj_get_index(H.pager) : -1;
    if (H.pager) {
        lv_obj_delete(H.pager);
        H.pager = NULL;
    }
    H.focus_obj = NULL;
    memset(H.pg, 0, sizeof(H.pg));
    memset(H.slot, 0, sizeof(H.slot));
    memset(H.wroot, 0, sizeof(H.wroot));
    memset(H.dock_slot, 0, sizeof(H.dock_slot));

    H.pager = lv_obj_create(H.scr);
    if (pager_idx >= 0) {
        lv_obj_move_to_index(H.pager, pager_idx);
    }
    hv_plain(H.pager);
    lv_obj_set_pos(H.pager, 0, HV_PAGER_Y);
    lv_obj_set_size(H.pager, w, page_h);
    lv_obj_set_flex_flow(H.pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(H.pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(H.pager, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(H.pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(H.pager, LV_OBJ_FLAG_SCROLL_ONE);
    if (!H.edit) {
        lv_obj_add_flag(H.pager, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_event_cb(H.pager, hv_pager_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    for (int p = 0; p < H.layout.pages; p++) {
        hv_slot_t slots[HV_MAX_PER_PAGE];
        lv_obj_t *page = lv_obj_create(H.pager);

        hv_plain(page);
        lv_obj_set_size(page, w, page_h);
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(page, hv_page_event_cb, LV_EVENT_ALL, (void *)(intptr_t)p);
        H.pg[p] = page;
        if (!hv_layout_pack(H.layout.it[p], H.layout.n[p], slots)) {
            ESP_LOGW(SYSTEM_UI_TAG, "home page %d does not fit, skipped", p);
            continue;
        }
        for (int i = 0; i < H.layout.n[p]; i++) {
            const hv_item_t *it = &H.layout.it[p][i];
            const hv_widget_desc_t *d = hv_widgets_find(it->w);
            int32_t x, y, sw, sh;
            lv_obj_t *slot;
            lv_obj_t *label;

            hv_slot_geom(&slots[i], &x, &y, &sw, &sh);
            slot = lv_obj_create(page);
            hv_plain(slot);
            lv_obj_add_flag(slot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
            lv_obj_set_pos(slot, x, y);
            lv_obj_set_size(slot, sw, sh + HV_LABEL_H);
            lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
            H.slot[p][i] = slot;

            H.wroot[p][i] = hv_widget_create(slot, it->w, it->sz, sw, sh);
            lv_obj_add_flag(H.wroot[p][i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(H.wroot[p][i], 26, 0);
            lv_obj_add_event_cb(H.wroot[p][i], hv_widget_event_cb, LV_EVENT_ALL,
                                (void *)(intptr_t)(p * HV_MAX_PER_PAGE + i));
            label = hv_text(slot, g_hvf.f16, HV_C_MUTED, d ? d->name : it->w);
            lv_obj_set_width(label, sw);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(label, 0, sh + 4);
            if (d && d->ai) {
                lv_obj_t *badge = hv_box(slot, 0, 0, 30, 18, HV_C_ACCENT, 9);
                lv_obj_t *t = hv_text(badge, &lv_font_montserrat_14, HV_C_ACCENT_INK, "AI");
                lv_obj_center(t);
                lv_obj_set_pos(badge, sw - 42, 10);
            }
            if (H.edit) {
                hv_decorate_slot(slot, it, sw, sh, p * HV_MAX_PER_PAGE + i);
            }
        }
        if (p == 0) {
            hv_build_dock(page);
        }
    }

    /* 分页点（同样保持原层级） */
    int32_t dots_idx = H.dots ? (int32_t)lv_obj_get_index(H.dots) : -1;
    if (H.dots) {
        lv_obj_delete(H.dots);
    }
    H.dots = hv_box(H.scr, 0, HV_DOTS_Y, w, 24, 0, 0);
    if (dots_idx >= 0) {
        lv_obj_move_to_index(H.dots, dots_idx);
    }
    lv_obj_set_style_bg_opa(H.dots, LV_OPA_TRANSP, 0);
    for (int p = 0; p < H.layout.pages; p++) {
        H.dot[p] = hv_box(H.dots, 0, 0, 10, 10, 0x4B5160, 5);
        lv_obj_add_flag(H.dot[p], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(H.dot[p], 10);
        lv_obj_add_event_cb(H.dot[p], hv_dot_cb, LV_EVENT_CLICKED, (void *)(intptr_t)p);
        lv_obj_set_pos(H.dot[p], w / 2 - H.layout.pages * 11 + p * 22 + 1, 7);
    }
    if (H.page >= H.layout.pages) {
        H.page = H.layout.pages - 1;
    }
    hv_set_page(H.page, false);
    if (H.toast) {
        lv_obj_move_foreground(H.toast);
    }
}

static void hv_dots_update(void)
{
    for (int p = 0; p < H.layout.pages; p++) {
        if (!H.dot[p]) {
            continue;
        }
        bool on = p == H.page;
        lv_obj_set_size(H.dot[p], on ? 12 : 10, on ? 12 : 10);
        lv_obj_set_style_bg_color(H.dot[p], lv_color_hex(on ? HV_C_ACCENT : 0x4B5160), 0);
    }
    /* 底部应用栏只属于第 1 页；分页点在第 2 页以后下移到原应用栏位置之上保持不动。 */
}

void hv_set_page(int page, bool anim)
{
    if (!H.pager || page < 0 || page >= H.layout.pages) {
        return;
    }
    H.page = page;
    lv_obj_scroll_to_x(H.pager, page * (int32_t)s_ui.width, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    hv_dots_update();
    if (H.focus >= hv_focus_count()) {
        H.focus = hv_focus_count() - 1;
    }
    if (H.focus < 0) {
        H.focus = 0;
    }
    hv_focus_apply();
}

int hv_page(void)
{
    return H.page;
}

static void hv_pager_scroll_cb(lv_event_t *e)
{
    (void)e;
    int32_t x = lv_obj_get_scroll_x(H.pager);
    int page = (int)((x + (int32_t)s_ui.width / 2) / (int32_t)s_ui.width);
    if (page != H.page && page >= 0 && page < H.layout.pages) {
        H.page = page;
        H.focus = 0;
        hv_dots_update();
        hv_focus_apply();
    }
}

static void hv_dot_cb(lv_event_t *e)
{
    hv_set_page((int)(intptr_t)lv_event_get_user_data(e), true);
}

/* ------------------------------------------------------------ 打开 */

static void hv_open_widget(const hv_item_t *it)
{
    const hv_widget_desc_t *d = hv_widgets_find(it->w);

    if (strcmp(it->w, "voice") == 0) {
        hv_toast("按住 M 键说话，松开发送");
        return;
    }
    if (strcmp(it->w, "clock") == 0) {
        hv_toast("时钟随联网自动对时");
        return;
    }
    if (d && d->app[0]) {
        if (!hv_catalog_find(d->app)) {
            hv_toast("对应的应用没有安装");
            return;
        }
        (void)hv_catalog_launch(d->app, strcmp(d->app, "garden") == 0 ? "{\"page\":\"mine\"}" : NULL);
    }
}

static void hv_activate_focus(void)
{
    int idx = H.focus;

    if (idx < H.layout.n[H.page]) {
        hv_open_widget(&H.layout.it[H.page][idx]);
        return;
    }
    idx -= H.layout.n[H.page];
    if (H.page == 0 && idx < H.layout.dock_n) {
        (void)hv_catalog_launch(H.layout.dock[idx], NULL);
    }
}

/* ------------------------------------------------------------ 事件 */

static bool s_suppress_click;

/* 系统手势（顶部下拉 / 底部上滑）刚刚触发过：这次按压不再当作长按或点按。 */
bool system_ui_gesture_recent(uint32_t window_ms);

static void hv_widget_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int key = (int)(intptr_t)lv_event_get_user_data(e);
    int p = key / HV_MAX_PER_PAGE, i = key % HV_MAX_PER_PAGE;
    const hv_item_t *it;

    if (p >= H.layout.pages || i >= H.layout.n[p]) {
        return;
    }
    it = &H.layout.it[p][i];
    if (H.edit) {
        extern void hv_edit_widget_event(lv_event_t *e, int page, int index);
        hv_edit_widget_event(e, p, i);
        return;
    }
    switch (code) {
    case LV_EVENT_PRESSED:
        s_suppress_click = false;
        break;
    case LV_EVENT_LONG_PRESSED:
        s_suppress_click = true;
        if (system_ui_gesture_recent(1200)) {
            break;
        }
        if (strcmp(it->w, "mine") == 0 || strcmp(it->w, "voice") == 0) {
            hv_voice_press();
        } else {
            hv_enter_edit();
        }
        break;
    case LV_EVENT_RELEASED:
        if (s_suppress_click && hv_voice_state() == HV_VOICE_LISTENING) {
            hv_voice_release();
        }
        break;
    case LV_EVENT_CLICKED:
        if (!s_suppress_click) {
            H.focus = (H.edit ? 1 : 0) + i;
            hv_focus_clear();
            hv_open_widget(it);
        }
        break;
    default:
        break;
    }
}

static void hv_dock_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    if (H.edit) {
        if (code == LV_EVENT_CLICKED) {
            if (i < H.layout.dock_n) {
                /* 点图标左上角 − 或图标本身：移出底栏（不是卸载） */
                hv_layout_t l = H.layout;
                for (int j = i; j < l.dock_n - 1; j++) {
                    strlcpy(l.dock[j], l.dock[j + 1], sizeof(l.dock[j]));
                }
                l.dock_n--;
                (void)hv_apply_layout(&l, true);
                hv_toast("已从底部应用栏移出");
            } else {
                hv_add_open(true);
            }
        }
        return;
    }
    switch (code) {
    case LV_EVENT_PRESSED:
        s_suppress_click = false;
        break;
    case LV_EVENT_LONG_PRESSED:
        s_suppress_click = true;
        if (i < H.layout.dock_n) {
            hv_sheet_open(H.layout.dock[i]);
        }
        break;
    case LV_EVENT_CLICKED:
        if (!s_suppress_click && i < H.layout.dock_n) {
            hv_focus_clear();
            (void)hv_catalog_launch(H.layout.dock[i], NULL);
        }
        break;
    default:
        break;
    }
}

static void hv_page_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED && !H.edit && !system_ui_gesture_recent(1200)) {
        hv_enter_edit();
    }
}

/* ----------------------------------------------------------- 编辑模式 */

void hv_enter_edit(void)
{
    if (H.edit) {
        return;
    }
    if (H.view != HV_VIEW_HOME && H.view != HV_VIEW_CONTROL) {
        return;
    }
    if (H.view == HV_VIEW_CONTROL) {
        hv_close_control();
    }
    system_ui_click_feedback();
    H.edit = true;
    H.picked = -1;
    H.view = HV_VIEW_EDIT;
    H.focus = 0;
    lv_obj_add_flag(H.header, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(H.edit_bar, LV_OBJ_FLAG_HIDDEN);
    hv_build_pages();
    hv_softkeys_for_view();
    ESP_LOGI(SYSTEM_UI_TAG, "HV_STATE edit");
}

void hv_exit_edit(void)
{
    if (!H.edit) {
        return;
    }
    H.edit = false;
    H.picked = -1;
    H.dragging = false;
    H.view = HV_VIEW_HOME;
    lv_obj_remove_flag(H.header, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(H.edit_bar, LV_OBJ_FLAG_HIDDEN);
    hv_build_pages();
    hv_softkeys_for_view();
    (void)hv_layout_save(&H.layout);
    hv_toast("首页布局已保存");
    ESP_LOGI(SYSTEM_UI_TAG, "HV_STATE home");
}

/* 编辑操作需要的内部访问 */
hv_layout_t *hv_layout_mut(void)
{
    return &H.layout;
}

int hv_edit_picked(void)
{
    return H.picked;
}

void hv_edit_set_picked(int idx)
{
    H.picked = idx;
    hv_softkeys_for_view();
}

int hv_focus_index(void)
{
    return H.focus;
}

void hv_focus_set(int idx, bool visible)
{
    H.focus = idx;
    H.focus_visible = visible;
    hv_focus_apply();
}

lv_obj_t *hv_slot_obj(int page, int index)
{
    return (page < HV_MAX_PAGES && index < HV_MAX_PER_PAGE) ? H.slot[page][index] : NULL;
}

lv_obj_t *hv_pager_obj(void)
{
    return H.pager;
}

void hv_rebuild(void)
{
    hv_build_pages();
}

esp_err_t hv_apply_layout(const hv_layout_t *l, bool save)
{
    for (int p = 0; p < l->pages; p++) {
        if (!hv_layout_pack(l->it[p], l->n[p], NULL)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    H.layout = *l;
    if (H.scr) {
        hv_build_pages();
    }
    return save ? hv_layout_save(&H.layout) : ESP_OK;
}

const hv_layout_t *hv_layout(void)
{
    return &H.layout;
}

/* ------------------------------------------------------------- 视图 */

void hv_view_set(hv_view_t v)
{
    H.view = v;
    hv_softkeys_for_view();
    hv_focus_apply();
    ESP_LOGI(SYSTEM_UI_TAG, "HV_STATE %s", hv_view_name(v));
}

hv_view_t hv_view(void)
{
    return H.view;
}

const char *hv_view_name(hv_view_t v)
{
    switch (v) {
    case HV_VIEW_HOME: return "home";
    case HV_VIEW_OVERVIEW: return "overview";
    case HV_VIEW_SHEET: return "sheet";
    case HV_VIEW_CONFIRM: return "confirm";
    case HV_VIEW_RENAME: return "rename";
    case HV_VIEW_INFO: return "info";
    case HV_VIEW_CONTROL: return "control";
    case HV_VIEW_EDIT: return "edit";
    case HV_VIEW_ADD: return "add";
    case HV_VIEW_VOICE: return "voice";
    default: return "?";
    }
}

void hv_go_home(void)
{
    if (hv_voice_visible()) {
        hv_voice_hide();
    }
    if (hv_modal_open()) {
        hv_modal_close();
    }
    if (H.view == HV_VIEW_SHEET) {
        hv_sheet_close();
    }
    if (H.view == HV_VIEW_ADD) {
        hv_add_close();
    }
    if (H.view == HV_VIEW_OVERVIEW) {
        hv_close_overview();
    }
    if (H.view == HV_VIEW_CONTROL) {
        hv_close_control();
    }
    if (H.edit) {
        hv_exit_edit();
    }
    hv_view_set(HV_VIEW_HOME);
}

void hv_key(hv_key_t key)
{
    if (!H.scr) {
        return;
    }
    /* 语音卡片在最上层：先给它 */
    if (hv_voice_visible() && key != HV_KEY_M_LONG_START) {
        hv_voice_key(key);
        return;
    }
    switch (H.view) {
    case HV_VIEW_HOME: {
        int n = hv_focus_count();
        switch (key) {
        case HV_KEY_PREV:
        case HV_KEY_NEXT:
            if (!H.focus_visible) {
                H.focus_visible = true;
                H.focus = key == HV_KEY_NEXT ? 0 : n - 1;
            } else if (key == HV_KEY_NEXT) {
                if (H.focus + 1 < n) {
                    H.focus++;
                } else if (H.page + 1 < H.layout.pages) {
                    hv_set_page(H.page + 1, true);
                    H.focus = 0;
                } else {
                    H.focus = n - 1;
                }
            } else {
                if (H.focus > 0) {
                    H.focus--;
                } else if (H.page > 0) {
                    hv_set_page(H.page - 1, true);
                    H.focus = hv_focus_count() - 1;
                }
            }
            hv_focus_apply();
            break;
        case HV_KEY_M:
            if (!H.focus_visible) {
                H.focus_visible = true;
                H.focus = 0;
                hv_focus_apply();
            } else {
                hv_activate_focus();
            }
            break;
        case HV_KEY_M_LONG_START:
            hv_voice_press();
            break;
        case HV_KEY_M_LONG_END:
            hv_voice_release();
            break;
        case HV_KEY_SET:
            hv_open_overview();
            break;
        case HV_KEY_SET_LONG:
            hv_open_control();
            break;
        }
        break;
    }
    case HV_VIEW_OVERVIEW:
        hv_overview_key(key);
        break;
    case HV_VIEW_SHEET:
        hv_sheet_key(key);
        break;
    case HV_VIEW_CONFIRM:
    case HV_VIEW_RENAME:
    case HV_VIEW_INFO:
        hv_modal_key(key);
        break;
    case HV_VIEW_CONTROL:
        hv_control_key(key);
        break;
    case HV_VIEW_EDIT:
        hv_edit_key(key);
        break;
    case HV_VIEW_ADD:
        hv_add_key(key);
        break;
    case HV_VIEW_VOICE:
        hv_voice_key(key);
        break;
    }
}

/* ---------------------------------------------------------- 定时刷新 */

static void hv_refresh_widgets(bool data)
{
    for (int p = 0; p < H.layout.pages; p++) {
        for (int i = 0; i < H.layout.n[p]; i++) {
            if (!H.wroot[p][i]) {
                continue;
            }
            if (data) {
                hv_widget_refresh(H.wroot[p][i]);
            } else {
                hv_widget_tick_second(H.wroot[p][i]);
            }
        }
    }
}

static void hv_tick_cb(lv_timer_t *t)
{
    (void)t;
    H.seconds++;
    /* 首页不在前台（应用独占或原生页面）时不刷新，省 CPU。 */
    if (lv_screen_active() != H.scr) {
        return;
    }
    hv_refresh_widgets(false);
    if (H.seconds % 5 == 0) {
        hv_refresh_status();
    }
    if (H.seconds % 30 == 0) {
        hv_refresh_widgets(true);
        if (H.view == HV_VIEW_CONTROL) {
            hv_control_refresh();
        }
    }
}

void hv_on_show(void)
{
    if (!H.scr) {
        return;
    }
    hv_widgets_rescan();
    hv_refresh_status();
    hv_refresh_widgets(true);
}

void hv_refresh_widget_kind(const char *wid)
{
    for (int p = 0; p < H.layout.pages; p++) {
        for (int i = 0; i < H.layout.n[p]; i++) {
            if (H.wroot[p][i] && !strcmp(H.layout.it[p][i].w, wid)) {
                hv_widget_refresh(H.wroot[p][i]);
            }
        }
    }
}

void hv_reload_apps(void)
{
    hv_catalog_refresh();
    if (!H.scr) {
        return;
    }
    hv_build_pages();
    hv_overview_refresh();
}

/* ------------------------------------------------------------ 创建 */

lv_obj_t *hv_create_screen(void)
{
    int32_t w = (int32_t)s_ui.width;

    if (H.scr) {
        return H.scr;
    }
    memset(&H, 0, sizeof(H));
    H.picked = -1;
    hv_fonts_init();
    hv_widgets_rescan();
    hv_catalog_refresh();
    (void)hv_layout_load(&H.layout);

    H.scr = lv_obj_create(NULL);
    hv_plain(H.scr);
    lv_obj_set_size(H.scr, w, (int32_t)s_ui.height);
    lv_obj_set_style_bg_opa(H.scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(H.scr, lv_color_hex(0x050608), 0);
    lv_obj_set_style_bg_grad_color(H.scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(H.scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_text_font(H.scr, s_ui.font, 0);

    /* 壁纸：两团很淡的同心光晕。不用阴影（样机软件渲染太贵，时钟每秒重绘会被拖慢）。 */
    for (int g = 0; g < 2; g++) {
        uint32_t c = g == 0 ? 0x2A2206 : 0x06201F;
        int32_t cx = g == 0 ? 40 : w - 30, cy = g == 0 ? 210 : 600;
        for (int r = 3; r >= 1; r--) {
            int32_t d = 140 + r * 90;
            lv_obj_t *o = hv_box(H.scr, cx - d / 2, cy - d / 2, d, d, c, LV_RADIUS_CIRCLE);
            lv_obj_set_style_bg_opa(o, (lv_opa_t)(26 + (3 - r) * 18), 0);
        }
    }

    hv_status_build();
    hv_header_build();
    hv_build_pages();
    /* 底部上滑指示条；所在的一条区域吸收按压，上滑不会落到页面或底栏上 */
    lv_obj_t *swipe_zone = hv_box(H.scr, 0, (int32_t)s_ui.height - HV_SOFTKEY_H - 40, w, 40, 0, 0);
    lv_obj_set_style_bg_opa(swipe_zone, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(swipe_zone, LV_OBJ_FLAG_CLICKABLE);
    H.indicator = hv_box(H.scr, (w - 120) / 2, (int32_t)s_ui.height - HV_SOFTKEY_H - 14, 120, 5, 0xFFFFFF, 3);
    lv_obj_set_style_bg_opa(H.indicator, LV_OPA_40, 0);
    hv_softkeys_build();
    H.view = HV_VIEW_HOME;
    hv_softkeys_for_view();
    hv_refresh_status();
    H.tick = lv_timer_create(hv_tick_cb, 1000, NULL);
    ESP_LOGI(SYSTEM_UI_TAG, "HV_STATE home pages=%d dock=%d apps=%u", H.layout.pages, H.layout.dock_n,
             (unsigned)hv_catalog_count());
    return H.scr;
}

bool hv_is_active(void)
{
    return H.scr != NULL && lv_screen_active() == H.scr;
}

lv_obj_t *hv_screen(void)
{
    return H.scr;
}

/* 状态栏是系统外框：任何面板打开后都把它提到最上层。 */
void hv_status_to_front(void)
{
    if (H.status_bar) {
        lv_obj_move_foreground(H.status_bar);
    }
}

void hv_destroy(void)
{
    if (!H.scr) {
        return;
    }
    if (H.tick) {
        lv_timer_delete(H.tick);
    }
    if (H.toast_timer) {
        lv_timer_delete(H.toast_timer);
    }
    extern void hv_overview_forget(void);
    extern void hv_control_forget(void);
    extern void hv_voice_forget(void);
    hv_overview_forget();
    hv_control_forget();
    hv_voice_forget();
    lv_obj_delete(H.scr);
    memset(&H, 0, sizeof(H));
    hv_fonts_deinit();
}

/* ------------------------------------------------------------ 状态读回 */

cJSON *hv_state_json(void)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return NULL;
    }
    cJSON_AddStringToObject(j, "view", H.scr ? hv_view_name(H.view) : "none");
    cJSON_AddBoolToObject(j, "home_foreground", hv_is_active());
    cJSON_AddNumberToObject(j, "page", H.page);
    cJSON_AddNumberToObject(j, "pages", H.layout.pages);
    cJSON_AddNumberToObject(j, "focus", H.focus_visible ? H.focus : -1);
    cJSON_AddBoolToObject(j, "edit", H.edit);
    cJSON_AddNumberToObject(j, "picked", H.picked);
    cJSON_AddStringToObject(j, "mode", hv_plat_mode_get() == HV_MODE_DOCK ? "dock" : "clip");
    cJSON_AddStringToObject(j, "voice", hv_voice_state_name(hv_voice_state()));
    cJSON_AddStringToObject(j, "voice_route", hv_voice_route_name());
    cJSON_AddItemToObject(j, "layout", hv_layout_to_json(&H.layout));
    cJSON_AddNumberToObject(j, "apps", (double)hv_catalog_count());
    cJSON_AddNumberToObject(j, "volume", hv_plat_volume_get());
    cJSON_AddNumberToObject(j, "brightness", hv_control_brightness());
    const char *fid = hv_overview_focus_id();
    const char *tid = hv_overview_target_id();
    if (fid && H.view == HV_VIEW_OVERVIEW) {
        cJSON_AddStringToObject(j, "ov_focus", fid);
    }
    if (tid) {
        cJSON_AddStringToObject(j, "target", tid);
    }
    if (H.toast && !lv_obj_has_flag(H.toast, LV_OBJ_FLAG_HIDDEN)) {
        cJSON_AddStringToObject(j, "toast", lv_label_get_text(H.toast_l));
    }
    return j;
}
