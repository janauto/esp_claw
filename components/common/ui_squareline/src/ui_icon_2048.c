/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The 2048 carousel icon, drawn with LVGL instead of loaded from the SD card.
 *
 * Every other page points at a pre-rendered <name>.bin on the card. This one is
 * generated at runtime, which works because the carousel slots are lv_image
 * widgets and lv_image_set_src() accepts an lv_draw_buf_t directly
 * (lv_image.c:196) - so a buffer we render into is a drop-in replacement for a
 * file, and the slide/zoom animations in ui.c need no changes.
 *
 * Size is fixed at 256x256 to match the artwork icons. The zoom values in
 * _ui_carousel_reset() (98 for the neighbours, 128 for the centre) are absolute
 * fractions of 256, so a differently sized source would render at a different
 * on-screen size than the icons it sits beside.
 *
 * The icon is pure geometry: a rounded board with a 2x2 tile grid in the game's
 * signature palette. Drawing the digits was the obvious alternative, but the
 * only fonts built into this firmware are Montserrat 14 and ui_font_Font1
 * (line_height 16); at the 38%-50% scale these slots use, that text lands at
 * 5-7 px on screen and is unreadable. The colours carry the recognition instead.
 */
#include "ui_icon_2048.h"

#include <stddef.h>

/* For lv_canvas_t, to detach the buffer before deleting the canvas. */
#include "src/widgets/canvas/lv_canvas_private.h"

/* Matches the artwork icons, so ui.c's zoom values apply unchanged. */
#define ICON_SIDE       256

/* Board plate inset from the canvas edge, and its corner rounding. */
#define BOARD_INSET     14
#define BOARD_RADIUS    26

/* 2x2 tile grid inside the plate. */
#define TILE_GAP        14
#define TILE_RADIUS     12

/* Palette lifted from game_2048.lua so the icon and the running game agree.
 * The board plate is its PANEL colour; the four tiles are the values a player
 * sees most: the two opening tiles, the mid-game orange, and the 2048 gold. */
#define COLOR_BOARD     0x9D8F84    /* rgb(157, 143, 132) - PANEL   */
#define COLOR_TILE_2    0xEEE4DA    /* rgb(238, 228, 218) - [2]     */
#define COLOR_TILE_4    0xEDE0C8    /* rgb(237, 224, 200) - [4]     */
#define COLOR_TILE_64   0xF65E3B    /* rgb(246,  94,  59) - [64]    */
#define COLOR_TILE_2048 0xEDC22E    /* rgb(237, 194,  46) - [2048]  */

/* How far the unselected variant is mixed towards the backdrop. The neighbour
 * slots are also scaled down and unframed, so they only need to recede a little;
 * dimming them too hard makes the carousel look like it has empty slots. */
#define DIM_MIX_PERCENT 58

/* RGB565 rather than ARGB8888: the icon is opaque and the carousel sits on a
 * black backdrop, so the transparent corners an alpha channel would buy are
 * indistinguishable from the black the rounded plate leaves behind - at half the
 * memory (128 KiB per variant instead of 256 KiB). */
#define ICON_COLOR_FORMAT LV_COLOR_FORMAT_RGB565

/* Drawn once each, then reused by every slot that shows this page's icon. */
static lv_draw_buf_t *s_icon_selected;
static lv_draw_buf_t *s_icon_normal;

static lv_color_t icon_2048_shade(uint32_t hex, bool selected)
{
    lv_color_t color = lv_color_hex(hex);

    if (selected) {
        return color;
    }
    return lv_color_mix(lv_color_black(), color, (255 * DIM_MIX_PERCENT) / 100);
}

static void icon_2048_rect(lv_layer_t *layer, int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t radius, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t area;

    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    dsc.border_width = 0;

    area.x1 = x;
    area.y1 = y;
    area.x2 = x + w - 1;
    area.y2 = y + h - 1;
    lv_draw_rect(layer, &dsc, &area);
}

/* Render one variant into a fresh buffer.
 *
 * A canvas is used purely as the drawing target: lv_canvas_init_layer() points a
 * layer at our buffer, the rectangles are drawn through it, and
 * lv_canvas_finish_layer() flushes the queued draw tasks. The canvas is then
 * deleted without freeing the buffer, which outlives it and becomes the image
 * source.
 *
 * The buffer is detached before the delete. lv_canvas_destructor() (LVGL 9.5 and
 * 9.6) calls lv_image_cache_drop(&canvas->draw_buf) - the address of the field,
 * not the buffer - so both cache lookups read the low byte of our buffer pointer
 * as an image magic. Depending on where the allocator put the buffer that logs
 * "invalid magic" twice (low byte < 0x20, e.g. 0x10), or silently treats the
 * field as a file path and strcmp()s it against cached paths (0x20..0x7F). With
 * draw_buf NULL the destructor returns early. Nothing is lost by skipping its
 * drop: lv_canvas_set_draw_buf() already dropped the buffer, an allocated RGB565
 * buffer is used directly by the decoder rather than cached, and the header cache
 * only holds file sources. Drop the detach once LVGL passes canvas->draw_buf.
 */
static lv_draw_buf_t *icon_2048_render(lv_obj_t *parent, bool selected)
{
    lv_draw_buf_t *buf;
    lv_obj_t *canvas;
    lv_layer_t layer;
    int32_t plate;
    int32_t tile;
    int32_t origin;

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

    /* Black to match the carousel backdrop: whatever the rounded plate does not
     * cover has to disappear into it, since RGB565 carries no alpha. */
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    lv_canvas_init_layer(canvas, &layer);

    plate = ICON_SIDE - BOARD_INSET * 2;
    icon_2048_rect(&layer, BOARD_INSET, BOARD_INSET, plate, plate, BOARD_RADIUS,
                   icon_2048_shade(COLOR_BOARD, selected));

    /* Two columns and two rows with a gap on every side, so the grid keeps an
     * even margin inside the plate. */
    tile = (plate - TILE_GAP * 3) / 2;
    origin = BOARD_INSET + TILE_GAP;

    icon_2048_rect(&layer, origin, origin, tile, tile, TILE_RADIUS,
                   icon_2048_shade(COLOR_TILE_2, selected));
    icon_2048_rect(&layer, origin + tile + TILE_GAP, origin, tile, tile, TILE_RADIUS,
                   icon_2048_shade(COLOR_TILE_4, selected));
    icon_2048_rect(&layer, origin, origin + tile + TILE_GAP, tile, tile, TILE_RADIUS,
                   icon_2048_shade(COLOR_TILE_64, selected));
    /* The 2048 gold goes bottom-right, where the eye lands last. */
    icon_2048_rect(&layer, origin + tile + TILE_GAP, origin + tile + TILE_GAP, tile, tile,
                   TILE_RADIUS, icon_2048_shade(COLOR_TILE_2048, selected));

    lv_canvas_finish_layer(canvas, &layer);
    ((lv_canvas_t *)canvas)->draw_buf = NULL;
    lv_obj_delete(canvas);
    return buf;
}

const void *ui_icon_2048_src(lv_obj_t *parent, bool selected)
{
    lv_draw_buf_t **slot = selected ? &s_icon_selected : &s_icon_normal;

    if (*slot == NULL && parent != NULL) {
        *slot = icon_2048_render(parent, selected);
    }
    return *slot;
}

void ui_icon_2048_release(void)
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
