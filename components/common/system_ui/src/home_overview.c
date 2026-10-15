/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 应用总览（底部上滑 / SET）与应用管理菜单（长按图标 / 长按 M）。
 *
 * 总览按“内置应用 / 官方应用 / 我的创作”分组，EPP（Lua）应用带 AI 角标。
 * 菜单：打开、加入/移出底部应用栏、重命名、卸载（二次确认）、详情、取消。
 * 系统原生与固件内置应用不能卸载、不能改名；卸载和改名在 UI 事件任务里执行，
 * 完成后刷新目录、首页与总览，并用提示条说明结果。
 */
#include "home_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

void hv_view_set(hv_view_t v);
hv_layout_t *hv_layout_mut(void);
lv_obj_t *hv_screen(void);

#define HV_OV_COLS 4

typedef struct {
    lv_obj_t *panel;
    lv_obj_t *list;
    lv_obj_t *subtitle;
    lv_obj_t *chips[HV_GROUP_COUNT];
    lv_obj_t *section[HV_GROUP_COUNT];
    lv_obj_t *icons[96];
    char ids[96][HV_APP_ID_LEN];
    int count;
    int focus;
    bool focus_visible;
    bool suppress_click;
} hv_ov_t;

typedef struct {
    lv_obj_t *scrim;
    lv_obj_t *card;
    lv_obj_t *btn[7];
    int action[7];
    int count;
    int focus;
    char id[HV_APP_ID_LEN];
} hv_sh_t;

typedef struct {
    lv_obj_t *scrim;
    lv_obj_t *card;
    lv_obj_t *btn[2];
    lv_obj_t *ta;
    lv_obj_t *kb;
    int focus;
    hv_view_t kind;
    char id[HV_APP_ID_LEN];
} hv_md_t;

static hv_ov_t *s_ov;
static hv_sh_t *s_sh;
static hv_md_t *s_md;
#define OV (*s_ov)
#define SH (*s_sh)
#define MD (*s_md)

void hv_overview_mem_init(void)
{
    if (!s_ov) s_ov = hv_psram_calloc(sizeof(*s_ov));
    if (!s_sh) s_sh = hv_psram_calloc(sizeof(*s_sh));
    if (!s_md) s_md = hv_psram_calloc(sizeof(*s_md));
}

enum { ACT_OPEN = 0, ACT_DOCK_ADD, ACT_DOCK_REMOVE, ACT_RENAME, ACT_UNINSTALL, ACT_INFO, ACT_CANCEL };

static void hv_overview_build_list(void);
static void hv_confirm_open(const char *id);
static void hv_rename_open(const char *id);
static void hv_info_open(const char *id);

void hv_overview_forget(void)
{
    memset(&OV, 0, sizeof(OV));
    memset(&SH, 0, sizeof(SH));
    memset(&MD, 0, sizeof(MD));
}

/* --------------------------------------------------------------- 动画 */

static void hv_anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y(obj, v);
}

static void hv_hide_ready_cb(lv_anim_t *a)
{
    lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void hv_slide(lv_obj_t *obj, int32_t from, int32_t to, bool hide_after)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_exec_cb(&a, hv_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (hide_after) {
        lv_anim_set_completed_cb(&a, hv_hide_ready_cb);
    }
    lv_anim_start(&a);
}

/* ------------------------------------------------------------- 总览 */

static void hv_ov_focus_apply(void)
{
    for (int i = 0; i < OV.count; i++) {
        hv_focus_ring(OV.icons[i], OV.focus_visible && i == OV.focus);
    }
    if (OV.focus_visible && OV.focus < OV.count && OV.icons[OV.focus]) {
        lv_obj_scroll_to_view_recursive(OV.icons[OV.focus], LV_ANIM_ON);
    }
}

static void hv_ov_icon_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    if (i >= OV.count) {
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        OV.suppress_click = false;
    } else if (code == LV_EVENT_LONG_PRESSED) {
        OV.suppress_click = true;
        OV.focus = i;
        hv_sheet_open(OV.ids[i]);
    } else if (code == LV_EVENT_CLICKED && !OV.suppress_click) {
        OV.focus = i;
        OV.focus_visible = false;
        hv_ov_focus_apply();
        char id[HV_APP_ID_LEN];
        strlcpy(id, OV.ids[i], sizeof(id));
        hv_close_overview();
        (void)hv_catalog_launch(id, NULL);
    }
}

