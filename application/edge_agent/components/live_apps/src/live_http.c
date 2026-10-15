#include "live_http.h"
#include "esp_crt_bundle.h"
#include <stdlib.h>
#include <string.h>

struct live_http_t {
    esp_http_client_handle_t client;
    char origin[192];
};

void live_http_close(live_http_handle_t *slot) {
    if (!slot || !*slot) return;
    if ((*slot)->client) {
        esp_http_client_set_user_data((*slot)->client, NULL);
        esp_http_client_cleanup((*slot)->client);
    }
    free(*slot);
    *slot = NULL;
}

esp_err_t live_http_get(live_http_handle_t *slot, const char *url, const char *etag,
                        http_event_handle_cb handler, void *data,
                        void (*reset_response)(void *data), int *status) {
    if (!slot || !url || strncmp(url, "https://", 8) || !handler || !status)
        return ESP_ERR_INVALID_ARG;
    const char *slash = strchr(url + 8, '/');
    size_t length = slash ? (size_t)(slash - url) : strlen(url);
    if (length <= 8 || length >= sizeof(((struct live_http_t *)0)->origin))
        return ESP_ERR_INVALID_ARG;
    bool reused = *slot && strlen((*slot)->origin) == length &&
                  !strncmp((*slot)->origin, url, length);
    if (*slot && !reused) live_http_close(slot);
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        if (!*slot) {
            *slot = calloc(1, sizeof(**slot));
            if (!*slot) return ESP_ERR_NO_MEM;
            memcpy((*slot)->origin, url, length);
            esp_http_client_config_t *config = calloc(1, sizeof(*config));
            if (!config) { live_http_close(slot); return ESP_ERR_NO_MEM; }
            config->url = url;
            config->timeout_ms = 9000;
            config->buffer_size = 1024;
            config->buffer_size_tx = 1024;
            config->keep_alive_enable = true;
            config->crt_bundle_attach = esp_crt_bundle_attach;
            config->event_handler = handler;
            config->user_data = data;
            (*slot)->client = esp_http_client_init(config);
            free(config);
            if (!(*slot)->client) { live_http_close(slot); return ESP_ERR_NO_MEM; }
        }
        esp_http_client_handle_t client = (*slot)->client;
        esp_err_t err = esp_http_client_set_url(client, url);
        if (err == ESP_OK) err = esp_http_client_set_user_data(client, data);
        if (err == ESP_OK) err = esp_http_client_set_event_handler(client, handler);
        if (err == ESP_OK) err = esp_http_client_set_header(client, "User-Agent", "PisenAIClip/BLE-live-1");
        if (err == ESP_OK) err = esp_http_client_set_header(client, "Accept-Encoding", "identity");
        if (err == ESP_OK) {
            if (etag && *etag) err = esp_http_client_set_header(client, "If-None-Match", etag);
            else {
                err = esp_http_client_delete_header(client, "If-None-Match");
                if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
            }
        }
        if (err == ESP_OK) err = esp_http_client_perform(client);
        *status = esp_http_client_get_status_code(client);
        if (err == ESP_OK) return ESP_OK; /* HTTP error codes are not transport retries. */
        live_http_close(slot);
        if (!reused || attempt || !reset_response) return err;
        reset_response(data);
        reused = false;
    }
    return ESP_FAIL;
}
