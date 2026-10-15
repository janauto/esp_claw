/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 控制中心（顶部下拉 / 长按 SET）。最上面是两种工作模式：
 *   背夹 · 手机：不插电随身用，语音经低功耗蓝牙交给手机识别与对话；
 *   底座 · Wi-Fi：插电放在底座上，背夹自己经 Wi-Fi 直连云端完成语音链路。
 * 下面是连接状态、震动反馈、音量、亮度（软件调光）、编辑首页、后台任务与「我的」主题入口。
 */
#include "home_priv.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

void hv_view_set(hv_view_t v);
lv_obj_t *hv_screen(void);

enum {
    CC_MODE_CLIP = 0,
    CC_MODE_DOCK,
    CC_WIFI,
    CC_PHONE,
    CC_VIBRATE,
    CC_AI,
    CC_VOLUME,
    CC_DIM,
    CC_EDIT,
    CC_TASKS,
    CC_THEME,
    CC_COUNT,
};

static struct {
    lv_obj_t *panel;
    lv_obj_t *time_l, *date_l, *power_l, *phone_l;
    lv_obj_t *item[CC_COUNT];
    lv_obj_t *mode_title[2], *mode_desc[2], *mode_icon[2];
    lv_obj_t *route_l;
    lv_obj_t *tile_value[4];
    lv_obj_t *slider[2];
    lv_obj_t *slider_l[2];
    lv_obj_t *dim;          /* 软件调光遮罩（顶层，所有界面生效） */
    int dim_level;          /* 0-60 */
    int focus;
    bool adjusting;
} CC;

void hv_control_forget(void)
{
    lv_obj_t *dim = CC.dim;
    int level = CC.dim_level;
    memset(&CC, 0, sizeof(CC));
    CC.dim = dim;
    CC.dim_level = level;
}

static void hv_cc_anim_y(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

static void hv_cc_hidden(lv_anim_t *a)
{
    lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void hv_cc_slide(int32_t from, int32_t to, bool hide)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, CC.panel);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_exec_cb(&a, hv_cc_anim_y);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (hide) {
        lv_anim_set_completed_cb(&a, hv_cc_hidden);
    }
    lv_anim_start(&a);
}

