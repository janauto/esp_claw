/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * businesscard_screen — Card wallet viewer.
 *
 * Displays each card from tools/add_ui with the exact layout those screens use:
 * a framed card container, per-card accent colour, card image on the left half,
 * and five text labels on the right. VOL-/VOL+ (routed through
 * system_ui_home_key_apply_locked) cycle between cards; MODE or the back button
 * returns to the carousel. Images are loaded from SD card via
 * ui_squareline_resolve_img(), exactly as the main carousel does.
 */
#include "system_ui_private.h"
#include "businesscard_screen.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "ui_squareline.h"

/* External fonts from ui_squareline */
extern const lv_font_t ui_font_Font1;  /* Smaller font for type_tag and id_line */
extern const lv_font_t ui_font_Font2;  /* 32px font for main text */

/* Chrome geometry — matches every add_ui screen's root container */
#define BC_ROOT_W   804
#define BC_ROOT_H   490
#define BC_ROOT_Y   -3

/* Shared chrome colours (same across all screens) */
#define BC_C_BG          0x000000
#define BC_C_PANEL       0x080C08
#define BC_C_TEXT_WHITE  0xFAFBF5
#define BC_C_TEXT_DIM    0x848A82
#define BC_C_BAR_BG      0x384038
#define BC_C_ACCENT_HI   0xF8E458   /* status-bar left marker */
#define BC_C_BTN_PRESS   0x2A3A2A   /* Dark green press state */
#define BC_C_BTN_BORDER  0xF0C430

/* Page-indicator dot sizes */
#define BC_DOT_ACTIVE_SZ 18
#define BC_DOT_IDLE_SZ   15
#define BC_DOT_STEP      25

/*
 * Per-card descriptor.
 *
 * frame_color   — border of the 587×245 card container (TextArea10 bg colour
 *                 matches it in every add_ui screen)
 * accent_color  — strip inside the frame, TextArea12/13/id_line text colour
 * badge_bg      — TextArea10 (name badge) background; usually == frame_color
 * badge_text    — TextArea10 text colour (0xFFFFAA, 0xFFFFEE)
 * name          — TextArea10 text
 * subtitle      — TextArea11 text (white)
 * type_tag      — TextArea12 text (accent coloured, left)
 * detail        — TextArea13 text (accent coloured, right of type_tag)
 * id_line       — 5th label (x=201 y=88, same colour as accent)
 * img_name      — key for ui_squareline_resolve_img()
 */
typedef struct {
    uint32_t    frame_color;
    uint32_t    accent_color;
    uint32_t    badge_bg;
    uint32_t    badge_text;
    const char *name;
    const char *subtitle;
    const char *type_tag;
    const char *detail;
    const char *id_line;
    const char *img_name;
} bc_card_desc_t;

