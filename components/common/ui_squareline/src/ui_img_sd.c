/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ui_img_sd.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui_img_sd";

/* LVGL POSIX FS drive letter configured in sdkconfig.defaults as 'S' (83).
 * The path format is "S:/fatfs/ui/<name>.bin" or "S:/sdcard_mount/ui/<name>.bin". */
#define UI_IMG_SD_DRIVE_LETTER  'S'
#define UI_IMG_SD_SUBDIR        "ui"

/* Enough slots to cover all concurrent lv_img_set_src calls on one screen.
 * SquareLine screens call set_src up to 8 times during init, so 8 slots with
 * round-robin rotation is safe — LVGL copies the pointer before the next call. */
#define UI_IMG_SD_SLOTS         8
#define UI_IMG_SD_PATH_MAX      128

static char s_base_path[64] = "";
static char s_paths[UI_IMG_SD_SLOTS][UI_IMG_SD_PATH_MAX];
static int  s_slot = 0;

void ui_img_sd_set_base_path(const char *base_path)
{
    if (base_path) {
        snprintf(s_base_path, sizeof(s_base_path), "%s", base_path);
        ESP_LOGI(TAG, "base path set: \"%s\"", s_base_path);
    } else {
        s_base_path[0] = '\0';
        ESP_LOGW(TAG, "base path cleared (NULL passed)");
    }
}

const char *ui_img_sd_path(const char *name)
{
    if (!name) {
        ESP_LOGE(TAG, "ui_img_sd_path: name is NULL");
        return NULL;
    }
    if (s_base_path[0] == '\0') {
        ESP_LOGE(TAG, "ui_img_sd_path: base path not set, cannot resolve \"%s\"", name);
        return NULL;
    }
    char *buf = s_paths[s_slot % UI_IMG_SD_SLOTS];
    s_slot++;
    snprintf(buf, UI_IMG_SD_PATH_MAX, "%c:%s/%s/%s.bin",
             UI_IMG_SD_DRIVE_LETTER, s_base_path, UI_IMG_SD_SUBDIR, name);

    /* 1. POSIX fopen — verifies the file exists on the VFS layer */
    const char *vfs_path = buf + 2; /* skip "S:" */
    FILE *f = fopen(vfs_path, "rb");
    if (f) {
        /* Read header (12 bytes) + up to 32 bytes of palette/data for diagnostics */
        uint8_t hdr[12] = {0};
        uint8_t extra[32] = {0};
        size_t nr = fread(hdr, 1, sizeof(hdr), f);
        size_t nr_extra = fread(extra, 1, sizeof(extra), f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);

        /* hdr layout (LVGL v9, little-endian):
         *   [0]    magic  (0x19)
         *   [1]    cf     (0x09=I1, 0x0A=I8)
         *   [2-3]  flags
         *   [4-5]  w
         *   [6-7]  h
         *   [8-9]  stride
         *   [10-11] reserved */
        uint8_t cf   = hdr[1];
        uint16_t w   = (uint16_t)(hdr[4] | (hdr[5] << 8));
        uint16_t h   = (uint16_t)(hdr[6] | (hdr[7] << 8));
        uint16_t stride = (uint16_t)(hdr[8] | (hdr[9] << 8));
        const char *cf_str = (cf == 0x09) ? "I1" :
                             (cf == 0x0A) ? "I8" :
                             (cf == 0x0B) ? "I2" :
                             (cf == 0x0C) ? "I4" : "??";

        ESP_LOGI(TAG, "POSIX OK  %s  size=%ld cf=%s(0x%02x) %ux%u stride=%u hdr=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 buf, sz, cf_str, cf, w, h, stride,
                 hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5],
                 hdr[6], hdr[7], hdr[8], hdr[9], hdr[10], hdr[11]);

        /* For indexed formats, the palette immediately follows the header.
         * I1: 2 entries × 4 bytes (RGBA) = 8 bytes.
         * I2: 4 entries × 4 bytes (RGBA) = 16 bytes.
         * I4: 16 entries × 4 bytes (RGBA) = 64 bytes.
         * I8: 256 entries × 4 bytes = 1024 bytes (only print first 2 here).
         * Log as "color_index: R G B A" to make invisible/transparent entries obvious. */
        if (cf == 0x09 && nr_extra >= 8) {
            /* I1: 2-color palette */
            ESP_LOGI(TAG, "  I1 palette[0] RGBA=%02x %02x %02x %02x  palette[1] RGBA=%02x %02x %02x %02x",
                     extra[0], extra[1], extra[2], extra[3],
                     extra[4], extra[5], extra[6], extra[7]);
        } else if (cf == 0x0B && nr_extra >= 16) {
            /* I2: 4-color palette */
            ESP_LOGI(TAG, "  I2 palette[0] RGBA=%02x %02x %02x %02x  palette[1] RGBA=%02x %02x %02x %02x",
                     extra[0], extra[1], extra[2], extra[3],
                     extra[4], extra[5], extra[6], extra[7]);
            ESP_LOGI(TAG, "     palette[2] RGBA=%02x %02x %02x %02x  palette[3] RGBA=%02x %02x %02x %02x",
                     extra[8], extra[9], extra[10], extra[11],
                     extra[12], extra[13], extra[14], extra[15]);
        } else if (cf == 0x0C && nr_extra >= 16) {
            /* I4: 16-color palette, print first 4 entries */
            ESP_LOGI(TAG, "  I4 palette[0] RGBA=%02x %02x %02x %02x  palette[1] RGBA=%02x %02x %02x %02x",
                     extra[0], extra[1], extra[2], extra[3],
                     extra[4], extra[5], extra[6], extra[7]);
            ESP_LOGI(TAG, "     palette[2] RGBA=%02x %02x %02x %02x  palette[3] RGBA=%02x %02x %02x %02x  ...",
                     extra[8], extra[9], extra[10], extra[11],
                     extra[12], extra[13], extra[14], extra[15]);
        } else if (cf == 0x0A && nr_extra >= 8) {
            /* I8: print just first two palette entries so log isn't flooded */
            ESP_LOGI(TAG, "  I8 palette[0] RGBA=%02x %02x %02x %02x  palette[1] RGBA=%02x %02x %02x %02x  ...",
                     extra[0], extra[1], extra[2], extra[3],
                     extra[4], extra[5], extra[6], extra[7]);
        }
    } else {
        ESP_LOGE(TAG, "POSIX FAIL %s  (file missing or unreadable on VFS)", buf);
    }

    /* 2. lv_fs_open — verifies the LVGL POSIX driver is registered for 'S' */
    lv_fs_file_t lv_file;
    lv_fs_res_t lv_res = lv_fs_open(&lv_file, buf, LV_FS_MODE_RD);
    if (lv_res == LV_FS_RES_OK) {
        lv_fs_close(&lv_file);
        ESP_LOGI(TAG, "LV_FS  OK  %s", buf);
    } else {
        ESP_LOGE(TAG, "LV_FS FAIL %s  lv_fs_res=%d  (driver not registered or path rejected)", buf, (int)lv_res);
    }

    return buf;
}
