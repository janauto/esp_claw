idf_component_register(SRCS "src/lua_module_clip_ui.c" "src/clip_stock_board.c" "src/clip_reader_board.c" "src/clip_feed_widgets.c" INCLUDE_DIRS "include" EMBED_FILES "clip_fallback.ttf"
    REQUIRES cap_lua display_service esp_timer cjson claw_core)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
