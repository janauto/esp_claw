#pragma once
#include "lvgl.h"
#include "cJSON.h"
typedef struct { void *data;lv_font_t *fonts[6],*symbols[6]; } clip_fonts_t;
void cf_load(clip_fonts_t *fonts);
void cf_free(clip_fonts_t *fonts);
lv_obj_t *cf_box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius);
lv_obj_t *cf_label(clip_fonts_t *fonts,lv_obj_t *parent,int x,int y,int w,const char *text,int size,uint32_t color);
lv_obj_t *cf_button(clip_fonts_t *fonts,lv_obj_t *parent,int x,int y,int w,int h,const char *text,uint32_t bg,lv_event_cb_t cb,void *ctx);
const char *cf_str(const cJSON *j,const char *key);
double cf_num(const cJSON *j,const char *key);
void cf_wrap(lv_obj_t *label);

void cf_scroll(lv_obj_t *obj,lv_dir_t direction);
void cf_action(char **pending,uint32_t *id,const char *op,const char *query,const char *key,const cJSON *value);
