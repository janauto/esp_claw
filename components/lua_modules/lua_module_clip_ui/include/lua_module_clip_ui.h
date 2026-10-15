#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
typedef const char *(*clip_ui_key_cb_t)(void *ctx);
typedef esp_err_t (*clip_ui_feed_cb_t)(const char *id, char **payload, int64_t *age_ms, void *ctx);
typedef bool (*clip_ui_feed_refresh_cb_t)(const char *id, void *ctx);
esp_err_t lua_module_clip_ui_register(clip_ui_key_cb_t callback, void *ctx);
void lua_module_clip_ui_register_feeds(clip_ui_feed_cb_t read, clip_ui_feed_refresh_cb_t refresh, void *ctx);

typedef esp_err_t (*clip_ui_stock_add_cb_t)(const char *symbol,void *ctx);
void lua_module_clip_ui_register_stocks(clip_ui_stock_add_cb_t add);

typedef esp_err_t (*clip_ui_action_cb_t)(const char *request,char **response,void *ctx);
void lua_module_clip_ui_register_actions(clip_ui_action_cb_t action);

esp_err_t lua_module_clip_ui_inspect(char **json);
bool lua_module_clip_ui_active(void);
