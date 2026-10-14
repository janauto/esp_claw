/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sheets and toasts for the home carousel's long-press flow: the 卸载 / 详细 /
 * 取消 menu, the uninstall confirmation and the app details all use one sheet.
 *
 * The screen above the footer is dimmed and a panel rises from just above the
 * footer, growing upwards to fit an optional heading, message and key/value
 * lines followed by the selectable rows. The footer is left live: its PREV /
 * NEXT hit targets and the matching physical keys move the focus, M picks the
 * focused row. Callers pick a safe row (取消, 关闭) as the initial focus so a
 * stray M press never does anything destructive.
 *
 * This file only draws and reports the chosen row; the carousel decides what
 * each row means.
 */

#include "ui_page_menu.h"

#include <stddef.h>

#define UI_PAGE_MENU_MAX_ROWS       4

#define UI_PAGE_MENU_SCRIM_COLOR    0x000000
#define UI_PAGE_MENU_SCRIM_OPA      200
#define UI_PAGE_MENU_PANEL_COLOR    0x080C08
#define UI_PAGE_MENU_LINE_COLOR     0x384038
#define UI_PAGE_MENU_FOCUS_BG       0x303430
#define UI_PAGE_MENU_ACCENT         0xF0C430
#define UI_PAGE_MENU_TEXT_COLOR     0xFAFBF5
#define UI_PAGE_MENU_MUTED_COLOR    0x9AA09A
#define UI_PAGE_MENU_DANGER_COLOR   0xFF5A47

#define UI_PAGE_MENU_PANEL_RADIUS   14
#define UI_PAGE_MENU_PANEL_BORDER   2
#define UI_PAGE_MENU_PANEL_PAD      6
#define UI_PAGE_MENU_HEADER_PAD_X   14
#define UI_PAGE_MENU_HEADER_PAD_Y   12
#define UI_PAGE_MENU_HEADER_GAP     8
#define UI_PAGE_MENU_KEY_W          84
#define UI_PAGE_MENU_ROW_H          60
#define UI_PAGE_MENU_ROW_RADIUS     10
#define UI_PAGE_MENU_GROUP_GAP      9       /* the cancel row sits apart */
#define UI_PAGE_MENU_DIVIDER_INSET  16
#define UI_PAGE_MENU_BAR_W          4
#define UI_PAGE_MENU_BAR_H          28
#define UI_PAGE_MENU_ICON_X         22
#define UI_PAGE_MENU_ICON_SIZE      24
#define UI_PAGE_MENU_TEXT_X         62

#define UI_PAGE_MENU_TOAST_MS       2200
#define UI_PAGE_MENU_TOAST_BG       0x202420
#define UI_PAGE_MENU_TOAST_RADIUS   22
#define UI_PAGE_MENU_TOAST_PAD_X    22
#define UI_PAGE_MENU_TOAST_PAD_Y    10
#define UI_PAGE_MENU_TOAST_MAX_W    400

static struct {
    lv_obj_t              *layer;
    lv_obj_t              *rows[UI_PAGE_MENU_MAX_ROWS];
    ui_page_menu_row_t     row_desc[UI_PAGE_MENU_MAX_ROWS];
    uint8_t                row_count;
    uint8_t                focus;
    uint8_t                cancel_row;
    ui_page_menu_done_cb_t done_cb;
} s_menu;

static struct {
    lv_obj_t   *obj;
    lv_timer_t *timer;
} s_toast;

static const lv_font_t *s_font;

static const lv_font_t *ui_page_menu_font(void)
{
    return s_font != NULL ? s_font : LV_FONT_DEFAULT;
}

/* Plain, non-interactive container with every theme style removed. */
static lv_obj_t *ui_page_menu_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *ui_page_menu_text(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_page_menu_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    return label;
}

/* ------------------------------------------------------------------- icons */

/* Icons are drawn from primitives in a 24x24 box: the built-in symbol font only
 * ships at 14 px. Rect parts take their colour from bg (filled) or border
 * (outlined), lines from line_color; ui_page_menu_tint() sets all three. */