static const bc_card_desc_t s_cards[] = {
    {   /* 门禁卡 — ui_Screen4 */
        .frame_color = 0xFFAD17,
        .accent_color = 0xFFAD17,
        .badge_bg    = 0xFFAD17,
        .badge_text  = 0x000000,  /* Black text */
        .name        = "NFC门禁卡",
        .subtitle    = "公司门禁卡",
        .type_tag    = "ACCESS",
        .detail      = "5栋110",
        .id_line     = "ID:ABCDEFG01234",
        .img_name    = "ui_img_1994401079",
    },
    {   /* 交通卡 — ui_Screen5 */
        .frame_color = 0xD4EC56,
        .accent_color = 0xD4EC56,
        .badge_bg    = 0xD4EC56,
        .badge_text  = 0x000000,  /* Black text */
        .name        = "交通卡",
        .subtitle    = "深圳一卡通",
        .type_tag    = "TRANSIT",
        .detail      = "深圳通",
        .id_line     = "ID:XXXX0025",
        .img_name    = "ui_img_2049455574",
    },
    {   /* 银行支付卡 — ui_Screen6 */
        .frame_color = 0xFFD800,
        .accent_color = 0xFFD800,
        .badge_bg    = 0xFFD800,
        .badge_text  = 0x000000,  /* Black text */
        .name        = "银行支付卡",
        .subtitle    = "银行支付",
        .type_tag    = "PATMENT",
        .detail      = "兴业银行",
        .id_line     = "ID:XXXX0001",
        .img_name    = "ui_img_2016100889",
    },
    {   /* 车钥匙 — ui_Screen7 */
        .frame_color = 0xFFE45B,
        .accent_color = 0xFFE45B,
        .badge_bg    = 0xFFE45B,
        .badge_text  = 0x000000,  /* Black text */
        .name        = "汽车钥匙",
        .subtitle    = "我的汽车钥匙",
        .type_tag    = "CAR KEY",
        .detail      = "比亚迪XX",
        .id_line     = "川A186",
        .img_name    = "ui_img_1512657957",
    },
    {   /* 电动车钥匙 — ui_Screen8 */
        .frame_color = 0xFFE45B,
        .accent_color = 0xFFE45B,
        .badge_bg    = 0xFFE45B,
        .badge_text  = 0x000000,  /* Black text */
        .name        = "电动车钥匙",
        .subtitle    = "我的电动车钥匙",
        .type_tag    = "E BIKE",
        .detail      = "雅迪XX",
        .id_line     = "川A186",
        .img_name    = "ui_img_639095225",
    },
    {   /* 校园卡 — ui_Screen10 */
        .frame_color = 0xF4C532,
        .accent_color = 0xF4C532,
        .badge_bg    = 0xF4C532,
        .badge_text  = 0x000000,  /* Black text */
        .name        = "校园卡",
        .subtitle    = "校园一卡通",
        .type_tag    = "CAMPUB",
        .detail      = "小明",
        .id_line     = "ID XXXXXX110",
        .img_name    = "ui_img_1125721784",
    },
};
#define BC_CARD_COUNT ((int)(sizeof(s_cards) / sizeof(s_cards[0])))

/* Dot row: BC_CARD_COUNT dots, 25 px apart, centred */
#define BC_DOT_FIRST  (-(BC_CARD_COUNT - 1) * BC_DOT_STEP / 2)

typedef struct {
    lv_obj_t *screen;

    /* Per-card dynamic widgets */
    lv_obj_t *card_frame;       /* 587×245 container — border colour changes */
    lv_obj_t *frame_strip;      /* accent strip inside frame */
    lv_obj_t *card_img;
    lv_obj_t *badge;            /* TextArea10: name pill */
    lv_obj_t *badge_label;
    lv_obj_t *subtitle;         /* TextArea11 */
    lv_obj_t *type_tag;         /* TextArea12 */
    lv_obj_t *detail;           /* TextArea13 */
    lv_obj_t *id_line;          /* 5th label */

    lv_obj_t *dots[BC_CARD_COUNT];
    lv_obj_t *counter;          /* "N/6" top-right */

    int current_idx;
} bc_ui_t;

static bc_ui_t s_bc;

/* ── helpers ──────────────────────────────────────────────────────────────── */

