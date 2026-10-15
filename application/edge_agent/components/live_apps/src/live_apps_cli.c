/* UART diagnostics for the BLE firmware; no Classic SPP commands or installs. */
#include "live_apps.h"
#include "cap_lua.h"
#include "claw_paths.h"
#include "claw_skill.h"
#include "claw_version.h"
#include "display_service.h"
#include "esp_console.h"
#include "esp_app_desc.h"
#include "system_ui_home.h"
#include "ble_host_bridge.h"
#include "esp_heap_caps.h"
#include "lua_module_clip_ui.h"
#include "mbedtls/base64.h"
#include "system_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static struct { uint8_t *screen_pixels; size_t screen_bytes;
                unsigned screen_width, screen_height, screen_id; } s_capture;
static const char *string_field(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static char *read_text(const char *path) {
    struct stat st;
    if (stat(path, &st) || st.st_size < 0 || st.st_size > 65536) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *text = calloc(st.st_size + 1, 1);
    if (text && fread(text, 1, st.st_size, f) != (size_t)st.st_size) { free(text); text = NULL; }
    fclose(f); return text;
}
static bool installed_entry(const claw_skill_catalog_entry_t *entry) {
    const char *data = claw_paths_get(CLAW_PATH_DATA);
    size_t n = strlen(data);
    return entry && entry->skill_dir && !strncmp(entry->skill_dir, data, n) &&
           !strncmp(entry->skill_dir + n, "/skills/", 8) && entry->execution &&
           entry->execution->entry && entry->execution->visible;
}
static esp_err_t list_entry(const claw_skill_catalog_entry_t *entry, void *ctx) {
    if (!installed_entry(entry)) return ESP_OK;
    cJSON *app = cJSON_CreateObject();
    if (!app) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(app, "id", entry->id);
    cJSON_AddStringToObject(app, "entry", entry->execution->entry);
    cJSON_AddItemToArray(ctx, app); return ESP_OK;
}
static cJSON *command_reply(const cJSON *command) {
    cJSON *reply = cJSON_CreateObject();
    if (!reply) return NULL;
    typeof(s_capture) *h = &s_capture;
    const char *op = string_field(command, "op"), *id = string_field(command, "id");
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (!op) {
        err = ESP_ERR_INVALID_ARG;
    } else if (!strcmp(op, "hello") || !strcmp(op, "device_info")) {
        cJSON_AddStringToObject(reply, "protocol", "esp-claw-ble-live-apps");
        cJSON_AddStringToObject(reply, "version", "backclip-" SYSTEM_UI_RELEASE);
        cJSON_AddStringToObject(reply, "release", SYSTEM_UI_RELEASE);
        {
            /* git describe 缓存可能过期：以应用 ELF 哈希识别实际刷写的构建 */
            char elf[17] = {0};
            (void)esp_app_get_elf_sha256(elf, sizeof(elf));
            cJSON_AddStringToObject(reply, "elf_sha256", elf);
        }
        cJSON_AddStringToObject(reply, "mode", system_ui_home_mode_name());
        cJSON_AddStringToObject(reply, "firmware", claw_get_git_version());
        cJSON_AddBoolToObject(reply, "ble_voice", true);
        cJSON_AddBoolToObject(reply, "live_actions", true);
        cJSON_AddBoolToObject(reply, "recording", system_ui_workbuddy_audio_capturing());
        cJSON_AddNumberToObject(reply, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(reply, "free_internal_8bit",
                               heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        cJSON_AddItemToObject(reply, "network", clip_live_status(live_apps_network()));
        err = ESP_OK;
    } else if (!strcmp(op, "live_ui_state")) {
        char *raw = NULL; err = lua_module_clip_ui_inspect(&raw);
        if (raw) cJSON_AddItemToObject(reply, "ui", cJSON_Parse(raw));
        free(raw);
    } else if (!strcmp(op, "app_list")) {
        cJSON *apps = cJSON_AddArrayToObject(reply, "apps");
        err = apps ? claw_skill_foreach_catalog_entry(list_entry, apps) : ESP_ERR_NO_MEM;
    } else if (!strcmp(op, "app_get") || !strcmp(op, "app_run")) {
        claw_skill_catalog_entry_t entry;
        err = id ? claw_skill_get_catalog_entry(id, &entry) : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && !installed_entry(&entry)) err = ESP_ERR_INVALID_ARG;
        if (err == ESP_OK && !strcmp(op, "app_get")) {
            char *text = read_text(entry.execution->entry);
            if (text) { cJSON_AddStringToObject(reply, "source", text); free(text); }
            else err = ESP_ERR_NOT_FOUND;
        } else if (err == ESP_OK) {
            char *output = calloc(1, 4096);
            if (!output) err = ESP_ERR_NO_MEM;
            else { err = cap_lua_run_script_async(entry.execution->entry, NULL, 0, id,
                       "display", true, output, 4096);
                   cJSON_AddStringToObject(reply, "job", output); free(output); }
        }
    } else if (!strcmp(op, "app_stop")) {
        char *output = calloc(1, 4096);
        err = output ? cap_lua_stop_all_jobs("display", 3000, output, 4096) : ESP_ERR_NO_MEM;
        free(output);
        if (err == ESP_OK) err = system_ui_show_home();
    } else if (!strcmp(op, "live_action")) {
        cJSON *result = NULL;
        err = clip_live_action(live_apps_network(), cJSON_GetObjectItemCaseSensitive(command, "request"), &result);
        if (result) cJSON_AddItemToObject(reply, "result", result);
    } else if (!strcmp(op, "network_get")) {
        cJSON *status = clip_live_status(live_apps_network());
        err = status ? ESP_OK : ESP_ERR_NO_MEM;
        if (status) cJSON_AddItemToObject(reply, "network", status);
    } else if (!strcmp(op, "feed_get")) {
        char *text = NULL; int64_t age;
        err = live_apps_feed_read(id, &text, &age, NULL);
        if (err == ESP_OK) { cJSON_AddItemToObject(reply, "payload", cJSON_Parse(text));
                            cJSON_AddNumberToObject(reply, "age_ms", age); }
        free(text);
    } else if (!strcmp(op, "screen_capture")) {
        const cJSON *scale_value = cJSON_GetObjectItemCaseSensitive(command, "scale");
        int scale = cJSON_IsNumber(scale_value) ? scale_value->valueint : 1;
        err = (scale == 1 || scale == 2) ? display_service_lock() : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) {
            lv_obj_update_layout(lv_screen_active());
            lv_draw_buf_t *image = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
            display_service_unlock();
            if (!image) {
                err = ESP_ERR_NO_MEM;
            } else {
                unsigned w = image->header.w / (unsigned)scale, hgt = image->header.h / (unsigned)scale;
                size_t bytes = (size_t)w * hgt * 2;
                uint8_t *pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!pixels) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    for (unsigned y = 0; y < hgt; y++) {
                        for (unsigned x = 0; x < w; x++) {
                            memcpy(pixels + ((size_t)y * w + x) * 2,
                                   image->data + (size_t)y * scale * image->header.stride +
                                       (size_t)x * scale * 2,
                                   2);
                        }
                    }
                    bool rle = false;
                    const char *encoding = string_field(command, "encoding");
                    if (encoding && !strcmp(encoding, "rle")) {
                        size_t runs = 0;
                        for (size_t i = 0; i < bytes / 2;) {
                            size_t n = 1;
                            while (i + n < bytes / 2 && n < 65535 &&
                                   !memcmp(pixels + i * 2, pixels + (i + n) * 2, 2)) {
                                n++;
                            }
                            runs++;
                            i += n;
                        }
                        uint8_t *encoded = heap_caps_malloc(runs * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (encoded) {
                            size_t out = 0;
                            for (size_t i = 0; i < bytes / 2;) {
                                size_t n = 1;
                                while (i + n < bytes / 2 && n < 65535 &&
                                       !memcmp(pixels + i * 2, pixels + (i + n) * 2, 2)) {
                                    n++;
                                }
                                encoded[out++] = (uint8_t)n;
                                encoded[out++] = (uint8_t)(n >> 8);
                                encoded[out++] = pixels[i * 2];
                                encoded[out++] = pixels[i * 2 + 1];
                                i += n;
                            }
                            free(pixels);
                            pixels = encoded;
                            bytes = runs * 4;
                            rle = true;
                        }
                    }
                    free(h->screen_pixels);
                    h->screen_pixels = pixels;
                    h->screen_bytes = bytes;
                    h->screen_width = w;
                    h->screen_height = hgt;
                    h->screen_id++;
                    cJSON_AddNumberToObject(reply, "capture_id", h->screen_id);
                    cJSON_AddNumberToObject(reply, "width", w);
                    cJSON_AddNumberToObject(reply, "height", hgt);
                    cJSON_AddNumberToObject(reply, "bytes", (double)bytes);
                    cJSON_AddStringToObject(reply, "format", rle ? "rle565le" : "rgb565le");
                }
                lv_draw_buf_destroy(image);
            }
        }
    } else if (!strcmp(op, "screen_read")) {
        const cJSON *capture = cJSON_GetObjectItemCaseSensitive(command, "capture_id"),
                    *offset = cJSON_GetObjectItemCaseSensitive(command, "offset"),
                    *maximum = cJSON_GetObjectItemCaseSensitive(command, "max_bytes");
        err = ESP_ERR_INVALID_ARG;
        if (h->screen_pixels && cJSON_IsNumber(capture) && capture->valueint == (int)h->screen_id &&
            cJSON_IsNumber(offset) && offset->valuedouble >= 0 && offset->valuedouble <= h->screen_bytes &&
            offset->valuedouble == offset->valueint && cJSON_IsNumber(maximum) && maximum->valueint > 0 &&
            maximum->valueint <= 16384) {
            size_t start = (size_t)offset->valueint, n = h->screen_bytes - start;
            if (n > (size_t)maximum->valueint) {
                n = (size_t)maximum->valueint;
            }
            size_t capacity = n * 4 / 3 + 8, used = 0;
            uint8_t *encoded = malloc(capacity);
            err = encoded ? ESP_OK : ESP_ERR_NO_MEM;
            if (encoded) {
                if (mbedtls_base64_encode(encoded, capacity, &used, h->screen_pixels + start, n)) {
                    err = ESP_FAIL;
                } else {
                    encoded[used] = 0;
                    cJSON_AddStringToObject(reply, "data", (char *)encoded);
                    cJSON_AddNumberToObject(reply, "offset", (double)start);
                    cJSON_AddNumberToObject(reply, "bytes", (double)n);
                }
                free(encoded);
            }
        }
    } else if (!strcmp(op, "screen_release")) {
        free(h->screen_pixels);
        h->screen_pixels = NULL;
        h->screen_bytes = 0;
        err = ESP_OK;

    } else {
        /* 1.0 首页 / 模式 / 应用目录 / 模拟用户输入（ui_key、ui_touch、home_state、voice_text） */
        cJSON *out = NULL;
        err = system_ui_home_handle_op(op, command, true, &out);
        if (err == ESP_ERR_NOT_SUPPORTED && !strncmp(op, "app_", 4)) {
            /* 应用包协议：与手机 App 的 BLE 请求同一处理器 */
            char *text = NULL;
            err = ble_host_apps_handle(op, command, &text);
            out = text ? cJSON_Parse(text) : NULL;
            if (text && !out) {
                out = cJSON_CreateObject();
                if (out) cJSON_AddStringToObject(out, "data", text);
            }
            free(text);
        }
        if (out) {
            cJSON *item = out->child;
            while (item) {
                cJSON *next = item->next;
                cJSON_AddItemToObject(reply, item->string, cJSON_DetachItemViaPointer(out, item));
                item = next;
            }
            cJSON_Delete(out);
        }
    }
    cJSON_AddBoolToObject(reply, "ok", err == ESP_OK);
    if (err != ESP_OK) cJSON_AddStringToObject(reply, "error", esp_err_to_name(err));
    return reply;
}
static int console_command(int argc, char **argv) {
    if (argc != 2) { printf("clip_cmd <JSON>\n"); return 1; }
    cJSON *request = cJSON_Parse(argv[1]);
    cJSON *result = command_reply(request);
    char *text = result ? cJSON_PrintUnformatted(result) : NULL;
    if (text) printf("CLIP_RESULT %s\n", text);
    free(text); cJSON_Delete(request); cJSON_Delete(result); return 0;
}
esp_err_t live_apps_cli_register(void) {
    const esp_console_cmd_t command = {.command = "clip_cmd",
        .help = "BLE live-app diagnostics (no install/uninstall)", .func = console_command};
    return esp_console_cmd_register(&command);
}
