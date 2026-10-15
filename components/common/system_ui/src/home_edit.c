/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 编辑首页：
 *   触摸：点左上角 − 移出（不是卸载）；点右下角尺寸标签在声明的尺寸里循环；
 *         按住拖动换位置，拖到屏幕左右边缘放下 = 移到上/下一页；
 *         点「＋添加小组件」或底栏空位「＋」添加；「完成」保存。
 *   按键：◀ ▶ 选择；M 拿起 → ◀ ▶ 移动（走到页首/页尾换页）、M 换尺寸、SET 放下；
 *         未拿起时 SET = 完成。放不下时提示“这一页放不下”，不挤掉别的项。
 */
#include "home_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

void hv_view_set(hv_view_t v);
hv_layout_t *hv_layout_mut(void);
int hv_edit_picked(void);
void hv_edit_set_picked(int idx);
int hv_focus_index(void);
void hv_focus_set(int idx, bool visible);
lv_obj_t *hv_slot_obj(int page, int index);
lv_obj_t *hv_screen(void);
void hv_rebuild(void);

typedef struct {
    bool dragging;
    int page, index;
    lv_point_t start;
    int32_t ox, oy;
    bool moved;
} hv_drag_t;
static hv_drag_t *s_dr;
#define DR (*s_dr)

/* ------------------------------------------------------------ 布局操作 */

static bool hv_page_try(hv_layout_t *l, int p)
{
    return hv_layout_pack(l->it[p], l->n[p], NULL);
}

static bool hv_move_within(hv_layout_t *l, int p, int from, int to)
{
    hv_item_t tmp;
    if (from == to || to < 0 || to >= l->n[p]) {
        return false;
    }
    tmp = l->it[p][from];
    if (from < to) {
        memmove(&l->it[p][from], &l->it[p][from + 1], (size_t)(to - from) * sizeof(hv_item_t));
    } else {
        memmove(&l->it[p][to + 1], &l->it[p][to], (size_t)(from - to) * sizeof(hv_item_t));
    }
    l->it[p][to] = tmp;
    return hv_page_try(l, p);
}

/* 把 (p, i) 移到 dst 页末尾；dst 可以是新开的一页。 */
static bool hv_move_to_page(hv_layout_t *l, int p, int i, int dst, bool at_front)
{
    hv_item_t it = l->it[p][i];

    if (dst < 0 || dst >= HV_MAX_PAGES) {
        return false;
    }
    if (dst >= l->pages) {
        if (l->pages >= HV_MAX_PAGES) {
            return false;
        }
        l->n[l->pages] = 0;
        l->pages++;
        dst = l->pages - 1;
    }
    if (l->n[dst] >= HV_MAX_PER_PAGE) {
        return false;
    }
    if (at_front) {
        memmove(&l->it[dst][1], &l->it[dst][0], (size_t)l->n[dst] * sizeof(hv_item_t));
        l->it[dst][0] = it;
    } else {
        l->it[dst][l->n[dst]] = it;
    }
    l->n[dst]++;
    if (!hv_page_try(l, dst)) {
        return false;
    }
    memmove(&l->it[p][i], &l->it[p][i + 1], (size_t)(l->n[p] - i - 1) * sizeof(hv_item_t));
    l->n[p]--;
    /* 空页（不是第 1 页）自动收掉 */
    if (l->n[p] == 0 && p > 0) {
        for (int q = p; q < l->pages - 1; q++) {
            l->n[q] = l->n[q + 1];
            memcpy(l->it[q], l->it[q + 1], sizeof(l->it[q]));
        }
        l->pages--;
    }
    return true;
}

static void hv_commit(hv_layout_t *l, int page)
{
    (void)hv_apply_layout(l, true);
    hv_set_page(page < l->pages ? page : l->pages - 1, true);
}

void hv_edit_remove(int p, int i);
void hv_edit_cycle(int p, int i);

