/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_button.h"

#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "cap_lua.h"

#ifdef CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT
#include "esp_board_manager.h"
#include "dev_button.h"
#endif /* CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT */

static const char *TAG = "lua_button";

#ifdef CONFIG_ADC_BUTTON_MAX_BUTTON_PER_CHANNEL
#define BTN_GROUP_MAX CONFIG_ADC_BUTTON_MAX_BUTTON_PER_CHANNEL
#else
#define BTN_GROUP_MAX 8
#endif

/* Enough slots for one full board group plus standalone GPIO buttons. */
#define BTN_MAX_HANDLES      (BTN_GROUP_MAX + 8)
#define BTN_EVENT_QUEUE_SIZE 32
#define BTN_HANDLE_METATABLE "lua_button_handle"
#define BTN_GROUP_METATABLE  "lua_button_group"

typedef struct {
    button_handle_t handle;
    button_event_t  event;
    uint8_t         repeat_count;
    uint32_t        pressed_time_ms;
} btn_event_item_t;

typedef struct {
    button_handle_t handle;
    const char     *label;                      /* board label, NULL for plain GPIO buttons */
    int             index;                      /* index inside a board group, -1 otherwise */
    int             lua_refs[BUTTON_EVENT_MAX]; /* LUA_NOREF if not registered */
    bool            cbs_registered[BUTTON_EVENT_MAX];
} btn_lua_reg_t;

typedef struct {
    button_handle_t handle;
    bool            owned; /* true when this module created the handle and must delete it */
} btn_lua_handle_ud_t;

typedef struct {
    uint8_t          num_buttons;
    button_handle_t  handles[BTN_GROUP_MAX];
    const char      *labels[BTN_GROUP_MAX];
} btn_lua_group_ud_t;

/* Resolved set of button handles a call applies to (one handle, or a whole group). */
typedef struct {
    int             count;
    button_handle_t handles[BTN_GROUP_MAX];
} btn_target_t;

static btn_lua_reg_t s_regs[BTN_MAX_HANDLES];
static int           s_num_regs = 0;
static QueueHandle_t s_queue    = NULL;

static const char *const s_event_names[BUTTON_EVENT_MAX] = {
    "press_down",
    "press_up",
    "press_repeat",
    "press_repeat_done",
    "single_click",
    "double_click",
    "multiple_click",
    "long_press_start",
    "long_press_hold",
    "long_press_up",
    "press_end",
};

static const char *btn_event_to_str(button_event_t ev)
{
    if ((unsigned)ev < BUTTON_EVENT_MAX) {
        return s_event_names[ev];
    }
    return "unknown";
}

static button_event_t btn_event_from_str(const char *s)
{
    for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
        if (strcmp(s, s_event_names[i]) == 0) {
            return (button_event_t)i;
        }
    }
    return BUTTON_EVENT_MAX;
}

static btn_lua_reg_t *btn_find_reg(button_handle_t handle)
{
    for (int i = 0; i < s_num_regs; i++) {
        if (s_regs[i].handle == handle) {
            return &s_regs[i];
        }
    }
    return NULL;
}

static btn_lua_reg_t *btn_add_reg(button_handle_t handle, const char *label, int index)
{
    btn_lua_reg_t *reg = btn_find_reg(handle);
    if (reg) {
        return reg;
    }
    if (s_num_regs >= BTN_MAX_HANDLES) {
        return NULL;
    }

    reg = &s_regs[s_num_regs++];
    memset(reg, 0, sizeof(*reg));
    reg->handle = handle;
    reg->label  = label;
    reg->index  = index;
    for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
        reg->lua_refs[i] = LUA_NOREF;
    }
    return reg;
}

static void btn_remove_reg(lua_State *L, btn_lua_reg_t *reg)
{
    int idx = (int)(reg - s_regs);
    if (idx < 0 || idx >= s_num_regs) {
        return;
    }

    for (int i = 0; i < BUTTON_EVENT_MAX; i++) {
        if (reg->cbs_registered[i]) {
            esp_err_t err = iot_button_unregister_cb(reg->handle, (button_event_t)i, NULL);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "unregister_cb failed for event %s: %s",
                         btn_event_to_str((button_event_t)i), esp_err_to_name(err));
            }
        }
        if (reg->lua_refs[i] != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[i]);
        }
    }

    s_num_regs--;
    if (idx != s_num_regs) {
        s_regs[idx] = s_regs[s_num_regs];
    }
    memset(&s_regs[s_num_regs], 0, sizeof(s_regs[s_num_regs]));
}

