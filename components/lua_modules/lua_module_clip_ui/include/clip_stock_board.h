#pragma once
#include "lvgl.h"
#include "cJSON.h"
typedef struct clip_stock_board_t *clip_stock_board_handle_t;
clip_stock_board_handle_t clip_stock_board_create(lv_obj_t *screen);
void clip_stock_board_delete(clip_stock_board_handle_t board);
void clip_stock_board_update(clip_stock_board_handle_t board,const char *json,int selected,const char *status);
int clip_stock_board_selected(clip_stock_board_handle_t board);
bool clip_stock_board_refresh_requested(clip_stock_board_handle_t board);
void clip_stock_board_key(clip_stock_board_handle_t board,const char *key);
const char *clip_stock_board_query(clip_stock_board_handle_t board);
void clip_stock_board_query_result(clip_stock_board_handle_t board,bool ok);

char *clip_stock_board_take_action(clip_stock_board_handle_t board);
void clip_stock_board_action_result(clip_stock_board_handle_t board,const char *result);

cJSON *clip_stock_board_inspect(clip_stock_board_handle_t board);
