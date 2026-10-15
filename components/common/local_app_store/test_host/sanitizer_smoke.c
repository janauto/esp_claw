#include "local_app_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    assert(argc == 2);
    local_app_store_handle_t store = NULL;
    local_app_store_config_t cfg = {.root = argv[1]};
    assert(local_app_store_create(&cfg, &store) == ESP_OK);
    const char *code = "print(1)\n";
    char id[32];
    char *json = malloc(1024);
    assert(json);
    for (int i = 0; i < 41; i++) {
        snprintf(id, sizeof(id), "app%02d", i);
        snprintf(json, 1024,
                 "{\"schema\":1,\"app_id\":\"%s\",\"title\":\"Safe\",\"frame_color\":1,\"pill_color\":2,"
                 "\"files\":{\"main.lua\":{\"size\":9,\"sha256\":"
                 "\"cc42155088fca5730758db72b2a5bca33112a941dfaa2d43098ec422ce4ea213\"}}}",
                 id);
        cJSON *m = cJSON_Parse(json);
        assert(m);
        assert(local_app_store_begin(store, m) == ESP_OK);
        assert(local_app_store_write(store, id, "main.lua", 0, code, strlen(code)) == ESP_OK);
        assert(local_app_store_commit(store, id) == ESP_OK);
        cJSON_Delete(m);
    }
    for (int i = 0; i < 41; i++) {
        cJSON *v = NULL;
        assert(local_app_store_at(store, i, &v) == ESP_OK);
        snprintf(id, sizeof(id), "app%02d", i);
        assert(strcmp(cJSON_GetObjectItem(v, "app_id")->valuestring, id) == 0);
        cJSON_Delete(v);
    }
    for (int i = 10; i < 20; i++) {
        snprintf(id, sizeof(id), "app%02d", i);
        assert(local_app_store_remove(store, id) == ESP_OK);
    }
    assert(local_app_store_rename(store, "app40", "New title") == ESP_OK);
    assert(local_app_store_reload(store) == ESP_OK);
    cJSON *summary = NULL;
    assert(local_app_store_summary(store, &summary) == ESP_OK);
    assert(cJSON_GetObjectItem(summary, "count")->valueint == 31);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(summary, "last"), "title"));
    cJSON_Delete(summary);
    free(json);
    local_app_store_delete(store);
    puts("ASan/UBSan: install, group navigation, remove empty group, rename, recovery passed");
    return 0;
}
