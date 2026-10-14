/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Long-press menu for the home carousel: 卸载 / 详细 / 取消.
 *
 * The screen above the footer is dimmed and a panel lists the three rows below
 * the selected icon; the caller lifts its own copy of that icon above the scrim
 * so the user can see which app the menu is about. The footer is left live: its
 * PREV / NEXT hit targets and the matching physical keys move the focus, M picks
 * the focused row. 取消 starts focused so a stray M press never uninstalls.
 *
 * This file only draws and reports the choice. What uninstalling or showing the
 * details means is decided by whoever receives the done callback.
 */

#include "ui_page_menu.h"

#include <stddef.h>

#define UI_PAGE_MENU_SCRIM_COLOR    0x000000
#define UI_PAGE_MENU_SCRIM_OPA      200
#define UI_PAGE_MENU_PANEL_COLOR    0x080C08
#define UI_PAGE_MENU_LINE_COLOR     0x384038
#define UI_PAGE_MENU_FOCUS_BG       0x303430
#define UI_PAGE_MENU_ACCENT         0xF0C430
#define UI_PAGE_MENU_TEXT_COLOR     0xFAFBF5
#define UI_PAGE_MENU_DANGER_COLOR   0xFF5A47

#define UI_PAGE_MENU_PANEL_W        336
#define UI_PAGE_MENU_PANEL_RADIUS   14
#define UI_PAGE_MENU_PANEL_BORDER   2
#define UI_PAGE_MENU_PANEL_PAD      6
#define UI_PAGE_MENU_ROW_W          (UI_PAGE_MENU_PANEL_W - 2 * (UI_PAGE_MENU_PANEL_BORDER + UI_PAGE_MENU_PANEL_PAD))
#define UI_PAGE_MENU_ROW_H          60
#define UI_PAGE_MENU_ROW_RADIUS     10
#define UI_PAGE_MENU_GROUP_GAP      9       /* 取消 sits apart from the actions */
#define UI_PAGE_MENU_DIVIDER_INSET  16
#define UI_PAGE_MENU_BAR_W          4
#define UI_PAGE_MENU_BAR_H          28
#define UI_PAGE_MENU_ICON_X         22
#define UI_PAGE_MENU_ICON_SIZE      24
#define UI_PAGE_MENU_TEXT_X         62
#define UI_PAGE_MENU_PANEL_H        (2 * (UI_PAGE_MENU_PANEL_BORDER + UI_PAGE_MENU_PANEL_PAD) + \
                                     UI_PAGE_MENU_ITEM_COUNT * UI_PAGE_MENU_ROW_H + UI_PAGE_MENU_GROUP_GAP)

typedef struct {
    const char *caption;
    bool        danger;     /* drawn in red whether focused or not */
    void      (*draw_icon)(lv_obj_t *box);
} ui_page_menu_row_desc_t;

static struct {
    lv_obj_t              *layer;
    lv_obj_t              *rows[UI_PAGE_MENU_ITEM_COUNT];
    ui_page_menu_item_t    focus;
    ui_page_menu_done_cb_t done_cb;
} s_menu;

static const lv_font_t *s_font;

/* ------------------------------------------------------------------- icons */

/* Icons are drawn from primitives in a 24x24 box: the built-in symbol font only
 * ships at 14 px. Rect parts take their colour from bg (filled) or border
 * (outlined), lines from line_color; ui_page_menu_tint() sets all three. */
static lv_obj_t *ui_page_menu_part(lv_obj_t *box, int32_t x, int32_t y, int32_t w, int32_t h,
                                   int32_t outline, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(box);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    if (outline) {
        lv_obj_set_style_border_width(obj, outline, LV_PART_MAIN);
        lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    }
    return obj;
}

