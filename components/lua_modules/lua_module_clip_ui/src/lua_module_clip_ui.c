#include "lua_module_clip_ui.h"
#include "cap_lua.h"
#include "clip_reader_board.h"
#include "clip_stock_board.h"
#include "display_service.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#define CLIP_UI_STATE "clip_ui.state"
typedef struct {
    display_service_session_handle_t session;
    lv_obj_t *screen, *title, *value, *help;
    clip_stock_board_handle_t stocks;
    clip_reader_board_handle_t reader;
    bool open, exit_requested;
} clip_ui_state_t;
static clip_ui_state_t *s_active;
static atomic_bool s_running;
static clip_ui_key_cb_t s_key_callback;
static void *s_key_ctx;
static clip_ui_feed_cb_t s_feed_callback;
static clip_ui_feed_refresh_cb_t s_feed_refresh;
static void *s_feed_ctx;
static clip_ui_stock_add_cb_t s_stock_add;
static clip_ui_action_cb_t s_action;

static clip_ui_state_t *state(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, CLIP_UI_STATE);
    clip_ui_state_t *s = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return s;
}
static void cleanup(lua_State *L) {
    clip_ui_state_t *s = state(L);
    if (!s || !s->open) {
        return;
    }
    s->open = false;
    atomic_store(&s_running, false);
    if (s_active == s) {
        s_active = NULL;
    }
    if (s->session) {
        display_service_close(s->session);
    }
    s->session = NULL;
    if (s->screen && display_service_lock() == ESP_OK) {
        clip_stock_board_delete(s->stocks);
        s->stocks = NULL;
        clip_reader_board_delete(s->reader);
        s->reader = NULL;
        lv_obj_delete(s->screen);
        s->screen = NULL;
        display_service_unlock();
    }
}
static int close_ui(lua_State *L) {
    cleanup(L);
    return 0;
}
static int open_ui(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    uint32_t color = (uint32_t)luaL_optinteger(L, 2, 0x246BFD);
    cleanup(L);
    clip_ui_state_t *s = state(L);
    if (!s) {
        s = lua_newuserdatauv(L, sizeof(*s), 0);
        *s = (clip_ui_state_t){0};
        luaL_setmetatable(L, CLIP_UI_STATE);
        lua_setfield(L, LUA_REGISTRYINDEX, CLIP_UI_STATE);
    }
    display_service_session_config_t config = {.owner_name = "lua-clip-app",
                                               .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
                                               .flags =
                                                   DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE};
    esp_err_t err = display_service_open(&config, &s->session);
    if (err != ESP_OK) {
        return luaL_error(L, "display open: %s", esp_err_to_name(err));
    }
    s->open = true;
    s->exit_requested = false;
    /* Drop keys left by a previous app session. */
    while (s_key_callback && s_key_callback(s_key_ctx)) {}
    atomic_store(&s_running, true);
    s_active = s;
    err = display_service_lock();
    if (err != ESP_OK) {
        cleanup(L);
        return luaL_error(L, "display lock failed");
    }
    s->screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s->screen, lv_color_hex(0xF3F6FB), 0);
    /* Reuse the system's loaded Chinese font, which lives for the UI lifetime. */
    lv_obj_set_style_text_font(s->screen, lv_obj_get_style_text_font(lv_layer_top(), 0), 0);
    s->title = lv_label_create(s->screen);
    lv_obj_set_width(s->title, LV_PCT(88));
    lv_label_set_long_mode(s->title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s->title, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(s->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s->title, title);
    lv_obj_align(s->title, LV_ALIGN_TOP_MID, 0, 85);
    s->value = lv_label_create(s->screen);
    lv_obj_set_width(s->value, LV_PCT(88));
    lv_label_set_long_mode(s->value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s->value, lv_color_hex(0x172B4D), 0);
    lv_obj_set_style_text_align(s->value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s->value, "Ready");
    lv_obj_center(s->value);
    s->help = lv_label_create(s->screen);
    lv_obj_set_width(s->help, LV_PCT(86));
    lv_label_set_long_mode(s->help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s->help, lv_color_hex(0x65758B), 0);
    lv_obj_set_style_text_align(s->help, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s->help, "SET: Home");
    lv_obj_align(s->help, LV_ALIGN_BOTTOM_MID, 0, -85);
    err = display_service_session_load_screen_locked(s->session, s->screen);
    display_service_unlock();
    if (err != ESP_OK) {
        cleanup(L);
        return luaL_error(L, "display load failed");
    }
    return 0;
}
static int card(lua_State *L) {
    const char *title = luaL_checkstring(L, 1), *value = luaL_checkstring(L, 2),
               *help = luaL_optstring(L, 3, "SET: Home");
    clip_ui_state_t *s = state(L);
    if (!s || !s->open) {
        return luaL_error(L, "call ui.open first");
    }
    if (display_service_lock() != ESP_OK) {
        return luaL_error(L, "display lock failed");
    }
    lv_label_set_text(s->title, title);
    lv_label_set_text(s->value, value);
    lv_label_set_text(s->help, help);
    lv_obj_align(s->title, LV_ALIGN_TOP_MID, 0, 85);
    lv_obj_center(s->value);
    lv_obj_align(s->help, LV_ALIGN_BOTTOM_MID, 0, -85);
    display_service_unlock();
    return 0;
}
static int key(lua_State *L) {
    const char *name = s_key_callback ? s_key_callback(s_key_ctx) : NULL;
    if (name && !strcmp(name, "exit")) {
        clip_ui_state_t *s = state(L);
        if (s) s->exit_requested = true;
        name = NULL;
    }
    if (name) {
        lua_pushstring(L, name);
    } else {
        lua_pushnil(L);
    }
    return 1;
}
static int stock_dashboard(lua_State *L) {
    const char *raw = luaL_optstring(L, 1, "");
    int selected = (int)luaL_optinteger(L, 2, 1) - 1;
    const char *status = luaL_optstring(L, 3, "等待联网更新");
    clip_ui_state_t *s = state(L);
    if (!s || !s->open) {
        return luaL_error(L, "call ui.open first");
    }
    if (display_service_lock() != ESP_OK) {
        return luaL_error(L, "display lock failed");
    }
    if (!s->stocks) {
        s->stocks = clip_stock_board_create(s->screen);
    }
    if (s->stocks) {
        clip_stock_board_update(s->stocks, raw, selected, status);
    }
    display_service_unlock();
    if (!s->stocks) {
        return luaL_error(L, "stock dashboard allocation failed");
    }
    return 0;
}
static int stock_selection(lua_State *L) {
    clip_ui_state_t *s = state(L);
    int selected = 1;
    if (s && s->stocks && display_service_lock() == ESP_OK) {
        selected = clip_stock_board_selected(s->stocks) + 1;
        display_service_unlock();
    }
    lua_pushinteger(L, selected);
    return 1;
}
static int stock_refresh(lua_State *L) {
    clip_ui_state_t *s = state(L);
    bool refresh = false;
    if (s && s->stocks && display_service_lock() == ESP_OK) {
        refresh = clip_stock_board_refresh_requested(s->stocks);
        display_service_unlock();
    }
    lua_pushboolean(L, refresh);
    return 1;
}
static int feed_dashboard(lua_State *L) {
    const char *kind = luaL_checkstring(L, 1), *raw = luaL_optstring(L, 2, ""),
               *status = luaL_optstring(L, 3, ""), *key = luaL_optstring(L, 4, "");
    clip_ui_state_t *s = state(L);
    if (!s || !s->open) {
        return luaL_error(L, "call ui.open first");
    }
    if (strcmp(kind, "stocks") && strcmp(kind, "weather") && strcmp(kind, "aihot")) {
        return luaL_error(L, "invalid feed");
    }
    char *action_result = NULL;
    if (s_action) {
        s_action("{\"op\":\"status\"}", &action_result, s_feed_ctx);
    }
    if (display_service_lock() != ESP_OK) {
        free(action_result);
        return luaL_error(L, "display lock failed");
    }
    lv_obj_add_flag(s->title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s->value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s->help, LV_OBJ_FLAG_HIDDEN);
    char symbol[20] = {0};
    char *action_request = NULL;
    if (!strcmp(kind, "stocks")) {
        if (!s->stocks) {
            s->stocks = clip_stock_board_create(s->screen);
        }
        clip_stock_board_update(s->stocks, raw, 0, status);
        if (*key) {
            clip_stock_board_key(s->stocks, key);
        }
        clip_stock_board_action_result(s->stocks, action_result);
        action_request = clip_stock_board_take_action(s->stocks);
        const char *query = clip_stock_board_query(s->stocks);
        if (query) {
            snprintf(symbol, sizeof(symbol), "%s", query);
        }
    } else {
        if (!s->reader) {
            s->reader = clip_reader_board_create(s->screen, !strcmp(kind, "weather"));
        }
        clip_reader_board_update(s->reader, raw, status);
        if (*key) {
            clip_reader_board_key(s->reader, key);
        }
        clip_reader_board_action_result(s->reader, action_result);
        action_request = clip_reader_board_take_action(s->reader);
    }
    bool allocated = s->stocks || s->reader;
    display_service_unlock();
    free(action_result);
    if (action_request) {
        char *response = NULL;
        if (s_action) {
            s_action(action_request, &response, s_feed_ctx);
        }
        free(response);
        free(action_request);
    }
    if (*symbol) {
        bool ok = s_stock_add && s_stock_add(symbol, s_feed_ctx) == ESP_OK;
        if (display_service_lock() == ESP_OK) {
            clip_stock_board_query_result(s->stocks, ok);
            display_service_unlock();
        }
    }
    if (!allocated) {
        return luaL_error(L, "feed display allocation failed");
    }
    return 0;
}
static int feed_requested(lua_State *L) {
    clip_ui_state_t *s = state(L);
    bool refresh = false;
    if (s && display_service_lock() == ESP_OK) {
        refresh =
            clip_stock_board_refresh_requested(s->stocks) || clip_reader_board_refresh_requested(s->reader);
        display_service_unlock();
    }
    lua_pushboolean(L, refresh);
    return 1;
}
static int millis(lua_State *L) {
    lua_pushinteger(L, esp_timer_get_time() / 1000);
    return 1;
}
static int read_feed(lua_State *L) {
    const char *id = luaL_checkstring(L, 1);
    char *text = NULL;
    int64_t age = -1;
    esp_err_t err = s_feed_callback ? s_feed_callback(id, &text, &age, s_feed_ctx) : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        free(text);
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushstring(L, text);
    free(text);
    lua_pushinteger(L, age);
    return 2;
}
static int refresh_feed(lua_State *L) {
    const char *id = luaL_checkstring(L, 1);
    lua_pushboolean(L, s_feed_refresh && s_feed_refresh(id, s_feed_ctx));
    return 1;
}
static int running(lua_State *L) {
    clip_ui_state_t *s = state(L);
    lua_pushboolean(L, s && s->open && !s->exit_requested && !cap_lua_runtime_stop_requested(L) &&
                           display_service_session_is_active(s->session));
    return 1;
}
static int luaopen_clip_ui(lua_State *L) {
    luaL_newmetatable(L, CLIP_UI_STATE);
    lua_pushcfunction(L, close_ui);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    const luaL_Reg functions[] = {{"open", open_ui},
                                  {"card", card},
                                  {"key", key},
                                  {"millis", millis},
                                  {"running", running},
                                  {"close", close_ui},
                                  {"feed", read_feed},
                                  {"feed_refresh", refresh_feed},
                                  {"feed_dashboard", feed_dashboard},
                                  {"feed_refresh_requested", feed_requested},
                                  {"stock_dashboard", stock_dashboard},
                                  {"stock_selection", stock_selection},
                                  {"stock_refresh_requested", stock_refresh},
                                  {NULL, NULL}};
    luaL_newlib(L, functions);
    return 1;
}
esp_err_t lua_module_clip_ui_register(clip_ui_key_cb_t callback, void *ctx) {
    s_key_callback = callback;
    s_key_ctx = ctx;
    esp_err_t err = cap_lua_register_exit_cleanup(cleanup);
    return err == ESP_OK ? cap_lua_register_module("clip_ui", luaopen_clip_ui) : err;
}
void lua_module_clip_ui_register_feeds(clip_ui_feed_cb_t read, clip_ui_feed_refresh_cb_t refresh, void *ctx) {
    s_feed_callback = read;
    s_feed_refresh = refresh;
    s_feed_ctx = ctx;
}

void lua_module_clip_ui_register_stocks(clip_ui_stock_add_cb_t add) {
    s_stock_add = add;
}

void lua_module_clip_ui_register_actions(clip_ui_action_cb_t action) {
    s_action = action;
}

esp_err_t lua_module_clip_ui_inspect(char **json) {
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }
    *json = NULL;
    if (display_service_lock() != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = s_active ? (s_active->stocks ? clip_stock_board_inspect(s_active->stocks)
                                            : clip_reader_board_inspect(s_active->reader))
                        : NULL;
    if (j) {
        *json = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
    }
    display_service_unlock();
    return *json ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool lua_module_clip_ui_active(void) { return atomic_load(&s_running); }
