#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

typedef struct clip_feed_t *clip_feed_handle_t;
/* Calls are serialized by the owning clip_mvp object. Returned text is caller-owned. */
bool clip_feed_id_valid(const char *id);
esp_err_t clip_feed_create(const char *data_root, clip_feed_handle_t *out);
void clip_feed_delete(clip_feed_handle_t h);
esp_err_t clip_feed_update(clip_feed_handle_t h, const char *id, const cJSON *payload, uint64_t now_ms);
esp_err_t clip_feed_read(clip_feed_handle_t h, const char *id, uint64_t now_ms, char **payload, int64_t *age_ms);
