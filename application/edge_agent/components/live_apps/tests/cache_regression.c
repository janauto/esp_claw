#include "clip_feed.h"
#include "clip_live_format.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    clip_live_settings_t original, restored;
    clip_live_settings_defaults(&original);
    strcpy(original.city, "紫阳县");
    strcpy(original.symbols, "usBRK.B,sh600000,sz000001");
    original.latitude = 32.521;
    original.longitude = 108.535;
    cJSON *settings = clip_live_settings_json(&original);
    assert(clip_live_settings_parse(settings, &restored));
    assert(!strcmp(original.city, restored.city));
    assert(!strcmp(original.symbols, restored.symbols));
    assert(!strcmp(original.mode, restored.mode));
    assert(original.latitude == restored.latitude && original.longitude == restored.longitude);
    cJSON_Delete(settings);

    char symbol[20];
    assert(clip_live_stock_symbol("600000", symbol, sizeof(symbol)) && !strcmp(symbol, "sh600000"));
    assert(clip_live_stock_symbol("000001", symbol, sizeof(symbol)) && !strcmp(symbol, "sz000001"));
    assert(!clip_live_stock_symbol("../../bad", symbol, sizeof(symbol)));

    clip_feed_handle_t feed;
    assert(clip_feed_create(argv[1], &feed) == ESP_OK);
    cJSON *cards = cJSON_CreateArray(), *card = cJSON_CreateObject();
    cJSON_AddStringToObject(card, "title", "完整资讯正文");
    cJSON_AddStringToObject(card, "value", "第一段");
    cJSON_AddStringToObject(card, "detail", "三段正文及天气地区元数据保留");
    char body[9001]; memset(body, 'x', 9000); body[9000] = 0;
    cJSON_AddStringToObject(card, "body", body);
    cJSON_AddItemToArray(cards, card);
    cJSON *payload = clip_live_payload("aihot", cards, 1792050000000ULL);
    assert(payload && clip_feed_update(feed, "aihot", payload, 1000) == ESP_OK);
    char *before = NULL, *after = NULL; int64_t age;
    assert(clip_feed_read(feed, "aihot", 1500, &before, &age) == ESP_OK && age == 500);
    assert(strstr(before, body));
    /* A failed refresh must leave the previous usable data intact. */
    assert(clip_feed_update(feed, "weather", payload, 2000) == ESP_ERR_INVALID_ARG);
    cJSON *bad = cJSON_CreateObject();
    assert(clip_feed_update(feed, "aihot", bad, 2000) == ESP_ERR_INVALID_ARG);
    cJSON_Delete(bad);
    assert(clip_feed_read(feed, "aihot", 2500, &after, &age) == ESP_OK && !strcmp(before, after));
    free(after);
    clip_feed_delete(feed);
    assert(clip_feed_create(argv[1], &feed) == ESP_OK);
    assert(clip_feed_read(feed, "aihot", 10, &after, &age) == ESP_OK && age == -1);
    assert(!strcmp(before, after)); free(after);
    /* Interrupted atomic replacement recovers the last complete file. */
    clip_feed_delete(feed);
    char live[1024], old[1024];
    snprintf(live, sizeof(live), "%s/clip_feeds/aihot.json", argv[1]);
    snprintf(old, sizeof(old), "%s/clip_feeds/aihot.old", argv[1]);
    assert(rename(live, old) == 0);
    assert(clip_feed_create(argv[1], &feed) == ESP_OK);
    assert(clip_feed_read(feed, "aihot", 20, &after, &age) == ESP_OK && !strcmp(before, after));
    free(before); free(after); cJSON_Delete(payload); clip_feed_delete(feed);
    puts("PASS: settings/symbols, 9000-byte body, failed refresh, restart and interrupted replacement");
    return 0;
}
