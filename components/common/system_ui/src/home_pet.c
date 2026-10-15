/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 首页上的「小磁」：头顶马蹄形磁铁的黄色圆团，直接立在壁纸上（没有底面）。
 * 只用 LVGL 基本图形拼出来，不占图片资源；眨眼用一个低频定时器。
 * 进入「我的」后，Lua 花园里的小磁换成对应主题（像素 / 点阵 / 玻璃）。
 */
#include "home_priv.h"

#include <stdlib.h>

typedef struct {
    lv_obj_t *eye[2];
    lv_obj_t *shine[2];
    lv_obj_t *mouth;
    lv_timer_t *blink;
    int32_t eye_h;
    uint8_t phase;
} hv_pet_t;

static lv_obj_t *hv_pet_oval(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = hv_box(parent, x, y, w, h, color, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(o, opa, 0);
    return o;
}

static void hv_pet_blink_cb(lv_timer_t *t)
{
    hv_pet_t *pet = lv_timer_get_user_data(t);

    /* 0: 睁眼等待；1: 闭眼一帧；回到 0。周期约 3.6 s。 */
    pet->phase = (uint8_t)((pet->phase + 1) % 12);
    bool closed = pet->phase == 11;
    for (int i = 0; i < 2; i++) {
        lv_obj_set_height(pet->eye[i], closed ? 3 : pet->eye_h);
        if (closed) {
            lv_obj_add_flag(pet->shine[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(pet->shine[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void hv_pet_delete_cb(lv_event_t *e)
{
    hv_pet_t *pet = lv_event_get_user_data(e);
    if (pet) {
        if (pet->blink) {
            lv_timer_delete(pet->blink);
        }
        free(pet);
    }
}

lv_obj_t *hv_pet_create(lv_obj_t *parent, int32_t size)
{
    hv_pet_t *pet = calloc(1, sizeof(*pet));
    lv_obj_t *root = lv_obj_create(parent);
    int32_t s = size;
    int32_t bw = s * 72 / 100, bh = s * 56 / 100;
    int32_t bx = (s - bw) / 2, by = s * 36 / 100;

    hv_plain(root);
    lv_obj_set_size(root, s, s);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    if (!pet) {
        return root;
    }

    /* 地面阴影 */
    hv_pet_oval(root, bx + bw / 8, by + bh - s * 2 / 100, bw * 3 / 4, s * 9 / 100, 0x000000, LV_OPA_50);

    /* 磁铁：深灰 U 形弧，两端红/白磁极 */
    lv_obj_t *arc = lv_arc_create(root);
    int32_t aw = s * 30 / 100;
    lv_obj_set_size(arc, aw, aw);
    lv_obj_set_pos(arc, (s - aw) / 2, s * 10 / 100);
    lv_arc_set_bg_angles(arc, 0, 180);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_style(arc, NULL, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, s * 7 / 100, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x3A3D45), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc, 0, 0);
    /* 两个磁极接在 U 形两端的上方 */
    int32_t pole = s * 7 / 100;
    int32_t pole_h = s * 9 / 100;
    int32_t mid_y = s * 10 / 100 + aw / 2;
    hv_box(root, (s - aw) / 2, mid_y - pole_h, pole, pole_h, 0xE5484D, 2);
    hv_box(root, (s + aw) / 2 - pole, mid_y - pole_h, pole, pole_h, 0xE8EAEE, 2);

    /* 身体：黄色圆团，上亮下暗 */
    lv_obj_t *body = hv_box(root, bx, by, bw, bh, 0xFFE066, bh * 45 / 100);
    lv_obj_set_style_bg_grad_color(body, lv_color_hex(0xE9AE0C), 0);
    lv_obj_set_style_bg_grad_dir(body, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(body, s * 10 / 100, 0);
    lv_obj_set_style_shadow_color(body, lv_color_hex(0xF0C430), 0);
    lv_obj_set_style_shadow_opa(body, LV_OPA_20, 0);
    /* 高光 */
    hv_pet_oval(body, bw * 12 / 100, bh * 10 / 100, bw * 30 / 100, bh * 16 / 100, 0xFFFFFF, LV_OPA_40);

    /* 眼睛 */
    pet->eye_h = bh * 30 / 100;
    int32_t ew = bw * 14 / 100;
    for (int i = 0; i < 2; i++) {
        int32_t ex = i == 0 ? bw * 28 / 100 : bw * 58 / 100;
        pet->eye[i] = hv_pet_oval(body, ex, bh * 30 / 100, ew, pet->eye_h, 0x15130C, LV_OPA_COVER);
        pet->shine[i] = hv_pet_oval(pet->eye[i], ew * 20 / 100, pet->eye_h * 12 / 100,
                                    ew * 45 / 100, ew * 45 / 100, 0xFFFFFF, LV_OPA_COVER);
    }
    /* 腮红 */
    hv_pet_oval(body, bw * 10 / 100, bh * 58 / 100, bw * 16 / 100, bh * 12 / 100, 0xF08A24, LV_OPA_50);
    hv_pet_oval(body, bw * 74 / 100, bh * 58 / 100, bw * 16 / 100, bh * 12 / 100, 0xF08A24, LV_OPA_50);
    /* 嘴：小弧 */
    pet->mouth = lv_arc_create(body);
    int32_t mw = bw * 16 / 100;
    lv_obj_set_size(pet->mouth, mw, mw);
    lv_obj_set_pos(pet->mouth, (bw - mw) / 2, bh * 46 / 100);
    lv_arc_set_bg_angles(pet->mouth, 20, 160);
    lv_obj_remove_style(pet->mouth, NULL, LV_PART_KNOB);
    lv_obj_remove_style(pet->mouth, NULL, LV_PART_INDICATOR);
    lv_obj_remove_flag(pet->mouth, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(pet->mouth, s * 2 / 100 + 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(pet->mouth, lv_color_hex(0x15130C), LV_PART_MAIN);
    lv_obj_set_style_pad_all(pet->mouth, 0, 0);

    pet->blink = lv_timer_create(hv_pet_blink_cb, 300, pet);
    lv_obj_add_event_cb(root, hv_pet_delete_cb, LV_EVENT_DELETE, pet);
    lv_obj_set_user_data(root, pet);
    return root;
}

void hv_pet_set_mood(lv_obj_t *root, int mood)
{
    hv_pet_t *pet = root ? lv_obj_get_user_data(root) : NULL;
    if (!pet || !pet->mouth) {
        return;
    }
    /* 说话时嘴张大一点 */
    if (mood == 2) {
        lv_arc_set_bg_angles(pet->mouth, 0, 180);
    } else if (mood == 1) {
        lv_arc_set_bg_angles(pet->mouth, 10, 170);
    } else {
        lv_arc_set_bg_angles(pet->mouth, 30, 150);
    }
}