static btn_lua_handle_ud_t *lua_btn_check_ud(lua_State *L, int idx)
{
    btn_lua_handle_ud_t *ud =
        (btn_lua_handle_ud_t *)luaL_checkudata(L, idx, BTN_HANDLE_METATABLE);

    if (!ud || !ud->handle) {
        luaL_error(L, "button handle expected");
        return NULL;
    }
    return ud;
}

static button_handle_t lua_btn_check_handle(lua_State *L, int idx)
{
    return lua_btn_check_ud(L, idx)->handle;
}

/* Push a non-owning handle userdata: close/__gc must not delete a board-owned button. */
static void lua_btn_push_handle(lua_State *L, button_handle_t handle, bool owned)
{
    btn_lua_handle_ud_t *ud =
        (btn_lua_handle_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->handle = handle;
    ud->owned  = owned;
    luaL_getmetatable(L, BTN_HANDLE_METATABLE);
    lua_setmetatable(L, -2);
}

/*
 * Accept either a single button handle or a board group as the target of
 * on/off/close, so a whole ADC group can be subscribed in one call.
 */
static int lua_btn_resolve_target(lua_State *L, int idx, btn_target_t *out)
{
    out->count = 0;

    btn_lua_group_ud_t *group =
        (btn_lua_group_ud_t *)luaL_testudata(L, idx, BTN_GROUP_METATABLE);
    if (group) {
        for (int i = 0; i < group->num_buttons; i++) {
            if (group->handles[i]) {
                out->handles[out->count++] = group->handles[i];
            }
        }
        return out->count > 0 ? 0 : -1;
    }

    btn_lua_handle_ud_t *ud =
        (btn_lua_handle_ud_t *)luaL_testudata(L, idx, BTN_HANDLE_METATABLE);
    if (ud && ud->handle) {
        out->handles[out->count++] = ud->handle;
        return 0;
    }

    return -1;
}

static int lua_btn_handle_gc(lua_State *L)
{
    btn_lua_handle_ud_t *ud =
        (btn_lua_handle_ud_t *)luaL_testudata(L, 1, BTN_HANDLE_METATABLE);
    btn_lua_reg_t *reg = NULL;

    if (!ud || !ud->handle) {
        return 0;
    }

    reg = btn_find_reg(ud->handle);
    if (reg) {
        btn_remove_reg(L, reg);
    }
    /* Board-managed handles are deleted by the board manager, not here. */
    if (ud->owned) {
        iot_button_delete(ud->handle);
    }
    ud->handle = NULL;
    return 0;
}

static int lua_btn_group_gc(lua_State *L)
{
    btn_lua_group_ud_t *group =
        (btn_lua_group_ud_t *)luaL_testudata(L, 1, BTN_GROUP_METATABLE);

    if (!group) {
        return 0;
    }

    /* Drop subscriptions only; the board manager owns and deletes the handles. */
    for (int i = 0; i < group->num_buttons; i++) {
        if (!group->handles[i]) {
            continue;
        }
        btn_lua_reg_t *reg = btn_find_reg(group->handles[i]);
        if (reg) {
            btn_remove_reg(L, reg);
        }
        group->handles[i] = NULL;
    }
    group->num_buttons = 0;
    return 0;
}

static void btn_c_event_cb(void *btn_handle, void *user_data)
{
    (void)user_data;

    if (!s_queue) {
        return;
    }

    button_handle_t handle = (button_handle_t)btn_handle;
    btn_event_item_t item = {
        .handle          = handle,
        .event           = iot_button_get_event(handle),
        .repeat_count    = iot_button_get_repeat(handle),
        .pressed_time_ms = iot_button_get_pressed_time(handle),
    };

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped button event");
    }
}

