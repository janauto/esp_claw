#include "clip_feed_widgets.h"
#include "claw_paths.h"
extern const unsigned char clip_symbols_ttf[] asm("_binary_clip_fallback_ttf_start");
extern const unsigned char clip_symbols_ttf_end[] asm("_binary_clip_fallback_ttf_end");
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static const int sizes[] = {14, 18, 22, 28, 40, 88};
const char *cf_str(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
double cf_num(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : NAN;
}
void cf_load(clip_fonts_t *f) {
    char *path = malloc(512);
    if (!path) {
        return;
    }
    FILE *fp = NULL;
    const claw_path_root_t roots[] = {CLAW_PATH_SYSTEM, CLAW_PATH_DATA};
    for (int i = 0; i < 2 && !fp; i++) {
        if (claw_paths_join(roots[i], "fonts/NotoSansSC-Regular-sub.ttf", path, 512) == ESP_OK) {
            fp = fopen(path, "rb");
        }
    }
    free(path);
    if (!fp) {
        return;
    }
    if (fseek(fp, 0, SEEK_END)) {
        fclose(fp);
        return;
    }
    long n = ftell(fp);
    rewind(fp);
    if (n <= 0 || n > 4 * 1024 * 1024) {
        fclose(fp);
        return;
    }
    f->data = malloc((size_t)n);
    if (!f->data || fread(f->data, 1, (size_t)n, fp) != (size_t)n) {
        free(f->data);
        f->data = NULL;
        fclose(fp);
        return;
    }
    fclose(fp);
    for (int i = 0; i < 6; i++) {
        f->fonts[i] = lv_tiny_ttf_create_data_ex(f->data, (size_t)n, sizes[i], LV_FONT_KERNING_NORMAL, 128);
        f->symbols[i] =
            lv_tiny_ttf_create_data_ex(clip_symbols_ttf, (size_t)(clip_symbols_ttf_end - clip_symbols_ttf),
                                       sizes[i], LV_FONT_KERNING_NORMAL, 16);
        if (f->fonts[i]) {
            f->fonts[i]->fallback = f->symbols[i];
        }
    }
}
void cf_free(clip_fonts_t *f) {
    for (int i = 0; i < 6; i++) {
        if (f->fonts[i]) {
            lv_tiny_ttf_destroy(f->fonts[i]);
        }
        if (f->symbols[i]) {
            lv_tiny_ttf_destroy(f->symbols[i]);
        }
    }
    free(f->data);
}
lv_obj_t *cf_box(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}
lv_obj_t *cf_label(clip_fonts_t *f, lv_obj_t *p, int x, int y, int w, const char *text, int size,
                   uint32_t color) {
    lv_obj_t *o = lv_label_create(p);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(
        o, f->fonts[size] ? f->fonts[size] : lv_obj_get_style_text_font(lv_layer_top(), 0), 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_text(o, text);
    return o;
}
void cf_wrap(lv_obj_t *o) {
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(o, 9, 0);
}
lv_obj_t *cf_button(clip_fonts_t *f, lv_obj_t *p, int x, int y, int w, int h, const char *text, uint32_t bg,
                    lv_event_cb_t cb, void *ctx) {
    lv_obj_t *o = cf_box(p, x, y, w, h, bg, h / 2);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *l = cf_label(f, o, 0, 0, w, text, 1, 0xF5F5F7);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return o;
}

void cf_scroll(lv_obj_t *obj, lv_dir_t direction) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                             LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_scroll_dir(obj, direction);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
}
void cf_action(char **pending, uint32_t *id, const char *op, const char *query, const char *key,
               const cJSON *value) {
    static uint32_t sequence = 0;
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return;
    }
    *id = ++sequence;
    cJSON_AddStringToObject(j, "op", op);
    cJSON_AddNumberToObject(j, "requestId", *id);
    if (query) {
        cJSON_AddStringToObject(j, "query", query);
    }
    if (key && value) {
        cJSON_AddItemToObject(j, key, cJSON_Duplicate(value, true));
    }
    char *raw = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (raw) {
        free(*pending);
        *pending = raw;
    }
}
