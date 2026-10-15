/*
 * SPDX-FileCopyrightText: 2026 Pisen / Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 首页布局模型：每页 2 列 x 2 行网格，S=1x1、M=2x1、L=2x2；最多 3 页；
 * 底部应用栏最多 4 个、只在第 1 页。持久化到 <DATA>/home/layout.json。
 * 文件损坏或校验失败时回退默认布局，并把坏文件另存为 .bad 以便排查。
 */
#include "home_priv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

#define HV_LAYOUT_DIR   "home"
#define HV_LAYOUT_FILE  "home/layout.json"
#define HV_LAYOUT_MAX   4096

static const char *const s_size_names[] = {"S", "M", "L"};

const char *hv_size_name(uint8_t sz)
{
    return sz <= HV_SZ_L ? s_size_names[sz] : "S";
}

int hv_size_from_name(const char *name)
{
    for (int i = 0; name && i <= HV_SZ_L; i++) {
        if (strcmp(name, s_size_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void hv_item_set(hv_item_t *it, const char *w, uint8_t sz)
{
    strlcpy(it->w, w, sizeof(it->w));
    it->sz = sz;
}

void hv_layout_default(hv_layout_t *l)
{
    memset(l, 0, sizeof(*l));
    l->pages = 2;
    hv_item_set(&l->it[0][0], "mine", HV_SZ_S);
    hv_item_set(&l->it[0][1], "clock", HV_SZ_S);
    hv_item_set(&l->it[0][2], "weather", HV_SZ_S);
    hv_item_set(&l->it[0][3], "voice", HV_SZ_S);
    l->n[0] = 4;
    hv_item_set(&l->it[1][0], "stocks", HV_SZ_S);
    hv_item_set(&l->it[1][1], "system", HV_SZ_S);
    hv_item_set(&l->it[1][2], "aihot", HV_SZ_M);
    l->n[1] = 3;
    l->dock_n = 4;
    strlcpy(l->dock[0], "sys:meeting", sizeof(l->dock[0]));
    strlcpy(l->dock[1], "sys:ir", sizeof(l->dock[1]));
    strlcpy(l->dock[2], "sys:card", sizeof(l->dock[2]));
    strlcpy(l->dock[3], "sys:workbuddy", sizeof(l->dock[3]));
}

static void hv_size_cells(uint8_t sz, int *cols, int *rows)
{
    *cols = sz == HV_SZ_S ? 1 : 2;
    *rows = sz == HV_SZ_L ? 2 : 1;
}

bool hv_layout_pack(const hv_item_t *items, int n, hv_slot_t *out)
{
    bool used[2][2] = {{false, false}, {false, false}};

    if (n > HV_MAX_PER_PAGE) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        int cols, rows;
        bool placed = false;

        hv_size_cells(items[i].sz, &cols, &rows);
        for (int r = 0; r + rows <= 2 && !placed; r++) {
            for (int c = 0; c + cols <= 2 && !placed; c++) {
                bool free_cells = true;
                for (int dr = 0; dr < rows && free_cells; dr++) {
                    for (int dc = 0; dc < cols; dc++) {
                        if (used[r + dr][c + dc]) {
                            free_cells = false;
                            break;
                        }
                    }
                }
                if (!free_cells) {
                    continue;
                }
                for (int dr = 0; dr < rows; dr++) {
                    for (int dc = 0; dc < cols; dc++) {
                        used[r + dr][c + dc] = true;
                    }
                }
                if (out) {
                    out[i] = (hv_slot_t){.col = (uint8_t)c, .row = (uint8_t)r,
                                         .cols = (uint8_t)cols, .rows = (uint8_t)rows};
                }
                placed = true;
            }
        }
        if (!placed) {
            return false;
        }
    }
    return true;
}

bool hv_layout_find(const hv_layout_t *l, const char *w, int *page, int *index)
{
    for (int p = 0; p < l->pages; p++) {
        for (int i = 0; i < l->n[p]; i++) {
            if (strcmp(l->it[p][i].w, w) == 0) {
                if (page) {
                    *page = p;
                }
                if (index) {
                    *index = i;
                }
                return true;
            }
        }
    }
    return false;
}

bool hv_layout_dock_has(const hv_layout_t *l, const char *app_id)
{
    for (int i = 0; i < l->dock_n; i++) {
        if (strcmp(l->dock[i], app_id) == 0) {
            return true;
        }
    }
    return false;
}

cJSON *hv_layout_to_json(const hv_layout_t *l)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *pages = root ? cJSON_AddArrayToObject(root, "pages") : NULL;
    cJSON *dock = root ? cJSON_AddArrayToObject(root, "dock") : NULL;

    if (!root || !pages || !dock) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "v", 1);
    for (int p = 0; p < l->pages; p++) {
        cJSON *page = cJSON_CreateArray();
        if (!page) {
            break;
        }
        for (int i = 0; i < l->n[p]; i++) {
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                break;
            }
            cJSON_AddStringToObject(item, "w", l->it[p][i].w);
            cJSON_AddStringToObject(item, "s", hv_size_name(l->it[p][i].sz));
            cJSON_AddItemToArray(page, item);
        }
        cJSON_AddItemToArray(pages, page);
    }
    for (int i = 0; i < l->dock_n; i++) {
        cJSON_AddItemToArray(dock, cJSON_CreateString(l->dock[i]));
    }
    return root;
}

static void hv_err(char *err, size_t size, const char *text)
{
    if (err && size) {
        strlcpy(err, text, size);
    }
}

