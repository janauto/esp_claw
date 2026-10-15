#include "clip_live.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

static const char *const kinds[] = {"weather", "stocks", "aihot"};
static const uint64_t intervals[] = {600000, 60000, 300000};
typedef struct {
    uint64_t next, last_attempt, last_success, not_before;
    unsigned failures;
    int http;
    esp_err_t error;
    char etag[160];
} fetch_state_t;
struct clip_live_t {
    clip_live_config_t callbacks;
    clip_live_settings_t settings;
    SemaphoreHandle_t lock;
    nvs_handle_t nvs;
    char ssid[33], password[64], ip[16];
    esp_netif_t *netif;
    esp_event_handler_instance_t wifi_handler, ip_handler;
    atomic_bool connected, reconfigure;
    atomic_uint apps, generation;
    bool initialized, started;
    esp_err_t wifi_error;
    uint64_t connect_attempt;
    fetch_state_t states[3];
    TaskHandle_t task;
    cJSON *action_request, *action_result, *locations, *queried_stock;
    char default_location[24];
    atomic_bool action_pending;
    unsigned action_seq, next_location;
};
typedef struct {
    char *body;
    size_t used, capacity;
    bool prefix_only;
    char etag[160];
    uint64_t retry_ms;
} capture_t;
static int kind_index(const char *id) {
    for (int i = 0; i < 3; i++) {
        if (id && !strcmp(id, kinds[i])) {
            return i;
        }
    }
    return -1;
}
static uint64_t mono(void) {
    return (uint64_t)(esp_timer_get_time() / 1000);
}
static uint64_t wall(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_usec / 1000;
}
static const char *string(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *data) {
    clip_live_handle_t h = ctx;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        xSemaphoreTake(h->lock, portMAX_DELAY);
        snprintf(h->ip, sizeof(h->ip), IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGive(h->lock);
        atomic_store(&h->connected, true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&h->connected, false);
    }
}
static esp_err_t wifi_apply(clip_live_handle_t h) {
    if (h->callbacks.wifi_status) return ESP_OK;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool wanted = strcmp(h->settings.mode, "phone") && h->ssid[0];
    wifi_config_t *station = calloc(1, sizeof(*station));
    if (station) {
        memcpy(station->sta.ssid, h->ssid, strlen(h->ssid));
        memcpy(station->sta.password, h->password, strlen(h->password));
        station->sta.threshold.authmode = h->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    }
    xSemaphoreGive(h->lock);
    if (!station) {
        return ESP_ERR_NO_MEM;
    }
    if (!wanted) {
        if (h->initialized) {
            esp_wifi_stop();
            esp_wifi_deinit();
            h->initialized = false;
            atomic_store(&h->connected, false);
        }
        free(station);
        return ESP_OK;
    }
    esp_err_t err = ESP_OK;
    if (!h->initialized) {
        if (!h->netif) {
            h->netif = esp_netif_create_default_wifi_sta();
        }
        if (!h->netif) {
            free(station);
            return ESP_ERR_NO_MEM;
        }
        wifi_init_config_t *config = malloc(sizeof(*config));
        if (!config) {
            free(station);
            return ESP_ERR_NO_MEM;
        }
        *config = (wifi_init_config_t)WIFI_INIT_CONFIG_DEFAULT();
        config->static_rx_buf_num = 3;
        config->dynamic_rx_buf_num = 6;
        config->static_tx_buf_num = 4;
        config->cache_tx_buf_num = 4;
        err = esp_wifi_init(config);
        free(config);
        if (err != ESP_OK) {
            free(station);
            return err;
        }
        h->initialized = true;
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err == ESP_OK) {
            err = esp_wifi_set_mode(WIFI_MODE_STA);
        }
    } else {
        esp_wifi_disconnect();
        esp_wifi_stop();
        atomic_store(&h->connected, false);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, station);
    }
    free(station);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        h->connect_attempt = mono();
        err = esp_wifi_connect();
    }
    if (err != ESP_OK && h->initialized) {
        esp_wifi_stop();
        esp_wifi_deinit();
        h->initialized = false;
        atomic_store(&h->connected, false);
    }
    return err;
}
static esp_err_t http_event(esp_http_client_event_t *event) {
    capture_t *c = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        if (!strcasecmp(event->header_key, "ETag")) {
            snprintf(c->etag, sizeof(c->etag), "%s", event->header_value);
        }
        if (!strcasecmp(event->header_key, "Retry-After")) {
            unsigned long seconds = strtoul(event->header_value, NULL, 10);
            c->retry_ms = (seconds > 86400 ? 86400 : seconds) * 1000ULL;
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        size_t incoming = (size_t)event->data_len;
        if (c->prefix_only) {
            if (c->used >= 65536) {
                return ESP_OK;
            }
            if (incoming > 65536 - c->used) {
                incoming = 65536 - c->used;
            }
        }
        size_t size = c->used + incoming + 1;
        if (size > 131072) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (size > c->capacity) {
            size_t capacity = size < 4096 ? 4096 : size;
            char *body = realloc(c->body, capacity);
            if (!body) {
                return ESP_ERR_NO_MEM;
            }
            c->body = body;
            c->capacity = capacity;
        }
        memcpy(c->body + c->used, event->data, incoming);
        c->used += incoming;
        c->body[c->used] = 0;
    }
    return ESP_OK;
}
static esp_err_t get(const char *url, const char *etag, capture_t *capture, int *status) {
    esp_http_client_config_t *config = calloc(1, sizeof(*config));
    if (!config) {
        return ESP_ERR_NO_MEM;
    }
    config->url = url;
    config->timeout_ms = 9000;
    config->buffer_size = 1024;
    config->buffer_size_tx = 1024;
    config->crt_bundle_attach = esp_crt_bundle_attach;
    config->event_handler = http_event;
    config->user_data = capture;
    esp_http_client_handle_t client = esp_http_client_init(config);
    free(config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "User-Agent", "PisenAIClip/0.4.0 (internal-test)");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (etag && *etag) {
        esp_http_client_set_header(client, "If-None-Match", etag);
    }
    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}