static void ui_page_menu_line(lv_obj_t *box, const lv_point_precise_t *points)
{
    lv_obj_t *line = lv_line_create(box);
    lv_line_set_points(line, points, 2);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(line, 3, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
}

static void ui_page_menu_icon_trash(lv_obj_t *box)
{
    ui_page_menu_part(box, 8, 0, 8, 3, 0, 1);           /* handle      */
    ui_page_menu_part(box, 2, 3, 20, 3, 0, 1);          /* lid         */
    ui_page_menu_part(box, 5, 8, 14, 16, 2, 2);         /* can         */
    ui_page_menu_part(box, 9, 12, 2, 8, 0, 1);          /* ribs        */
    ui_page_menu_part(box, 13, 12, 2, 8, 0, 1);
}

static void ui_page_menu_icon_info(lv_obj_t *box)
{
    ui_page_menu_part(box, 1, 1, 22, 22, 2, LV_RADIUS_CIRCLE);
    ui_page_menu_part(box, 11, 6, 3, 3, 0, 1);          /* dot of the i */
    ui_page_menu_part(box, 11, 10, 3, 8, 0, 1);         /* stem         */
}

static void ui_page_menu_icon_close(lv_obj_t *box)
{
    /* lv_line keeps the pointer, so the points must outlive the widget. */
    static const lv_point_precise_t down[] = { { 5, 5 }, { 19, 19 } };
    static const lv_point_precise_t up[]   = { { 19, 5 }, { 5, 19 } };

    ui_page_menu_line(box, down);
    ui_page_menu_line(box, up);
}

static const ui_page_menu_row_desc_t s_rows[UI_PAGE_MENU_ITEM_COUNT] = {
    [UI_PAGE_MENU_UNINSTALL] = { "卸载", true,  ui_page_menu_icon_trash },
    [UI_PAGE_MENU_DETAIL]    = { "详细", false, ui_page_menu_icon_info  },
    [UI_PAGE_MENU_CANCEL]    = { "取消", false, ui_page_menu_icon_close },
};

static void ui_page_menu_tint(lv_obj_t *box, lv_color_t color)
{
    uint32_t n = lv_obj_get_child_count(box);

    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *part = lv_obj_get_child(box, (int32_t)i);
        lv_obj_set_style_bg_color(part, color, LV_PART_MAIN);
        lv_obj_set_style_border_color(part, color, LV_PART_MAIN);
        lv_obj_set_style_line_color(part, color, LV_PART_MAIN);
    }
}

/* -------------------------------------------------------------------- focus */

/* Row children, in creation order. */
enum { ROW_CHILD_BAR, ROW_CHILD_ICON, ROW_CHILD_TEXT };

static void ui_page_menu_apply_focus(void)
{
    for (int i = 0; i < UI_PAGE_MENU_ITEM_COUNT; i++) {
        lv_obj_t *row = s_menu.rows[i];
        bool focused = (i == (int)s_menu.focus);
        uint32_t text = s_rows[i].danger ? UI_PAGE_MENU_DANGER_COLOR
                        : focused ? UI_PAGE_MENU_ACCENT : UI_PAGE_MENU_TEXT_COLOR;

        lv_obj_set_style_bg_opa(row, focused ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        if (focused) {
            lv_obj_clear_flag(lv_obj_get_child(row, ROW_CHILD_BAR), LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lv_obj_get_child(row, ROW_CHILD_BAR), LV_OBJ_FLAG_HIDDEN);
        }
        ui_page_menu_tint(lv_obj_get_child(row, ROW_CHILD_ICON), lv_color_hex(text));
        lv_obj_set_style_text_color(lv_obj_get_child(row, ROW_CHILD_TEXT), lv_color_hex(text),
                                    LV_PART_MAIN);
    }
}

/* Tear the menu down, then report. The layer may be deleted from inside one of
 * its own children's event callbacks, as the infrared remote's pickers do. */
static void ui_page_menu_finish(ui_page_menu_item_t item)
{
    ui_page_menu_done_cb_t done_cb = s_menu.done_cb;

    if (s_menu.layer == NULL) {
        return;
    }
    lv_obj_del(s_menu.layer);
    lv_memzero(&s_menu, sizeof(s_menu));
    if (done_cb != NULL) {
        done_cb(item);
    }
}

/* ------------------------------------------------------------------- events */

static void ui_page_menu_scrim_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_page_menu_finish(UI_PAGE_MENU_CANCEL);
    }
}

static void ui_page_menu_row_cb(lv_event_t *e)
{
    ui_page_menu_item_t item = (ui_page_menu_item_t)(intptr_t)lv_event_get_user_data(e);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:      /* the highlight follows the finger */
        s_menu.focus = item;
        ui_page_menu_apply_focus();
        break;
    case LV_EVENT_CLICKED:
        ui_page_menu_finish(item);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ widgets */

static lv_obj_t *ui_page_menu_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *ui_page_menu_row(lv_obj_t *panel, ui_page_menu_item_t item, int32_t y)
{
    const ui_page_menu_row_desc_t *desc = &s_rows[item];
    lv_obj_t *row = ui_page_menu_box(panel, UI_PAGE_MENU_ROW_W, UI_PAGE_MENU_ROW_H);
    lv_obj_t *bar;
    lv_obj_t *icon;
    lv_obj_t *text;

    lv_obj_set_pos(row, 0, y);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(row, UI_PAGE_MENU_ROW_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_PAGE_MENU_FOCUS_BG), LV_PART_MAIN);
    lv_obj_add_event_cb(row, ui_page_menu_row_cb, LV_EVENT_PRESSED, (void *)(intptr_t)item);
    lv_obj_add_event_cb(row, ui_page_menu_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)item);

    /* Same yellow marker as the status bar's left edge. */
    bar = ui_page_menu_box(row, UI_PAGE_MENU_BAR_W, UI_PAGE_MENU_BAR_H);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(bar, UI_PAGE_MENU_BAR_W / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_PAGE_MENU_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);

    icon = ui_page_menu_box(row, UI_PAGE_MENU_ICON_SIZE, UI_PAGE_MENU_ICON_SIZE);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, UI_PAGE_MENU_ICON_X, 0);
    desc->draw_icon(icon);

    text = lv_label_create(row);
    lv_label_set_text(text, desc->caption);
    lv_obj_set_style_text_font(text, s_font != NULL ? s_font : LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, UI_PAGE_MENU_TEXT_X, 0);
    return row;
}