esp_err_t hv_layout_from_json(const cJSON *j, hv_layout_t *out, char *err, size_t err_size)
{
    hv_layout_t l;
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(j, "pages");
    const cJSON *dock = cJSON_GetObjectItemCaseSensitive(j, "dock");
    const cJSON *page;
    bool has_mine = false;

    memset(&l, 0, sizeof(l));
    if (!cJSON_IsObject(j) || !cJSON_IsArray(pages)) {
        hv_err(err, err_size, "layout 需要 pages 数组");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_GetArraySize(pages) < 1 || cJSON_GetArraySize(pages) > HV_MAX_PAGES) {
        hv_err(err, err_size, "页数需为 1-3");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_ArrayForEach(page, pages) {
        const cJSON *item;
        int p = l.pages;

        if (!cJSON_IsArray(page) || cJSON_GetArraySize(page) > HV_MAX_PER_PAGE) {
            hv_err(err, err_size, "每页最多 4 个小组件");
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_ArrayForEach(item, page) {
            const cJSON *w = cJSON_GetObjectItemCaseSensitive(item, "w");
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "s");
            const hv_widget_desc_t *desc;
            int sz;

            if (!cJSON_IsString(w) || !cJSON_IsString(s)) {
                hv_err(err, err_size, "小组件需要 w 与 s");
                return ESP_ERR_INVALID_ARG;
            }
            desc = hv_widgets_find(w->valuestring);
            sz = hv_size_from_name(s->valuestring);
            if (!desc) {
                hv_err(err, err_size, "未知的小组件");
                return ESP_ERR_NOT_FOUND;
            }
            if (sz < 0 || !(desc->sizes & HV_SZ_BIT(sz))) {
                hv_err(err, err_size, "小组件不支持这个尺寸");
                return ESP_ERR_INVALID_ARG;
            }
            if (hv_layout_find(&l, w->valuestring, NULL, NULL)) {
                hv_err(err, err_size, "同一个小组件只能放一次");
                return ESP_ERR_INVALID_ARG;
            }
            hv_item_set(&l.it[p][l.n[p]], w->valuestring, (uint8_t)sz);
            l.n[p]++;
            if (strcmp(w->valuestring, "mine") == 0) {
                has_mine = true;
            }
        }
        if (!hv_layout_pack(l.it[p], l.n[p], NULL)) {
            hv_err(err, err_size, "这一页放不下");
            return ESP_ERR_INVALID_SIZE;
        }
        l.pages++;
    }
    if (!has_mine) {
        hv_err(err, err_size, "「我的」小组件不能移出");
        return ESP_ERR_INVALID_ARG;
    }
    if (dock) {
        const cJSON *app;
        if (!cJSON_IsArray(dock) || cJSON_GetArraySize(dock) > HV_DOCK_MAX) {
            hv_err(err, err_size, "底部应用栏最多 4 个");
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_ArrayForEach(app, dock) {
            if (!cJSON_IsString(app) || strlen(app->valuestring) >= HV_APP_ID_LEN ||
                    !app->valuestring[0]) {
                hv_err(err, err_size, "底部应用 ID 无效");
                return ESP_ERR_INVALID_ARG;
            }
            if (hv_layout_dock_has(&l, app->valuestring)) {
                hv_err(err, err_size, "底部应用重复");
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(l.dock[l.dock_n++], app->valuestring, HV_APP_ID_LEN);
        }
    }
    *out = l;
    return ESP_OK;
}

static int hv_layout_path(char *out, size_t size, const char *suffix)
{
    return snprintf(out, size, "%s/%s%s", hv_plat_data_root(), HV_LAYOUT_FILE, suffix ? suffix : "");
}

esp_err_t hv_layout_load(hv_layout_t *l)
{
    char path[SYSTEM_UI_PATH_MAX];
    char *text;
    cJSON *json;
    esp_err_t err;
    char why[48] = {0};

    hv_layout_default(l);
    hv_layout_path(path, sizeof(path), NULL);
    text = hv_plat_read_text(path, HV_LAYOUT_MAX);
    if (!text) {
        return ESP_ERR_NOT_FOUND;
    }
    json = cJSON_Parse(text);
    free(text);
    err = json ? hv_layout_from_json(json, l, why, sizeof(why)) : ESP_ERR_INVALID_STATE;
    cJSON_Delete(json);
    if (err != ESP_OK) {
        char bad[SYSTEM_UI_PATH_MAX];
        ESP_LOGW(SYSTEM_UI_TAG, "home layout invalid (%s), using default", why[0] ? why : "parse");
        hv_layout_path(bad, sizeof(bad), ".bad");
        unlink(bad);
        rename(path, bad);
        hv_layout_default(l);
    }
    return err;
}

esp_err_t hv_layout_save(const hv_layout_t *l)
{
    char dir[SYSTEM_UI_PATH_MAX];
    char path[SYSTEM_UI_PATH_MAX];
    char tmp[SYSTEM_UI_PATH_MAX];
    cJSON *json = hv_layout_to_json(l);
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    FILE *f;
    bool ok;

    cJSON_Delete(json);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(dir, sizeof(dir), "%s/%s", hv_plat_data_root(), HV_LAYOUT_DIR);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        cJSON_free(text);
        return ESP_FAIL;
    }
    hv_layout_path(path, sizeof(path), NULL);
    hv_layout_path(tmp, sizeof(tmp), ".tmp");
    f = fopen(tmp, "wb");
    if (!f) {
        cJSON_free(text);
        return ESP_FAIL;
    }
    ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    ok = (fclose(f) == 0) && ok;
    cJSON_free(text);
    if (!ok) {
        unlink(tmp);
        return ESP_FAIL;
    }
    /* FAT 不允许 rename 覆盖已有文件：先删旧文件。 */
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}
