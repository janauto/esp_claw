#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cJSON.h"
typedef struct { char city[96]; double latitude,longitude; char symbols[320]; char mode[8]; } clip_live_settings_t;
void clip_live_settings_defaults(clip_live_settings_t *settings);
bool clip_live_settings_parse(const cJSON *json, clip_live_settings_t *settings);
cJSON *clip_live_settings_json(const clip_live_settings_t *settings);
cJSON *clip_live_parse_weather(const cJSON *response, const clip_live_settings_t *settings, uint64_t now_ms);
cJSON *clip_live_parse_hot(const cJSON *response, uint64_t now_ms);
cJSON *clip_live_parse_stock_card(const cJSON *response, const char *symbol);
cJSON *clip_live_payload(const char *kind, cJSON *cards, uint64_t now_ms);

void clip_live_enrich_hot_card(cJSON *card, const char *html);
void clip_live_stock_history(cJSON *card, const cJSON *response, const char *symbol);

bool clip_live_stock_symbol(const char *input,char *out,size_t size);