static lv_obj_t *ui_page_menu_panel(lv_obj_t *layer, int32_t top)
{
    lv_obj_t *panel = ui_page_menu_box(layer, UI_PAGE_MENU_PANEL_W, UI_PAGE_MENU_PANEL_H);
    lv_obj_t *divider;
    int32_t y = 0;

    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_style_radius(panel, UI_PAGE_MENU_PANEL_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PAGE_MENU_PANEL_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_PAGE_MENU_LINE_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, UI_PAGE_MENU_PANEL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, UI_PAGE_MENU_PANEL_PAD, LV_PART_MAIN);

    for (int i = 0; i < UI_PAGE_MENU_ITEM_COUNT; i++) {
        if (i == UI_PAGE_MENU_CANCEL) {
            divider = ui_page_menu_box(panel, UI_PAGE_MENU_ROW_W - 2 * UI_PAGE_MENU_DIVIDER_INSET, 1);
            lv_obj_set_pos(divider, UI_PAGE_MENU_DIVIDER_INSET, y + UI_PAGE_MENU_GROUP_GAP / 2);
            lv_obj_set_style_bg_color(divider, lv_color_hex(UI_PAGE_MENU_LINE_COLOR), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
            y += UI_PAGE_MENU_GROUP_GAP;
        }
        s_menu.rows[i] = ui_page_menu_row(panel, (ui_page_menu_item_t)i, y);
        y += UI_PAGE_MENU_ROW_H;
    }
    return panel;
}

/* --------------------------------------------------------------------- api */

void ui_page_menu_set_font(const lv_font_t *font)
{
    s_font = font;
}

lv_obj_t *ui_page_menu_open(const ui_page_menu_config_t *config)
{
    lv_obj_t *scrim;

    if (config == NULL || config->parent == NULL || s_menu.layer != NULL) {
        return NULL;
    }

    /* Full-screen and not clickable, so taps on the live footer fall through. */
    s_menu.layer = ui_page_menu_box(config->parent, lv_obj_get_width(config->parent),
                                    lv_obj_get_height(config->parent));
    if (s_menu.layer == NULL) {
        return NULL;
    }
    lv_obj_align(s_menu.layer, LV_ALIGN_TOP_LEFT, 0, 0);

    scrim = ui_page_menu_box(s_menu.layer, lv_obj_get_width(config->parent), config->scrim_h);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(UI_PAGE_MENU_SCRIM_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, UI_PAGE_MENU_SCRIM_OPA, LV_PART_MAIN);
    lv_obj_add_event_cb(scrim, ui_page_menu_scrim_cb, LV_EVENT_CLICKED, NULL);

    ui_page_menu_panel(s_menu.layer, config->panel_top);
    s_menu.done_cb = config->done_cb;
    s_menu.focus = UI_PAGE_MENU_CANCEL;
    ui_page_menu_apply_focus();
    return s_menu.layer;
}

void ui_page_menu_close(void)
{
    ui_page_menu_finish(UI_PAGE_MENU_CANCEL);
}

bool ui_page_menu_is_open(void)
{
    return s_menu.layer != NULL;
}

void ui_page_menu_step(int step)
{
    int focus;

    if (s_menu.layer == NULL) {
        return;
    }
    focus = (int)s_menu.focus + step;
    if (focus < 0) {
        focus = 0;
    } else if (focus >= UI_PAGE_MENU_ITEM_COUNT) {
        focus = UI_PAGE_MENU_ITEM_COUNT - 1;
    }
    s_menu.focus = (ui_page_menu_item_t)focus;
    ui_page_menu_apply_focus();
}

void ui_page_menu_confirm(void)
{
    ui_page_menu_finish(s_menu.focus);
}
