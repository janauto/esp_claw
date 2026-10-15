#pragma once
#include "app_config.h"
#include "clip_live.h"
#include "esp_err.h"

/* Copy existing companion Wi-Fi credentials only when the new app config has
 * none. Existing user configurations and pisen-live NVS remain untouched. */
esp_err_t live_apps_import_wifi(app_config_t *config);
esp_err_t live_apps_prepare(void);
esp_err_t live_apps_start(void);
esp_err_t live_apps_cli_register(void);
clip_live_handle_t live_apps_network(void);
esp_err_t live_apps_feed_read(const char *id, char **text, int64_t *age, void *ctx);