static cJSON *fetch_one(clip_live_handle_t h, int index, const clip_live_settings_t *settings,
                        capture_t *capture, int *status, esp_err_t *error) {
    char *url = malloc(640);
    if (!url) {
        *error = ESP_ERR_NO_MEM;
        return NULL;
    }
    cJSON *payload = NULL;
    if (index == 1) {
        cJSON *cards = cJSON_CreateArray();
        char *symbols = strdup(settings->symbols);
        char *save = NULL;
        if (!cards || !symbols) {
            cJSON_Delete(cards);
            free(symbols);
            free(url);
            *error = ESP_ERR_NO_MEM;
            return NULL;
        }
        for (char *symbol = strtok_r(symbols, ",", &save); symbol; symbol = strtok_r(NULL, ",", &save)) {
            if (atomic_load(&h->action_pending)) {
                cJSON_Delete(cards);
                cards = NULL;
                *error = ESP_ERR_NOT_FINISHED;
                break;
            }
            snprintf(url, 640, "https://web.ifzq.gtimg.cn/appstock/app/%s/query?code=%s",
                     !strncmp(symbol, "us", 2)   ? "usMinute"
                     : !strncmp(symbol, "hk", 2) ? "hkMinute"
                                                 : "minute",
                     symbol);
            *error = get(url, NULL, capture, status);
            cJSON *r =
                *error == ESP_OK && *status == 200 && capture->body ? cJSON_Parse(capture->body) : NULL;
            cJSON *c = r ? clip_live_parse_stock_card(r, symbol) : NULL;
            cJSON_Delete(r);
            free(capture->body);
            capture->body = NULL;
            capture->used = capture->capacity = 0;
            if (!c) {
                c = cJSON_CreateObject();
                if (!c) {
                    cJSON_Delete(cards);
                    cards = NULL;
                    break;
                }
                cJSON_AddStringToObject(c, "title", symbol);
                cJSON_AddStringToObject(c, "value", "暂无有效报价");
                cJSON_AddStringToObject(c, "detail", "请检查代码或稍后刷新");
                cJSON_AddStringToObject(c, "symbol", symbol);
                cJSON_AddStringToObject(c, "name", symbol);
                cJSON_AddItemToArray(cards, c);
                continue;
            }
            char *old_text = NULL;
            int64_t age;
            cJSON *old = NULL;
            if (h->callbacks.read("stocks", &old_text, &age, h->callbacks.ctx) == ESP_OK) {
                old = cJSON_Parse(old_text);
            }
            free(old_text);
            const cJSON *old_card = NULL, *candidate;
            cJSON_ArrayForEach(candidate, cJSON_GetObjectItemCaseSensitive(old, "cards")) {
                const char *sym = string(candidate, "symbol");
                if (sym && !strcmp(sym, symbol)) {
                    old_card = candidate;
                    break;
                }
            }
            const char *time = string(c, "quoteTime"), *history_date = string(old_card, "historyDate");
            if (time && history_date && !strncmp(history_date, time, 8) &&
                cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(old_card, "ranges")) > 0) {
                const cJSON *ranges = cJSON_GetObjectItemCaseSensitive(old_card, "ranges");
                if (ranges) {
                    cJSON_AddItemToObject(c, "ranges", cJSON_Duplicate(ranges, true));
                }
            } else {
                const char *history_symbol = string(c, "historySymbol");
                if (!history_symbol) {
                    history_symbol = symbol;
                }
                snprintf(url, 640, "https://web.ifzq.gtimg.cn/appstock/app/%s/get?param=%s,day,,,250,qfq",
                         !strncmp(symbol, "us", 2) ? "usfqkline" : "fqkline", history_symbol);
                int history_status = 0;
                esp_err_t history_error = get(url, NULL, capture, &history_status);
                cJSON *history = history_error == ESP_OK && history_status == 200 && capture->body
                                     ? cJSON_Parse(capture->body)
                                     : NULL;
                if (history) {
                    clip_live_stock_history(c, history, history_symbol);
                }
                cJSON_Delete(history);
                free(capture->body);
                capture->body = NULL;
                capture->used = capture->capacity = 0;
            }
            if (time) {
                char date[9];
                memcpy(date, time, 8);
                date[8] = 0;
                cJSON_AddStringToObject(c, "historyDate", date);
            }
            cJSON_Delete(old);
            cJSON_AddItemToArray(cards, c);
        }
        free(symbols);
        if (cards) {
            payload = clip_live_payload("stocks", cards, wall());
        }
    } else {
        if (index == 0) {
            snprintf(url, 640,
                     "https://api.open-meteo.com/v1/"
                     "forecast?latitude=%.6f&longitude=%.6f&current=temperature_2m,relative_humidity_2m,"
                     "apparent_temperature,weather_code,wind_speed_10m,is_day&hourly=temperature_2m,weather_"
                     "code,precipitation_probability&forecast_hours=24&daily=temperature_2m_max,temperature_"
                     "2m_min,precipitation_probability_max,weather_code,sunrise,sunset&forecast_days=10&"
                     "timezone=Asia%%2FShanghai",
                     settings->latitude, settings->longitude);
        } else {
            snprintf(url, 640, "https://aihot.news/api/v1/hot-topics");
        }
        *error = get(url, index == 2 ? h->states[index].etag : NULL, capture, status);
        if (*error == ESP_OK && *status == 304) {
            char *text = NULL;
            int64_t age;
            if (h->callbacks.read(kinds[index], &text, &age, h->callbacks.ctx) == ESP_OK) {
                payload = cJSON_Parse(text);
                free(text);
                if (payload) {
                    cJSON_ReplaceItemInObjectCaseSensitive(payload, "fetchedAt",
                                                           cJSON_CreateNumber((double)wall()));
                    cJSON_ReplaceItemInObjectCaseSensitive(payload, "via", cJSON_CreateString("wifi"));
                }
            }
        } else if (*error == ESP_OK && *status == 200 && capture->body) {
            cJSON *r = cJSON_Parse(capture->body);
            payload =
                index == 0 ? clip_live_parse_weather(r, settings, wall()) : clip_live_parse_hot(r, wall());
            cJSON_Delete(r);
        }
    }
    if (payload && index == 0) {
        free(capture->body);
        capture->body = NULL;
        capture->used = capture->capacity = 0;
        snprintf(url, 640,
                 "https://air-quality-api.open-meteo.com/v1/"
                 "air-quality?latitude=%.6f&longitude=%.6f&current=us_aqi&timezone=Asia%%2FShanghai",
                 settings->latitude, settings->longitude);
        int air_status = 0;
        esp_err_t air_error = get(url, NULL, capture, &air_status);
        cJSON *air =
            air_error == ESP_OK && air_status == 200 && capture->body ? cJSON_Parse(capture->body) : NULL;
        const cJSON *current = cJSON_GetObjectItemCaseSensitive(air, "current");
        if (current) {
            cJSON_AddItemToObject(payload, "airQuality", cJSON_Duplicate(current, true));
        }
        cJSON_Delete(air);
    }
    if (payload && index == 2 && (*status == 200 || *status == 304)) {
        char *old_text = NULL;
        int64_t age;
        cJSON *old = NULL;
        if (h->callbacks.read("aihot", &old_text, &age, h->callbacks.ctx) == ESP_OK) {
            old = cJSON_Parse(old_text);
        }
        free(old_text);
        cJSON *card;
        cJSON_ArrayForEach(card, cJSON_GetObjectItemCaseSensitive(payload, "cards")) {
            const char *story = string(card, "storyUrl");
            if (!story) {
                continue;
            }
            const char *path = NULL;
            if (!strncmp(story, "https://aihot.virxact.com/story/", 32)) {
                path = story + 25;
            } else if (!strncmp(story, "https://aihot.news/story/", 25)) {
                path = story + 18;
            }
            if (!path || strncmp(path, "/story/", 7)) {
                continue;
            }
            snprintf(url, 640, "https://aihot.news%s", path);
            free(capture->body);
            capture->body = NULL;
            capture->used = capture->capacity = 0;
            capture->prefix_only = true;
            int detail_status = 0;
            esp_err_t detail_error = get(url, NULL, capture, &detail_status);
            if (detail_error == ESP_OK && detail_status == 200) {
                clip_live_enrich_hot_card(card, capture->body);
            }
            if (!cJSON_GetObjectItemCaseSensitive(card, "body")) {
                const cJSON *candidate;
                cJSON_ArrayForEach(candidate, cJSON_GetObjectItemCaseSensitive(old, "cards")) {
                    const char *old_story = string(candidate, "storyUrl"), *body = string(candidate, "body");
                    if (old_story && body && !strcmp(old_story, story)) {
                        cJSON_AddStringToObject(card, "body", body);
                        break;
                    }
                }
            }
        }
        cJSON_Delete(old);
        capture->prefix_only = false;
    }
    free(url);
    return payload;
}

