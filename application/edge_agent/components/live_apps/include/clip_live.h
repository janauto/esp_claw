#pragma once
#include "clip_live_format.h"
#include "esp_err.h"
typedef struct clip_live_t *clip_live_handle_t;
typedef struct {
    esp_err_t (*publish)(const char *id,const cJSON *payload,void *ctx);
    esp_err_t (*read)(const char *id,char **payload,int64_t *age,void *ctx);
    bool (*paused)(void *ctx);
    bool (*wifi_status)(char *ip, size_t size, void *ctx);
    void *ctx;
} clip_live_config_t;
esp_err_t clip_live_create(const clip_live_config_t *config,clip_live_handle_t *out);
void clip_live_delete_unstarted(clip_live_handle_t h);
esp_err_t clip_live_start(clip_live_handle_t h);
esp_err_t clip_live_configure(clip_live_handle_t h,const cJSON *command);
cJSON *clip_live_status(clip_live_handle_t h);
void clip_live_set_apps(clip_live_handle_t h,unsigned mask);
void clip_live_refresh(clip_live_handle_t h,const char *id);
bool clip_live_wifi_connected(clip_live_handle_t h);

esp_err_t clip_live_add_stock(clip_live_handle_t h,const char *symbol);

esp_err_t clip_live_action(clip_live_handle_t h,const cJSON *request,cJSON **response);
