#pragma once
#include "local_app_store.h"
#include <stdbool.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#ifndef LOCAL_APP_HOST
/* ESP FAT VFS has no symlinks and exposes stat rather than lstat. */
#define lstat stat
#endif
#define APP_PATH_CAP 512
#define APP_NONE UINT32_MAX
struct local_app_store {
    char *root;
    cJSON *cached_group;
    uint32_t revision;
    uint32_t group;
    size_t start;
};
const char *app_string(const cJSON *, const char *);
bool app_id_valid(const char *);
bool app_title_valid(const char *);
bool app_manifest_valid(const cJSON *, const char *);
const cJSON *app_file_spec(const cJSON *, const char *);
bool app_is_dir(const char *);
bool app_exists(const char *);
esp_err_t app_path(local_app_store_handle_t, const char *, char *);
cJSON *app_json_read(const char *);
esp_err_t app_json_write(const char *, const cJSON *);
esp_err_t app_mkdir(const char *);
esp_err_t app_remove_tree(char *, unsigned);
esp_err_t app_index_ready(local_app_store_handle_t);
esp_err_t app_index_dirty(local_app_store_handle_t);
void app_index_invalidate(local_app_store_handle_t);
/* Test builds count real filesystem I/O; device builds use POSIX directly. */
#ifdef LOCAL_APP_TEST_IO
FILE *app_probe_fopen(const char *, const char *);
DIR *app_probe_opendir(const char *);
#define fopen app_probe_fopen
#define opendir app_probe_opendir
#endif

esp_err_t app_index_edit(local_app_store_handle_t, uint32_t group, const char *id, const cJSON *manifest);
