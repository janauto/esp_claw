/* Persistent, bounded groups. Only recovery/import scans directories. */
#include "local_app_private.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

const char *app_string(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
bool app_exists(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0;
}
bool app_is_dir(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
esp_err_t app_mkdir(const char *p)
{
    if (mkdir(p, 0775) == 0 || (errno == EEXIST && app_is_dir(p)))
        return ESP_OK;
    return ESP_FAIL;
}
esp_err_t app_path(local_app_store_handle_t s, const char *rel, char *out)
{
    int n = snprintf(out, APP_PATH_CAP, "%s/%s", s->root, rel);
    return n > 0 && n < APP_PATH_CAP ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
static cJSON *json_read_one(const char *path)
{
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode))
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(8193);
    cJSON *j = NULL;
    if (buf) {
        size_t n = fread(buf, 1, 8192, f);
        if (n < 8192 && !ferror(f)) {
            buf[n] = 0;
            j = cJSON_ParseWithLength(buf, n + 1);
        }
        free(buf);
    }
    fclose(f);
    return j;
}
cJSON *app_json_read(const char *path)
{
    cJSON *j = json_read_one(path);
    if (j)
        return j;
    char *candidate = malloc(APP_PATH_CAP);
    if (!candidate)
        return NULL;
    const char *suffixes[] = {".previous", ".tmp"};
    for (size_t i = 0; i < 2 && !j; i++) {
        int n = snprintf(candidate, APP_PATH_CAP, "%s%s", path, suffixes[i]);
        if (n > 0 && n < APP_PATH_CAP)
            j = json_read_one(candidate);
        if (j && !app_exists(path))
            (void)rename(candidate, path);
    }
    free(candidate);
    return j;
}
esp_err_t app_json_write(const char *path, const cJSON *j)
{
    char *text = cJSON_PrintUnformatted(j), *temp = malloc(APP_PATH_CAP), *previous = malloc(APP_PATH_CAP);
    if (!text || !temp || !previous) {
        free(temp);
        free(previous);
        cJSON_free(text);
        return ESP_ERR_NO_MEM;
    }
    int a = snprintf(temp, APP_PATH_CAP, "%s.tmp", path),
        b = snprintf(previous, APP_PATH_CAP, "%s.previous", path);
    esp_err_t err = ESP_FAIL;
    FILE *f = a > 0 && a < APP_PATH_CAP && b > 0 && b < APP_PATH_CAP ? fopen(temp, "wb") : NULL;
    if (f) {
        size_t len = strlen(text);
        bool ok = fwrite(text, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
        if (fclose(f) != 0)
            ok = false;
        /* FAT refuses rename-over-existing. Keep an old readable version until
         * the new JSON is in place; readers recover .previous after power loss. */
        if (ok && app_exists(path)) {
            if (app_exists(previous) && unlink(previous))
                ok = false;
            if (ok && rename(path, previous))
                ok = false;
        }
        if (ok && rename(temp, path) == 0) {
            err = ESP_OK;
            (void)unlink(previous);
        } else if (!app_exists(path) && app_exists(previous))
            (void)rename(previous, path);
    }
    if (err != ESP_OK)
        unlink(temp);
    free(temp);
    free(previous);
    cJSON_free(text);
    return err;
}
/* No symlink traversal, no arbitrary recursion, no fixed stack-size buffers. */
esp_err_t app_remove_tree(char *path, unsigned depth)
{
    struct stat st;
    if (depth > 4)
        return ESP_ERR_INVALID_ARG;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    DIR *dir = opendir(path);
    if (!dir)
        return ESP_FAIL;
    size_t base = strlen(path);
    esp_err_t err = ESP_OK;
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        int n = snprintf(path + base, APP_PATH_CAP - base, "/%s", de->d_name);
        if (n < 0 || (size_t)n >= APP_PATH_CAP - base) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = app_remove_tree(path, depth + 1);
        path[base] = 0;
        if (err != ESP_OK)
            break;
    }
    closedir(dir);
    path[base] = 0;
    if (err == ESP_OK && rmdir(path) != 0)
        err = ESP_FAIL;
    return err;
}
void app_index_invalidate(local_app_store_handle_t s)
{
    cJSON_Delete(s->cached_group);
    s->cached_group = NULL;
    s->group = APP_NONE;
}
esp_err_t app_index_dirty(local_app_store_handle_t s)
{
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    esp_err_t e = app_mkdir(s->root);
    if (e == ESP_OK)
        e = app_path(s, ".index-dirty", p);
    FILE *f = e == ESP_OK ? fopen(p, "wb") : NULL;
    if (f) {
        e = fflush(f) == 0 && fsync(fileno(f)) == 0 ? ESP_OK : ESP_FAIL;
        if (fclose(f))
            e = ESP_FAIL;
    } else
        e = ESP_FAIL;
    free(p);
    return e;
}
static bool group_id(const char *name, uint32_t *id)
{
    if (strlen(name) != 9 || name[0] != 'g')
        return false;
    uint32_t v = 0;
    for (int i = 1; i < 9; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    *id = v;
    return v != APP_NONE;
}
static int names_compare(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static esp_err_t names(const char *path, char ***out, size_t *count)
{
    *out = NULL;
    *count = 0;
    DIR *d = opendir(path);
    if (!d)
        return ESP_FAIL;
    esp_err_t err = ESP_OK;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        char **next = realloc(*out, (*count + 1) * sizeof(char *));
        if (!next) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        *out = next;
        (*out)[*count] = strdup(de->d_name);
        if (!(*out)[*count]) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        (*count)++;
    }
    closedir(d);
    if (*count > 1)
        qsort(*out, *count, sizeof(char *), names_compare);
    return err;
}
static void free_names(char **n, size_t count)
{
    for (size_t i = 0; i < count; i++)
        free(n[i]);
    free(n);
}
static cJSON *manifest_in(const char *dir, const char *id)
{
    if (!app_is_dir(dir))
        return NULL;
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return NULL;
    int n = snprintf(p, APP_PATH_CAP, "%s/app.json", dir);
    cJSON *m = n > 0 && n < APP_PATH_CAP ? app_json_read(p) : NULL;
    if (!app_manifest_valid(m, id)) {
        cJSON_Delete(m);
        m = NULL;
    }
    if (m) {
        struct stat st;
        const cJSON *spec = app_file_spec(m, "main.lua");
        n = snprintf(p, APP_PATH_CAP, "%s/main.lua", dir);
        if (n < 0 || n >= APP_PATH_CAP || lstat(p, &st) || !S_ISREG(st.st_mode) ||
            st.st_size != (off_t)cJSON_GetObjectItemCaseSensitive(spec, "size")->valuedouble) {
            cJSON_Delete(m);
            m = NULL;
        }
    }
    free(p);
    return m;
}
static cJSON *entry(const cJSON *m)
{
    cJSON *e = cJSON_CreateObject();
    if (!e)
        return NULL;
    const char *keys[] = {"app_id", "title", "frame_color", "pill_color"};
    for (size_t i = 0; i < 4; i++) {
        cJSON *v = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(m, keys[i]), true);
        if (!v || !cJSON_AddItemToObject(e, keys[i], v)) {
            cJSON_Delete(v);
            cJSON_Delete(e);
            return NULL;
        }
    }
    cJSON_AddBoolToObject(e, "icon_selected", app_file_spec(m, "icon_selected.bin") != NULL);
    cJSON_AddBoolToObject(e, "icon_unselected", app_file_spec(m, "icon_unselected.bin") != NULL);
    return e;
}
static esp_err_t group_path(local_app_store_handle_t s, uint32_t g, const char *tail, char *p)
{
    int n = snprintf(p, APP_PATH_CAP, "%s/g%08lx%s", s->root, (unsigned long)g, tail);
    return n > 0 && n < APP_PATH_CAP ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
/* Rebuild is deliberately exceptional: dirty journal, initial import, explicit reload.
 * Group locations never move during delete/recovery. Old per-group order is retained. */
esp_err_t local_app_store_reload(local_app_store_handle_t s)
{
    if (!s)
        return ESP_ERR_INVALID_ARG;
    char *p = malloc(APP_PATH_CAP), *dest = malloc(APP_PATH_CAP);
    cJSON *catalog = cJSON_CreateArray(), *seen = cJSON_CreateObject(), *summary = cJSON_CreateObject();
    char **roots = NULL;
    size_t count = 0;
    if (!p || !dest || !catalog || !seen || !summary) {
        free(p);
        free(dest);
        cJSON_Delete(catalog);
        cJSON_Delete(seen);
        cJSON_Delete(summary);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = app_index_dirty(s);
    if (err != ESP_OK)
        goto done;
    app_path(s, "index.json", p);
    cJSON *old = app_json_read(p);
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(old, "revision");
    uint32_t revision = cJSON_IsNumber(rev) ? (uint32_t)rev->valuedouble + 1u : 1u;
    cJSON_Delete(old);
    err = names(s->root, &roots, &count);
    if (err != ESP_OK)
        goto done;
    uint32_t max_group = 0, tail = 0;
    bool any_group = false;
    for (size_t i = 0; i < count; i++) {
        uint32_t g;
        app_path(s, roots[i], p);
        if (group_id(roots[i], &g) && app_is_dir(p)) {
            if (g >= max_group)
                max_group = g;
            any_group = true;
        }
    }
    tail = any_group ? max_group : 0;
    /* Flat app IDs may themselves look like group names. Such a directory is an
     * app if app.json is valid and must never become its own destination parent. */
    for (size_t i = 0; i < count; i++) {
        app_path(s, roots[i], p);
        cJSON *m = manifest_in(p, roots[i]);
        if (!m)
            continue;
        cJSON_Delete(m);
        for (;;) {
            if (tail == APP_NONE) {
                err = ESP_ERR_INVALID_SIZE;
                goto done;
            }
            group_path(s, tail, "", dest);
            char *probe = malloc(APP_PATH_CAP);
            if (!probe) {
                err = ESP_ERR_NO_MEM;
                goto done;
            }
            snprintf(probe, APP_PATH_CAP, "%s/app.json", dest);
            bool occupied_app = app_exists(probe);
            free(probe);
            if (!occupied_app && (!app_exists(dest) || app_is_dir(dest))) {
                err = app_mkdir(dest);
                if (err != ESP_OK)
                    goto done;
                char **members = NULL;
                size_t n = 0;
                err = names(dest, &members, &n);
                size_t valid = 0;
                for (size_t j = 0; j < n; j++) {
                    char *q = malloc(APP_PATH_CAP);
                    if (!q) {
                        err = ESP_ERR_NO_MEM;
                        break;
                    }
                    snprintf(q, APP_PATH_CAP, "%s/%s", dest, members[j]);
                    cJSON *v = manifest_in(q, members[j]);
                    if (v)
                        valid++;
                    cJSON_Delete(v);
                    free(q);
                }
                free_names(members, n);
                if (err != ESP_OK)
                    goto done;
                if (valid < LOCAL_APP_GROUP_SIZE)
                    break;
            }
            tail++;
        }
        size_t len = strlen(dest);
        int n = snprintf(dest + len, APP_PATH_CAP - len, "/%s", roots[i]);
        if (n < 0 || (size_t)n >= APP_PATH_CAP - len || app_exists(dest) || rename(p, dest)) {
            err = ESP_FAIL;
            goto done;
        }
    }
    free_names(roots, count);
    roots = NULL;
    count = 0;
    err = names(s->root, &roots, &count);
    if (err != ESP_OK)
        goto done;
    size_t total = 0;
    uint32_t next_group = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t g;
        if (!group_id(roots[i], &g))
            continue;
        group_path(s, g, "", p);
        if (!app_is_dir(p))
            continue;
        group_path(s, g, "/app.json", dest);
        if (app_exists(dest))
            continue;
        if (g >= next_group)
            next_group = g + 1;
        char **members = NULL;
        size_t n = 0;
        err = names(p, &members, &n);
        if (err != ESP_OK)
            goto done;
        cJSON *available = cJSON_CreateObject(), *apps = cJSON_CreateArray();
        if (!available || !apps) {
            free_names(members, n);
            cJSON_Delete(available);
            cJSON_Delete(apps);
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        for (size_t j = 0; j < n; j++) {
            size_t len = strlen(p);
            int r = snprintf(p + len, APP_PATH_CAP - len, "/%s", members[j]);
            cJSON *m = r > 0 && (size_t)r < APP_PATH_CAP - len ? manifest_in(p, members[j]) : NULL;
            p[len] = 0;
            if (!m)
                continue;
            if (cJSON_HasObjectItem(seen, members[j])) {
                cJSON_Delete(m);
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            cJSON_AddBoolToObject(seen, members[j], true);
            cJSON *e = entry(m);
            cJSON_Delete(m);
            if (!e || !cJSON_AddItemToObject(available, members[j], e)) {
                cJSON_Delete(e);
                err = ESP_ERR_NO_MEM;
                break;
            }
        }
        free_names(members, n);
        group_path(s, g, "/index.json", dest);
        cJSON *previous = app_json_read(dest);
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(previous, "apps"))
        {
            const char *id = app_string(v, "app_id");
            cJSON *a = id ? cJSON_DetachItemFromObjectCaseSensitive(available, id) : NULL;
            if (a)
                cJSON_AddItemToArray(apps, a);
        }
        cJSON_Delete(previous);
        while (available->child) {
            cJSON *a = cJSON_DetachItemViaPointer(available, available->child);
            cJSON_AddItemToArray(apps, a);
        }
        cJSON_Delete(available);
        int size = cJSON_GetArraySize(apps);
        if (err != ESP_OK || size > LOCAL_APP_GROUP_SIZE) {
            cJSON_Delete(apps);
            if (err == ESP_OK)
                err = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        if (!size) {
            cJSON_Delete(apps);
            continue;
        }
        cJSON *group = cJSON_CreateObject();
        if (!group) {
            cJSON_Delete(apps);
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        cJSON_AddNumberToObject(group, "schema", 1);
        cJSON_AddNumberToObject(group, "group", g);
        cJSON_AddNumberToObject(group, "prev", APP_NONE);
        cJSON_AddNumberToObject(group, "next", APP_NONE);
        cJSON_AddItemToObject(group, "apps", apps);
        cJSON_AddItemToArray(catalog, group);
        total += size;
    }
    int groups = cJSON_GetArraySize(catalog);
    for (int i = 0; i < groups; i++) {
        cJSON *g = cJSON_GetArrayItem(catalog, i);
        if (i)
            cJSON_SetNumberValue(
                cJSON_GetObjectItemCaseSensitive(g, "prev"),
                cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(catalog, i - 1), "group")->valuedouble);
        if (i + 1 < groups)
            cJSON_SetNumberValue(
                cJSON_GetObjectItemCaseSensitive(g, "next"),
                cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(catalog, i + 1), "group")->valuedouble);
        group_path(s, (uint32_t)cJSON_GetObjectItemCaseSensitive(g, "group")->valuedouble, "/index.json", p);
        err = app_json_write(p, g);
        if (err != ESP_OK)
            goto done;
    }
    cJSON_AddNumberToObject(summary, "schema", 1);
    cJSON_AddNumberToObject(summary, "revision", revision);
    cJSON_AddNumberToObject(summary, "group_size", LOCAL_APP_GROUP_SIZE);
    cJSON_AddNumberToObject(summary, "count", (double)total);
    cJSON_AddNumberToObject(summary, "group_count", groups);
    cJSON_AddNumberToObject(summary, "next_group", next_group);
    for (int side = 0; side < 2; side++) {
        cJSON *endpoint = NULL;
        if (groups) {
            cJSON *g = cJSON_GetArrayItem(catalog, side ? groups - 1 : 0);
            cJSON *apps = cJSON_GetObjectItemCaseSensitive(g, "apps");
            endpoint =
                cJSON_Duplicate(cJSON_GetArrayItem(apps, side ? cJSON_GetArraySize(apps) - 1 : 0), true);
            if (!endpoint) {
                err = ESP_ERR_NO_MEM;
                goto done;
            }
            cJSON_AddNumberToObject(endpoint, "group",
                                    cJSON_GetObjectItemCaseSensitive(g, "group")->valuedouble);
        } else
            endpoint = cJSON_CreateNull();
        cJSON_AddItemToObject(summary, side ? "last" : "first", endpoint);
    }
    app_path(s, "index.json", p);
    err = app_json_write(p, summary);
    if (err == ESP_OK) {
        app_path(s, ".index-dirty", p);
        if (unlink(p))
            err = ESP_FAIL;
    }
    app_index_invalidate(s);
done:
    free_names(roots, count);
    free(p);
    free(dest);
    cJSON_Delete(catalog);
    cJSON_Delete(seen);
    cJSON_Delete(summary);
    return err;
}
esp_err_t app_index_ready(local_app_store_handle_t s)
{
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    app_path(s, ".index-dirty", p);
    bool dirty = app_exists(p);
    app_path(s, "index.json", p);
    bool absent = !app_exists(p);
    free(p);
    return dirty || absent ? local_app_store_reload(s) : ESP_OK;
}
static bool index_number(const cJSON *v)
{
    return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= UINT32_MAX &&
           (double)(uint32_t)v->valuedouble == v->valuedouble;
}
static bool index_entry_valid(const cJSON *v)
{
    const cJSON *frame = cJSON_GetObjectItemCaseSensitive(v, "frame_color"),
                *pill = cJSON_GetObjectItemCaseSensitive(v, "pill_color");
    return cJSON_IsObject(v) && app_id_valid(app_string(v, "app_id")) &&
           app_title_valid(app_string(v, "title")) && index_number(frame) && frame->valuedouble <= 0xffffff &&
           index_number(pill) && pill->valuedouble <= 0xffffff &&
           cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(v, "icon_selected")) &&
           cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(v, "icon_unselected"));
}
static bool summary_valid(const cJSON *j)
{
    const char *keys[] = {"revision", "count", "group_count", "next_group"};
    for (size_t i = 0; i < 4; i++)
        if (!index_number(cJSON_GetObjectItemCaseSensitive(j, keys[i])))
            return false;
    double n = cJSON_GetObjectItemCaseSensitive(j, "count")->valuedouble;
    for (int side = 0; side < 2; side++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, side ? "last" : "first");
        if (n ? !index_entry_valid(v) || !index_number(cJSON_GetObjectItemCaseSensitive(v, "group"))
              : !cJSON_IsNull(v))
            return false;
    }
    return true;
}
static bool group_valid(const cJSON *j, uint32_t id)
{
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(j, "group"),
                *apps = cJSON_GetObjectItemCaseSensitive(j, "apps");
    if (!index_number(g) || (uint32_t)g->valuedouble != id || !cJSON_IsArray(apps) ||
        cJSON_GetArraySize(apps) < 1 || cJSON_GetArraySize(apps) > LOCAL_APP_GROUP_SIZE ||
        !index_number(cJSON_GetObjectItemCaseSensitive(j, "prev")) ||
        !index_number(cJSON_GetObjectItemCaseSensitive(j, "next")))
        return false;
    const cJSON *v;
    cJSON_ArrayForEach(v, apps) if (!index_entry_valid(v)) return false;
    return true;
}
esp_err_t local_app_store_summary(local_app_store_handle_t s, cJSON **out)
{
    if (!s || !out)
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    esp_err_t e = app_index_ready(s);
    if (e != ESP_OK)
        return e;
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    app_path(s, "index.json", p);
    *out = app_json_read(p);
    free(p);
    if (!summary_valid(*out)) {
        cJSON_Delete(*out);
        *out = NULL;
        e = local_app_store_reload(s);
        if (e != ESP_OK)
            return e;
        p = malloc(APP_PATH_CAP);
        if (!p)
            return ESP_ERR_NO_MEM;
        app_path(s, "index.json", p);
        *out = app_json_read(p);
        free(p);
        if (!summary_valid(*out)) {
            cJSON_Delete(*out);
            *out = NULL;
            return ESP_ERR_INVALID_STATE;
        }
    }
    uint32_t revision = (uint32_t)cJSON_GetObjectItemCaseSensitive(*out, "revision")->valuedouble;
    if (revision != s->revision) {
        app_index_invalidate(s);
        s->revision = revision;
    }
    return ESP_OK;
}
esp_err_t local_app_store_at(local_app_store_handle_t s, size_t position, cJSON **out)
{
    if (!s || !out)
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    cJSON *summary = NULL;
    esp_err_t e = local_app_store_summary(s, &summary);
    if (e != ESP_OK)
        return e;
    size_t total = (size_t)cJSON_GetObjectItemCaseSensitive(summary, "count")->valuedouble;
    if (position >= total) {
        cJSON_Delete(summary);
        return ESP_ERR_NOT_FOUND;
    }
    bool backwards = position > total / 2;
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(summary, backwards ? "last" : "first");
    uint32_t group = (uint32_t)cJSON_GetObjectItemCaseSensitive(end, "group")->valuedouble;
    size_t start = backwards ? total : 0;
    if (s->cached_group) {
        size_t n = (size_t)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(s->cached_group, "apps"));
        if (position >= s->start && position < s->start + n) {
            group = s->group;
            start = s->start;
            backwards = false;
        }
    }
    cJSON_Delete(summary);
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    size_t guard = 0;
    while (group != APP_NONE && guard++ <= total) {
        if (!s->cached_group || s->group != group) {
            app_index_invalidate(s);
            group_path(s, group, "", p);
            bool directory = app_is_dir(p);
            group_path(s, group, "/index.json", p);
            s->cached_group = directory ? app_json_read(p) : NULL;
            s->group = group;
        }
        if (!group_valid(s->cached_group, group)) {
            e = ESP_ERR_INVALID_STATE;
            break;
        }
        cJSON *apps = cJSON_GetObjectItemCaseSensitive(s->cached_group, "apps");
        size_t n = (size_t)cJSON_GetArraySize(apps);
        if (!s->cached_group || !n || n > LOCAL_APP_GROUP_SIZE || (backwards && n > start)) {
            e = ESP_ERR_INVALID_STATE;
            break;
        }
        if (backwards)
            start -= n;
        s->start = start;
        if (position >= start && position < start + n) {
            *out = cJSON_Duplicate(cJSON_GetArrayItem(apps, (int)(position - start)), true);
            if (!*out) {
                e = ESP_ERR_NO_MEM;
                break;
            }
            int r = snprintf(p, APP_PATH_CAP, "g%08lx/%s", (unsigned long)group, app_string(*out, "app_id"));
            if (r < 0 || r >= APP_PATH_CAP || !cJSON_AddStringToObject(*out, "directory", p)) {
                cJSON_Delete(*out);
                *out = NULL;
                e = ESP_ERR_NO_MEM;
            }
            break;
        }
        const cJSON *link = cJSON_GetObjectItemCaseSensitive(s->cached_group, backwards ? "prev" : "next");
        if (!cJSON_IsNumber(link)) {
            e = ESP_ERR_INVALID_STATE;
            break;
        }
        group = (uint32_t)link->valuedouble;
        if (!backwards)
            start += n;
    }
    free(p);
    return *out ? ESP_OK : e == ESP_OK ? ESP_ERR_INVALID_STATE : e;
}
esp_err_t local_app_store_directory(local_app_store_handle_t s, const char *id, char *out, size_t capacity)
{
    if (!s || !app_id_valid(id) || !out || !capacity)
        return ESP_ERR_INVALID_ARG;
    cJSON *summary = NULL;
    esp_err_t e = local_app_store_summary(s, &summary);
    if (e != ESP_OK)
        return e;
    size_t total = (size_t)cJSON_GetObjectItemCaseSensitive(summary, "count")->valuedouble;
    cJSON_Delete(summary);
    (void)total;
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    e = local_app_store_summary(s, &summary);
    if (e != ESP_OK) {
        free(p);
        return e;
    }
    const cJSON *first = cJSON_GetObjectItemCaseSensitive(summary, "first");
    uint32_t g = cJSON_IsObject(first)
                     ? (uint32_t)cJSON_GetObjectItemCaseSensitive(first, "group")->valuedouble
                     : APP_NONE;
    cJSON_Delete(summary);
    size_t guard = 0;
    while (g != APP_NONE && guard++ <= total) {
        group_path(s, g, "", p);
        bool directory = app_is_dir(p);
        group_path(s, g, "/index.json", p);
        cJSON *group = directory ? app_json_read(p) : NULL;
        if (!group_valid(group, g)) {
            cJSON_Delete(group);
            free(p);
            return ESP_ERR_INVALID_STATE;
        }
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(group, "apps"))
        {
            const char *key = app_string(v, "app_id");
            if (key && !strcmp(key, id)) {
                int n = snprintf(out, capacity, "g%08lx/%s", (unsigned long)g, id);
                cJSON_Delete(group);
                free(p);
                return n > 0 && (size_t)n < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
            }
        }
        const cJSON *next = cJSON_GetObjectItemCaseSensitive(group, "next");
        if (!cJSON_IsNumber(next)) {
            cJSON_Delete(group);
            free(p);
            return ESP_ERR_INVALID_STATE;
        }
        g = (uint32_t)next->valuedouble;
        cJSON_Delete(group);
    }
    free(p);
    return ESP_ERR_NOT_FOUND;
}

