/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Carousel icon for installed apps.
 *
 * Apps installed onto the data partition (e.g. from the Web Lua Studio) carry no
 * carousel artwork, so they share one icon in the style of the built-in ones: a
 * dark rounded plate with a grey rim, a yellow "</>" for the generated Lua code,
 * and a small caption. It is drawn once per variant, exactly like the 2048 icon
 * (see ui_icon_2048.c for why the canvas is detached before it is deleted).
 */

#include "ui_icon_app.h"

#include <stddef.h>

#include "ui.h"

/* For lv_canvas_t, to detach the buffer before deleting the canvas. */
#include "src/widgets/canvas/lv_canvas_private.h"

/* Matches the artwork icons, so ui.c's zoom values apply unchanged. */
#define ICON_SIDE           256

#define PLATE_INSET         20
#define PLATE_RADIUS        40
#define PLATE_RIM           4

#define GLYPH_WIDTH         14
#define CAPTION_Y           194
#define CAPTION_H           24
#define CAPTION_TEXT        "LUA APP"

#define COLOR_PLATE         0x101410
#define COLOR_RIM           0x5A5E5A
#define COLOR_GLYPH         0xF0C430
#define COLOR_CAPTION       0xD8DCD6

#define DIM_MIX_PERCENT     58

#define ICON_COLOR_FORMAT   LV_COLOR_FORMAT_RGB565

static lv_draw_buf_t *s_icon_selected;
static lv_draw_buf_t *s_icon_normal;

/* "<", "/", ">" as polylines, in icon pixels. */
static const lv_point_precise_t s_glyph_lt[]    = { { 98, 92 }, { 64, 126 }, { 98, 160 } };
static const lv_point_precise_t s_glyph_slash[] = { { 142, 80 }, { 114, 172 } };
static const lv_point_precise_t s_glyph_gt[]    = { { 158, 92 }, { 192, 126 }, { 158, 160 } };

static lv_color_t icon_app_shade(uint32_t hex, bool selected)
{
    lv_color_t color = lv_color_hex(hex);

    if (selected) {
        return color;
    }
    return lv_color_mix(lv_color_black(), color, (255 * DIM_MIX_PERCENT) / 100);
}

static void icon_app_polyline(lv_layer_t *layer, const lv_point_precise_t *points, size_t count,
                              lv_color_t color)
{
    lv_draw_line_dsc_t dsc;

    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = GLYPH_WIDTH;
    dsc.round_start = 1;
    dsc.round_end = 1;
    for (size_t i = 0; i + 1 < count; i++) {
        dsc.p1 = points[i];
        dsc.p2 = points[i + 1];
        lv_draw_line(layer, &dsc);
    }
}

static lv_draw_buf_t *icon_app_render(lv_obj_t *parent, bool selected)
{
    lv_draw_rect_dsc_t plate;
    lv_draw_label_dsc_t caption;
    lv_draw_buf_t *buf;
    lv_obj_t *canvas;
    lv_layer_t layer;
    lv_area_t area;
    lv_color_t glyph = icon_app_shade(COLOR_GLYPH, selected);

    buf = lv_draw_buf_create(ICON_SIDE, ICON_SIDE, ICON_COLOR_FORMAT, 0);
    if (buf == NULL) {
        return NULL;
    }

    canvas = lv_canvas_create(parent);
    if (canvas == NULL) {
        lv_draw_buf_destroy(buf);
        return NULL;
    }
    lv_canvas_set_draw_buf(canvas, buf);

    /* Black to match the carousel backdrop: RGB565 carries no alpha. */
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_rect_dsc_init(&plate);
    plate.bg_color = icon_app_shade(COLOR_PLATE, selected);
    plate.bg_opa = LV_OPA_COVER;
    plate.radius = PLATE_RADIUS;
    plate.border_color = icon_app_shade(COLOR_RIM, selected);
    plate.border_width = PLATE_RIM;
    plate.border_opa = LV_OPA_COVER;
    area.x1 = PLATE_INSET;
    area.y1 = PLATE_INSET;
    area.x2 = ICON_SIDE - PLATE_INSET - 1;
    area.y2 = ICON_SIDE - PLATE_INSET - 1;
    lv_draw_rect(&layer, &plate, &area);

    icon_app_polyline(&layer, s_glyph_lt, sizeof(s_glyph_lt) / sizeof(s_glyph_lt[0]), glyph);
    icon_app_polyline(&layer, s_glyph_slash, sizeof(s_glyph_slash) / sizeof(s_glyph_slash[0]), glyph);
    icon_app_polyline(&layer, s_glyph_gt, sizeof(s_glyph_gt) / sizeof(s_glyph_gt[0]), glyph);

    lv_draw_label_dsc_init(&caption);
    caption.font = &ui_font_Font1;
    caption.color = icon_app_shade(COLOR_CAPTION, selected);
    caption.align = LV_TEXT_ALIGN_CENTER;
    caption.text = CAPTION_TEXT;
    area.x1 = PLATE_INSET;
    area.y1 = CAPTION_Y;
    area.x2 = ICON_SIDE - PLATE_INSET - 1;
    area.y2 = CAPTION_Y + CAPTION_H - 1;
    lv_draw_label(&layer, &caption, &area);

    lv_canvas_finish_layer(canvas, &layer);
    ((lv_canvas_t *)canvas)->draw_buf = NULL;
    lv_obj_delete(canvas);
    return buf;
}

const void *ui_icon_app_src(lv_obj_t *parent, bool selected)
{
    lv_draw_buf_t **slot = selected ? &s_icon_selected : &s_icon_normal;

    if (*slot == NULL && parent != NULL) {
        *slot = icon_app_render(parent, selected);
    }
    return *slot;
}

void ui_icon_app_release(void)
{
    if (s_icon_selected != NULL) {
        lv_draw_buf_destroy(s_icon_selected);
        s_icon_selected = NULL;
    }
    if (s_icon_normal != NULL) {
        lv_draw_buf_destroy(s_icon_normal);
        s_icon_normal = NULL;
    }
}
