#pragma once
#include "lvgl.h"
#include "cJSON.h"
typedef struct clip_reader_board_t *clip_reader_board_handle_t;
clip_reader_board_handle_t clip_reader_board_create(lv_obj_t *screen,bool weather);
void clip_reader_board_update(clip_reader_board_handle_t board,const char *raw,const char *status);
void clip_reader_board_delete(clip_reader_board_handle_t board);
void clip_reader_board_key(clip_reader_board_handle_t board,const char *key);
bool clip_reader_board_refresh_requested(clip_reader_board_handle_t board);

char *clip_reader_board_take_action(clip_reader_board_handle_t board);
void clip_reader_board_action_result(clip_reader_board_handle_t board,const char *result);

cJSON *clip_reader_board_inspect(clip_reader_board_handle_t board);
