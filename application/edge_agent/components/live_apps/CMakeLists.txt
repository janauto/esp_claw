idf_component_register(
    SRCS "src/live_apps.c" "src/live_apps_cli.c" "src/clip_feed.c"
         "src/clip_live.c" "src/clip_live_format.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "china_regions.tsv"
    REQUIRES app_claw app_config cap_lua claw_core claw_skill
             lua_module_clip_ui display_service system_ui wifi_manager
             esp_board_manager console esp_timer cjson nvs_flash
             esp_http_client esp_event esp_netif esp_wifi mbedtls)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