static lv_obj_t *ui_page_menu_part(lv_obj_t *box, int32_t x, int32_t y, int32_t w, int32_t h,
                                   int32_t outline, int32_t radius)
{
    lv_obj_t *obj = ui_page_menu_box(box, w, h);
    lv_obj_set_pos(obj, x, y);
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

static void ui_page_menu_draw_icon(lv_obj_t *box, ui_page_menu_icon_t icon)
{
    /* lv_line keeps the pointer, so the points must outlive the widget. */
    static const lv_point_precise_t close_down[] = { { 5, 5 }, { 19, 19 } };
    static const lv_point_precise_t close_up[]   = { { 19, 5 }, { 5, 19 } };

    switch (icon) {
    case UI_PAGE_MENU_ICON_TRASH:
        ui_page_menu_part(box, 8, 0, 8, 3, 0, 1);       /* handle */
        ui_page_menu_part(box, 2, 3, 20, 3, 0, 1);      /* lid    */
        ui_page_menu_part(box, 5, 8, 14, 16, 2, 2);     /* can    */
        ui_page_menu_part(box, 9, 12, 2, 8, 0, 1);      /* ribs   */
        ui_page_menu_part(box, 13, 12, 2, 8, 0, 1);
        break;
    case UI_PAGE_MENU_ICON_INFO:
        ui_page_menu_part(box, 1, 1, 22, 22, 2, LV_RADIUS_CIRCLE);
        ui_page_menu_part(box, 11, 6, 3, 3, 0, 1);      /* dot of the i */
        ui_page_menu_part(box, 11, 10, 3, 8, 0, 1);     /* stem         */
        break;
    case UI_PAGE_MENU_ICON_CLOSE:
    default:
        ui_page_menu_line(box, close_down);
        ui_page_menu_line(box, close_up);
        break;
    }
}

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
    for (uint8_t i = 0; i < s_menu.row_count; i++) {
        lv_obj_t *row = s_menu.rows[i];
        bool focused = (i == s_menu.focus);
        uint32_t text = s_menu.row_desc[i].danger ? UI_PAGE_MENU_DANGER_COLOR
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

/* Tear the sheet down, then report. The layer may be deleted from inside one of
 * its own children's event callbacks, as the infrared remote's pickers do. */
static void ui_page_menu_finish(uint8_t row)
{
    ui_page_menu_done_cb_t done_cb = s_menu.done_cb;

    if (s_menu.layer == NULL) {
        return;
    }
    lv_obj_del(s_menu.layer);
    lv_memzero(&s_menu, sizeof(s_menu));
    if (done_cb != NULL) {
        done_cb(row);
    }
}

/* ------------------------------------------------------------------- events */

static void ui_page_menu_scrim_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_page_menu_finish(s_menu.cancel_row);
    }
}