/* UI actions and saved weather locations. */
#define WEATHER_LOCATIONS_MAX 8
static cJSON *locations_metadata(const cJSON *locations) {
    cJSON *copy = cJSON_Duplicate(locations, true), *item;
    cJSON_ArrayForEach(item, copy) cJSON_DeleteItemFromObjectCaseSensitive(item, "forecast");
    return copy;
}
static esp_err_t locations_save(clip_live_handle_t h) {
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(j, "defaultId", h->default_location);
    cJSON_AddNumberToObject(j, "nextId", h->next_location);
    cJSON_AddItemToObject(j, "locations", locations_metadata(h->locations));
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nvs_set_str(h->nvs, "locations", text);
    free(text);
    if (err == ESP_OK) {
        err = nvs_commit(h->nvs);
    }
    return err;
}
static void locations_load(clip_live_handle_t h) {
    size_t size = 0;
    if (nvs_get_str(h->nvs, "locations", NULL, &size) == ESP_OK && size < 8192) {
        char *text = malloc(size);
        if (text && nvs_get_str(h->nvs, "locations", text, &size) == ESP_OK) {
            cJSON *j = cJSON_Parse(text), *a = cJSON_GetObjectItemCaseSensitive(j, "locations");
            const char *id = string(j, "defaultId");
            if (cJSON_IsArray(a) && cJSON_GetArraySize(a) > 0 &&
                cJSON_GetArraySize(a) <= WEATHER_LOCATIONS_MAX && id) {
                h->locations = cJSON_Duplicate(a, true);
                snprintf(h->default_location, sizeof(h->default_location), "%s", id);
                const cJSON *next = cJSON_GetObjectItemCaseSensitive(j, "nextId");
                h->next_location = cJSON_IsNumber(next) ? (unsigned)next->valueint : WEATHER_LOCATIONS_MAX;
            }
            cJSON_Delete(j);
        }
        free(text);
    }
    if (!h->locations) {
        h->locations = cJSON_CreateArray();
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "id", "loc0");
        cJSON_AddStringToObject(c, "name", h->settings.city);
        cJSON_AddNumberToObject(c, "latitude", h->settings.latitude);
        cJSON_AddNumberToObject(c, "longitude", h->settings.longitude);
        cJSON_AddItemToArray(h->locations, c);
        snprintf(h->default_location, sizeof(h->default_location), "loc0");
        h->next_location = 1;
    }
}
static cJSON *location_find(cJSON *locations, const char *id) {
    cJSON *c;
    cJSON_ArrayForEach(c, locations) {
        const char *v = string(c, "id");
        if (v && id && !strcmp(v, id)) {
            return c;
        }
    }
    return NULL;
}
static cJSON *locations_payload(clip_live_handle_t h, cJSON *locations) {
    xSemaphoreTake(h->lock, portMAX_DELAY);
    cJSON *primary = location_find(locations, h->default_location);
    char id[24];
    snprintf(id, sizeof(id), "%s", h->default_location);
    xSemaphoreGive(h->lock);
    cJSON *forecast = cJSON_GetObjectItemCaseSensitive(primary, "forecast");
    if (!forecast) {
        cJSON *c;
        cJSON_ArrayForEach(c, locations) {
            forecast = cJSON_GetObjectItemCaseSensitive(c, "forecast");
            if (forecast) {
                break;
            }
        }
    }
    cJSON *payload = forecast ? cJSON_Duplicate(forecast, true) : NULL;
    if (!payload) {
        return NULL;
    }
    cJSON_AddStringToObject(payload, "defaultId", id);
    cJSON_AddItemToObject(payload, "locations", cJSON_Duplicate(locations, true));
    return payload;
}
static cJSON *fetch_locations(clip_live_handle_t h, const clip_live_settings_t *settings, capture_t *capture,
                              int *status, esp_err_t *error) {
    xSemaphoreTake(h->lock, portMAX_DELAY);
    unsigned generation = atomic_load(&h->generation);
    cJSON *locations = cJSON_Duplicate(h->locations, true);
    xSemaphoreGive(h->lock);
    if (!locations) {
        *error = ESP_ERR_NO_MEM;
        return NULL;
    }
    cJSON *c;
    int fetched = 0;
    cJSON_ArrayForEach(c, locations) {
        if (atomic_load(&h->action_pending)) {
            *error = ESP_ERR_NOT_FINISHED;
            cJSON_Delete(locations);
            return NULL;
        }
        clip_live_settings_t *s = malloc(sizeof(*s));
        if (!s) {
            *error = ESP_ERR_NO_MEM;
            break;
        }
        *s = *settings;
        snprintf(s->city, sizeof(s->city), "%s", string(c, "name") ? string(c, "name") : settings->city);
        const cJSON *lat = cJSON_GetObjectItemCaseSensitive(c, "latitude"),
                    *lon = cJSON_GetObjectItemCaseSensitive(c, "longitude");
        s->latitude = cJSON_IsNumber(lat) ? lat->valuedouble : settings->latitude;
        s->longitude = cJSON_IsNumber(lon) ? lon->valuedouble : settings->longitude;
        cJSON *forecast = fetch_one(h, 0, s, capture, status, error);
        free(s);
        free(capture->body);
        capture->body = NULL;
        capture->used = capture->capacity = 0;
        if (forecast) {
            cJSON_DeleteItemFromObjectCaseSensitive(c, "forecast");
            cJSON_AddItemToObject(c, "forecast", forecast);
            fetched++;
        }
    }
    cJSON *payload = locations_payload(h, locations);
    if (fetched) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        cJSON *copy = generation == atomic_load(&h->generation) ? cJSON_Duplicate(locations, true) : NULL;
        if (copy) {
            cJSON_Delete(h->locations);
            h->locations = copy;
            *error = ESP_OK;
        } else {
            cJSON_Delete(payload);
            payload = NULL;
            *error = ESP_ERR_NOT_FINISHED;
        }
        xSemaphoreGive(h->lock);
    }
    cJSON_Delete(locations);
    return payload;
}
static void capture_reset(capture_t *capture) {
    free(capture->body);
    capture->body = NULL;
    capture->used = capture->capacity = 0;
    capture->prefix_only = false;
}
static cJSON *query_stock(clip_live_handle_t h, const char *query, capture_t *capture) {
    char symbol[16] = {0};
    if (!clip_live_stock_symbol(query, symbol, sizeof(symbol))) {
        return NULL;
    }
    char *url = malloc(256);
    if (!url) {
        return NULL;
    }
    (void)h;
    char primary[3] = {symbol[0], symbol[1], 0};
    cJSON *card = NULL;
    const char *markets[] = {NULL, "sh", "sz", "bj"};
    for (int i = 0; i < 4 && !card; i++) {
        if (i && !strcmp(primary, markets[i])) {
            continue;
        }
        if (i) {
            snprintf(symbol, sizeof(symbol), "%s%s", markets[i], query);
        }
        capture_reset(capture);
        snprintf(url, 256, "https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=%s", symbol);
        int status = 0;
        esp_err_t err = get(url, NULL, capture, &status);
        cJSON *r = err == ESP_OK && status == 200 && capture->body ? cJSON_Parse(capture->body) : NULL;
        if (r) {
            card = clip_live_parse_stock_card(r, symbol);
        }
        cJSON_Delete(r);
    }
    if (card) {
        cJSON_AddNumberToObject(card, "queriedAt", (double)wall());
    }
    free(url);
    return card;
}
static esp_err_t stock_publish_query(clip_live_handle_t h, const char *symbol) {
    char *text = NULL;
    int64_t age = 0;
    esp_err_t err = h->callbacks.read("stocks", &text, &age, h->callbacks.ctx);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *payload = cJSON_Parse(text);
    free(text);
    if (!payload) {
        return ESP_FAIL;
    }
    const char *found = string(h->queried_stock, "symbol");
    cJSON *cards = cJSON_GetObjectItemCaseSensitive(payload, "cards");
    if (found && symbol && !strcmp(found, symbol)) {
        bool exists = false;
        for (int i = 0; i < cJSON_GetArraySize(cards); i++) {
            const char *s = string(cJSON_GetArrayItem(cards, i), "symbol");
            if (s && !strcmp(s, symbol)) {
                cJSON_ReplaceItemInArray(cards, i, cJSON_Duplicate(h->queried_stock, true));
                exists = true;
                break;
            }
        }
        if (!exists) {
            cJSON_AddItemToArray(cards, cJSON_Duplicate(h->queried_stock, true));
        }
        cJSON_DeleteItemFromObjectCaseSensitive(payload, "ageAtSendMs");
        cJSON_AddNumberToObject(payload, "ageAtSendMs", age > 0 ? (double)age : 0);
        err = h->callbacks.publish("stocks", payload, h->callbacks.ctx);
    }
    cJSON_Delete(payload);
    return err;
}
static char *url_encode(const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(text);
    char *out = malloc(n * 3 + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }
    out[j] = 0;
    return out;
}
extern const char china_regions_start[] asm("_binary_china_regions_tsv_start");
extern const char china_regions_end[] asm("_binary_china_regions_tsv_end");
static cJSON *city_search_catalog(const char *query) {
    cJSON *results = cJSON_CreateArray();
    char *line = malloc(512);
    if (!results || !line) {
        cJSON_Delete(results);
        free(line);
        return NULL;
    }
    const char *p = china_regions_start;
    while (p < china_regions_end && cJSON_GetArraySize(results) < 8) {
        const char *end = memchr(p, '\n', (size_t)(china_regions_end - p));
        if (!end) {
            break;
        }
        size_t n = (size_t)(end - p);
        if (n < 511) {
            memcpy(line, p, n);
            line[n] = 0;
            char *parts[6] = {0}, *save = NULL;
            int count = 0;
            for (char *v = strtok_r(line, "\t", &save); v && count < 6; v = strtok_r(NULL, "\t", &save)) {
                parts[count++] = v;
            }
            if (count == 6 &&
                (strcasestr(parts[0], query) || strcasestr(parts[1], query) || strcasestr(parts[3], query))) {
                cJSON *c = cJSON_CreateObject();
                cJSON_AddNumberToObject(c, "adcode", atoi(parts[0]));
                cJSON_AddStringToObject(c, "name", parts[1]);
                cJSON_AddStringToObject(c, "admin1", parts[2]);
                cJSON_AddNumberToObject(c, "latitude", strtod(parts[4], NULL));
                cJSON_AddNumberToObject(c, "longitude", strtod(parts[5], NULL));
                cJSON_AddStringToObject(c, "geoSource", "DataV");
                cJSON_AddItemToArray(results, c);
            }
        }
        p = end + 1;
    }
    free(line);
    return results;
}
static cJSON *area_properties(const char *html) {
    const char *start = html ? strstr(html, "\"properties\"") : NULL;
    if (!start) {
        return NULL;
    }
    start = strchr(start, '{');
    if (!start) {
        return NULL;
    }
    bool quoted = false, escape = false;
    int depth = 0;
    const char *p = start;
    for (; *p; p++) {
        if (quoted) {
            if (escape) {
                escape = false;
            } else if (*p == '\\') {
                escape = true;
            } else if (*p == '\"') {
                quoted = false;
            }
        } else if (*p == '\"') {
            quoted = true;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}' && !--depth) {
            return cJSON_ParseWithLength(start, (size_t)(p - start + 1));
        }
    }
    return NULL;
}
static cJSON *city_search_remote(const char *query, capture_t *capture) {
    cJSON *local = city_search_catalog(query);
    if (local && cJSON_GetArraySize(local) > 0) {
        return local;
    }
    cJSON_Delete(local);
    char *encoded = url_encode(query), *url = malloc(1024);
    if (!encoded || !url) {
        free(encoded);
        free(url);
        return NULL;
    }
    snprintf(url, 1024,
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=8&language=zh&format=json",
             encoded);
    free(encoded);
    int status = 0;
    esp_err_t err = get(url, NULL, capture, &status);
    free(url);
    cJSON *j = err == ESP_OK && status == 200 && capture->body ? cJSON_Parse(capture->body) : NULL;
    cJSON *a = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(j, "results"), true);
    cJSON_Delete(j);
    return a;
}
static void process_action(clip_live_handle_t h) {
    if (!atomic_load(&h->action_pending)) {
        return;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    cJSON *request = h->action_request;
    h->action_request = NULL;
    atomic_store(&h->action_pending, false);
    xSemaphoreGive(h->lock);
    if (!request) {
        return;
    }
    const char *op = string(request, "op");
    cJSON *result = cJSON_CreateObject(), *rid = cJSON_GetObjectItemCaseSensitive(request, "requestId");
    cJSON_AddStringToObject(result, "op", op ? op : "");
    cJSON_AddItemToObject(result, "requestId", cJSON_Duplicate(rid, true));
    cJSON_AddStringToObject(result, "state", "complete");
    capture_t *capture = calloc(1, sizeof(*capture));
    esp_err_t err = capture ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK && !strcmp(op, "stock_search")) {
        const char *query = string(request, "query");
        cJSON *stock = query ? query_stock(h, query, capture) : NULL;
        if (stock) {
            cJSON_Delete(h->queried_stock);
            h->queried_stock = cJSON_Duplicate(stock, true);
            cJSON_AddItemToObject(result, "stock", stock);
        } else {
            err = ESP_ERR_NOT_FOUND;
        }
    } else if (err == ESP_OK && !strcmp(op, "stock_add")) {
        const char *symbol = string(request, "symbol");
        err = clip_live_add_stock(h, symbol);
        if (err == ESP_OK) {
            err = stock_publish_query(h, symbol);
        }
    } else if (err == ESP_OK && !strcmp(op, "city_search")) {
        const char *query = string(request, "query");
        cJSON *cities = query ? city_search_remote(query, capture) : NULL;
        if (cities && cJSON_GetArraySize(cities) > 0) {
            cJSON_AddItemToObject(result, "cities", cities);
        } else {
            cJSON_Delete(cities);
            err = ESP_ERR_NOT_FOUND;
        }
    } else if (err == ESP_OK && !strcmp(op, "weather_add")) {
        cJSON *city = cJSON_GetObjectItemCaseSensitive(request, "city");
        const cJSON *adcode = cJSON_GetObjectItemCaseSensitive(city, "adcode");
        if (cJSON_IsNumber(adcode) && adcode->valueint >= 100000 && adcode->valueint <= 999999) {
            char url[100];
            snprintf(url, sizeof(url), "https://geo.datav.aliyun.com/areas_v3/bound/%d.json",
                     adcode->valueint);
            capture_reset(capture);
            capture->prefix_only = true;
            int status = 0;
            esp_err_t geo = get(url, NULL, capture, &status);
            cJSON *props = geo == ESP_OK && status == 200 ? area_properties(capture->body) : NULL;
            const cJSON *center = cJSON_GetObjectItemCaseSensitive(props, "center"),
                        *x = cJSON_GetArrayItem(center, 0), *y = cJSON_GetArrayItem(center, 1);
            if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) {
                cJSON_ReplaceItemInObjectCaseSensitive(city, "latitude", cJSON_CreateNumber(y->valuedouble));
                cJSON_ReplaceItemInObjectCaseSensitive(city, "longitude", cJSON_CreateNumber(x->valuedouble));
            } else {
                err = ESP_FAIL;
            }
            cJSON_Delete(props);
        }
        const cJSON *lat = cJSON_GetObjectItemCaseSensitive(city, "latitude"),
                    *lon = cJSON_GetObjectItemCaseSensitive(city, "longitude");
        const char *name = string(city, "name");
        if (err != ESP_OK) {
        } else if (!name || strlen(name) > 90 || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) ||
                   !isfinite(lat->valuedouble) || !isfinite(lon->valuedouble) ||
                   fabs(lat->valuedouble) > 90 || fabs(lon->valuedouble) > 180) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            xSemaphoreTake(h->lock, portMAX_DELAY);
            cJSON *locations = cJSON_Duplicate(h->locations, true);
            xSemaphoreGive(h->lock);
            cJSON *existing = NULL, *item;
            cJSON_ArrayForEach(item, locations) {
                const cJSON *a = cJSON_GetObjectItemCaseSensitive(item, "latitude"),
                            *b = cJSON_GetObjectItemCaseSensitive(item, "longitude");
                if (cJSON_IsNumber(a) && cJSON_IsNumber(b) &&
                    fabs(a->valuedouble - lat->valuedouble) < .005 &&
                    fabs(b->valuedouble - lon->valuedouble) < .005) {
                    existing = item;
                    break;
                }
            }
            if (!existing && cJSON_GetArraySize(locations) >= WEATHER_LOCATIONS_MAX) {
                err = ESP_ERR_INVALID_SIZE;
            } else {
                cJSON *loc = existing;
                if (!loc) {
                    loc = cJSON_CreateObject();
                    char id[24];
                    snprintf(id, sizeof(id), "loc%u", h->next_location++);
                    cJSON_AddStringToObject(loc, "id", id);
                    cJSON_AddStringToObject(loc, "name", name);
                    cJSON_AddNumberToObject(loc, "latitude", lat->valuedouble);
                    cJSON_AddNumberToObject(loc, "longitude", lon->valuedouble);
                    cJSON_AddItemToArray(locations, loc);
                }
                clip_live_settings_t *s = malloc(sizeof(*s));
                if (!s) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    xSemaphoreTake(h->lock, portMAX_DELAY);
                    *s = h->settings;
                    xSemaphoreGive(h->lock);
                    snprintf(s->city, sizeof(s->city), "%s", name);
                    s->latitude = lat->valuedouble;
                    s->longitude = lon->valuedouble;
                    int status = 0;
                    capture_reset(capture);
                    cJSON *forecast = fetch_one(h, 0, s, capture, &status, &err);
                    free(s);
                    if (forecast) {
                        cJSON_DeleteItemFromObjectCaseSensitive(loc, "forecast");
                        cJSON_AddItemToObject(loc, "forecast", forecast);
                        cJSON_AddStringToObject(result, "locationId", string(loc, "id"));
                        xSemaphoreTake(h->lock, portMAX_DELAY);
                        cJSON_Delete(h->locations);
                        h->locations = cJSON_Duplicate(locations, true);
                        err = locations_save(h);
                        xSemaphoreGive(h->lock);
                        cJSON *payload = locations_payload(h, locations);
                        if (payload) {
                            if (err == ESP_OK) {
                                err = h->callbacks.publish("weather", payload, h->callbacks.ctx);
                            }
                            cJSON_Delete(payload);
                        } else {
                            err = ESP_FAIL;
                        }
                    } else if (err == ESP_OK) {
                        err = ESP_FAIL;
                    }
                }
            }
            cJSON_Delete(locations);
        }
    } else if (err == ESP_OK && !strcmp(op, "weather_pin")) {
        const char *id = string(request, "id");
        xSemaphoreTake(h->lock, portMAX_DELAY);
        cJSON *loc = location_find(h->locations, id);
        if (loc) {
            snprintf(h->default_location, sizeof(h->default_location), "%s", id);
            snprintf(h->settings.city, sizeof(h->settings.city), "%s", string(loc, "name"));
            h->settings.latitude = cJSON_GetObjectItemCaseSensitive(loc, "latitude")->valuedouble;
            h->settings.longitude = cJSON_GetObjectItemCaseSensitive(loc, "longitude")->valuedouble;
            err = locations_save(h);
        } else {
            err = ESP_ERR_NOT_FOUND;
        }
        cJSON *locations = cJSON_Duplicate(h->locations, true);
        xSemaphoreGive(h->lock);
        cJSON *payload = locations_payload(h, locations);
        if (payload) {
            if (err == ESP_OK) {
                err = h->callbacks.publish("weather", payload, h->callbacks.ctx);
            }
            cJSON_Delete(payload);
        }
        cJSON_Delete(locations);
    }
    if (capture) {
        capture_reset(capture);
        free(capture);
    }
    cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(result, "error",
                                err == ESP_ERR_NOT_FOUND      ? "未找到结果，请检查输入或稍后重试"
                                : err == ESP_ERR_INVALID_SIZE ? "最多添加8个地区"
                                                              : "联网请求失败，请检查背夹 Wi-Fi 后重试");
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    const cJSON *current = cJSON_GetObjectItemCaseSensitive(h->action_result, "requestId");
    if (cJSON_IsNumber(current) && cJSON_IsNumber(rid) && current->valueint == rid->valueint) {
        cJSON_Delete(h->action_result);
        h->action_result = result;
        result = NULL;
    }
    xSemaphoreGive(h->lock);
    cJSON_Delete(result);
    cJSON_Delete(request);
}
esp_err_t clip_live_action(clip_live_handle_t h, const cJSON *request, cJSON **response) {
    if (!h || !request || !response) {
        return ESP_ERR_INVALID_ARG;
    }
    *response = NULL;
    const char *op = string(request, "op");
    if (!op) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (!strcmp(op, "status")) {
        *response = h->action_result ? cJSON_Duplicate(h->action_result, true) : cJSON_CreateObject();
    } else if (strcmp(op, "stock_search") && strcmp(op, "stock_add") && strcmp(op, "city_search") &&
               strcmp(op, "weather_add") && strcmp(op, "weather_pin")) {
        xSemaphoreGive(h->lock);
        return ESP_ERR_INVALID_ARG;
    } else {
        cJSON *copy = cJSON_Duplicate(request, true);
        if (!copy) {
            xSemaphoreGive(h->lock);
            return ESP_ERR_NO_MEM;
        }
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(copy, "requestId");
        if (!cJSON_IsNumber(rid)) {
            cJSON_AddNumberToObject(copy, "requestId", ++h->action_seq);
        }
        cJSON_Delete(h->action_result);
        h->action_result = cJSON_CreateObject();
        cJSON_AddStringToObject(h->action_result, "op", op);
        cJSON_AddItemToObject(h->action_result, "requestId",
                              cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(copy, "requestId"), true));
        cJSON_AddStringToObject(h->action_result, "state", "pending");
        if (!atomic_load(&h->connected) && strcmp(op, "weather_pin")) {
            cJSON_AddBoolToObject(h->action_result, "ok", false);
            cJSON_ReplaceItemInObjectCaseSensitive(h->action_result, "state", cJSON_CreateString("complete"));
            cJSON_AddStringToObject(h->action_result, "error", "请先连接背夹 Wi-Fi");
            cJSON_Delete(copy);
        } else {
            cJSON_Delete(h->action_request);
            h->action_request = copy;
            atomic_store(&h->action_pending, true);
        }
        *response = cJSON_Duplicate(h->action_result, true);
    }
    if (*response) {
        cJSON_AddBoolToObject(*response, "wifi_connected", atomic_load(&h->connected));
    }
    xSemaphoreGive(h->lock);
    return *response ? ESP_OK : ESP_ERR_NO_MEM;
}