static void hv_remove_item(int p, int i)
{
    hv_layout_t l = *hv_layout();
    const hv_widget_desc_t *d = hv_widgets_find(l.it[p][i].w);

    if (d && d->fixed) {
        hv_toast("「我的」小组件固定在首页，可以换尺寸");
        return;
    }
    memmove(&l.it[p][i], &l.it[p][i + 1], (size_t)(l.n[p] - i - 1) * sizeof(hv_item_t));
    l.n[p]--;
    if (l.n[p] == 0 && p > 0) {
        for (int q = p; q < l.pages - 1; q++) {
            l.n[q] = l.n[q + 1];
            memcpy(l.it[q], l.it[q + 1], sizeof(l.it[q]));
        }
        l.pages--;
    }
    hv_edit_set_picked(-1);
    hv_commit(&l, p);
    hv_toast("已移出首页（应用仍在总览里）");
}

static void hv_cycle_size(int p, int i)
{
    hv_layout_t l = *hv_layout();
    const hv_widget_desc_t *d = hv_widgets_find(l.it[p][i].w);
    uint8_t cur = l.it[p][i].sz;

    if (!d) {
        return;
    }
    for (int step = 1; step <= 3; step++) {
        uint8_t next = (uint8_t)((cur + step) % 3);
        if (!(d->sizes & HV_SZ_BIT(next))) {
            continue;
        }
        if (next == cur) {
            break;
        }
        l.it[p][i].sz = next;
        if (hv_page_try(&l, p)) {
            hv_commit(&l, p);
            return;
        }
        /* 这一页放不下新尺寸：挪到有空位的页（必要时新开一页），而不是让用户自己腾位置 */
        for (int q = 0; q <= HV_MAX_PAGES; q++) {
            hv_layout_t t = l;
            char w[HV_WID_LEN];
            int landed = -1;
            if (q == p || (q >= t.pages && q != t.pages) || q >= HV_MAX_PAGES + 1) {
                continue;
            }
            if (q == t.pages && t.pages >= HV_MAX_PAGES) {
                continue;
            }
            strlcpy(w, t.it[p][i].w, sizeof(w));
            if (hv_move_to_page(&t, p, i, q, false) && hv_layout_find(&t, w, &landed, NULL)) {
                char msg[64];
                hv_commit(&t, landed);
                snprintf(msg, sizeof(msg), "这一页放不下，已换尺寸并移到第 %d 页", landed + 1);
                hv_toast(msg);
                return;
            }
        }
        l.it[p][i].sz = cur;
        hv_toast("首页 3 页都放不下这个尺寸，先移走一个");
        return;
    }
    hv_toast("这个小组件只有一种尺寸");
}

/* ------------------------------------------------------------- 触摸 */