/* Normal publication touches a single group and its neighbours. Recovery above
 * remains the authority when power is lost between these durable writes. */
esp_err_t app_index_publish(local_app_store_handle_t s, uint32_t id, const cJSON *apps)
{
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    app_path(s, "index.json", p);
    cJSON *summary = app_json_read(p);
    group_path(s, id, "/index.json", p);
    cJSON *group = app_json_read(p);
    if (!summary) {
        free(p);
        cJSON_Delete(group);
        return ESP_ERR_INVALID_STATE;
    }
    int old_size = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(group, "apps"));
    int new_size = cJSON_GetArraySize(apps);
    const cJSON *last = cJSON_GetObjectItemCaseSensitive(summary, "last");
    uint32_t prev = APP_NONE, next = APP_NONE;
    if (old_size) {
        prev = (uint32_t)cJSON_GetObjectItemCaseSensitive(group, "prev")->valuedouble;
        next = (uint32_t)cJSON_GetObjectItemCaseSensitive(group, "next")->valuedouble;
    } else if (cJSON_IsObject(last))
        prev = (uint32_t)cJSON_GetObjectItemCaseSensitive(last, "group")->valuedouble;
    cJSON_Delete(group);
    group = cJSON_CreateObject();
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!group || new_size > LOCAL_APP_GROUP_SIZE)
        goto done;
    cJSON_AddNumberToObject(group, "schema", 1);
    cJSON_AddNumberToObject(group, "group", id);
    cJSON_AddNumberToObject(group, "prev", prev);
    cJSON_AddNumberToObject(group, "next", next);
    cJSON *copy = cJSON_Duplicate(apps, true);
    if (!copy || !cJSON_AddItemToObject(group, "apps", copy)) {
        cJSON_Delete(copy);
        goto done;
    }
    group_path(s, id, "/index.json", p);
    err = app_json_write(p, group);
    if (err != ESP_OK)
        goto done;
    if ((!old_size && new_size) || (old_size && !new_size)) {
        uint32_t neighbors[] = {prev, next};
        for (int side = 0; side < 2; side++) {
            if (neighbors[side] == APP_NONE)
                continue;
            group_path(s, neighbors[side], "/index.json", p);
            cJSON *neighbor = app_json_read(p);
            if (!neighbor) {
                err = ESP_ERR_INVALID_STATE;
                goto done;
            }
            const char *key = side ? "prev" : "next";
            cJSON *link = cJSON_GetObjectItemCaseSensitive(neighbor, key);
            if (!cJSON_IsNumber(link))
                err = ESP_ERR_INVALID_STATE;
            else {
                cJSON_SetNumberValue(link, new_size ? id : neighbors[1 - side]);
                err = app_json_write(p, neighbor);
            }
            cJSON_Delete(neighbor);
            if (err != ESP_OK)
                goto done;
        }
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(summary, "count");
    size_t total = (size_t)value->valuedouble + (size_t)new_size - (size_t)old_size;
    cJSON_SetNumberValue(value, (double)total);
    value = cJSON_GetObjectItemCaseSensitive(summary, "group_count");
    cJSON_SetNumberValue(value, value->valuedouble + (new_size > 0) - (old_size > 0));
    value = cJSON_GetObjectItemCaseSensitive(summary, "revision");
    cJSON_SetNumberValue(value, (uint32_t)value->valuedouble + 1u);
    value = cJSON_GetObjectItemCaseSensitive(summary, "next_group");
    if (new_size && id >= value->valuedouble)
        cJSON_SetNumberValue(value, id + 1u);
    for (int side = 0; side < 2; side++) {
        const char *key = side ? "last" : "first";
        cJSON *end = cJSON_GetObjectItemCaseSensitive(summary, key), *replacement = NULL;
        uint32_t endpoint =
            cJSON_IsObject(end) ? (uint32_t)cJSON_GetObjectItemCaseSensitive(end, "group")->valuedouble : id;
        if (!total)
            replacement = cJSON_CreateNull();
        else if (endpoint == id || (!old_size && side)) {
            uint32_t g = new_size ? id : side ? prev : next;
            group_path(s, g, "/index.json", p);
            cJSON *v = app_json_read(p);
            cJSON *array = cJSON_GetObjectItemCaseSensitive(v, "apps");
            replacement =
                cJSON_Duplicate(cJSON_GetArrayItem(array, side ? cJSON_GetArraySize(array) - 1 : 0), true);
            if (replacement)
                cJSON_AddNumberToObject(replacement, "group", g);
            cJSON_Delete(v);
        } else
            replacement = cJSON_Duplicate(end, true);
        if (!replacement || !cJSON_ReplaceItemInObjectCaseSensitive(summary, key, replacement)) {
            cJSON_Delete(replacement);
            err = ESP_ERR_NO_MEM;
            goto done;
        }
    }
    app_path(s, "index.json", p);
    err = app_json_write(p, summary);
    if (err == ESP_OK) {
        app_path(s, ".index-dirty", p);
        if (unlink(p))
            err = ESP_FAIL;
    }
    app_index_invalidate(s);
done:
    cJSON_Delete(summary);
    cJSON_Delete(group);
    free(p);
    return err;
}

esp_err_t app_index_edit(local_app_store_handle_t s, uint32_t group, const char *id, const cJSON *manifest)
{
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    group_path(s, group, "/index.json", p);
    cJSON *j = app_json_read(p);
    free(p);
    cJSON *apps =
        j ? cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(j, "apps"), true) : cJSON_CreateArray();
    cJSON_Delete(j);
    if (!apps)
        return ESP_ERR_NO_MEM;
    int found = -1;
    for (int i = 0; i < cJSON_GetArraySize(apps); i++) {
        const char *key = app_string(cJSON_GetArrayItem(apps, i), "app_id");
        if (key && !strcmp(key, id)) {
            found = i;
            break;
        }
    }
    if (found >= 0)
        cJSON_DeleteItemFromArray(apps, found);
    esp_err_t err = ESP_OK;
    if (manifest) {
        cJSON *v = entry(manifest);
        if (!v)
            err = ESP_ERR_NO_MEM;
        else if (found >= 0)
            cJSON_InsertItemInArray(apps, found, v);
        else
            cJSON_AddItemToArray(apps, v);
    }
    if (err == ESP_OK)
        err = app_index_publish(s, group, apps);
    cJSON_Delete(apps);
    return err;
}
