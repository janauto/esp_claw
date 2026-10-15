#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
#define LOCAL_APP_DIRECTORY "apps"
#define LOCAL_APP_GROUP_SIZE 10
#define LOCAL_APP_CHUNK_MAX 1024
#define LOCAL_APP_SCRIPT_MAX 65536
#define LOCAL_APP_ICON_SIZE 131084
/* Handles require external serialization (the display lock on devices). */
typedef struct local_app_store *local_app_store_handle_t;
typedef struct {
    const char *root;
} local_app_store_config_t;
esp_err_t local_app_store_create(const local_app_store_config_t *, local_app_store_handle_t *);
void local_app_store_delete(local_app_store_handle_t);
esp_err_t local_app_store_begin(local_app_store_handle_t, const cJSON *);
esp_err_t local_app_store_write(local_app_store_handle_t, const char *, const char *, size_t, const void *,
                                size_t);
esp_err_t local_app_store_commit(local_app_store_handle_t, const char *);
esp_err_t local_app_store_abort(local_app_store_handle_t, const char *);
esp_err_t local_app_store_remove(local_app_store_handle_t, const char *);
esp_err_t local_app_store_rename(local_app_store_handle_t, const char *, const char *);
esp_err_t local_app_store_get(local_app_store_handle_t, const char *, cJSON **);
esp_err_t local_app_store_directory(local_app_store_handle_t, const char *, char *, size_t);
esp_err_t local_app_store_read(local_app_store_handle_t, const char *, const char *, const char *, size_t,
                               void *, size_t, size_t *, size_t *);
esp_err_t local_app_store_list(local_app_store_handle_t, size_t, cJSON **);
esp_err_t local_app_store_summary(local_app_store_handle_t, cJSON **);
esp_err_t local_app_store_at(local_app_store_handle_t, size_t, cJSON **);
esp_err_t local_app_store_reload(local_app_store_handle_t);

/* Stable package task identity, fits cap_lua's 31-byte name field. */
esp_err_t local_app_store_job_name(const char *id, char *out, size_t size);