void hv_edit_widget_event(lv_event_t *e, int p, int i)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt;
    lv_obj_t *slot = hv_slot_obj(p, i);

    if (!indev || !slot) {
        return;
    }
    lv_indev_get_point(indev, &pt);
    switch (code) {
    case LV_EVENT_PRESSED: {
        /* 命中左上角 − 或右下角尺寸标签：直接执行 */
        lv_area_t a;
        lv_obj_get_coords(slot, &a);
        int32_t lx = pt.x - a.x1, ly = pt.y - a.y1;
        int32_t sw = lv_obj_get_width(slot), sh = lv_obj_get_height(slot) - HV_LABEL_H;
        DR.moved = false;
        DR.dragging = false;
        if (lx < 44 && ly < 44) {
            hv_remove_item(p, i);
            return;
        }
        if (lx > sw - 52 && ly > sh - 46 && ly < sh) {
            hv_cycle_size(p, i);
            return;
        }
        DR.dragging = true;
        DR.page = p;
        DR.index = i;
        DR.start = pt;
        DR.ox = lv_obj_get_x(slot);
        DR.oy = lv_obj_get_y(slot);
        break;
    }
    case LV_EVENT_PRESSING:
        if (DR.dragging && DR.page == p && DR.index == i) {
            int32_t dx = pt.x - DR.start.x, dy = pt.y - DR.start.y;
            if (!DR.moved && (dx * dx + dy * dy) > 144) {
                DR.moved = true;
                lv_obj_move_foreground(slot);
                lv_obj_set_style_transform_scale(slot, 270, 0);
                lv_obj_set_style_transform_pivot_x(slot, lv_obj_get_width(slot) / 2, 0);
                lv_obj_set_style_transform_pivot_y(slot, lv_obj_get_height(slot) / 2, 0);
            }
            if (DR.moved) {
                lv_obj_set_pos(slot, DR.ox + dx, DR.oy + dy);
            }
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (DR.dragging && DR.moved && DR.page == p && DR.index == i) {
            hv_layout_t l = *hv_layout();
            int32_t w = (int32_t)s_ui.width;
            DR.dragging = false;
            if (pt.x < 28 && p > 0) {
                if (hv_move_to_page(&l, p, i, p - 1, false)) {
                    hv_commit(&l, p - 1);
                } else {
                    hv_toast("上一页放不下");
                    hv_rebuild();
                }
                return;
            }
            if (pt.x > w - 28) {
                if (hv_move_to_page(&l, p, i, p + 1, true)) {
                    hv_commit(&l, p + 1);
                } else {
                    hv_toast(p + 1 >= HV_MAX_PAGES ? "最多 3 页" : "下一页放不下");
                    hv_rebuild();
                }
                return;
            }
            /* 找落点最近的那一项，移动到它的位置 */
            int best = i;
            int32_t best_d = INT32_MAX;
            for (int k = 0; k < l.n[p]; k++) {
                lv_obj_t *o = hv_slot_obj(p, k);
                lv_area_t a;
                if (!o) {
                    continue;
                }
                lv_obj_get_coords(o, &a);
                int32_t cx = k == i ? (DR.ox + HV_CELL_W / 2) : (a.x1 + a.x2) / 2;
                int32_t cy = k == i ? (DR.oy + HV_PAGER_Y + HV_CELL_H / 2) : (a.y1 + a.y2) / 2;
                int32_t dd = (cx - pt.x) * (cx - pt.x) + (cy - pt.y) * (cy - pt.y);
                if (dd < best_d) {
                    best_d = dd;
                    best = k;
                }
            }
            if (best != i && hv_move_within(&l, p, i, best)) {
                hv_commit(&l, p);
            } else {
                if (best != i) {
                    hv_toast("这一页放不下");
                }
                hv_rebuild();
            }
        } else {
            DR.dragging = false;
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------- 按键 */

static void hv_edit_focus_to_item(int p, int i)
{
    hv_set_page(p, true);
    hv_focus_set(1 + i, true);
}

void hv_edit_key(hv_key_t key)
{
    const hv_layout_t *lay = hv_layout();
    int p = hv_page();
    int picked = hv_edit_picked();
    int focus = hv_focus_index();
    int n_items = lay->n[p];
    int n_total = 1 + n_items + (p == 0 ? HV_DOCK_MAX : 0);

    if (picked >= 0) {
        hv_layout_t l = *lay;
        switch (key) {
        case HV_KEY_NEXT:
            if (picked + 1 < l.n[p]) {
                if (hv_move_within(&l, p, picked, picked + 1)) {
                    hv_commit(&l, p);
                    hv_edit_set_picked(picked + 1);
                    hv_edit_focus_to_item(p, picked + 1);
                } else {
                    hv_toast("这一页放不下");
                }
            } else if (hv_move_to_page(&l, p, picked, p + 1, true)) {
                int np = p + 1 < l.pages ? p + 1 : l.pages - 1;
                hv_commit(&l, np);
                hv_edit_set_picked(0);
                hv_edit_focus_to_item(np, 0);
            } else {
                hv_toast(p + 1 >= HV_MAX_PAGES ? "已经是最后一页" : "下一页放不下");
            }
            break;
        case HV_KEY_PREV:
            if (picked > 0) {
                if (hv_move_within(&l, p, picked, picked - 1)) {
                    hv_commit(&l, p);
                    hv_edit_set_picked(picked - 1);
                    hv_edit_focus_to_item(p, picked - 1);
                } else {
                    hv_toast("这一页放不下");
                }
            } else if (p > 0 && hv_move_to_page(&l, p, picked, p - 1, false)) {
                hv_commit(&l, p - 1);
                int idx = hv_layout()->n[p - 1] - 1;
                hv_edit_set_picked(idx);
                hv_edit_focus_to_item(p - 1, idx);
            } else {
                hv_toast(p == 0 ? "已经是第一页" : "上一页放不下");
            }
            break;
        case HV_KEY_M:
            hv_cycle_size(p, picked);
            hv_edit_focus_to_item(p, picked);
            break;
        case HV_KEY_SET:
        case HV_KEY_SET_LONG:
            hv_edit_set_picked(-1);
            (void)hv_layout_save(hv_layout());
            hv_toast("已放下");
            break;
        default:
            break;
        }
        return;
    }

    switch (key) {
    case HV_KEY_PREV:
        if (focus > 0) {
            hv_focus_set(focus - 1, true);
        } else if (p > 0) {
            hv_set_page(p - 1, true);
            hv_focus_set(1 + hv_layout()->n[p - 1] - 1 + (p - 1 == 0 ? HV_DOCK_MAX : 0), true);
        } else {
            hv_focus_set(0, true);
        }
        break;
    case HV_KEY_NEXT:
        if (focus + 1 < n_total) {
            hv_focus_set(focus + 1, true);
        } else if (p + 1 < lay->pages) {
            hv_set_page(p + 1, true);
            hv_focus_set(1, true);
        }
        break;
    case HV_KEY_M:
        if (focus == 0) {
            hv_add_open(false);
        } else if (focus - 1 < n_items) {
            hv_edit_set_picked(focus - 1);
            hv_toast("已拿起：上/下一个键移动，M 换尺寸，SET 放下");
        } else {
            int d = focus - 1 - n_items;
            if (d < lay->dock_n) {
                hv_layout_t l = *lay;
                for (int j = d; j < l.dock_n - 1; j++) {
                    strlcpy(l.dock[j], l.dock[j + 1], sizeof(l.dock[j]));
                }
                l.dock_n--;
                hv_commit(&l, p);
                hv_toast("已从底部应用栏移出");
            } else {
                hv_add_open(true);
            }
        }
        break;
    case HV_KEY_SET:
    case HV_KEY_SET_LONG:
        hv_exit_edit();
        break;
    default:
        break;
    }
}

void hv_edit_decorate(void)
{
}

/* --------------------------------------------------------- 添加面板 */

typedef struct {
    lv_obj_t *scrim;
    lv_obj_t *sheet;
    lv_obj_t *rows[48];
    char keys[48][HV_APP_ID_LEN];
    int count;
    int focus;
    bool dock;
} hv_add_t;
static hv_add_t *s_ad;
#define AD (*s_ad)

void hv_edit_mem_init(void)
{
    if (!s_dr) s_dr = hv_psram_calloc(sizeof(*s_dr));
    if (!s_ad) s_ad = hv_psram_calloc(sizeof(*s_ad));
}

static void hv_add_focus_apply(void)
{
    for (int i = 0; i < AD.count; i++) {
        hv_focus_ring(AD.rows[i], i == AD.focus);
    }
    if (AD.focus < AD.count && AD.rows[AD.focus]) {
        lv_obj_scroll_to_view(AD.rows[AD.focus], LV_ANIM_ON);
    }
}

static void hv_add_pick(int i)
{
    hv_layout_t l = *hv_layout();
    const char *key = AD.keys[i];

    if (AD.dock) {
        if (l.dock_n >= HV_DOCK_MAX) {
            hv_toast("底部应用栏最多 4 个，先移出一个");
            return;
        }
        strlcpy(l.dock[l.dock_n++], key, sizeof(l.dock[0]));
        hv_add_close();
        hv_commit(&l, 0);
        hv_toast("已加入底部应用栏");
        return;
    }
    const hv_widget_desc_t *d = hv_widgets_find(key);
    if (!d) {
        return;
    }
    uint8_t sz = (d->sizes & HV_SZ_BIT(HV_SZ_S)) ? HV_SZ_S : (d->sizes & HV_SZ_BIT(HV_SZ_M)) ? HV_SZ_M : HV_SZ_L;
    /* 先放当前页，再往后找；都放不下且不足 3 页就新开一页 */
    int start = hv_page();
    for (int k = 0; k < HV_MAX_PAGES; k++) {
        int p = (start + k) % HV_MAX_PAGES;
        if (p >= l.pages) {
            if (l.pages >= HV_MAX_PAGES) {
                continue;
            }
            p = l.pages;
            l.n[p] = 0;
            l.pages++;
        }
        if (l.n[p] >= HV_MAX_PER_PAGE) {
            continue;
        }
        strlcpy(l.it[p][l.n[p]].w, key, sizeof(l.it[p][0].w));
        l.it[p][l.n[p]].sz = sz;
        l.n[p]++;
        if (hv_page_try(&l, p)) {
            hv_add_close();
            hv_commit(&l, p);
            hv_toast(p == start ? "已添加到这一页" : "这一页放不下，已放到有空位的页");
            return;
        }
        l.n[p]--;
    }
    hv_toast("首页 3 页都满了，先移出一个");
}

static void hv_add_row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    system_ui_click_feedback();
    hv_add_pick(i);
}

static void hv_add_scrim_cb(lv_event_t *e)
{
    (void)e;
    hv_add_close();
}

void hv_add_open(bool dock)
{
    int32_t w = (int32_t)s_ui.width;
    int32_t h = 560;
    const hv_layout_t *l = hv_layout();
    lv_obj_t *list;
    int32_t y = 0;

    hv_add_close();
    AD.dock = dock;
    AD.count = 0;
    AD.focus = 0;
    AD.scrim = hv_box(hv_screen(), 0, 0, w, (int32_t)s_ui.height - HV_SOFTKEY_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(AD.scrim, LV_OPA_60, 0);
    lv_obj_add_flag(AD.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(AD.scrim, hv_add_scrim_cb, LV_EVENT_CLICKED, NULL);
    AD.sheet = hv_box(AD.scrim, 0, (int32_t)s_ui.height - HV_SOFTKEY_H - h, w, h, 0x111318, 28);
    lv_obj_add_flag(AD.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = hv_text(AD.sheet, g_hvf.f24, HV_C_TEXT, dock ? "加入底部应用栏" : "添加小组件");
    lv_obj_set_pos(t, HV_PAD, 20);
    lv_obj_t *s = hv_text(AD.sheet, g_hvf.f16, HV_C_MUTED,
                          dock ? "最多 4 个，只在第 1 页显示" : "任何应用声明的小组件都能放上首页");
    lv_obj_set_pos(s, HV_PAD, 56);
    list = lv_obj_create(AD.sheet);
    hv_plain(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(list, 0, 90);
    lv_obj_set_size(list, w, h - 100);

    if (dock) {
        for (size_t i = 0; i < hv_catalog_count() && AD.count < 48; i++) {
            const hv_app_t *app = hv_catalog_at(i);
            if (hv_layout_dock_has(l, app->id)) {
                continue;
            }
            lv_obj_t *row = hv_box(list, HV_PAD, y, w - 2 * HV_PAD, 84, 0x1A1D23, 20);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, hv_add_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)AD.count);
            lv_obj_t *icon = hv_app_icon_create(row, app, 52, false);
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(icon, 6, 12);
            lv_obj_t *n = hv_text(row, g_hvf.f20, HV_C_TEXT, app->title);
            lv_obj_set_pos(n, 96, 14);
            lv_obj_t *g = hv_text(row, g_hvf.f16, HV_C_MUTED, hv_group_name(app->group));
            lv_obj_set_pos(g, 96, 46);
            AD.rows[AD.count] = row;
            strlcpy(AD.keys[AD.count], app->id, sizeof(AD.keys[0]));
            AD.count++;
            y += 94;
        }
    } else {
        hv_widgets_rescan();
        for (size_t i = 0; i < hv_widgets_count() && AD.count < 48; i++) {
            const hv_widget_desc_t *d = hv_widgets_at(i);
            char sizes[24] = {0};
            if (hv_layout_find(l, d->id, NULL, NULL)) {
                continue;
            }
            lv_obj_t *row = hv_box(list, HV_PAD, y, w - 2 * HV_PAD, 84, 0x1A1D23, 20);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, hv_add_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)AD.count);
            lv_obj_t *n = hv_text(row, g_hvf.f20, HV_C_TEXT, d->name);
            lv_obj_set_pos(n, 20, 12);
            lv_obj_t *ds = hv_text(row, g_hvf.f16, HV_C_MUTED, d->desc);
            lv_obj_set_width(ds, w - 2 * HV_PAD - 120);
            lv_label_set_long_mode(ds, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_pos(ds, 20, 46);
            for (int sz = 0; sz <= HV_SZ_L; sz++) {
                if (d->sizes & HV_SZ_BIT(sz)) {
                    strlcat(sizes, hv_size_name((uint8_t)sz), sizeof(sizes));
                    strlcat(sizes, " ", sizeof(sizes));
                }
            }
            lv_obj_t *sz_l = hv_text(row, g_hvf.f16, HV_C_ACCENT, sizes);
            lv_obj_align(sz_l, LV_ALIGN_RIGHT_MID, -16, 0);
            if (d->ai) {
                lv_obj_t *badge = hv_box(row, 0, 0, 30, 18, HV_C_ACCENT, 9);
                lv_obj_t *bt = hv_text(badge, &lv_font_montserrat_14, HV_C_ACCENT_INK, "AI");
                lv_obj_center(bt);
                lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -12, 10);
            }
            AD.rows[AD.count] = row;
            strlcpy(AD.keys[AD.count], d->id, sizeof(AD.keys[0]));
            AD.count++;
            y += 94;
        }
    }
    if (AD.count == 0) {
        lv_obj_t *e = hv_text(list, g_hvf.f20, HV_C_MUTED, dock ? "所有应用都已在底部应用栏" : "所有小组件都已在首页");
        lv_obj_set_pos(e, HV_PAD, 20);
    }
    hv_add_focus_apply();
    hv_view_set(HV_VIEW_ADD);
    hv_softkeys_set("上一个", "添加", "下一个", "取消");
}

void hv_add_close(void)
{
    if (AD.scrim) {
        lv_obj_delete(AD.scrim);
        AD.scrim = NULL;
        AD.sheet = NULL;
        AD.count = 0;
        hv_view_set(HV_VIEW_EDIT);
    }
}

void hv_add_key(hv_key_t key)
{
    switch (key) {
    case HV_KEY_PREV:
        if (AD.focus > 0) {
            AD.focus--;
        }
        hv_add_focus_apply();
        break;
    case HV_KEY_NEXT:
        if (AD.focus + 1 < AD.count) {
            AD.focus++;
        }
        hv_add_focus_apply();
        break;
    case HV_KEY_M:
        if (AD.focus < AD.count) {
            hv_add_pick(AD.focus);
        }
        break;
    case HV_KEY_SET:
    case HV_KEY_SET_LONG:
        hv_add_close();
        break;
    default:
        break;
    }
}

void hv_edit_remove(int p, int i)
{
    hv_remove_item(p, i);
}

void hv_edit_cycle(int p, int i)
{
    hv_cycle_size(p, i);
}
