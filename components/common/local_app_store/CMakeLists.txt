idf_component_register(SRCS "src/local_app_store.c" "src/local_app_index.c"
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" REQUIRES cjson mbedtls)