static lv_obj_t *bc_block(lv_obj_t *parent, int32_t w, int32_t h,
                           int32_t x, int32_t y, int32_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_width(obj, w);
    lv_obj_set_height(obj, h);
    lv_obj_set_x(obj, x);
    lv_obj_set_y(obj, y);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (radius) {
        lv_obj_set_style_radius(obj, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    return obj;
}

static void bc_fill(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* Plain text label, anchored from centre like the SquareLine convention */
static lv_obj_t *bc_label(lv_obj_t *parent, int32_t w, int32_t h,
                           int32_t x, int32_t y, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, w);
    lv_obj_set_height(label, h);
    lv_obj_set_x(label, x);
    lv_obj_set_y(label, y);
    lv_obj_set_align(label, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    system_ui_apply_font(label);
    return label;
}

/* ── content refresh — swaps colours and texts without rebuilding layout ──── */

static void bc_update_locked(void)
{
    const bc_card_desc_t *c = &s_cards[s_bc.current_idx];

    /* Card frame border + strip accent */
    lv_obj_set_style_border_color(s_bc.card_frame, lv_color_hex(c->frame_color),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    bc_fill(s_bc.frame_strip, c->accent_color);

    /* Card image */
    const char *path = ui_squareline_resolve_img(c->img_name);
    if (path != NULL) {
        lv_image_set_src(s_bc.card_img, path);
        lv_obj_remove_flag(s_bc.card_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        ESP_LOGW(SYSTEM_UI_TAG, "businesscard img '%s' not found", c->img_name);
        lv_obj_add_flag(s_bc.card_img, LV_OBJ_FLAG_HIDDEN);
    }

    /* Name badge: update textarea background + text */
    lv_obj_set_style_bg_color(s_bc.badge_label, lv_color_hex(c->badge_bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_textarea_set_text(s_bc.badge_label, c->name);
    lv_obj_set_style_text_color(s_bc.badge_label, lv_color_hex(c->badge_text),
                                LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Subtitle (white) */
    lv_label_set_text(s_bc.subtitle, c->subtitle);

    /* Type tag + detail (accent coloured) */
    lv_label_set_text(s_bc.type_tag, c->type_tag);
    lv_obj_set_style_text_color(s_bc.type_tag, lv_color_hex(c->accent_color),
                                LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_label_set_text(s_bc.detail, c->detail);
    lv_obj_set_style_text_color(s_bc.detail, lv_color_hex(c->accent_color),
                                LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ID line */
    lv_label_set_text(s_bc.id_line, c->id_line);
    lv_obj_set_style_text_color(s_bc.id_line, lv_color_hex(c->accent_color),
                                LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Counter */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d/%d", s_bc.current_idx + 1, BC_CARD_COUNT);
    lv_label_set_text(s_bc.counter, buf);

    /* Dots */
    for (int i = 0; i < BC_CARD_COUNT; i++) {
        if (s_bc.dots[i] == NULL) {
            continue;
        }
        if (i == s_bc.current_idx) {
            lv_obj_set_width(s_bc.dots[i], BC_DOT_ACTIVE_SZ);
            lv_obj_set_height(s_bc.dots[i], BC_DOT_ACTIVE_SZ);
            bc_fill(s_bc.dots[i], s_cards[i].accent_color);
        } else {
            lv_obj_set_width(s_bc.dots[i], BC_DOT_IDLE_SZ);
            lv_obj_set_height(s_bc.dots[i], BC_DOT_IDLE_SZ);
            bc_fill(s_bc.dots[i], 0x384038);
        }
    }
}

/* ── key navigation ─────────────────────────────────────────────────────── */

#define SWIPE_THRESHOLD 44  /* Same as game_2048 */

static struct {
    bool is_pressed;
    int32_t start_x;
    int32_t start_y;
} touch_state = { false, 0, 0 };

static void bc_touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();

    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        touch_state.is_pressed = true;
        touch_state.start_x = point.x;
        touch_state.start_y = point.y;
        ESP_LOGI(SYSTEM_UI_TAG, "touch pressed at (%d, %d)", point.x, point.y);
    }
    else if (code == LV_EVENT_RELEASED) {
        if (touch_state.is_pressed) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            int32_t dx = point.x - touch_state.start_x;
            int32_t dy = point.y - touch_state.start_y;

            ESP_LOGI(SYSTEM_UI_TAG, "touch released at (%d, %d), delta=(%d, %d)",
                     point.x, point.y, dx, dy);

            /* Check if this is a horizontal swipe */
            if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                if (dx > 0) {
                    /* Swipe right -> previous card */
                    ESP_LOGI(SYSTEM_UI_TAG, "swipe right -> prev card");
                    system_ui_businesscard_prev_locked();
                } else {
                    /* Swipe left -> next card */
                    ESP_LOGI(SYSTEM_UI_TAG, "swipe left -> next card");
                    system_ui_businesscard_next_locked();
                }
            }

            touch_state.is_pressed = false;
        }
    }
}

void system_ui_businesscard_prev_locked(void)
{
    if (s_bc.screen == NULL) {
        return;
    }
    s_bc.current_idx = (s_bc.current_idx + BC_CARD_COUNT - 1) % BC_CARD_COUNT;
    bc_update_locked();
}

void system_ui_businesscard_next_locked(void)
{
    if (s_bc.screen == NULL) {
        return;
    }
    s_bc.current_idx = (s_bc.current_idx + 1) % BC_CARD_COUNT;
    bc_update_locked();
}

/* ── button callbacks ────────────────────────────────────────────────────── */

static void bc_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    (void)system_ui_hide_businesscard();
}

static void bc_copy_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ESP_LOGI(SYSTEM_UI_TAG, "businesscard copy: %s", s_cards[s_bc.current_idx].name);
}

static void bc_share_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ESP_LOGI(SYSTEM_UI_TAG, "businesscard share: %s", s_cards[s_bc.current_idx].name);
}

static void bc_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    ESP_LOGI(SYSTEM_UI_TAG, "businesscard delete: %s", s_cards[s_bc.current_idx].name);
}

/* ── create ─────────────────────────────────────────────────────────────── */

esp_err_t system_ui_create_businesscard_locked(void)
{
    lv_obj_t *root;
    lv_obj_t *bar;
    lv_obj_t *obj;
    lv_obj_t *btn;

    if (s_bc.screen != NULL) {
        return ESP_OK;
    }

    s_bc.screen = lv_obj_create(NULL);
    ESP_RETURN_ON_FALSE(s_bc.screen != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG,
                        "create businesscard screen failed");
    lv_obj_clear_flag(s_bc.screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Root container (matches ui_Container40 / ui_Container39 / etc.) */
    root = bc_block(s_bc.screen, BC_ROOT_W, BC_ROOT_H, -1, BC_ROOT_Y, 0);
    bc_fill(root, BC_C_BG);

    /* ── Status bar (ui_Container1 + ui_Container17) ── */
    bar = bc_block(root, 791, 30, -1, -222, 0);
    bc_fill(bar, BC_C_PANEL);
    bc_fill(bc_block(bar, 9, 39, -388, -1, 0), BC_C_ACCENT_HI);

    /* Time (ui_TextArea24 / equivalent) */
    bc_label(root, 88, 54, 313, -213, "9:51", BC_C_TEXT_WHITE);

    /* Battery bar (ui_Bar3 / equivalent) */
    obj = lv_bar_create(root);
    lv_bar_set_value(obj, 70, LV_ANIM_OFF);
    lv_bar_set_start_value(obj, 0, LV_ANIM_OFF);
    lv_obj_set_width(obj, 32);
    lv_obj_set_height(obj, 9);
    lv_obj_set_x(obj, 367);
    lv_obj_set_y(obj, -225);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    bc_fill(obj, BC_C_BAR_BG);
    lv_obj_set_style_radius(obj, 1, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xF0DC18), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    /* Header strip (ui_Container18) + "nfc卡片" title + counter */
    bar = bc_block(root, 240, 41, 0, -182, 0);
    bc_fill(bar, BC_C_PANEL);
    bc_label(root, 164, 54, -11, -178, "nfc卡片", BC_C_TEXT_WHITE);
    bc_label(root, 135, 58, -316, -213, "LANGUAGE", BC_C_TEXT_WHITE);
    s_bc.counter = bc_label(root, 80, 40, 330, -182, "1/6", BC_C_TEXT_DIM);

    /* ── Card frame (ui_Container19 — 587×245, radius 20) ── */
    s_bc.card_frame = bc_block(root, 587, 245, 0, -16, 20);
    lv_obj_set_style_border_color(s_bc.card_frame, lv_color_hex(0xFFAD17),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_bc.card_frame, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_bc.card_frame, 6, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Accent strip (ui_Container20 — 520×13, radius 10, at y=-95) inside card_frame */
    s_bc.frame_strip = bc_block(s_bc.card_frame, 520, 13, 0, -95, 10);
    bc_fill(s_bc.frame_strip, 0xFFAD17);

    /* Card image (ui_Image7 — x=-156, y=-14, zoom 640) inside card_frame */
    s_bc.card_img = lv_image_create(s_bc.card_frame);
    lv_obj_set_x(s_bc.card_img, -156);
    lv_obj_set_y(s_bc.card_img, -14);
    lv_obj_set_align(s_bc.card_img, LV_ALIGN_CENTER);
    lv_image_set_scale(s_bc.card_img, 640);
    lv_obj_clear_flag(s_bc.card_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Move strip to foreground so it's not covered by the image */
    lv_obj_move_foreground(s_bc.frame_strip);

    /* TextArea10: name badge (260×54 at x=129, y=-49, radius 15) - use textarea directly, no wrapper */
    s_bc.badge_label = lv_textarea_create(root);
    lv_obj_set_width(s_bc.badge_label, 260);
    lv_obj_set_height(s_bc.badge_label, 54);
    lv_obj_set_x(s_bc.badge_label, 129);
    lv_obj_set_y(s_bc.badge_label, -49);
    lv_obj_set_align(s_bc.badge_label, LV_ALIGN_CENTER);
    lv_textarea_set_text(s_bc.badge_label, "");
    lv_obj_clear_flag(s_bc.badge_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_bc.badge_label, LV_OBJ_FLAG_CLICKABLE);  /* Disable click to prevent cursor */
    lv_obj_clear_flag(s_bc.badge_label, LV_OBJ_FLAG_CLICK_FOCUSABLE);  /* Disable focus */
    lv_obj_set_style_text_color(s_bc.badge_label, lv_color_hex(0xFFFFAA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(s_bc.badge_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_bc.badge_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_bc.badge_label, &ui_font_Font2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_bc.badge_label, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_bc.badge_label, lv_color_hex(0xFFAD17), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_bc.badge_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(s_bc.badge_label, LV_BORDER_SIDE_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* Hide cursor by making it invisible */
    lv_obj_set_style_opa(s_bc.badge_label, 0, LV_PART_CURSOR | LV_STATE_DEFAULT);
    /* Note: s_bc.badge is not used, only s_bc.badge_label */

    /* TextArea11: subtitle (239×54 at x=123, y=15) white - parent is ROOT */
    s_bc.subtitle = bc_label(root, 239, 54, 123, 15, "", BC_C_TEXT_WHITE);
    lv_obj_set_style_text_font(s_bc.subtitle, &ui_font_Font2, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* TextArea12: type tag (164×54 at x=92, y=55) accent - parent is ROOT */
    s_bc.type_tag = bc_label(root, 164, 54, 92, 55, "", 0xFFAD17);
    lv_obj_set_style_text_font(s_bc.type_tag, &ui_font_Font1, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* TextArea13: detail (164×54 at x=201, y=59) accent - parent is ROOT */
    s_bc.detail = bc_label(root, 164, 54, 201, 59, "", 0xFFAD17);
    lv_obj_set_style_text_font(s_bc.detail, &ui_font_Font2, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 5th label: id_line (164×54 at x=201, y=88) accent - parent is ROOT - now Font1 has 川 */
    s_bc.id_line = bc_label(root, 164, 54, 201, 88, "", 0xFFAD17);
    lv_obj_set_style_text_font(s_bc.id_line, &ui_font_Font1, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Page-indicator dots (match add_ui placement: y=134, parent is ROOT not card_frame) ── */
    for (int i = 0; i < BC_CARD_COUNT; i++) {
        /* Centre the row: first dot offset is -(N-1)/2 * step, then +i*step */
        int32_t dot_x = -(BC_CARD_COUNT - 1) * BC_DOT_STEP / 2
                        + i * BC_DOT_STEP;
        lv_obj_t *dot = lv_obj_create(root);
        lv_obj_remove_style_all(dot);
        lv_obj_set_width(dot, BC_DOT_IDLE_SZ);
        lv_obj_set_height(dot, BC_DOT_IDLE_SZ);
        lv_obj_set_x(dot, dot_x);
        lv_obj_set_y(dot, 134);
        lv_obj_set_align(dot, LV_ALIGN_CENTER);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(dot, 75, LV_PART_MAIN | LV_STATE_DEFAULT);
        bc_fill(dot, 0x384038);
        s_bc.dots[i] = dot;
    }

    /* ── Action buttons row (same geometry as add_ui) ── */
    /* Copy button (ui_Button10 / equivalent) */
    btn = lv_button_create(root);
    lv_obj_set_width(btn, 122);
    lv_obj_set_height(btn, 71);
    lv_obj_set_x(btn, -176);
    lv_obj_set_y(btn, 193);
    lv_obj_set_align(btn, LV_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    bc_fill(btn, 0x000000);
    lv_obj_set_style_border_color(btn, lv_color_hex(BC_C_BTN_BORDER),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BC_C_BTN_PRESS),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_PRESSED);
    system_ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, bc_copy_cb, LV_EVENT_CLICKED, NULL);
    obj = lv_label_create(btn);
    lv_label_set_text(obj, "复制");
    lv_obj_set_style_text_color(obj, lv_color_hex(BC_C_BTN_BORDER),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    system_ui_apply_font(obj);
    lv_obj_center(obj);

    /* Share button (center) */
    btn = lv_button_create(root);
    lv_obj_set_width(btn, 122);
    lv_obj_set_height(btn, 71);
    lv_obj_set_x(btn, -34);
    lv_obj_set_y(btn, 193);
    lv_obj_set_align(btn, LV_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    bc_fill(btn, 0x000000);
    lv_obj_set_style_border_color(btn, lv_color_hex(BC_C_BTN_BORDER),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BC_C_BTN_PRESS),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_PRESSED);
    system_ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, bc_share_cb, LV_EVENT_CLICKED, NULL);
    obj = lv_label_create(btn);
    lv_label_set_text(obj, "分享");
    lv_obj_set_style_text_color(obj, lv_color_hex(BC_C_BTN_BORDER),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    system_ui_apply_font(obj);
    lv_obj_center(obj);

    /* Delete button (right) */
    btn = lv_button_create(root);
    lv_obj_set_width(btn, 122);
    lv_obj_set_height(btn, 71);
    lv_obj_set_x(btn, 108);
    lv_obj_set_y(btn, 193);
    lv_obj_set_align(btn, LV_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    bc_fill(btn, 0x000000);
    lv_obj_set_style_border_color(btn, lv_color_hex(BC_C_BTN_BORDER),  /* Same border as others */
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BC_C_BTN_PRESS),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_PRESSED);
    system_ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, bc_delete_cb, LV_EVENT_CLICKED, NULL);
    obj = lv_label_create(btn);
    lv_label_set_text(obj, "删除");
    lv_obj_set_style_text_color(obj, lv_color_hex(BC_C_BTN_BORDER),  /* Same text color as others */
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    system_ui_apply_font(obj);
    lv_obj_center(obj);

    /* Back button (below LANGUAGE container, left side) */
    btn = lv_button_create(root);
    lv_obj_set_width(btn, 100);
    lv_obj_set_height(btn, 40);
    lv_obj_set_x(btn, -316);
    lv_obj_set_y(btn, -170);
    lv_obj_set_align(btn, LV_ALIGN_CENTER);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    bc_fill(btn, 0x2A2A2A);
    lv_obj_set_style_border_color(btn, lv_color_hex(BC_C_BTN_BORDER),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BC_C_BTN_PRESS),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_PRESSED);
    system_ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, bc_back_cb, LV_EVENT_CLICKED, NULL);
    obj = lv_label_create(btn);
    lv_label_set_text(obj, "< 返回");
    lv_obj_set_style_text_color(obj, lv_color_hex(BC_C_BTN_BORDER),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    system_ui_apply_font(obj);
    lv_obj_center(obj);

    s_bc.current_idx = 0;
    bc_update_locked();

    /* Register touch events for swipe navigation - use screen instead of root to catch all touches */
    lv_obj_add_event_cb(s_bc.screen, bc_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_bc.screen, bc_touch_event_cb, LV_EVENT_RELEASED, NULL);

    /* Also register on root container to ensure we catch touches not blocked by children */
    lv_obj_add_event_cb(root, bc_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, bc_touch_event_cb, LV_EVENT_RELEASED, NULL);

    s_ui.businesscard_screen = s_bc.screen;
    ESP_LOGI(SYSTEM_UI_TAG, "businesscard screen created (%d cards)", BC_CARD_COUNT);
    return ESP_OK;
}

void system_ui_delete_businesscard_locked(void)
{
    if (s_bc.screen != NULL) {
        lv_obj_del(s_bc.screen);
    }
    lv_memzero(&s_bc, sizeof(s_bc));
    s_ui.businesscard_screen = NULL;
    s_ui.businesscard_visible = false;
}

esp_err_t system_ui_show_businesscard(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_bc.screen, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "businesscard screen not available");
    ESP_RETURN_ON_FALSE(!display_service_has_exclusive_session(), ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "exclusive display session active");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    s_ui.businesscard_visible = true;
    system_ui_load_screen_locked(s_bc.screen);
    system_ui_unlock();
    return ESP_OK;
}

esp_err_t system_ui_hide_businesscard(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.home_screen, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "home screen not available");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    s_ui.businesscard_visible = false;
    ui_squareline_show_page(ui_squareline_current_page());
    system_ui_load_screen_locked(s_ui.home_screen);
    system_ui_unlock();
    return ESP_OK;
}