static void worker(void *ctx) {
    clip_live_handle_t h = ctx;
    clip_live_settings_t *settings = malloc(sizeof(*settings));
    if (!settings) {
        h->task = NULL;
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        if (h->callbacks.wifi_status) {
            char ip[16] = {0};
            bool connected = h->callbacks.wifi_status(ip, sizeof(ip), h->callbacks.ctx);
            atomic_store(&h->connected, connected);
            xSemaphoreTake(h->lock, portMAX_DELAY);
            snprintf(h->ip, sizeof(h->ip), "%s", ip);
            xSemaphoreGive(h->lock);
        }
        xSemaphoreTake(h->lock, portMAX_DELAY);
        bool retry_wifi = !h->callbacks.wifi_status && !h->initialized && h->ssid[0] && strcmp(h->settings.mode, "phone") &&
                          mono() - h->connect_attempt >= 60000;
        xSemaphoreGive(h->lock);
        if (atomic_exchange(&h->reconfigure, false) || retry_wifi) {
            h->connect_attempt = mono();
            esp_err_t err = wifi_apply(h);
            xSemaphoreTake(h->lock, portMAX_DELAY);
            h->wifi_error = err;
            xSemaphoreGive(h->lock);
        }
        if (!h->callbacks.wifi_status && h->initialized && !atomic_load(&h->connected) && mono() - h->connect_attempt > 10000) {
            h->connect_attempt = mono();
            esp_wifi_connect();
        }
        if (!h->callbacks.paused(h->callbacks.ctx)) {
            process_action(h);
        }
        if (!atomic_load(&h->connected) || wall() < 1700000000000ULL ||
            h->callbacks.paused(h->callbacks.ctx)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        for (int i = 0; i < 3; i++) {
            if (atomic_load(&h->action_pending)) {
                break;
            }
            xSemaphoreTake(h->lock, portMAX_DELAY);
            *settings = h->settings;
            bool due = strcmp(settings->mode, "phone") && (atomic_load(&h->apps) & (1U << i)) &&
                       mono() >= h->states[i].next;
            unsigned generation = atomic_load(&h->generation);
            if (due) {
                h->states[i].last_attempt = mono();
            }
            xSemaphoreGive(h->lock);
            if (!due) {
                continue;
            }
            capture_t *capture = calloc(1, sizeof(*capture));
            if (!capture) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int status = 0;
            esp_err_t err = ESP_OK;
            cJSON *payload = i == 0 ? fetch_locations(h, settings, capture, &status, &err)
                                    : fetch_one(h, i, settings, capture, &status, &err);
            if (payload && generation == atomic_load(&h->generation)) {
                err = h->callbacks.publish(kinds[i], payload, h->callbacks.ctx);
            } else if (err == ESP_OK) {
                err = ESP_FAIL;
            }
            xSemaphoreTake(h->lock, portMAX_DELAY);
            fetch_state_t *state = &h->states[i];
            state->http = status;
            state->error = err;
            if (err == ESP_ERR_NOT_FINISHED) {
                state->next = mono();
            } else if (err == ESP_OK) {
                state->failures = 0;
                state->last_success = mono();
                state->next = mono() + intervals[i];
                state->not_before = mono() + 60000;
                if (capture->etag[0]) {
                    snprintf(state->etag, sizeof(state->etag), "%s", capture->etag);
                }
            } else {
                state->failures = state->failures < 5 ? state->failures + 1 : 5;
                uint64_t wait = 60000ULL << (state->failures - 1);
                if (wait < intervals[i]) {
                    wait = intervals[i];
                }
                if (wait < capture->retry_ms) {
                    wait = capture->retry_ms;
                }
                if ((status == 566 || status == 567) && state->failures >= 2) {
                    wait = 86400000;
                }
                state->next = mono() + wait;
                state->not_before =
                    mono() +
                    ((status == 429 || status == 503 || status == 566 || status == 567) ? wait : 60000);
            }
            xSemaphoreGive(h->lock);
            cJSON_Delete(payload);
            free(capture->body);
            free(capture);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
esp_err_t clip_live_create(const clip_live_config_t *config, clip_live_handle_t *out) {
    if (!config || !out || !config->publish || !config->read || !config->paused) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    clip_live_handle_t h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    h->callbacks = *config;
    h->lock = xSemaphoreCreateMutex();
    atomic_init(&h->connected, false);
    atomic_init(&h->action_pending, false);
    atomic_init(&h->reconfigure, true);
    atomic_init(&h->apps, 0);
    atomic_init(&h->generation, 0);
    clip_live_settings_defaults(&h->settings);
    if (!h->lock) {
        clip_live_delete_unstarted(h);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nvs_open("pisen-live", NVS_READWRITE, &h->nvs);
    if (err != ESP_OK) {
        clip_live_delete_unstarted(h);
        return err;
    }
    size_t size = sizeof(h->ssid);
    nvs_get_str(h->nvs, "ssid", h->ssid, &size);
    size = sizeof(h->password);
    nvs_get_str(h->nvs, "password", h->password, &size);
    size = 0;
    if (nvs_get_str(h->nvs, "settings", NULL, &size) == ESP_OK && size < 4096) {
        char *text = malloc(size);
        if (text && nvs_get_str(h->nvs, "settings", text, &size) == ESP_OK) {
            cJSON *json = cJSON_Parse(text);
            clip_live_settings_parse(json, &h->settings);
            cJSON_Delete(json);
        }
        free(text);
    }
    if (!strcmp(h->settings.symbols, "sh000001,sz399001,sz399006")) {
        clip_live_settings_t *defaults = malloc(sizeof(*defaults));
        if (defaults) {
            clip_live_settings_defaults(defaults);
            snprintf(h->settings.symbols, sizeof(h->settings.symbols), "%s", defaults->symbols);
            free(defaults);
        }
    }
    locations_load(h);
    cJSON *primary = location_find(h->locations, h->default_location);
    if (primary) {
        snprintf(h->settings.city, sizeof(h->settings.city), "%s", string(primary, "name"));
        h->settings.latitude = cJSON_GetObjectItemCaseSensitive(primary, "latitude")->valuedouble;
        h->settings.longitude = cJSON_GetObjectItemCaseSensitive(primary, "longitude")->valuedouble;
    }
    *out = h;
    return ESP_OK;
}
void clip_live_delete_unstarted(clip_live_handle_t h) {
    if (h) {
        cJSON_Delete(h->locations);
        cJSON_Delete(h->action_request);
        cJSON_Delete(h->action_result);
        cJSON_Delete(h->queried_stock);
        if (h->nvs) {
            nvs_close(h->nvs);
        }
        if (h->lock) {
            vSemaphoreDelete(h->lock);
        }
        free(h);
    }
}
esp_err_t clip_live_start(clip_live_handle_t h) {
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err =
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, h, &h->wifi_handler);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, h, &h->ip_handler);
    if (err != ESP_OK) {
        return err;
    }
    return xTaskCreate(worker, "clip-network", 8192, h, 2, &h->task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t clip_live_configure(clip_live_handle_t h, const cJSON *command) {
    clip_live_settings_t *settings = malloc(sizeof(*settings));
    if (!settings) {
        return ESP_ERR_NO_MEM;
    }
    if (!clip_live_settings_parse(cJSON_GetObjectItemCaseSensitive(command, "settings"), settings)) {
        free(settings);
        return ESP_ERR_INVALID_ARG;
    }
    const char *ssid = string(command, "wifi_ssid"), *password = string(command, "wifi_password");
    if ((ssid && strlen(ssid) > 32) || (password && (*password && strlen(password) < 8)) ||
        (password && strlen(password) > 63) || (ssid && !password)) {
        free(settings);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *json = clip_live_settings_json(settings);
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!text) {
        free(settings);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    esp_err_t err = nvs_set_str(h->nvs, "settings", text);
    if (err == ESP_OK && ssid) {
        err = nvs_set_str(h->nvs, "ssid", ssid);
    }
    if (err == ESP_OK && ssid) {
        err = nvs_set_str(h->nvs, "password", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h->nvs);
    }
    if (err == ESP_OK) {
        bool radio_changed = strcmp(h->settings.mode, settings->mode) || (ssid && strcmp(ssid, h->ssid)) ||
                             (ssid && strcmp(password, h->password));
        bool city_changed = strcmp(h->settings.city, settings->city) ||
                            h->settings.latitude != settings->latitude || h->settings.longitude != settings->longitude;
        h->settings = *settings;
        if (city_changed) {
            cJSON *primary = location_find(h->locations, h->default_location);
            if (primary) {
                cJSON_ReplaceItemInObjectCaseSensitive(primary, "name", cJSON_CreateString(settings->city));
                cJSON_ReplaceItemInObjectCaseSensitive(primary, "latitude", cJSON_CreateNumber(settings->latitude));
                cJSON_ReplaceItemInObjectCaseSensitive(primary, "longitude", cJSON_CreateNumber(settings->longitude));
                cJSON_DeleteItemFromObjectCaseSensitive(primary, "forecast");
                err = locations_save(h);
            }
        }
        if (ssid) {
            snprintf(h->ssid, sizeof(h->ssid), "%s", ssid);
            snprintf(h->password, sizeof(h->password), "%s", password);
        }
        atomic_fetch_add(&h->generation, 1);
        for (int i = 0; i < 3; i++) {
            uint64_t minimum = h->states[i].last_attempt + 60000;
            if (minimum < h->states[i].not_before) {
                minimum = h->states[i].not_before;
            }
            if (h->states[i].last_attempt && minimum > mono()) {
                h->states[i].next = minimum;
            } else {
                h->states[i].next = 0;
            }
        }
        if (radio_changed) {
            atomic_store(&h->connected, false);
            h->ip[0] = 0;
            atomic_store(&h->reconfigure, true);
        }
    }
    xSemaphoreGive(h->lock);
    cJSON_free(text);
    free(settings);
    const cJSON *ms = cJSON_GetObjectItemCaseSensitive(command, "unix_ms");
    if (err == ESP_OK && cJSON_IsNumber(ms) && ms->valuedouble > 1700000000000.0 &&
        ms->valuedouble < 4102444800000.0) {
        struct timeval tv = {.tv_sec = (time_t)(ms->valuedouble / 1000),
                             .tv_usec = (suseconds_t)((uint64_t)ms->valuedouble % 1000) * 1000};
        settimeofday(&tv, NULL);
    }
    return err;
}
cJSON *clip_live_status(clip_live_handle_t h) {
    xSemaphoreTake(h->lock, portMAX_DELAY);
    cJSON *j = clip_live_settings_json(&h->settings);
    if (j) {
        cJSON_AddStringToObject(j, "weatherDefaultId", h->default_location);
        cJSON_AddItemToObject(j, "weatherLocations", locations_metadata(h->locations));
        cJSON_AddBoolToObject(j, "wifi_connected", atomic_load(&h->connected));
        cJSON_AddBoolToObject(j, "wifi_configured", h->ssid[0] != 0);
        cJSON_AddStringToObject(j, "wifi_ssid", h->ssid);
        cJSON_AddStringToObject(j, "wifi_ip", h->ip);
        cJSON_AddStringToObject(j, "wifi_error", esp_err_to_name(h->wifi_error));
        cJSON *feeds = cJSON_AddObjectToObject(j, "feeds");
        for (int i = 0; feeds && i < 3; i++) {
            cJSON *state = cJSON_AddObjectToObject(feeds, kinds[i]);
            if (!state) {
                continue;
            }
            cJSON_AddNumberToObject(state, "http_status", h->states[i].http);
            cJSON_AddStringToObject(state, "error", esp_err_to_name(h->states[i].error));
            cJSON_AddNumberToObject(state, "failures", h->states[i].failures);
            cJSON_AddNumberToObject(state, "last_success_ms", (double)h->states[i].last_success);
        }
    }
    xSemaphoreGive(h->lock);
    return j;
}
void clip_live_set_apps(clip_live_handle_t h, unsigned mask) {
    atomic_store(&h->apps, mask);
}
bool clip_live_wifi_connected(clip_live_handle_t h) {
    return h && atomic_load(&h->connected);
}
void clip_live_refresh(clip_live_handle_t h, const char *id) {
    int i = kind_index(id);
    if (i < 0) {
        return;
    }
    xSemaphoreTake(h->lock, portMAX_DELAY);
    uint64_t minimum = h->states[i].last_attempt + 60000;
    if (minimum < h->states[i].not_before) {
        minimum = h->states[i].not_before;
    }
    h->states[i].next = h->states[i].last_attempt && minimum > mono() ? minimum : 0;
    xSemaphoreGive(h->lock);
}

esp_err_t clip_live_add_stock(clip_live_handle_t h, const char *symbol) {
    if (!h || !symbol) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *config = clip_live_status(h);
    if (!config) {
        return ESP_ERR_NO_MEM;
    }
    const char *old = string(config, "symbols");
    char *list = calloc(320, 1);
    if (!list) {
        cJSON_Delete(config);
        return ESP_ERR_NO_MEM;
    }
    bool found = false;
    char *copy = strdup(old ? old : ""), *save = NULL;
    if (!copy) {
        free(list);
        cJSON_Delete(config);
        return ESP_ERR_NO_MEM;
    }
    for (char *p = strtok_r(copy, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        if (!strcmp(p, symbol)) {
            found = true;
        }
    }
    free(copy);
    if (found) {
        free(list);
        cJSON_Delete(config);
        return ESP_OK;
    }
    snprintf(list, 320, "%s%s%s", old ? old : "", old && *old ? "," : "", symbol);
    cJSON_ReplaceItemInObjectCaseSensitive(config, "symbols", cJSON_CreateString(list));
    free(list);
    cJSON *command = cJSON_CreateObject();
    if (!command) {
        cJSON_Delete(config);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(command, "settings", config);
    esp_err_t err = clip_live_configure(h, command);
    cJSON_Delete(command);
    return err;
}
