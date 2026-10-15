#pragma once
#include "esp_err.h"
#include <stdbool.h>
typedef struct { void *user_data; } esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *event);
typedef struct mock_client *esp_http_client_handle_t;
typedef struct {
    const char *url;
    int timeout_ms, buffer_size, buffer_size_tx;
    bool keep_alive_enable;
    esp_err_t (*crt_bundle_attach)(void *);
    http_event_handle_cb event_handler;
    void *user_data;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data);
esp_err_t esp_http_client_set_event_handler(esp_http_client_handle_t client, http_event_handle_cb cb);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
