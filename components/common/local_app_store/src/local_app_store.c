#include "local_app_private.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef LOCAL_APP_HOST
#include <openssl/evp.h>
#else
#include "psa/crypto.h"
#endif

bool app_id_valid(const char *id)
{
    if (!id || !*id || strlen(id) > 32)
        return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return false;
    return true;
}
bool app_title_valid(const char *s)
{
    if (!s || !*s || strlen(s) > 95)
        return false;
    bool visible = false;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p++;
        if (c < 32 || c == 127)
            return false;
        if (c != ' ')
            visible = true;
        if (c < 128)
            continue;
        unsigned count;
        uint32_t point, minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            point = c & 31;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            point = c & 15;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            point = c & 7;
            minimum = 0x10000;
        } else
            return false;
        while (count--) {
            if ((*p & 0xc0) != 0x80)
                return false;
            point = point * 64 + (*p++ & 63);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
            return false;
    }
    return visible;
}
static bool integer(const cJSON *v, double min, double max)
{
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= min && v->valuedouble <= max &&
           floor(v->valuedouble) == v->valuedouble;
}
static bool digest_valid(const char *s)
{
    if (!s || strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}
const cJSON *app_file_spec(const cJSON *m, const char *file)
{
    if (!file || (strcmp(file, "main.lua") && strcmp(file, "icon_selected.bin") &&
                  strcmp(file, "icon_unselected.bin")))
        return NULL;
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "files"), file);
}
bool app_manifest_valid(const cJSON *m, const char *id)
{
    const char *mid = app_string(m, "app_id");
    if (!cJSON_IsObject(m) || !integer(cJSON_GetObjectItemCaseSensitive(m, "schema"), 1, 1) ||
        !app_id_valid(mid) || (id && strcmp(mid, id)) || !app_title_valid(app_string(m, "title")) ||
        !integer(cJSON_GetObjectItemCaseSensitive(m, "frame_color"), 0, 0xffffff) ||
        !integer(cJSON_GetObjectItemCaseSensitive(m, "pill_color"), 0, 0xffffff))
        return false;
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(m, "files");
    if (!cJSON_IsObject(files) || !app_file_spec(m, "main.lua"))
        return false;
    const cJSON *f;
    unsigned n = 0;
    cJSON_ArrayForEach(f, files)
    {
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(f, "size");
        if (++n > 3 || !app_file_spec(m, f->string) || !cJSON_IsObject(f) ||
            !digest_valid(app_string(f, "sha256")) ||
            !integer(size, !strcmp(f->string, "main.lua") ? 1 : LOCAL_APP_ICON_SIZE,
                     !strcmp(f->string, "main.lua") ? LOCAL_APP_SCRIPT_MAX : LOCAL_APP_ICON_SIZE))
            return false;
        for (const cJSON *other = f->next; other; other = other->next)
            if (!strcmp(f->string, other->string))
                return false;
    }
    return true;
}
esp_err_t local_app_store_create(const local_app_store_config_t *config, local_app_store_handle_t *out)
{
    if (!config || !config->root || !out || strlen(config->root) > APP_PATH_CAP - 96)
        return ESP_ERR_INVALID_ARG;
    *out = calloc(1, sizeof(**out));
    if (!*out)
        return ESP_ERR_NO_MEM;
    (*out)->root = strdup(config->root);
    (*out)->group = APP_NONE;
    if (!(*out)->root) {
        free(*out);
        *out = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void local_app_store_delete(local_app_store_handle_t s)
{
    if (s) {
        app_index_invalidate(s);
        free(s->root);
        free(s);
    }
}
static esp_err_t staging(local_app_store_handle_t s, const char *id, char *p)
{
    if (!s || !app_id_valid(id))
        return ESP_ERR_INVALID_ARG;
    int n = snprintf(p, APP_PATH_CAP, "%s/.install-%s", s->root, id);
    return n > 0 && n < APP_PATH_CAP ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t local_app_store_begin(local_app_store_handle_t s, const cJSON *manifest)
{
    if (!s || !app_manifest_valid(manifest, NULL))
        return ESP_ERR_INVALID_ARG;
    const char *id = app_string(manifest, "app_id");
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    esp_err_t e = local_app_store_directory(s, id, p, APP_PATH_CAP);
    if (e == ESP_OK) {
        free(p);
        return ESP_ERR_INVALID_STATE;
    }
    if (e != ESP_ERR_NOT_FOUND) {
        free(p);
        return e;
    }
    e = staging(s, id, p);
    if (e == ESP_OK)
        e = app_remove_tree(p, 0);
    if (e == ESP_OK)
        e = app_mkdir(p);
    if (e == ESP_OK) {
        size_t n = strlen(p);
        snprintf(p + n, APP_PATH_CAP - n, "/app.json");
        e = app_json_write(p, manifest);
    }
    free(p);
    return e;
}
esp_err_t local_app_store_abort(local_app_store_handle_t s, const char *id)
{
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    esp_err_t e = staging(s, id, p);
    if (e == ESP_OK)
        e = app_remove_tree(p, 0);
    free(p);
    return e;
}
static cJSON *staged_manifest(local_app_store_handle_t s, const char *id, char *p)
{
    if (staging(s, id, p) != ESP_OK)
        return NULL;
    size_t base = strlen(p);
    snprintf(p + base, APP_PATH_CAP - base, "/app.json");
    cJSON *m = app_json_read(p);
    p[base] = 0;
    if (!app_manifest_valid(m, id)) {
        cJSON_Delete(m);
        return NULL;
    }
    return m;
}
esp_err_t local_app_store_write(local_app_store_handle_t s, const char *id, const char *file, size_t offset,
                                const void *bytes, size_t size)
{
    if (!s || !app_id_valid(id) || !bytes || !size || size > LOCAL_APP_CHUNK_MAX)
        return ESP_ERR_INVALID_ARG;
    char *p = malloc(APP_PATH_CAP);
    if (!p)
        return ESP_ERR_NO_MEM;
    cJSON *m = staged_manifest(s, id, p);
    const cJSON *spec = app_file_spec(m, file);
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (spec) {
        size_t expected = (size_t)cJSON_GetObjectItemCaseSensitive(spec, "size")->valuedouble;
        if (offset <= expected && size <= expected - offset) {
            size_t base = strlen(p);
            snprintf(p + base, APP_PATH_CAP - base, "/%s", file);
            struct stat st;
            bool exists = lstat(p, &st) == 0;
            if ((!exists && offset == 0) || (exists && S_ISREG(st.st_mode) && (size_t)st.st_size == offset)) {
                FILE *f = fopen(p, offset ? "ab" : "wb");
                if (f) {
                    bool ok = fwrite(bytes, 1, size, f) == size && fflush(f) == 0;
                    if (fclose(f))
                        ok = false;
                    e = ok ? ESP_OK : ESP_FAIL;
                } else
                    e = ESP_FAIL;
            }
        }
    }
    cJSON_Delete(m);
    free(p);
    return e;
}
static esp_err_t verify_file(const char *path, const char *name, const cJSON *spec)
{
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) ||
        st.st_size != (off_t)cJSON_GetObjectItemCaseSensitive(spec, "size")->valuedouble)
        return ESP_ERR_INVALID_SIZE;
    FILE *f = fopen(path, "rb");
    if (!f)
        return ESP_FAIL;
    unsigned char *buf = malloc(LOCAL_APP_CHUNK_MAX), *hash = malloc(32);
    if (!buf || !hash) {
        free(buf);
        free(hash);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = ESP_OK;
#ifdef LOCAL_APP_HOST
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool started = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
#else
    psa_hash_operation_t ctx = PSA_HASH_OPERATION_INIT;
    bool started = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&ctx, PSA_ALG_SHA_256) == PSA_SUCCESS;
#endif
    if (!started)
        e = ESP_FAIL;
    size_t n, offset = 0;
    static const unsigned char icon_header[12] = {0x19, 0x12, 0, 0, 0, 1, 0, 1, 0, 2, 0, 0};
    while (e == ESP_OK && (n = fread(buf, 1, LOCAL_APP_CHUNK_MAX, f))) {
        if (!offset && strcmp(name, "main.lua") &&
            (n < sizeof(icon_header) || memcmp(buf, icon_header, sizeof(icon_header))))
            e = ESP_ERR_INVALID_ARG;
#ifdef LOCAL_APP_HOST
        if (EVP_DigestUpdate(ctx, buf, n) != 1)
            e = ESP_FAIL;
#else
        if (psa_hash_update(&ctx, buf, n) != PSA_SUCCESS)
            e = ESP_FAIL;
#endif
        offset += n;
    }
    if (ferror(f))
        e = ESP_FAIL;
#ifdef LOCAL_APP_HOST
    if (started && EVP_DigestFinal_ex(ctx, hash, NULL) != 1)
        e = ESP_FAIL;
    EVP_MD_CTX_free(ctx);
#else
    size_t hash_size = 0;
    if (started && (psa_hash_finish(&ctx, hash, 32, &hash_size) != PSA_SUCCESS || hash_size != 32))
        e = ESP_FAIL;
    psa_hash_abort(&ctx);
#endif
    if (e == ESP_OK) {
        const char *digest = app_string(spec, "sha256");
        const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++)
            if (digest[2 * i] != hex[hash[i] >> 4] || digest[2 * i + 1] != hex[hash[i] & 15]) {
                e = ESP_ERR_INVALID_CRC;
                break;
            }
    }
    free(buf);
    free(hash);
    fclose(f);
    return e;
}
esp_err_t local_app_store_commit(local_app_store_handle_t s, const char *id)
{
    if (!s || !app_id_valid(id))
        return ESP_ERR_INVALID_ARG;
    char *p = malloc(APP_PATH_CAP), *dest = malloc(APP_PATH_CAP);
    if (!p || !dest) {
        free(p);
        free(dest);
        return ESP_ERR_NO_MEM;
    }
    cJSON *m = staged_manifest(s, id, p);
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (!m)
        goto done;
    const cJSON *f;
    size_t base = strlen(p);
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(m, "files"))
    {
        snprintf(p + base, APP_PATH_CAP - base, "/%s", f->string);
        e = verify_file(p, f->string, f);
        p[base] = 0;
        if (e != ESP_OK)
            goto done;
    }
    e = local_app_store_directory(s, id, dest, APP_PATH_CAP);
    if (e == ESP_OK) {
        e = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (e != ESP_ERR_NOT_FOUND)
        goto done;
    cJSON *summary = NULL;
    e = local_app_store_summary(s, &summary);
    if (e != ESP_OK)
        goto done;
    uint32_t group = (uint32_t)cJSON_GetObjectItemCaseSensitive(summary, "next_group")->valuedouble;
    size_t count = (size_t)cJSON_GetObjectItemCaseSensitive(summary, "count")->valuedouble;
    if (count) {
        cJSON *last = cJSON_GetObjectItemCaseSensitive(summary, "last");
        uint32_t tail = (uint32_t)cJSON_GetObjectItemCaseSensitive(last, "group")->valuedouble;
        snprintf(dest, APP_PATH_CAP, "%s/g%08lx/index.json", s->root, (unsigned long)tail);
        cJSON *g = app_json_read(dest);
        if (g && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(g, "apps")) < LOCAL_APP_GROUP_SIZE)
            group = tail;
        cJSON_Delete(g);
    }
    cJSON_Delete(summary);
    if (group == APP_NONE) {
        e = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    snprintf(dest, APP_PATH_CAP, "%s/g%08lx", s->root, (unsigned long)group);
    e = app_mkdir(dest);
    if (e != ESP_OK)
        goto done;
    size_t len = strlen(dest);
    snprintf(dest + len, APP_PATH_CAP - len, "/%s", id);
    if (app_exists(dest)) {
        e = ESP_ERR_INVALID_STATE;
        goto done;
    }
    e = app_index_dirty(s);
    if (e != ESP_OK)
        goto done;
    if (rename(p, dest)) {
        e = ESP_FAIL;
        goto done;
    }
    e = app_index_edit(s, group, id, m);
done:
    cJSON_Delete(m);
    free(p);
    free(dest);
    return e;
}
esp_err_t local_app_store_get(local_app_store_handle_t s, const char *id, cJSON **out)
{
    if (!s || !out || !app_id_valid(id))
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    char *p = malloc(APP_PATH_CAP), *rel = malloc(APP_PATH_CAP);
    if (!p || !rel) {
        free(p);
        free(rel);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = local_app_store_directory(s, id, rel, APP_PATH_CAP);
    if (e == ESP_OK) {
        int n = snprintf(p, APP_PATH_CAP, "%s/%s", s->root, rel);
        bool directory = n > 0 && n < APP_PATH_CAP && app_is_dir(p);
        if (directory) {
            char *slash = strrchr(p, '/');
            *slash = 0;
            directory = app_is_dir(p);
            *slash = '/';
        }
        n = snprintf(p, APP_PATH_CAP, "%s/%s/app.json", s->root, rel);
        *out = directory && n > 0 && n < APP_PATH_CAP ? app_json_read(p) : NULL;
        if (!app_manifest_valid(*out, id)) {
            cJSON_Delete(*out);
            *out = NULL;
            e = ESP_ERR_INVALID_STATE;
        }
    }
    free(p);
    free(rel);
    return e;
}
esp_err_t local_app_store_list(local_app_store_handle_t s, size_t offset, cJSON **out)
{
    if (!s || !out)
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    cJSON *summary = NULL;
    esp_err_t e = local_app_store_summary(s, &summary);
    if (e != ESP_OK)
        return e;
    size_t count = (size_t)cJSON_GetObjectItemCaseSensitive(summary, "count")->valuedouble;
    cJSON_Delete(summary);
    cJSON *result = cJSON_CreateObject(), *apps = cJSON_CreateArray();
    if (!result || !apps) {
        cJSON_Delete(result);
        cJSON_Delete(apps);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(result, "apps", apps);
    size_t i = offset;
    for (; i < count && i - offset < LOCAL_APP_GROUP_SIZE; i++) {
        cJSON *v = NULL, *m = NULL;
        e = local_app_store_at(s, i, &v);
        if (e != ESP_OK)
            break;
        const char *id = app_string(v, "app_id");
        e = local_app_store_get(s, id, &m);
        /* An offline edit can invalidate a cached index; list skips invalid packages. */
        if (e == ESP_OK) {
            char *p = malloc(APP_PATH_CAP);
            struct stat st;
            if (!p)
                e = ESP_ERR_NO_MEM;
            else {
                snprintf(p, APP_PATH_CAP, "%s/%s/main.lua", s->root, app_string(v, "directory"));
                if (lstat(p, &st) || !S_ISREG(st.st_mode) ||
                    st.st_size !=
                        (off_t)cJSON_GetObjectItemCaseSensitive(app_file_spec(m, "main.lua"), "size")
                            ->valuedouble) {
                    cJSON_Delete(m);
                    m = NULL;
                }
                free(p);
            }
        }
        cJSON_Delete(v);
        if (m)
            cJSON_AddItemToArray(apps, m);
        if (e == ESP_ERR_NO_MEM)
            break;
        e = ESP_OK;
    }
    if (e != ESP_OK) {
        cJSON_Delete(result);
        return e;
    }
    if (i < count)
        cJSON_AddNumberToObject(result, "next_offset", (double)i);
    else
        cJSON_AddNullToObject(result, "next_offset");
    *out = result;
    return ESP_OK;
}
esp_err_t local_app_store_read(local_app_store_handle_t s, const char *id, const char *file,
                               const char *digest, size_t offset, void *bytes, size_t capacity, size_t *size,
                               size_t *total)
{
    if (!s || !bytes || !size || !total || !capacity || capacity > LOCAL_APP_CHUNK_MAX ||
        !digest_valid(digest))
        return ESP_ERR_INVALID_ARG;
    *size = *total = 0;
    cJSON *m = NULL;
    esp_err_t e = local_app_store_get(s, id, &m);
    if (e != ESP_OK)
        return e;
    const cJSON *spec = app_file_spec(m, file);
    char *rel = malloc(APP_PATH_CAP), *p = malloc(APP_PATH_CAP);
    if (!rel || !p) {
        e = ESP_ERR_NO_MEM;
        goto done;
    }
    if (!spec || strcmp(app_string(spec, "sha256"), digest)) {
        e = ESP_ERR_INVALID_STATE;
        goto done;
    }
    *total = (size_t)cJSON_GetObjectItemCaseSensitive(spec, "size")->valuedouble;
    if (offset > *total) {
        e = ESP_ERR_INVALID_ARG;
        goto done;
    }
    e = local_app_store_directory(s, id, rel, APP_PATH_CAP);
    if (e != ESP_OK)
        goto done;
    snprintf(p, APP_PATH_CAP, "%s/%s/%s", s->root, rel, file);
    struct stat st;
    if (lstat(p, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size != *total) {
        e = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    FILE *f = fopen(p, "rb");
    if (!f) {
        e = ESP_FAIL;
        goto done;
    }
    size_t want = capacity < *total - offset ? capacity : *total - offset;
    if (fseek(f, (long)offset, SEEK_SET))
        e = ESP_FAIL;
    else {
        *size = fread(bytes, 1, want, f);
        if (*size != want || ferror(f))
            e = ESP_FAIL;
    }
    fclose(f);
done:
    free(rel);
    free(p);
    cJSON_Delete(m);
    return e;
}
esp_err_t local_app_store_remove(local_app_store_handle_t s, const char *id)
{
    if (!s || !app_id_valid(id))
        return ESP_ERR_INVALID_ARG;
    char *rel = malloc(APP_PATH_CAP), *p = malloc(APP_PATH_CAP), *hidden = malloc(APP_PATH_CAP);
    if (!rel || !p || !hidden) {
        free(rel);
        free(p);
        free(hidden);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = local_app_store_directory(s, id, rel, APP_PATH_CAP);
    if (e == ESP_OK) {
        app_path(s, rel, p);
        snprintf(hidden, APP_PATH_CAP, "%s/.removed-%s", s->root, id);
        e = app_remove_tree(hidden, 0);
        if (e == ESP_OK)
            e = app_index_dirty(s);
        if (e == ESP_OK && rename(p, hidden))
            e = ESP_FAIL;
        if (e == ESP_OK) {
            unsigned group = 0;
            sscanf(rel, "g%8x/", &group);
            e = app_index_edit(s, group, id, NULL);
        }
        if (e == ESP_OK)
            e = app_remove_tree(hidden, 0);
    }
    free(rel);
    free(p);
    free(hidden);
    return e;
}
esp_err_t local_app_store_rename(local_app_store_handle_t s, const char *id, const char *title)
{
    if (!app_title_valid(title))
        return ESP_ERR_INVALID_ARG;
    cJSON *m = NULL;
    esp_err_t e = local_app_store_get(s, id, &m);
    if (e != ESP_OK)
        return e;
    char *rel = malloc(APP_PATH_CAP), *p = malloc(APP_PATH_CAP);
    if (!rel || !p)
        e = ESP_ERR_NO_MEM;
    else {
        e = local_app_store_directory(s, id, rel, APP_PATH_CAP);
        if (e == ESP_OK)
            e = app_index_dirty(s);
        if (e == ESP_OK) {
            cJSON *v = cJSON_CreateString(title);
            if (!v || !cJSON_ReplaceItemInObjectCaseSensitive(m, "title", v)) {
                cJSON_Delete(v);
                e = ESP_ERR_NO_MEM;
            } else {
                snprintf(p, APP_PATH_CAP, "%s/%s/app.json", s->root, rel);
                e = app_json_write(p, m);
            }
        }
        if (e == ESP_OK) {
            unsigned group = 0;
            sscanf(rel, "g%8x/", &group);
            e = app_index_edit(s, group, id, m);
        }
    }
    free(rel);
    free(p);
    cJSON_Delete(m);
    return e;
}

/* cap_lua has a 31-byte job-name limit, while package IDs allow 32 bytes.
 * A stable 112-bit digest avoids truncating different IDs into the same job. */
esp_err_t local_app_store_job_name(const char *id, char *out, size_t size)
{
    if (!app_id_valid(id) || !out || size < 31)
        return ESP_ERR_INVALID_ARG;
    unsigned char hash[32];
#ifdef LOCAL_APP_HOST
    if (EVP_Digest(id, strlen(id), hash, NULL, EVP_sha256(), NULL) != 1)
        return ESP_FAIL;
#else
    size_t length = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)id, strlen(id), hash, sizeof(hash), &length) !=
            PSA_SUCCESS ||
        length != sizeof(hash))
        return ESP_FAIL;
#endif
    const char hex[] = "0123456789abcdef";
    out[0] = 'p';
    out[1] = ':';
    for (int i = 0; i < 14; i++) {
        out[2 + i * 2] = hex[hash[i] >> 4];
        out[3 + i * 2] = hex[hash[i] & 15];
    }
    out[30] = 0;
    return ESP_OK;
}