static void hv_cc_apply_dim(void)
{
    lv_obj_t *top = lv_layer_top();
    if (!CC.dim) {
        CC.dim = lv_obj_create(top);
        hv_plain(CC.dim);
        lv_obj_remove_flag(CC.dim, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(CC.dim, (int32_t)s_ui.width, (int32_t)s_ui.height);
        lv_obj_set_style_bg_color(CC.dim, lv_color_hex(0x000000), 0);
    }
    lv_obj_set_style_bg_opa(CC.dim, (lv_opa_t)(CC.dim_level * 255 / 100), 0);
    if (CC.dim_level <= 0) {
        lv_obj_add_flag(CC.dim, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(CC.dim, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hv_cc_focus_apply(void)
{
    for (int i = 0; i < CC_COUNT; i++) {
        if (CC.item[i]) {
            hv_focus_ring(CC.item[i], i == CC.focus);
        }
    }
    if (CC.item[CC.focus]) {
        lv_obj_scroll_to_view(CC.item[CC.focus], LV_ANIM_ON);
    }
    hv_softkeys_set("上一项", CC.adjusting ? "确定" : (CC.focus == CC_VOLUME || CC.focus == CC_DIM) ? "调节" : "切换",
                    "下一项", "收起");
}

static void hv_cc_set_mode(hv_mode_t mode)
{
    if (hv_plat_mode_get() == mode) {
        return;
    }
    if (hv_plat_mode_set(mode) != ESP_OK) {
        hv_toast("切换模式失败");
        return;
    }
    if (mode == HV_MODE_DOCK) {
        char ssid[40];
        if (!hv_plat_wifi(ssid, sizeof(ssid))) {
            hv_toast("已切到底座模式；Wi-Fi 未连接，语音暂时不可用");
        } else if (!hv_plat_ai_ready()) {
            hv_toast("已切到底座模式；请先在手机 App 推送 AI 配置");
        } else {
            hv_toast("已切到底座模式：背夹自己联网对话");
        }
    } else {
        hv_toast(hv_plat_phone_linked() ? "已切到背夹模式：语音经手机处理"
                                        : "已切到背夹模式；手机未连接时自动改用底座直连");
    }
    hv_control_refresh();
    hv_refresh_status();
}

static void hv_cc_activate(int item)
{
    switch (item) {
    case CC_MODE_CLIP:
        hv_cc_set_mode(HV_MODE_CLIP);
        break;
    case CC_MODE_DOCK:
        hv_cc_set_mode(HV_MODE_DOCK);
        break;
    case CC_WIFI: {
        char ssid[40];
        hv_toast(hv_plat_wifi(ssid, sizeof(ssid)) ? "Wi-Fi 已连接；更换网络请用手机 App 配网"
                                                  : "Wi-Fi 未连接：请用手机 App 配网");
        break;
    }
    case CC_PHONE:
        hv_toast(hv_plat_ble_connected() ? "手机已通过蓝牙连接" : "打开手机 App 的背夹页即可连接");
        break;
    case CC_VIBRATE:
        hv_plat_vibration_set(!hv_plat_vibration_get());
        hv_control_refresh();
        break;
    case CC_AI:
        hv_toast(hv_plat_ai_ready() ? "底座 AI 服务已配置" : "在手机 App 设备页推送 AI 配置到背夹");
        break;
    case CC_VOLUME:
    case CC_DIM:
        CC.adjusting = !CC.adjusting;
        hv_cc_focus_apply();
        break;
    case CC_EDIT:
        hv_close_control();
        hv_enter_edit();
        break;
    case CC_TASKS:
        hv_close_control();
        system_ui_jobs_set_visible_locked(true);
        break;
    case CC_THEME:
        hv_close_control();
        (void)hv_catalog_launch("garden", "{\"page\":\"mine\"}");
        break;
    default:
        break;
    }
}

static void hv_cc_click_cb(lv_event_t *e)
{
    int item = (int)(intptr_t)lv_event_get_user_data(e);
    system_ui_click_feedback();
    CC.focus = item;
    if (item == CC_VOLUME || item == CC_DIM) {
        return;
    }
    hv_cc_activate(item);
}

static void hv_cc_slider_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    int v = (int)lv_slider_get_value(CC.slider[which]);
    char buf[32];

    if (which == 0) {
        (void)hv_plat_volume_set(v);
        snprintf(buf, sizeof(buf), "音量 %d%%", v);
    } else {
        CC.dim_level = 60 - v * 60 / 100;
        hv_cc_apply_dim();
        snprintf(buf, sizeof(buf), "亮度 %d%%", v);
    }
    lv_label_set_text(CC.slider_l[which], buf);
}

static lv_obj_t *hv_cc_tile(lv_obj_t *parent, int item, int32_t x, int32_t y, int32_t w, int32_t h,
                            const char *symbol, const char *title, lv_obj_t **value_out)
{
    lv_obj_t *t = hv_box(parent, x, y, w, h, 0x1A1D23, 24);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(t, lv_color_hex(0x2A2E36), LV_STATE_PRESSED);
    lv_obj_add_event_cb(t, hv_cc_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)item);
    lv_obj_t *icon = hv_text(t, hv_symbol_font(20), HV_C_TEXT, symbol);
    lv_obj_set_pos(icon, 16, 14);
    lv_obj_t *l = hv_text(t, g_hvf.f20, HV_C_TEXT, title);
    lv_obj_set_pos(l, 16, h - 54);
    if (value_out) {
        *value_out = hv_text(t, g_hvf.f16, HV_C_MUTED, "");
        lv_obj_set_pos(*value_out, 16, h - 28);
        lv_obj_set_width(*value_out, w - 28);
        lv_label_set_long_mode(*value_out, LV_LABEL_LONG_MODE_DOTS);
    }
    CC.item[item] = t;
    return t;
}

lv_obj_t *hv_control_build(lv_obj_t *scr)
{
    int32_t w = (int32_t)s_ui.width;
    int32_t h = (int32_t)s_ui.height - HV_SOFTKEY_H;
    int32_t col_w = (w - 2 * HV_PAD - 14) / 2;
    lv_obj_t *body;
    int32_t y;

    if (CC.panel) {
        return CC.panel;
    }
    CC.panel = hv_box(scr, 0, -h, w, h, 0x0A0B0E, 0);
    lv_obj_add_flag(CC.panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(CC.panel, LV_OPA_COVER, 0);

    body = lv_obj_create(CC.panel);
    hv_plain(body);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(body, 0, HV_STATUS_H);
    lv_obj_set_size(body, w, h - HV_STATUS_H - 30);

    CC.time_l = hv_text(body, g_hvf.f64, HV_C_TEXT, "--:--");
    lv_obj_set_pos(CC.time_l, HV_PAD, 0);
    CC.date_l = hv_text(body, g_hvf.f16, HV_C_MUTED, "");
    lv_obj_set_pos(CC.date_l, HV_PAD + 4, 72);
    CC.power_l = hv_text(body, g_hvf.f24, HV_C_TEXT, "");
    lv_obj_align(CC.power_l, LV_ALIGN_TOP_RIGHT, -HV_PAD, 14);
    CC.phone_l = hv_text(body, g_hvf.f16, HV_C_MUTED, "");
    lv_obj_align(CC.phone_l, LV_ALIGN_TOP_RIGHT, -HV_PAD, 50);

    /* 两种模式 */
    y = 104;
    lv_obj_t *mt = hv_text(body, g_hvf.f16, HV_C_MUTED, "工作模式");
    lv_obj_set_pos(mt, HV_PAD, y);
    y += 28;
    static const char *const titles[2] = {"背夹 · 手机", "底座 · Wi-Fi"};
    static const char *const descs[2] = {"不插电随身用\n语音经蓝牙交给手机", "插电放在底座\n背夹直连云端对话"};
    static const char *const icons[2] = {LV_SYMBOL_BLUETOOTH, LV_SYMBOL_WIFI};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *card = hv_box(body, HV_PAD + i * (col_w + 14), y, col_w, 136, 0x1A1D23, 24);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, hv_cc_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(CC_MODE_CLIP + i));
        CC.mode_icon[i] = hv_text(card, hv_symbol_font(28), HV_C_TEXT, icons[i]);
        lv_obj_set_pos(CC.mode_icon[i], 18, 16);
        CC.mode_title[i] = hv_text(card, g_hvf.f24, HV_C_TEXT, titles[i]);
        lv_obj_set_pos(CC.mode_title[i], 18, 56);
        CC.mode_desc[i] = hv_text(card, g_hvf.f16, HV_C_MUTED, descs[i]);
        lv_obj_set_pos(CC.mode_desc[i], 18, 88);
        CC.item[CC_MODE_CLIP + i] = card;
    }
    y += 146;
    CC.route_l = hv_text(body, g_hvf.f16, HV_C_ACCENT, "");
    lv_obj_set_width(CC.route_l, w - 2 * HV_PAD);
    lv_label_set_long_mode(CC.route_l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(CC.route_l, 4, 0);
    lv_obj_set_pos(CC.route_l, HV_PAD, y);
    y += 58;

    /* 连接与开关 */
    hv_cc_tile(body, CC_WIFI, HV_PAD, y, col_w, 112, LV_SYMBOL_WIFI, "Wi-Fi", &CC.tile_value[0]);
    hv_cc_tile(body, CC_PHONE, HV_PAD + col_w + 14, y, col_w, 112, LV_SYMBOL_BLUETOOTH, "手机", &CC.tile_value[1]);
    y += 124;
    hv_cc_tile(body, CC_VIBRATE, HV_PAD, y, col_w, 112, LV_SYMBOL_BELL, "震动反馈", &CC.tile_value[2]);
    hv_cc_tile(body, CC_AI, HV_PAD + col_w + 14, y, col_w, 112, LV_SYMBOL_SETTINGS, "底座 AI 服务", &CC.tile_value[3]);
    y += 124;

    /* 音量 / 亮度 */
    static const char *const sl_names[2] = {"音量", "亮度"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = hv_box(body, HV_PAD, y, w - 2 * HV_PAD, 76, 0x1A1D23, 22);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        CC.item[CC_VOLUME + i] = row;
        CC.slider_l[i] = hv_text(row, g_hvf.f16, HV_C_TEXT, sl_names[i]);
        lv_obj_set_pos(CC.slider_l[i], 18, 10);
        CC.slider[i] = lv_slider_create(row);
        lv_slider_set_range(CC.slider[i], 0, 100);
        lv_obj_set_size(CC.slider[i], w - 2 * HV_PAD - 44, 14);
        lv_obj_set_pos(CC.slider[i], 22, 44);
        lv_obj_set_style_bg_color(CC.slider[i], lv_color_hex(0x30343C), LV_PART_MAIN);
        lv_obj_set_style_bg_color(CC.slider[i], lv_color_hex(HV_C_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(CC.slider[i], lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_pad_all(CC.slider[i], 6, LV_PART_KNOB);
        lv_obj_add_event_cb(CC.slider[i], hv_cc_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        y += 86;
    }

    /* 入口 */
    static const char *const entries[3] = {"编辑首页布局", "后台任务", "我的主题与小家"};
    static const char *const entry_desc[3] = {"增删小组件、换尺寸、排顺序、底部应用栏",
                                              "查看与停止正在运行的应用", "在「我的」里换主题、照料花园"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = hv_box(body, HV_PAD, y, w - 2 * HV_PAD, 76, 0x1A1D23, 22);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2A2E36), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, hv_cc_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(CC_EDIT + i));
        lv_obj_t *t = hv_text(row, g_hvf.f20, HV_C_TEXT, entries[i]);
        lv_obj_set_pos(t, 18, 12);
        lv_obj_t *d = hv_text(row, g_hvf.f16, HV_C_MUTED, entry_desc[i]);
        lv_obj_set_pos(d, 18, 44);
        lv_obj_t *arrow = hv_text(row, &lv_font_montserrat_14, HV_C_MUTED, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -18, 0);
        CC.item[CC_EDIT + i] = row;
        y += 86;
    }
    hv_box(body, 0, y, 10, 20, 0, 0);

    lv_obj_t *hint = hv_text(CC.panel, g_hvf.f16, HV_C_FAINT, "上滑或按 SET 收起");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(CC.panel, LV_OBJ_FLAG_HIDDEN);

    lv_slider_set_value(CC.slider[0], hv_plat_volume_get(), LV_ANIM_OFF);
    lv_slider_set_value(CC.slider[1], 100 - CC.dim_level * 100 / 60, LV_ANIM_OFF);
    char buf[32];
    snprintf(buf, sizeof(buf), "音量 %d%%", hv_plat_volume_get());
    lv_label_set_text(CC.slider_l[0], buf);
    snprintf(buf, sizeof(buf), "亮度 %d%%", 100 - CC.dim_level * 100 / 60);
    lv_label_set_text(CC.slider_l[1], buf);
    return CC.panel;
}

void hv_control_refresh(void)
{
    struct tm now;
    char buf[96];
    char ssid[40] = {0};
    bool dock = hv_plat_mode_get() == HV_MODE_DOCK;
    bool wifi = hv_plat_wifi(ssid, sizeof(ssid));
    bool ai = hv_plat_ai_ready();
    bool phone = hv_plat_phone_linked();
    static const char *const wd[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

    if (!CC.panel) {
        return;
    }
    if (hv_plat_now(&now)) {
        snprintf(buf, sizeof(buf), "%02d:%02d", now.tm_hour, now.tm_min);
        lv_label_set_text(CC.time_l, buf);
        snprintf(buf, sizeof(buf), "%d月%d日 %s", now.tm_mon + 1, now.tm_mday, wd[now.tm_wday % 7]);
        lv_label_set_text(CC.date_l, buf);
    }
    lv_label_set_text(CC.power_l, "外接供电");
    lv_label_set_text(CC.phone_l, hv_plat_ble_connected() ? "手机已连接" : "手机未连接");
    for (int i = 0; i < 2; i++) {
        bool on = (i == 1) == dock;
        lv_obj_set_style_bg_color(CC.item[CC_MODE_CLIP + i], lv_color_hex(on ? HV_C_ACCENT : 0x1A1D23), 0);
        lv_obj_set_style_text_color(CC.mode_title[i], lv_color_hex(on ? HV_C_ACCENT_INK : HV_C_TEXT), 0);
        lv_obj_set_style_text_color(CC.mode_desc[i], lv_color_hex(on ? 0x4A3D10 : HV_C_MUTED), 0);
        lv_obj_set_style_text_color(CC.mode_icon[i], lv_color_hex(on ? HV_C_ACCENT_INK : HV_C_TEXT), 0);
    }
    if (dock) {
        snprintf(buf, sizeof(buf), "语音线路：背夹直连云端（Wi-Fi）\n%s",
                 !wifi ? "Wi-Fi 未连接，暂时用不了" : ai ? "已就绪，按住 M 说话" :
                 phone ? "未配置 AI 服务，暂时改用手机" : "未配置 AI 服务：在手机 App 推送配置");
    } else {
        snprintf(buf, sizeof(buf), "语音线路：交给手机识别（蓝牙）\n%s",
                 phone ? "手机已连接，按住 M 说话" : (wifi && ai) ? "手机未连接，暂时改用背夹直连" :
                 "手机未连接：打开手机 App 连接背夹");
    }
    lv_label_set_text(CC.route_l, buf);
    lv_label_set_text(CC.tile_value[0], wifi ? ssid : "未连接");
    lv_label_set_text(CC.tile_value[1], phone ? "已连接，语音可用" : hv_plat_ble_connected() ? "已连接" : "等待连接");
    lv_label_set_text(CC.tile_value[2], hv_plat_vibration_get() ? "开" : "关");
    lv_label_set_text(CC.tile_value[3], ai ? "已配置" : "未配置");
    lv_obj_set_style_bg_color(CC.item[CC_VIBRATE], lv_color_hex(hv_plat_vibration_get() ? 0x3A3F48 : 0x1A1D23), 0);
}

void hv_open_control(void)
{
    lv_obj_t *scr = hv_screen();
    int32_t h = (int32_t)s_ui.height - HV_SOFTKEY_H;

    if (!scr || hv_view() != HV_VIEW_HOME) {
        return;
    }
    hv_control_build(scr);
    hv_control_refresh();
    CC.focus = hv_plat_mode_get() == HV_MODE_DOCK ? CC_MODE_DOCK : CC_MODE_CLIP;
    CC.adjusting = false;
    lv_obj_remove_flag(CC.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(CC.panel);
    extern void hv_status_to_front(void);
    hv_status_to_front();
    hv_cc_slide(-h, 0, false);
    hv_view_set(HV_VIEW_CONTROL);
    hv_cc_focus_apply();
}

void hv_close_control(void)
{
    int32_t h = (int32_t)s_ui.height - HV_SOFTKEY_H;
    if (!CC.panel || lv_obj_has_flag(CC.panel, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    CC.adjusting = false;
    hv_cc_slide(lv_obj_get_y(CC.panel), -h, true);
    hv_view_set(HV_VIEW_HOME);
}

void hv_control_key(hv_key_t key)
{
    if (CC.adjusting && (key == HV_KEY_PREV || key == HV_KEY_NEXT)) {
        int which = CC.focus == CC_VOLUME ? 0 : 1;
        int v = (int)lv_slider_get_value(CC.slider[which]) + (key == HV_KEY_NEXT ? 10 : -10);
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        lv_slider_set_value(CC.slider[which], v, LV_ANIM_ON);
        lv_obj_send_event(CC.slider[which], LV_EVENT_VALUE_CHANGED, NULL);
        return;
    }
    switch (key) {
    case HV_KEY_PREV:
        CC.focus = CC.focus > 0 ? CC.focus - 1 : CC_COUNT - 1;
        hv_cc_focus_apply();
        break;
    case HV_KEY_NEXT:
        CC.focus = (CC.focus + 1) % CC_COUNT;
        hv_cc_focus_apply();
        break;
    case HV_KEY_M:
        hv_cc_activate(CC.focus);
        break;
    case HV_KEY_SET:
    case HV_KEY_SET_LONG:
        if (CC.adjusting) {
            CC.adjusting = false;
            hv_cc_focus_apply();
        } else {
            hv_close_control();
        }
        break;
    default:
        break;
    }
}

int hv_control_dim_level(void)
{
    return CC.dim_level;
}

void hv_control_set_brightness(int percent)
{
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    CC.dim_level = 60 - percent * 60 / 100;
    hv_cc_apply_dim();
    if (CC.slider[1]) {
        char buf[32];
        lv_slider_set_value(CC.slider[1], percent, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "亮度 %d%%", percent);
        lv_label_set_text(CC.slider_l[1], buf);
    }
}
