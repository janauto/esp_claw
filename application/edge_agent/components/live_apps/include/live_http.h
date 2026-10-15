#pragma once
#include "esp_http_client.h"
typedef struct live_http_t *live_http_handle_t;
/* One verified-TLS connection per owner; reuse only within the same origin.
 * reset_response discards a partial response before a single GET retry. */
esp_err_t live_http_get(live_http_handle_t *session, const char *url, const char *etag,
                        http_event_handle_cb handler, void *data,
                        void (*reset_response)(void *data), int *status);
void live_http_close(live_http_handle_t *session);