static void ui_page_menu_row_cb(lv_event_t *e)
{
    uint8_t row = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:      /* the highlight follows the finger */
        s_menu.focus = row;
        ui_page_menu_apply_focus();
        break;
    case LV_EVENT_CLICKED:
        ui_page_menu_finish(row);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ widgets */

static void ui_page_menu_divider(lv_obj_t *panel, int32_t gap)
{
    lv_obj_t *slot = ui_page_menu_box(panel, lv_pct(100), gap);
    lv_obj_t *line = ui_page_menu_box(slot, lv_pct(100), 1);

    lv_obj_set_style_pad_hor(slot, UI_PAGE_MENU_DIVIDER_INSET, LV_PART_MAIN);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_PAGE_MENU_LINE_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
}

static void ui_page_menu_header(lv_obj_t *panel, const ui_page_menu_config_t *config)
{
    lv_obj_t *header = ui_page_menu_box(panel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *label;

    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(header, UI_PAGE_MENU_HEADER_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(header, UI_PAGE_MENU_HEADER_PAD_Y, LV_PART_MAIN);
    lv_obj_set_style_pad_row(header, UI_PAGE_MENU_HEADER_GAP, LV_PART_MAIN);

    if (config->heading != NULL) {
        label = ui_page_menu_text(header, config->heading, UI_PAGE_MENU_TEXT_COLOR);
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    if (config->message != NULL) {
        label = ui_page_menu_text(header, config->message, UI_PAGE_MENU_MUTED_COLOR);
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    for (uint8_t i = 0; i < config->field_count; i++) {
        lv_obj_t *line = ui_page_menu_box(header, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_t *value;

        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        label = ui_page_menu_text(line, config->fields[i].key, UI_PAGE_MENU_MUTED_COLOR);
        lv_obj_set_width(label, UI_PAGE_MENU_KEY_W);
        value = ui_page_menu_text(line, config->fields[i].value, UI_PAGE_MENU_TEXT_COLOR);
        lv_obj_set_flex_grow(value, 1);
        lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    }
}

static lv_obj_t *ui_page_menu_row(lv_obj_t *panel, uint8_t index, const ui_page_menu_row_t *desc)
{
    lv_obj_t *row = ui_page_menu_box(panel, lv_pct(100), UI_PAGE_MENU_ROW_H);
    lv_obj_t *bar;
    lv_obj_t *icon;
    lv_obj_t *text;

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(row, UI_PAGE_MENU_ROW_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_PAGE_MENU_FOCUS_BG), LV_PART_MAIN);
    lv_obj_add_event_cb(row, ui_page_menu_row_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(row, ui_page_menu_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    /* Same yellow marker as the status bar's left edge. */
    bar = ui_page_menu_box(row, UI_PAGE_MENU_BAR_W, UI_PAGE_MENU_BAR_H);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(bar, UI_PAGE_MENU_BAR_W / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_PAGE_MENU_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);

    icon = ui_page_menu_box(row, UI_PAGE_MENU_ICON_SIZE, UI_PAGE_MENU_ICON_SIZE);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, UI_PAGE_MENU_ICON_X, 0);
    ui_page_menu_draw_icon(icon, desc->icon);

    text = ui_page_menu_text(row, desc->caption, UI_PAGE_MENU_TEXT_COLOR);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, UI_PAGE_MENU_TEXT_X, 0);
    return row;
}

static lv_obj_t *ui_page_menu_panel(lv_obj_t *layer, const ui_page_menu_config_t *config)
{
    lv_obj_t *panel = ui_page_menu_box(layer, config->panel_w, LV_SIZE_CONTENT);
    bool has_header = config->heading != NULL || config->message != NULL || config->field_count > 0;

    lv_obj_set_style_radius(panel, UI_PAGE_MENU_PANEL_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_PAGE_MENU_PANEL_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_PAGE_MENU_LINE_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, UI_PAGE_MENU_PANEL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, UI_PAGE_MENU_PANEL_PAD, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    if (has_header) {
        ui_page_menu_header(panel, config);
        ui_page_menu_divider(panel, UI_PAGE_MENU_GROUP_GAP);
    }
    for (uint8_t i = 0; i < config->row_count; i++) {
        if (i == config->cancel_row && i > 0) {
            ui_page_menu_divider(panel, UI_PAGE_MENU_GROUP_GAP);
        }
        s_menu.rows[i] = ui_page_menu_row(panel, i, &config->rows[i]);
        s_menu.row_desc[i] = config->rows[i];
    }

    /* Grow upwards from the requested bottom edge. */
    lv_obj_update_layout(panel);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, config->panel_bottom - lv_obj_get_height(panel));
    return panel;
}

/* --------------------------------------------------------------------- api */

void ui_page_menu_set_font(const lv_font_t *font)
{
    s_font = font;
}

const lv_font_t *ui_page_menu_get_font(void)
{
    return s_font;
}

lv_obj_t *ui_page_menu_open(const ui_page_menu_config_t *config)
{
    lv_obj_t *scrim;
    int32_t w;

    if (config == NULL || config->parent == NULL || s_menu.layer != NULL ||
            config->rows == NULL || config->row_count == 0 ||
            config->row_count > UI_PAGE_MENU_MAX_ROWS ||
            config->focus >= config->row_count || config->cancel_row >= config->row_count) {
        return NULL;
    }
    w = lv_obj_get_width(config->parent);

    /* Full-screen and not clickable, so taps on the live footer fall through. */
    s_menu.layer = ui_page_menu_box(config->parent, w, lv_obj_get_height(config->parent));
    if (s_menu.layer == NULL) {
        return NULL;
    }
    lv_obj_align(s_menu.layer, LV_ALIGN_TOP_LEFT, 0, 0);

    scrim = ui_page_menu_box(s_menu.layer, w, config->scrim_h);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(UI_PAGE_MENU_SCRIM_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, UI_PAGE_MENU_SCRIM_OPA, LV_PART_MAIN);
    lv_obj_add_event_cb(scrim, ui_page_menu_scrim_cb, LV_EVENT_CLICKED, NULL);

    ui_page_menu_panel(s_menu.layer, config);
    s_menu.row_count = config->row_count;
    s_menu.cancel_row = config->cancel_row;
    s_menu.done_cb = config->done_cb;
    s_menu.focus = config->focus;
    ui_page_menu_apply_focus();
    return s_menu.layer;
}

void ui_page_menu_close(void)
{
    ui_page_menu_finish(s_menu.cancel_row);
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
    } else if (focus >= (int)s_menu.row_count) {
        focus = (int)s_menu.row_count - 1;
    }
    s_menu.focus = (uint8_t)focus;
    ui_page_menu_apply_focus();
}

void ui_page_menu_confirm(void)
{
    ui_page_menu_finish(s_menu.focus);
}

/* -------------------------------------------------------------------- toast */

static void ui_page_menu_toast_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    s_toast.timer = NULL;       /* repeat count 1: LVGL deletes it after this call */
    if (s_toast.obj != NULL) {
        lv_obj_del(s_toast.obj);
        s_toast.obj = NULL;
    }
}

void ui_page_menu_toast_clear(void)
{
    if (s_toast.timer != NULL) {
        lv_timer_del(s_toast.timer);
        s_toast.timer = NULL;
    }
    if (s_toast.obj != NULL) {
        lv_obj_del(s_toast.obj);
        s_toast.obj = NULL;
    }
}

void ui_page_menu_toast(lv_obj_t *parent, int32_t y, const char *text)
{
    lv_obj_t *label;

    ui_page_menu_toast_clear();
    if (parent == NULL || text == NULL) {
        return;
    }

    s_toast.obj = ui_page_menu_box(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_toast.obj, UI_PAGE_MENU_TOAST_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_toast.obj, lv_color_hex(UI_PAGE_MENU_TOAST_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toast.obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_toast.obj, lv_color_hex(UI_PAGE_MENU_LINE_COLOR), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_toast.obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toast.obj, UI_PAGE_MENU_PANEL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_toast.obj, UI_PAGE_MENU_TOAST_PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_toast.obj, UI_PAGE_MENU_TOAST_PAD_Y, LV_PART_MAIN);

    label = ui_page_menu_text(s_toast.obj, text, UI_PAGE_MENU_TEXT_COLOR);
    lv_obj_set_style_max_width(label, UI_PAGE_MENU_TOAST_MAX_W, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_update_layout(s_toast.obj);
    lv_obj_align(s_toast.obj, LV_ALIGN_TOP_MID, 0, y - lv_obj_get_height(s_toast.obj) / 2);

    s_toast.timer = lv_timer_create(ui_page_menu_toast_timer_cb, UI_PAGE_MENU_TOAST_MS, NULL);
    if (s_toast.timer != NULL) {
        lv_timer_set_repeat_count(s_toast.timer, 1);
    }
}
