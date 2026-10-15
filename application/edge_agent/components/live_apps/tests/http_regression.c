#include "live_http.h"
#include "esp_crt_bundle.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct mock_client { char url[256]; void *data; http_event_handle_cb callback; bool etag; };
static unsigned created, closed, performed, resets;
static int fail_next, response_status = 200;
static void *expected_data;
esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(config->crt_bundle_attach == esp_crt_bundle_attach && config->keep_alive_enable);
    struct mock_client *client = calloc(1, sizeof(*client)); assert(client);
    snprintf(client->url, sizeof(client->url), "%s", config->url);
    client->data = config->user_data; client->callback = config->event_handler; created++;
    return client;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
    assert(client->data == NULL); free(client); closed++; return ESP_OK;
}
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url) {
    snprintf(c->url, sizeof(c->url), "%s", url); return ESP_OK;
}
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *data) { c->data=data; return ESP_OK; }
esp_err_t esp_http_client_set_event_handler(esp_http_client_handle_t c, http_event_handle_cb cb) {
    c->callback=cb; return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value) {
    (void)value; if (!strcmp(key,"If-None-Match")) c->etag=true; return ESP_OK;
}
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t c, const char *key) {
    assert(!strcmp(key,"If-None-Match")); if (!c->etag) return ESP_ERR_NOT_FOUND;
    c->etag=false; return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    performed++; assert(c->data==expected_data);
    if (fail_next) { fail_next--; return ESP_FAIL; }
    esp_http_client_event_t event = {.user_data=c->data}; return c->callback(&event);
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return response_status; }
static esp_err_t event(esp_http_client_event_t *e) { assert(e->user_data==expected_data); return ESP_OK; }
static void reset(void *data) { assert(data==expected_data); resets++; }

int main(void) {
    live_http_handle_t session=NULL; int a=1,b=2,status=0; expected_data=&a;
    assert(live_http_get(&session,"https://a.test/one","tag",event,&a,reset,&status)==ESP_OK);
    assert(created==1 && performed==1 && status==200);
    expected_data=&b;
    assert(live_http_get(&session,"https://a.test/two",NULL,event,&b,reset,&status)==ESP_OK);
    assert(created==1 && performed==2 && session != NULL);
    assert(live_http_get(&session,"https://b.test/three",NULL,event,&b,reset,&status)==ESP_OK);
    assert(created==2 && closed==1);
    fail_next=1;
    assert(live_http_get(&session,"https://b.test/four",NULL,event,&b,reset,&status)==ESP_OK);
    assert(created==3 && closed==2 && resets==1);
    response_status=503; unsigned old_performed=performed;
    assert(live_http_get(&session,"https://b.test/five",NULL,event,&b,reset,&status)==ESP_OK);
    assert(status==503 && performed==old_performed+1); /* no retry of an HTTP response */
    live_http_close(&session); assert(!session && created==closed);
    fail_next=1;
    assert(live_http_get(&session,"https://c.test/one",NULL,event,&b,reset,&status)==ESP_FAIL);
    assert(!session && resets==1 && created==closed); /* only stale reused transports retry */
    assert(live_http_get(&session,"http://plain.test/",NULL,event,&b,reset,&status)==ESP_ERR_INVALID_ARG);
    live_http_close(&session);
    puts("PASS: verified TLS reuse, origin isolation, callback renewal, bounded retry and HTTP failure handling");
}
