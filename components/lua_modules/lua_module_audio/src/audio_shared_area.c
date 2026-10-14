/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"
#include "lua_module_audio.h"

#define AUDIO_SHARED_AREA_MAX       8
#define AUDIO_SHARED_AREA_NAME_LEN  32

typedef struct {
    char name[AUDIO_SHARED_AREA_NAME_LEN];
    uint8_t *data;
    size_t size;
    size_t capacity;
    lua_audio_shared_format_t format;
    uint32_t seq;
    SemaphoreHandle_t updated;
    bool used;
} audio_shared_area_t;

static audio_shared_area_t s_areas[AUDIO_SHARED_AREA_MAX];
static SemaphoreHandle_t s_area_lock;

static esp_err_t audio_shared_area_lock(void)
{
    if (s_area_lock == NULL) {
        s_area_lock = xSemaphoreCreateMutex();
        if (s_area_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_area_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void audio_shared_area_unlock(void)
{
    if (s_area_lock != NULL) {
        xSemaphoreGive(s_area_lock);
    }
}

static audio_shared_area_t *audio_shared_area_find_locked(const char *name)
{
    for (size_t i = 0; i < AUDIO_SHARED_AREA_MAX; i++) {
        if (s_areas[i].used && strcmp(s_areas[i].name, name) == 0) {
            return &s_areas[i];
        }
    }
    return NULL;
}

static audio_shared_area_t *audio_shared_area_get_locked(const char *name, bool create)
{
    audio_shared_area_t *area = audio_shared_area_find_locked(name);
    if (area || !create) {
        return area;
    }
    for (size_t i = 0; i < AUDIO_SHARED_AREA_MAX; i++) {
        if (!s_areas[i].used) {
            s_areas[i].used = true;
            strlcpy(s_areas[i].name, name, sizeof(s_areas[i].name));
            s_areas[i].updated = xSemaphoreCreateBinary();
            if (s_areas[i].updated == NULL) {
                memset(&s_areas[i], 0, sizeof(s_areas[i]));
                return NULL;
            }
            return &s_areas[i];
        }
    }
    return NULL;
}

static bool audio_shared_area_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= AUDIO_SHARED_AREA_NAME_LEN) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
            continue;
        }
        return false;
    }
    return true;
}