static int lua_btn_new(lua_State *L)
{
    int gpio_num        = (int)luaL_checkinteger(L, 1);
    int active_level    = (int)luaL_optinteger(L, 2, 0);
    int long_press_ms   = (int)luaL_optinteger(L, 3, 0);
    int short_press_ms  = (int)luaL_optinteger(L, 4, 0);
    button_handle_t btn = NULL;

    if (active_level != 0 && active_level != 1) {
        lua_pushnil(L);
        lua_pushstring(L, "active_level must be 0 or 1");
        return 2;
    }

    button_config_t btn_cfg = {
        .long_press_time  = (uint16_t)long_press_ms,
        .short_press_time = (uint16_t)short_press_ms,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num          = gpio_num,
        .active_level      = (uint8_t)active_level,
        .enable_power_save = false,
        .disable_pull      = false,
    };

    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn);
    if (err != ESP_OK || btn == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "button.new failed for gpio %d: %s",
                        gpio_num, esp_err_to_name(err));
        return 2;
    }

    if (!btn_add_reg(btn, NULL, -1)) {
        iot_button_delete(btn);
        lua_pushnil(L);
        lua_pushfstring(L, "too many button handles (max %d)", BTN_MAX_HANDLES);
        return 2;
    }

    lua_btn_push_handle(L, btn, true);
    return 1;
}

/*
 * Open a button device declared in board_devices.yaml. For sub_type adc_multi
 * every button in the group is exposed together with its YAML button_labels
 * entry, which is what identifies the key that was pressed.
 */
static int lua_btn_open_group(lua_State *L)
{
#ifdef CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT
    const char *name = luaL_checkstring(L, 1);
    void *raw_handle = NULL;
    void *raw_config = NULL;

    esp_err_t err = esp_board_manager_get_device_handle(name, &raw_handle);
    if (err != ESP_OK || raw_handle == NULL) {
        /* Device may be declared with init_skip, bring it up on demand. */
        esp_err_t init_err = esp_board_manager_init_device_by_name(name);
        if (init_err != ESP_OK) {
            lua_pushnil(L);
            lua_pushfstring(L, "button.open_group('%s') init failed: %s",
                            name, esp_err_to_name(init_err));
            return 2;
        }
        err = esp_board_manager_get_device_handle(name, &raw_handle);
    }
    if (err != ESP_OK || raw_handle == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "button.open_group('%s') handle failed: %s",
                        name, esp_err_to_name(err));
        return 2;
    }

    err = esp_board_manager_get_device_config(name, &raw_config);
    if (err != ESP_OK || raw_config == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "button.open_group('%s') config failed: %s",
                        name, esp_err_to_name(err));
        return 2;
    }

    dev_button_handles_t *dev_handles = (dev_button_handles_t *)raw_handle;
    dev_button_config_t  *dev_cfg     = (dev_button_config_t *)raw_config;

    if (dev_handles->num_buttons == 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "button device '%s' exposes no buttons", name);
        return 2;
    }
    if (dev_handles->num_buttons > BTN_GROUP_MAX) {
        lua_pushnil(L);
        lua_pushfstring(L, "button device '%s' has %d buttons, max %d",
                        name, dev_handles->num_buttons, BTN_GROUP_MAX);
        return 2;
    }
    if (s_num_regs + dev_handles->num_buttons > BTN_MAX_HANDLES) {
        lua_pushnil(L);
        lua_pushfstring(L, "too many button handles (max %d)", BTN_MAX_HANDLES);
        return 2;
    }

    bool is_multi = dev_cfg->sub_type != NULL &&
                    strcmp(dev_cfg->sub_type, "adc_multi") == 0;

    btn_lua_group_ud_t *group =
        (btn_lua_group_ud_t *)lua_newuserdata(L, sizeof(*group));
    memset(group, 0, sizeof(*group));
    group->num_buttons = dev_handles->num_buttons;

    for (int i = 0; i < dev_handles->num_buttons; i++) {
        const char *label = NULL;
        if (is_multi) {
            label = dev_cfg->sub_cfg.adc.multi.button_labels[i];
        }
        if (label == NULL) {
            label = dev_cfg->name ? dev_cfg->name : name;
        }
        group->handles[i] = dev_handles->button_handles[i];
        group->labels[i]  = label;

        if (group->handles[i] && !btn_add_reg(group->handles[i], label, i)) {
            lua_pushnil(L);
            lua_pushfstring(L, "too many button handles (max %d)", BTN_MAX_HANDLES);
            return 2;
        }
    }

    luaL_getmetatable(L, BTN_GROUP_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
#else
    (void)L;
    lua_pushnil(L);
    lua_pushstring(L, "board button support is disabled");
    return 2;
#endif /* CONFIG_ESP_BOARD_DEV_BUTTON_SUPPORT */
}

static btn_lua_group_ud_t *lua_btn_check_group(lua_State *L, int idx)
{
    btn_lua_group_ud_t *group =
        (btn_lua_group_ud_t *)luaL_checkudata(L, idx, BTN_GROUP_METATABLE);

    if (!group || group->num_buttons == 0) {
        luaL_error(L, "button group expected");
        return NULL;
    }
    return group;
}

