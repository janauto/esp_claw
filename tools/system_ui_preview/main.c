/*
 * Host-side preview of the firmware's system UI screens.
 *
 * Builds the real ui_squareline / system_ui screen code against LVGL on the
 * host, renders one screen into an RGB565 framebuffer of the requested size and
 * writes it out as a BMP. Lets layout changes be checked without flashing.
 *
 *   system_ui_preview <width> <height> <screen> <out.bmp>
 *
 * screen: home | boot | businesscard | meeting | infrared | workbuddy
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "src/display/lv_display_private.h"

#include "system_ui_private.h"
#include "ui_squareline.h"

#ifndef PREVIEW_FONT_PATH
#define PREVIEW_FONT_PATH ""
#endif

static uint16_t *s_fb;
static int32_t s_w, s_h;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    const uint16_t *src = (const uint16_t *)px;
    int32_t w = lv_area_get_width(area);

    for (int32_t y = area->y1; y <= area->y2; y++) {
        memcpy(&s_fb[y * s_w + area->x1], src, (size_t)w * 2);
        src += w;
    }
    lv_display_flush_ready(disp);
}

static int write_bmp(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t row = (uint32_t)s_w * 3;
    uint32_t pad = (4 - row % 4) % 4;
    uint32_t data = (row + pad) * (uint32_t)s_h;
    uint8_t hdr[54] = { 'B', 'M' };

    if (!f) {
        return -1;
    }
    *(uint32_t *)&hdr[2] = 54 + data;
    *(uint32_t *)&hdr[10] = 54;
    *(uint32_t *)&hdr[14] = 40;
    *(int32_t *)&hdr[18] = s_w;
    *(int32_t *)&hdr[22] = -s_h;            /* top-down */
    *(uint16_t *)&hdr[26] = 1;
    *(uint16_t *)&hdr[28] = 24;
    *(uint32_t *)&hdr[34] = data;
    fwrite(hdr, 1, sizeof(hdr), f);
    for (int32_t y = 0; y < s_h; y++) {
        for (int32_t x = 0; x < s_w; x++) {
            uint16_t c = s_fb[y * s_w + x];
            uint8_t bgr[3] = {
                (uint8_t)((c & 0x1F) << 3),
                (uint8_t)(((c >> 5) & 0x3F) << 2),
                (uint8_t)(((c >> 11) & 0x1F) << 3),
            };
            fwrite(bgr, 1, 3, f);
        }
        for (uint32_t p = 0; p < pad; p++) {
            fputc(0, f);
        }
    }
    fclose(f);
    return 0;
}

/* The newest top-level screen; the create functions keep theirs private. */
static lv_obj_t *newest_screen(void)
{
    lv_display_t *disp = lv_display_get_default();
    return disp->screen_cnt ? disp->screens[disp->screen_cnt - 1] : NULL;
}

static void render(void)
{
    /* Let anything the screens schedule settle, then repaint everything. */
    for (int i = 0; i < 50; i++) {
        lv_tick_inc(20);
        lv_timer_handler();
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
}

int main(int argc, char **argv)
{
    static uint8_t buf[480 * 800 * 2];
    const char *screen;
    lv_display_t *disp;
    lv_obj_t *scr = NULL;

    if (argc != 5) {
        fprintf(stderr, "usage: %s <width> <height> <screen> <out.bmp>\n", argv[0]);
        return 2;
    }
    s_w = atoi(argv[1]);
    s_h = atoi(argv[2]);
    screen = argv[3];
    s_fb = calloc((size_t)s_w * s_h, 2);

    lv_init();
    disp = lv_display_create(s_w, s_h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    s_ui.width = (uint32_t)s_w;
    s_ui.height = (uint32_t)s_h;
    s_ui.started = true;
    if (PREVIEW_FONT_PATH[0]) {
        s_ui.font = lv_tiny_ttf_create_file("S:" PREVIEW_FONT_PATH, SYSTEM_UI_DEFAULT_FONT_SIZE);
        s_ui.clock_font = lv_tiny_ttf_create_file("S:" PREVIEW_FONT_PATH, SYSTEM_UI_CLOCK_FONT_SIZE);
    }

    if (strcmp(screen, "home") == 0) {
        ui_squareline_set_img_base_path(PREVIEW_IMG_BASE);
        scr = ui_squareline_init();
    } else if (strcmp(screen, "boot") == 0) {
        system_ui_create_boot_locked();
        scr = newest_screen();
    } else if (strcmp(screen, "businesscard") == 0) {
        system_ui_create_businesscard_locked();
        scr = newest_screen();
    } else if (strcmp(screen, "meeting") == 0) {
        system_ui_create_meeting_minutes_locked();
        scr = newest_screen();
    } else if (strcmp(screen, "infrared") == 0) {
        system_ui_create_infrared_remote_locked();
        scr = newest_screen();
    } else if (strcmp(screen, "workbuddy") == 0) {
        system_ui_create_workbuddy_locked();
        scr = newest_screen();
    }
    if (scr == NULL) {
        fprintf(stderr, "unknown screen or create failed: %s\n", screen);
        return 1;
    }
    lv_screen_load(scr);
    render();
    return write_bmp(argv[4]) == 0 ? 0 : 1;
}