static void hv_ov_done_cb(lv_event_t *e)
{
    (void)e;
    system_ui_click_feedback();
    hv_close_overview();
}

static void hv_ov_chip_cb(lv_event_t *e)
{
    int g = (int)(intptr_t)lv_event_get_user_data(e);
    if (OV.section[g]) {
        lv_obj_scroll_to_view(OV.section[g], LV_ANIM_ON);
        lv_obj_scroll_to_y(OV.list, lv_obj_get_y(OV.section[g]) - 8, LV_ANIM_ON);
    }
}

lv_obj_t *hv_overview_build(lv_obj_t *scr)
{
    int32_t w = (int32_t)s_ui.width;
    int32_t h = (int32_t)s_ui.height - HV_STATUS_H - HV_SOFTKEY_H;

    if (OV.panel) {
        return OV.panel;
    }
    OV.panel = hv_box(scr, 0, (int32_t)s_ui.height, w, h, 0x0D0F13, 28);
    lv_obj_add_flag(OV.panel, LV_OBJ_FLAG_CLICKABLE);    /* 挡住下面的首页 */
    lv_obj_set_style_border_width(OV.panel, 1, 0);
    lv_obj_set_style_border_color(OV.panel, lv_color_hex(0x262A31), 0);
    lv_obj_set_style_border_side(OV.panel, LV_BORDER_SIDE_TOP, 0);
    hv_box(OV.panel, (w - 44) / 2, 8, 44, 5, 0x3A3F48, 3);

    lv_obj_t *title = hv_text(OV.panel, g_hvf.f28, HV_C_TEXT, "应用总览");
    lv_obj_set_pos(title, HV_PAD, 26);
    OV.subtitle = hv_text(OV.panel, g_hvf.f16, HV_C_MUTED, "");
    lv_obj_set_pos(OV.subtitle, HV_PAD, 66);
    lv_obj_t *done = hv_box(OV.panel, w - HV_PAD - 76, 30, 76, 38, 0x262A31, 19);
    lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(done, hv_ov_done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_l = hv_text(done, g_hvf.f20, HV_C_TEXT, "完成");
    lv_obj_center(done_l);

    /* 角标说明 */
    lv_obj_t *legend = hv_box(OV.panel, HV_PAD, 96, w - 2 * HV_PAD, 26, 0, 0);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, 0);
    lv_obj_t *ai = hv_box(legend, 0, 2, 30, 20, HV_C_ACCENT, 10);
    lv_obj_t *ai_t = hv_text(ai, &lv_font_montserrat_14, HV_C_ACCENT_INK, "AI");
    lv_obj_center(ai_t);
    lv_obj_t *lt = hv_text(legend, g_hvf.f16, HV_C_MUTED, "EPP 应用（Lua，可 AI 生成）   无角标为系统原生");
    lv_obj_set_pos(lt, 38, 2);

    /* 分组标签 */
    for (int g = 0; g < HV_GROUP_COUNT; g++) {
        OV.chips[g] = hv_box(OV.panel, HV_PAD + g * 140, 130, 132, 34, g == 0 ? 0x2A2410 : 0x1B1E24, 17);
        lv_obj_add_flag(OV.chips[g], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(OV.chips[g], hv_ov_chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)g);
        lv_obj_t *t = hv_text(OV.chips[g], g_hvf.f16, g == 0 ? HV_C_ACCENT : HV_C_TEXT, "");
        lv_obj_center(t);
    }

    OV.list = lv_obj_create(OV.panel);
    hv_plain(OV.list);
    lv_obj_add_flag(OV.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(OV.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(OV.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(OV.list, 0, 176);
    lv_obj_set_size(OV.list, w, h - 176);
    lv_obj_add_flag(OV.panel, LV_OBJ_FLAG_HIDDEN);
    return OV.panel;
}

static void hv_overview_build_list(void)
{
    int32_t w = (int32_t)s_ui.width;
    int32_t cell_w = (w - 2 * 12) / HV_OV_COLS;
    int32_t y = 0;
    char buf[64];

    lv_obj_clean(OV.list);
    memset(OV.section, 0, sizeof(OV.section));
    OV.count = 0;
    snprintf(buf, sizeof(buf), "%u 个应用 · 3 个分组", (unsigned)hv_catalog_count());
    /* “·” 不在设备字库里：用空格分隔 */
    snprintf(buf, sizeof(buf), "%u 个应用   3 个分组", (unsigned)hv_catalog_count());
    lv_label_set_text(OV.subtitle, buf);
    for (int g = 0; g < HV_GROUP_COUNT; g++) {
        int col = 0;
        size_t n = hv_catalog_group_count((hv_group_t)g);
        static const char *const notes[HV_GROUP_COUNT] = {"系统原生，不可卸载", "官方商店 EPP", "Vibe Coding 生成"};

        snprintf(buf, sizeof(buf), "%s %u", hv_group_name((hv_group_t)g), (unsigned)n);
        lv_label_set_text(lv_obj_get_child(OV.chips[g], 0), buf);
        if (n == 0 && g != HV_GROUP_MINE) {
            continue;
        }
        OV.section[g] = hv_box(OV.list, 0, y, w, 36, 0, 0);
        lv_obj_set_style_bg_opa(OV.section[g], LV_OPA_TRANSP, 0);
        lv_obj_t *t = hv_text(OV.section[g], g_hvf.f20, HV_C_TEXT, hv_group_name((hv_group_t)g));
        lv_obj_align(t, LV_ALIGN_LEFT_MID, HV_PAD, 0);
        lv_obj_t *note = hv_text(OV.section[g], g_hvf.f16, HV_C_FAINT, notes[g]);
        lv_obj_align(note, LV_ALIGN_RIGHT_MID, -HV_PAD, 0);
        y += 44;
        if (n == 0) {
            lv_obj_t *empty = hv_text(OV.list, g_hvf.f16, HV_C_FAINT,
                                      "还没有自己的应用：在手机 App 里一句话生成，安装后出现在这里");
            lv_obj_set_width(empty, w - 2 * HV_PAD);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
            lv_obj_set_pos(empty, HV_PAD, y);
            y += 64;
            continue;
        }
        for (size_t i = 0; i < hv_catalog_count() && OV.count < 96; i++) {
            const hv_app_t *app = hv_catalog_at(i);
            lv_obj_t *icon;

            if (app->group != (hv_group_t)g) {
                continue;
            }
            icon = hv_app_icon_create(OV.list, app, HV_ICON, true);
            lv_obj_set_pos(icon, 12 + col * cell_w + (cell_w - (HV_ICON + 32)) / 2, y);
            lv_obj_add_event_cb(icon, hv_ov_icon_cb, LV_EVENT_ALL, (void *)(intptr_t)OV.count);
            if (hv_layout_dock_has(hv_layout(), app->id)) {
                lv_obj_t *dot = hv_box(icon, 0, 0, 8, 8, HV_C_ACCENT, 4);
                lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, 0);
            }
            OV.icons[OV.count] = icon;
            strlcpy(OV.ids[OV.count], app->id, sizeof(OV.ids[OV.count]));
            OV.count++;
            if (++col == HV_OV_COLS) {
                col = 0;
                y += HV_ICON + 50;
            }
        }
        if (col) {
            y += HV_ICON + 50;
        }
        y += 8;
    }
    /* 底部留白，最后一行能滚到软键栏上方 */
    hv_box(OV.list, 0, y, 10, 40, 0, 0);
    if (OV.focus >= OV.count) {
        OV.focus = OV.count ? OV.count - 1 : 0;
    }
    hv_ov_focus_apply();
}

void hv_overview_refresh(void)
{
    if (OV.panel && !lv_obj_has_flag(OV.panel, LV_OBJ_FLAG_HIDDEN)) {
        hv_overview_build_list();
    }
}

void hv_open_overview(void)
{
    lv_obj_t *scr = hv_screen();
    if (!scr) {
        return;
    }
    if (hv_view() != HV_VIEW_HOME) {
        return;
    }
    hv_overview_build(scr);
    hv_catalog_refresh();
    OV.focus = 0;
    OV.focus_visible = false;
    hv_overview_build_list();
    lv_obj_remove_flag(OV.panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(OV.panel);
    lv_obj_scroll_to_y(OV.list, 0, LV_ANIM_OFF);
    hv_slide(OV.panel, (int32_t)s_ui.height, HV_STATUS_H, false);
    hv_view_set(HV_VIEW_OVERVIEW);
    hv_softkeys_set("上一个", "打开", "下一个", "收起");
}

void hv_close_overview(void)
{
    if (!OV.panel || lv_obj_has_flag(OV.panel, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    hv_slide(OV.panel, lv_obj_get_y(OV.panel), (int32_t)s_ui.height, true);
    hv_view_set(HV_VIEW_HOME);
}

void hv_overview_key(hv_key_t key)
{
    switch (key) {
    case HV_KEY_PREV:
    case HV_KEY_NEXT:
        if (!OV.focus_visible) {
            OV.focus_visible = true;
        } else if (key == HV_KEY_NEXT && OV.focus + 1 < OV.count) {
            OV.focus++;
        } else if (key == HV_KEY_PREV && OV.focus > 0) {
            OV.focus--;
        }
        hv_ov_focus_apply();
        break;
    case HV_KEY_M:
        if (!OV.focus_visible) {
            OV.focus_visible = true;
            hv_ov_focus_apply();
        } else if (OV.focus < OV.count) {
            char id[HV_APP_ID_LEN];
            strlcpy(id, OV.ids[OV.focus], sizeof(id));
            hv_close_overview();
            (void)hv_catalog_launch(id, NULL);
        }
        break;
    case HV_KEY_M_LONG_START:
        if (OV.focus < OV.count) {
            OV.focus_visible = true;
            hv_ov_focus_apply();
            hv_sheet_open(OV.ids[OV.focus]);
        }
        break;
    case HV_KEY_SET:
    case HV_KEY_SET_LONG:
        hv_close_overview();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- 菜单 */

static void hv_sheet_do(int action)
{
    char id[HV_APP_ID_LEN];
    const hv_app_t *app;

    strlcpy(id, SH.id, sizeof(id));
    app = hv_catalog_find(id);
    hv_sheet_close();
    if (!app) {
        hv_toast("找不到这个应用");
        return;
    }
    switch (action) {
    case ACT_OPEN:
        hv_close_overview();
        (void)hv_catalog_launch(id, NULL);
        break;
    case ACT_DOCK_ADD: {
        hv_layout_t l = *hv_layout();
        if (l.dock_n >= HV_DOCK_MAX) {
            hv_toast("底部应用栏最多 4 个，先移出一个");
            break;
        }
        strlcpy(l.dock[l.dock_n++], id, sizeof(l.dock[0]));
        (void)hv_apply_layout(&l, true);
        hv_overview_refresh();
        hv_toast("已加入底部应用栏");
        break;
    }
    case ACT_DOCK_REMOVE: {
        hv_layout_t l = *hv_layout();
        int k = 0;
        for (int i = 0; i < l.dock_n; i++) {
            if (strcmp(l.dock[i], id) != 0) {
                strlcpy(l.dock[k++], l.dock[i], sizeof(l.dock[0]));
            }
        }
        l.dock_n = (uint8_t)k;
        (void)hv_apply_layout(&l, true);
        hv_overview_refresh();
        hv_toast("已从底部应用栏移出");
        break;
    }
    case ACT_RENAME:
        hv_rename_open(id);
        break;
    case ACT_UNINSTALL:
        hv_confirm_open(id);
        break;
    case ACT_INFO:
        hv_info_open(id);
        break;
    default:
        break;
    }
}

static void hv_sheet_btn_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    system_ui_click_feedback();
    if (i < SH.count) {
        hv_sheet_do(SH.action[i]);
    }
}

static void hv_scrim_cb(lv_event_t *e)
{
    (void)e;
    if (hv_view() == HV_VIEW_SHEET) {
        hv_sheet_close();
    } else if (hv_modal_open() && MD.kind != HV_VIEW_RENAME) {
        hv_modal_close();
    }
}

static lv_obj_t *hv_scrim(void)
{
    lv_obj_t *s = hv_box(hv_screen(), 0, 0, (int32_t)s_ui.width, (int32_t)s_ui.height - HV_SOFTKEY_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_60, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s, hv_scrim_cb, LV_EVENT_CLICKED, NULL);
    return s;
}

static void hv_sheet_focus_apply(void)
{
    for (int i = 0; i < SH.count; i++) {
        lv_obj_set_style_bg_color(SH.btn[i], lv_color_hex(i == SH.focus ? 0x2E323B : 0x1B1E24), 0);
        hv_focus_ring(SH.btn[i], i == SH.focus);
    }
}

void hv_sheet_open(const char *app_id)
{
    const hv_app_t *app = hv_catalog_find(app_id);
    int32_t w = (int32_t)s_ui.width;
    static const char *const names[] = {"打开", "加入底部应用栏", "移出底部应用栏", "重命名", "卸载", "详情", "取消"};
    char sub[96];

    if (!app || !hv_screen()) {
        return;
    }
    hv_sheet_close();
    system_ui_click_feedback();
    strlcpy(SH.id, app->id, sizeof(SH.id));
    SH.count = 0;
    SH.action[SH.count++] = ACT_OPEN;
    SH.action[SH.count++] = hv_layout_dock_has(hv_layout(), app->id) ? ACT_DOCK_REMOVE : ACT_DOCK_ADD;
    if (app->renamable) {
        SH.action[SH.count++] = ACT_RENAME;
    }
    if (app->removable) {
        SH.action[SH.count++] = ACT_UNINSTALL;
    }
    SH.action[SH.count++] = ACT_INFO;
    SH.action[SH.count++] = ACT_CANCEL;

    SH.scrim = hv_scrim();
    int32_t card_h = 104 + SH.count * 62;
    SH.card = hv_box(SH.scrim, 12, (int32_t)s_ui.height - HV_SOFTKEY_H - card_h - 12, w - 24, card_h, 0x15171C, 26);
    lv_obj_add_flag(SH.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *icon = hv_app_icon_create(SH.card, app, 56, false);
    lv_obj_set_pos(icon, 14, 16);
    lv_obj_t *t = hv_text(SH.card, g_hvf.f24, HV_C_TEXT, app->title);
    lv_obj_set_pos(t, 104, 22);
    snprintf(sub, sizeof(sub), "%s   %s", hv_group_name(app->group),
             app->removable ? "可卸载" : "系统应用，不可卸载");
    lv_obj_t *s = hv_text(SH.card, g_hvf.f16, HV_C_MUTED, sub);
    lv_obj_set_pos(s, 104, 56);
    for (int i = 0; i < SH.count; i++) {
        int act = SH.action[i];
        lv_obj_t *b = hv_box(SH.card, 14, 96 + i * 62, w - 52, 54, 0x1B1E24, 16);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, hv_sheet_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *bl = hv_text(b, g_hvf.f20, act == ACT_UNINSTALL ? HV_C_RED : (act == ACT_CANCEL ? HV_C_MUTED : HV_C_TEXT),
                               names[act]);
        lv_obj_align(bl, LV_ALIGN_LEFT_MID, 20, 0);
        SH.btn[i] = b;
    }
    SH.focus = 0;
    hv_sheet_focus_apply();
    hv_view_set(HV_VIEW_SHEET);
    hv_softkeys_set("上一项", "确认", "下一项", "取消");
    ESP_LOGI(SYSTEM_UI_TAG, "HV_SHEET %s actions=%d", app->id, SH.count);
}

void hv_sheet_close(void)
{
    bool was_open = SH.scrim != NULL;
    if (SH.scrim) {
        lv_obj_delete(SH.scrim);
    }
    SH.scrim = NULL;
    SH.card = NULL;
    SH.count = 0;
    if (was_open && hv_view() == HV_VIEW_SHEET) {
        bool overview = OV.panel && !lv_obj_has_flag(OV.panel, LV_OBJ_FLAG_HIDDEN);
        hv_view_set(overview ? HV_VIEW_OVERVIEW : HV_VIEW_HOME);
        if (overview) {
            hv_softkeys_set("上一个", "打开", "下一个", "收起");
        }
    }
}

void hv_sheet_key(hv_key_t key)
{
    switch (key) {
    case HV_KEY_PREV:
        SH.focus = SH.focus > 0 ? SH.focus - 1 : SH.count - 1;
        hv_sheet_focus_apply();
        break;
    case HV_KEY_NEXT:
        SH.focus = (SH.focus + 1) % (SH.count ? SH.count : 1);
        hv_sheet_focus_apply();
        break;
    case HV_KEY_M:
        if (SH.focus < SH.count) {
            hv_sheet_do(SH.action[SH.focus]);
        }
        break;
    case HV_KEY_SET:
    case HV_KEY_SET_LONG:
        hv_sheet_close();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ 对话框 */

bool hv_modal_open(void)
{
    return MD.scrim != NULL;
}

void hv_modal_close(void)
{
    if (MD.scrim) {
        lv_obj_delete(MD.scrim);
    }
    memset(&MD, 0, sizeof(MD));
    bool overview = OV.panel && !lv_obj_has_flag(OV.panel, LV_OBJ_FLAG_HIDDEN);
    hv_view_set(overview ? HV_VIEW_OVERVIEW : HV_VIEW_HOME);
    if (overview) {
        hv_softkeys_set("上一个", "打开", "下一个", "收起");
    }
}

static void hv_md_focus_apply(void)
{
    for (int i = 0; i < 2; i++) {
        if (MD.btn[i]) {
            hv_focus_ring(MD.btn[i], i == MD.focus);
        }
    }
}

static void hv_md_confirm(void)
{
    char id[HV_APP_ID_LEN];
    hv_view_t kind = MD.kind;

    strlcpy(id, MD.id, sizeof(id));
    if (kind == HV_VIEW_CONFIRM) {
        hv_modal_close();
        if (hv_catalog_request_uninstall(id) == ESP_OK) {
            hv_toast("正在卸载…");
        } else {
            hv_toast("这个应用不能卸载");
        }
    } else if (kind == HV_VIEW_RENAME) {
        const char *text = MD.ta ? lv_textarea_get_text(MD.ta) : "";
        char title[96];
        strlcpy(title, text, sizeof(title));
        /* 去掉首尾空格 */
        char *p = title;
        while (*p == ' ') {
            p++;
        }
        size_t n = strlen(p);
        while (n && p[n - 1] == ' ') {
            p[--n] = 0;
        }
        if (!*p) {
            hv_toast("名称不能为空");
            return;
        }
        hv_modal_close();
        if (hv_catalog_request_rename(id, p) == ESP_OK) {
            hv_toast("正在保存名称…");
        } else {
            hv_toast("这个应用不能改名");
        }
    } else {
        hv_modal_close();
    }
}

static void hv_md_btn_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    system_ui_click_feedback();
    if (i == 1) {
        hv_md_confirm();
    } else {
        hv_modal_close();
    }
}

static lv_obj_t *hv_md_card(int32_t h)
{
    int32_t w = (int32_t)s_ui.width;
    MD.scrim = hv_scrim();
    MD.card = hv_box(MD.scrim, 24, ((int32_t)s_ui.height - HV_SOFTKEY_H - h) / 2, w - 48, h, 0x17191F, 26);
    lv_obj_add_flag(MD.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(MD.card, 1, 0);
    lv_obj_set_style_border_color(MD.card, lv_color_hex(0x2C3038), 0);
    return MD.card;
}

static void hv_md_buttons(int32_t y, const char *cancel, const char *ok, uint32_t ok_color, uint32_t ok_text)
{
    int32_t w = (int32_t)s_ui.width - 48;
    const char *names[2] = {cancel, ok};
    for (int i = 0; i < 2; i++) {
        if (!names[i]) {
            continue;
        }
        bool single = !names[0];
        lv_obj_t *b = hv_box(MD.card, single ? 20 : 20 + i * ((w - 52) / 2 + 12), y,
                             single ? w - 40 : (w - 52) / 2, 54,
                             i == 1 ? ok_color : 0x262A31, 18);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, hv_md_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *t = hv_text(b, g_hvf.f20, i == 1 ? ok_text : HV_C_TEXT, names[i]);
        lv_obj_center(t);
        MD.btn[i] = b;
    }
    MD.focus = names[0] ? 0 : 1;
    hv_md_focus_apply();
}

static void hv_confirm_open(const char *id)
{
    const hv_app_t *app = hv_catalog_find(id);
    char text[160];

    if (!app) {
        return;
    }
    hv_md_card(260);
    MD.kind = HV_VIEW_CONFIRM;
    strlcpy(MD.id, id, sizeof(MD.id));
    snprintf(text, sizeof(text), "卸载「%s」？", app->title);
    lv_obj_t *t = hv_text(MD.card, g_hvf.f24, HV_C_TEXT, text);
    lv_obj_set_width(t, (int32_t)s_ui.width - 88);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(t, 20, 26);
    lv_obj_t *s = hv_text(MD.card, g_hvf.f16, HV_C_MUTED,
                          "会删除这个应用和它保存的数据，无法撤销。\n也可以只把它移出首页底部应用栏。");
    lv_obj_set_width(s, (int32_t)s_ui.width - 88);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s, 20, 76);
    hv_md_buttons(180, "取消", "卸载", HV_C_RED, 0xFFFFFF);
    hv_view_set(HV_VIEW_CONFIRM);
    hv_softkeys_set("取消", "确认", "卸载", "返回");
}

static void hv_rename_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        hv_md_confirm();
    } else if (code == LV_EVENT_CANCEL) {
        hv_modal_close();
    }
}

static void hv_rename_open(const char *id)
{
    const hv_app_t *app = hv_catalog_find(id);
    int32_t w = (int32_t)s_ui.width;

    if (!app) {
        return;
    }
    MD.scrim = hv_box(hv_screen(), 0, 0, w, (int32_t)s_ui.height - HV_SOFTKEY_H, 0x0B0C0F, 0);
    lv_obj_add_flag(MD.scrim, LV_OBJ_FLAG_CLICKABLE);
    MD.card = MD.scrim;
    MD.kind = HV_VIEW_RENAME;
    strlcpy(MD.id, id, sizeof(MD.id));
    lv_obj_t *t = hv_text(MD.card, g_hvf.f24, HV_C_TEXT, "重命名");
    lv_obj_set_pos(t, HV_PAD, HV_STATUS_H + 12);
    lv_obj_t *s = hv_text(MD.card, g_hvf.f16, HV_C_MUTED,
                          "背夹键盘只能输入英文和数字；中文名称可在手机 App 的应用管理里修改。");
    lv_obj_set_width(s, w - 2 * HV_PAD);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s, HV_PAD, HV_STATUS_H + 52);
    MD.ta = lv_textarea_create(MD.card);
    lv_textarea_set_one_line(MD.ta, true);
    lv_textarea_set_max_length(MD.ta, 40);
    lv_textarea_set_text(MD.ta, app->title);
    lv_obj_set_size(MD.ta, w - 2 * HV_PAD, 56);
    lv_obj_set_pos(MD.ta, HV_PAD, HV_STATUS_H + 110);
    lv_obj_set_style_text_font(MD.ta, g_hvf.f24, 0);
    lv_obj_set_style_bg_color(MD.ta, lv_color_hex(0x1B1E24), 0);
    lv_obj_set_style_text_color(MD.ta, lv_color_hex(HV_C_TEXT), 0);
    lv_obj_set_style_border_color(MD.ta, lv_color_hex(HV_C_ACCENT), 0);
    lv_obj_set_style_radius(MD.ta, 14, 0);
    MD.kb = lv_keyboard_create(MD.card);
    lv_obj_set_size(MD.kb, w, 300);
    lv_obj_align(MD.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(MD.kb, MD.ta);
    lv_obj_add_event_cb(MD.kb, hv_rename_kb_cb, LV_EVENT_ALL, NULL);
    hv_md_buttons(HV_STATUS_H + 180, "取消", "保存", HV_C_ACCENT, HV_C_ACCENT_INK);
    hv_view_set(HV_VIEW_RENAME);
    hv_softkeys_set("取消", "保存", "", "返回");
}

static void hv_info_open(const char *id)
{
    const hv_app_t *app = hv_catalog_find(id);
    char text[512];

    if (!app) {
        return;
    }
    hv_md_card(360);
    MD.kind = HV_VIEW_INFO;
    strlcpy(MD.id, id, sizeof(MD.id));
    lv_obj_t *t = hv_text(MD.card, g_hvf.f24, HV_C_TEXT, app->title);
    lv_obj_set_pos(t, 20, 22);
    snprintf(text, sizeof(text),
             "应用 ID：%s\n类型：%s\n分组：%s\nAI 角标：%s\n底部应用栏：%s\n入口：%s",
             app->id,
             app->kind == HV_KIND_SYSTEM ? "系统原生页面" :
             app->kind == HV_KIND_BUILTIN_LUA ? "固件内置 Lua" :
             app->kind == HV_KIND_SKILL ? "已安装应用（旧格式）" :
             app->kind == HV_KIND_PACKAGE ? "应用包" : "我的花园（Lua）",
             hv_group_name(app->group), app->ai ? "有（EPP）" : "无",
             hv_layout_dock_has(hv_layout(), app->id) ? "在" : "不在",
             app->script[0] ? app->script : "原生页面");
    lv_obj_t *s = hv_text(MD.card, g_hvf.f16, HV_C_MUTED, text);
    lv_obj_set_width(s, (int32_t)s_ui.width - 88);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(s, 20, 70);
    hv_md_buttons(282, NULL, "关闭", 0x262A31, HV_C_TEXT);
    hv_view_set(HV_VIEW_INFO);
    hv_softkeys_set("", "关闭", "", "返回");
}

void hv_modal_key(hv_key_t key)
{
    switch (key) {
    case HV_KEY_PREV:
        if (MD.kind == HV_VIEW_RENAME) {
            hv_modal_close();
            return;
        }
        if (MD.btn[0]) {
            MD.focus = 0;
        }
        hv_md_focus_apply();
        break;
    case HV_KEY_NEXT:
        if (MD.btn[1]) {
            MD.focus = 1;
        }
        hv_md_focus_apply();
        break;
    case HV_KEY_M:
        if (MD.kind == HV_VIEW_RENAME || MD.focus == 1) {
            hv_md_confirm();
        } else {
            hv_modal_close();
        }
        break;
    case HV_KEY_SET:
    case HV_KEY_SET_LONG:
        hv_modal_close();
        break;
    default:
        break;
    }
}