esp_err_t lua_audio_shared_area_write(const char *name,
                                      const void *data,
                                      size_t size,
                                      const lua_audio_shared_format_t *format,
                                      uint32_t *seq_out)
{
    esp_err_t err;
    audio_shared_area_t *area;

    if (!audio_shared_area_name_valid(name) || (size > 0 && data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = audio_shared_area_lock();
    if (err != ESP_OK) {
        return err;
    }

    area = audio_shared_area_get_locked(name, true);
    if (area == NULL) {
        audio_shared_area_unlock();
        return ESP_ERR_NO_MEM;
    }

    if (size > area->capacity) {
        uint8_t *new_data = realloc(area->data, size);
        if (new_data == NULL) {
            audio_shared_area_unlock();
            return ESP_ERR_NO_MEM;
        }
        area->data = new_data;
        area->capacity = size;
    }

    if (size > 0) {
        memcpy(area->data, data, size);
    }
    area->size = size;
    if (format != NULL) {
        area->format = *format;
    }
    area->seq++;
    if (area->updated != NULL) {
        xSemaphoreGive(area->updated);
    }
    if (seq_out != NULL) {
        *seq_out = area->seq;
    }

    audio_shared_area_unlock();
    return ESP_OK;
}

esp_err_t lua_audio_shared_area_read(const char *name,
                                     void *data,
                                     size_t capacity,
                                     size_t *size_out,
                                     lua_audio_shared_format_t *format_out,
                                     uint32_t *seq_out,
                                     uint32_t after_seq,
                                     bool clear_after_read)
{
    esp_err_t err;
    audio_shared_area_t *area;

    if (!audio_shared_area_name_valid(name) || data == NULL || size_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = audio_shared_area_lock();
    if (err != ESP_OK) {
        return err;
    }

    area = audio_shared_area_get_locked(name, false);
    if (area == NULL || area->size == 0 || area->seq == after_seq) {
        audio_shared_area_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (capacity < area->size) {
        *size_out = area->size;
        audio_shared_area_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(data, area->data, area->size);
    *size_out = area->size;
    if (format_out != NULL) {
        *format_out = area->format;
    }
    if (seq_out != NULL) {
        *seq_out = area->seq;
    }
    if (clear_after_read) {
        area->size = 0;
    }

    audio_shared_area_unlock();
    return ESP_OK;
}

esp_err_t lua_audio_shared_area_wait(const char *name,
                                     uint32_t after_seq,
                                     uint32_t timeout_ms)
{
    esp_err_t err;
    audio_shared_area_t *area;
    SemaphoreHandle_t updated;

    if (!audio_shared_area_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = audio_shared_area_lock();
    if (err != ESP_OK) {
        return err;
    }

    area = audio_shared_area_get_locked(name, true);
    if (area == NULL || area->updated == NULL) {
        audio_shared_area_unlock();
        return ESP_ERR_NO_MEM;
    }
    if (area->size > 0 && area->seq != after_seq) {
        audio_shared_area_unlock();
        return ESP_OK;
    }
    updated = area->updated;
    audio_shared_area_unlock();

    if (xSemaphoreTake(updated, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t lua_audio_shared_area_info(const char *name,
                                     size_t *size_out,
                                     lua_audio_shared_format_t *format_out,
                                     uint32_t *seq_out)
{
    esp_err_t err;
    audio_shared_area_t *area;

    if (!audio_shared_area_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = audio_shared_area_lock();
    if (err != ESP_OK) {
        return err;
    }

    area = audio_shared_area_get_locked(name, false);
    if (area == NULL) {
        audio_shared_area_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (size_out != NULL) {
        *size_out = area->size;
    }
    if (format_out != NULL) {
        *format_out = area->format;
    }
    if (seq_out != NULL) {
        *seq_out = area->seq;
    }

    audio_shared_area_unlock();
    return ESP_OK;
}

static void lua_audio_shared_push_info(lua_State *L,
                                       size_t size,
                                       const lua_audio_shared_format_t *format,
                                       uint32_t seq)
{
    lua_newtable(L);
    lua_pushinteger(L, size);
    lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, seq);
    lua_setfield(L, -2, "seq");
    if (format != NULL) {
        lua_pushinteger(L, format->sample_rate);
        lua_setfield(L, -2, "sample_rate");
        lua_pushinteger(L, format->channels);
        lua_setfield(L, -2, "channels");
        lua_pushinteger(L, format->bits);
        lua_setfield(L, -2, "bits");
    }
}

static lua_audio_shared_format_t lua_audio_shared_format_from_table(lua_State *L, int idx)
{
    lua_audio_shared_format_t format = {0};

    if (lua_istable(L, idx)) {
        format.sample_rate = lua_audio_get_u32_field(L, idx, "sample_rate", 0, 0);
        format.channels = lua_audio_get_u8_field(L, idx, "channels", 0, 0);
        format.bits = lua_audio_get_u8_field(L, idx, "bits", 0, 0);
    }
    return format;
}

static uint32_t lua_audio_shared_get_seq_field(lua_State *L, int idx, const char *name)
{
    lua_Integer value = 0;

    lua_getfield(L, idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    value = luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    if (value < 0 || value > UINT32_MAX) {
        luaL_error(L, "audio %s must be a non-negative integer", name);
        return 0;
    }
    return (uint32_t)value;
}

int lua_audio_shared_write(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_audio_shared_format_t format = lua_audio_shared_format_from_table(L, 3);
    uint32_t seq = 0;
    esp_err_t err = lua_audio_shared_area_write(name, data, len, &format, &seq);
    if (err != ESP_OK) {
        return lua_audio_push_errorf(L, "audio shared_write failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, seq);
    return 1;
}

int lua_audio_shared_read(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    uint32_t after_seq = 0;
    bool clear = false;
    size_t size = 0;
    lua_audio_shared_format_t format = {0};
    uint32_t seq = 0;
    uint8_t *buf = NULL;
    esp_err_t err;

    if (lua_istable(L, 2)) {
        after_seq = lua_audio_shared_get_seq_field(L, 2, "after_seq");
        lua_getfield(L, 2, "clear");
        clear = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    err = lua_audio_shared_area_info(name, &size, &format, &seq);
    if (err == ESP_ERR_NOT_FOUND || size == 0 || seq == after_seq) {
        lua_pushnil(L);
        return 1;
    }
    if (err != ESP_OK) {
        return lua_audio_push_errorf(L, "audio shared_read info failed: %s", esp_err_to_name(err));
    }

    buf = malloc(size);
    if (buf == NULL) {
        return lua_audio_push_error(L, "audio shared_read: out of memory");
    }
    err = lua_audio_shared_area_read(name, buf, size, &size, &format, &seq, after_seq, clear);
    if (err == ESP_ERR_INVALID_SIZE) {
        uint8_t *new_buf = realloc(buf, size);
        if (new_buf == NULL) {
            free(buf);
            return lua_audio_push_error(L, "audio shared_read: out of memory");
        }
        buf = new_buf;
        err = lua_audio_shared_area_read(name, buf, size, &size, &format, &seq, after_seq, clear);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        free(buf);
        lua_pushnil(L);
        return 1;
    }
    if (err != ESP_OK) {
        free(buf);
        return lua_audio_push_errorf(L, "audio shared_read failed: %s", esp_err_to_name(err));
    }

    lua_pushlstring(L, (const char *)buf, size);
    free(buf);
    lua_audio_shared_push_info(L, size, &format, seq);
    return 2;
}

int lua_audio_shared_info_lua(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    size_t size = 0;
    lua_audio_shared_format_t format = {0};
    uint32_t seq = 0;
    esp_err_t err = lua_audio_shared_area_info(name, &size, &format, &seq);
    if (err == ESP_ERR_NOT_FOUND) {
        lua_pushnil(L);
        return 1;
    }
    if (err != ESP_OK) {
        return lua_audio_push_errorf(L, "audio shared_info failed: %s", esp_err_to_name(err));
    }
    lua_audio_shared_push_info(L, size, &format, seq);
    return 1;
}