static int lua_btn_group_size(lua_State *L)
{
    btn_lua_group_ud_t *group = lua_btn_check_group(L, 1);
    lua_pushinteger(L, group->num_buttons);
    return 1;
}

static int lua_btn_group_labels(lua_State *L)
{
    btn_lua_group_ud_t *group = lua_btn_check_group(L, 1);

    lua_createtable(L, group->num_buttons, 0);
    for (int i = 0; i < group->num_buttons; i++) {
        lua_pushstring(L, group->labels[i] ? group->labels[i] : "");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* Fetch one button of a group by 1-based index or by its YAML label. */
static int lua_btn_group_button(lua_State *L)
{
    btn_lua_group_ud_t *group = lua_btn_check_group(L, 1);
    int found = -1;

    if (lua_type(L, 2) == LUA_TNUMBER) {
        int idx = (int)luaL_checkinteger(L, 2);
        if (idx < 1 || idx > group->num_buttons) {
            lua_pushnil(L);
            lua_pushfstring(L, "index %d out of range (1..%d)", idx, group->num_buttons);
            return 2;
        }
        found = idx - 1;
    } else {
        const char *label = luaL_checkstring(L, 2);
        for (int i = 0; i < group->num_buttons; i++) {
            if (group->labels[i] && strcmp(group->labels[i], label) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            lua_pushnil(L);
            lua_pushfstring(L, "no button labelled '%s' in group", label);
            return 2;
        }
    }

    lua_btn_push_handle(L, group->handles[found], false);
    return 1;
}

static int lua_btn_close(lua_State *L)
{
    /* Groups only drop their subscriptions; the board manager owns the handles. */
    btn_lua_group_ud_t *group =
        (btn_lua_group_ud_t *)luaL_testudata(L, 1, BTN_GROUP_METATABLE);
    if (group) {
        lua_btn_group_gc(L);
        lua_pushboolean(L, 1);
        return 1;
    }

    btn_lua_handle_ud_t *ud = lua_btn_check_ud(L, 1);
    button_handle_t handle = ud->handle;
    btn_lua_reg_t  *reg    = btn_find_reg(handle);
    esp_err_t       err;

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed button handle");
        return 2;
    }

    btn_remove_reg(L, reg);

    if (ud->owned) {
        err = iot_button_delete(handle);
        if (err != ESP_OK) {
            lua_pushnil(L);
            lua_pushfstring(L, "button.close failed: %s", esp_err_to_name(err));
            return 2;
        }
    }

    ud->handle = NULL;
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_btn_get_key_level(lua_State *L)
{
    button_handle_t handle = lua_btn_check_handle(L, 1);
    btn_lua_reg_t  *reg    = btn_find_reg(handle);

    if (!reg) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid or closed button handle");
        return 2;
    }

    lua_pushinteger(L, iot_button_get_key_level(handle));
    return 1;
}

static int lua_btn_on(lua_State *L)
{
    const char *event_str = luaL_checkstring(L, 2);
    btn_target_t target;

    luaL_checktype(L, 3, LUA_TFUNCTION);

    if (lua_btn_resolve_target(L, 1, &target) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "button handle or group expected");
        return 2;
    }

    button_event_t ev = btn_event_from_str(event_str);
    if (ev == BUTTON_EVENT_MAX) {
        lua_pushnil(L);
        lua_pushfstring(L, "unknown event '%s'", event_str);
        return 2;
    }

    if (!s_queue) {
        s_queue = xQueueCreate(BTN_EVENT_QUEUE_SIZE, sizeof(btn_event_item_t));
        if (!s_queue) {
            lua_pushnil(L);
            lua_pushstring(L, "failed to create event queue");
            return 2;
        }
    }

    for (int i = 0; i < target.count; i++) {
        btn_lua_reg_t *reg = btn_find_reg(target.handles[i]);
        if (!reg) {
            lua_pushnil(L);
            lua_pushstring(L, "invalid or closed button handle");
            return 2;
        }

        if (reg->lua_refs[ev] != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[ev]);
        }
        /* Each group member keeps its own ref to the same Lua function. */
        lua_pushvalue(L, 3);
        reg->lua_refs[ev] = luaL_ref(L, LUA_REGISTRYINDEX);

        if (!reg->cbs_registered[ev]) {
            esp_err_t err = iot_button_register_cb(target.handles[i], ev, NULL, btn_c_event_cb, NULL);
            if (err != ESP_OK) {
                luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[ev]);
                reg->lua_refs[ev] = LUA_NOREF;
                lua_pushnil(L);
                lua_pushfstring(L, "register_cb failed for event '%s': %s",
                                event_str, esp_err_to_name(err));
                return 2;
            }
            reg->cbs_registered[ev] = true;
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_btn_off(lua_State *L)
{
    const char  *event_str = luaL_optstring(L, 2, NULL);
    btn_target_t target;

    if (lua_btn_resolve_target(L, 1, &target) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "button handle or group expected");
        return 2;
    }

    button_event_t only_ev = BUTTON_EVENT_MAX;
    if (event_str) {
        only_ev = btn_event_from_str(event_str);
        if (only_ev == BUTTON_EVENT_MAX) {
            lua_pushnil(L);
            lua_pushfstring(L, "unknown event '%s'", event_str);
            return 2;
        }
    }

    for (int t = 0; t < target.count; t++) {
        btn_lua_reg_t *reg = btn_find_reg(target.handles[t]);
        if (!reg) {
            lua_pushnil(L);
            lua_pushstring(L, "invalid or closed button handle");
            return 2;
        }

        int first = event_str ? (int)only_ev : 0;
        int last  = event_str ? (int)only_ev : BUTTON_EVENT_MAX - 1;

        for (int i = first; i <= last; i++) {
            if (reg->lua_refs[i] != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, reg->lua_refs[i]);
                reg->lua_refs[i] = LUA_NOREF;
            }
            if (reg->cbs_registered[i]) {
                esp_err_t err = iot_button_unregister_cb(reg->handle, (button_event_t)i, NULL);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    lua_pushnil(L);
                    lua_pushfstring(L, "unregister_cb failed for event '%s': %s",
                                    btn_event_to_str((button_event_t)i), esp_err_to_name(err));
                    return 2;
                }
                reg->cbs_registered[i] = false;
            }
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_btn_dispatch(lua_State *L)
{
    int count = 0;
    if (!s_queue) {
        lua_pushinteger(L, 0);
        return 1;
    }

    btn_event_item_t item;
    while (xQueueReceive(s_queue, &item, 0) == pdTRUE) {
        btn_lua_reg_t *reg = btn_find_reg(item.handle);
        if (!reg) {
            continue;
        }

        button_event_t ev = item.event;
        if ((unsigned)ev >= BUTTON_EVENT_MAX) {
            continue;
        }

        int ref = reg->lua_refs[ev];
        if (ref == LUA_NOREF) {
            continue;
        }

        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        lua_newtable(L);
        lua_pushlightuserdata(L, item.handle);
        lua_setfield(L, -2, "handle");
        lua_pushstring(L, btn_event_to_str(ev));
        lua_setfield(L, -2, "event");
        lua_pushinteger(L, item.repeat_count);
        lua_setfield(L, -2, "repeat_count");
        lua_pushinteger(L, item.pressed_time_ms);
        lua_setfield(L, -2, "pressed_time_ms");
        /* label/index identify which key of a board group fired. */
        if (reg->label) {
            lua_pushstring(L, reg->label);
            lua_setfield(L, -2, "label");
        }
        if (reg->index >= 0) {
            lua_pushinteger(L, reg->index + 1);
            lua_setfield(L, -2, "index");
        }

        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            ESP_LOGW(TAG, "button callback error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        count++;
    }

    lua_pushinteger(L, count);
    return 1;
}

int luaopen_button(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"new",           lua_btn_new},
        {"open_group",    lua_btn_open_group},
        {"group_size",    lua_btn_group_size},
        {"group_labels",  lua_btn_group_labels},
        {"group_button",  lua_btn_group_button},
        {"close",         lua_btn_close},
        {"get_key_level", lua_btn_get_key_level},
        {"on",            lua_btn_on},
        {"off",           lua_btn_off},
        {"dispatch",      lua_btn_dispatch},
        {NULL, NULL},
    };
    if (luaL_newmetatable(L, BTN_HANDLE_METATABLE)) {
        lua_pushcfunction(L, lua_btn_handle_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, BTN_GROUP_METATABLE)) {
        lua_pushcfunction(L, lua_btn_group_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_button_register(void)
{
    return cap_lua_register_module("button", luaopen_button);
}
