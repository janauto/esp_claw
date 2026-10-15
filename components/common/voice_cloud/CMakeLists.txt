# 底座模式语音链路（voice_cloud）：Opus → Ogg → 百炼 ASR → OpenAI 兼容对话 + 工具。
# 纯逻辑：vc_ogg.c / vc_text.c / vc_proto.c / vc_pipeline.c（宿主测试见 test_host/）；
# 设备胶水：voice_cloud.c（NVS、收包缓存、工作任务）与 vc_http_esp.c（esp_http_client + 证书包）。
# 注意：IDF 6.1 里没有 json 组件，cJSON 来自托管组件 espressif/cjson（见 idf_component.yml），组件名为 cjson。
idf_component_register(
    SRCS
        "src/vc_ogg.c"
        "src/vc_text.c"
        "src/vc_proto.c"
        "src/vc_pipeline.c"
        "src/voice_cloud.c"
        "src/vc_http_esp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES cjson
    PRIV_REQUIRES esp_http_client mbedtls nvs_flash esp_timer freertos heap log
)
# 与 live_apps 一致：新组件按体积优化（5 MiB 应用分区余量很小）
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
# 1.0 样机内部 RAM 只剩约 13 KB：工作任务栈放 PSRAM。工作任务只做 HTTP/JSON，
# 界面更新与 NVS 写入都投递给 system_ui 的事件任务（内部栈），不在 PSRAM 栈上碰 Flash。
target_compile_definitions(${COMPONENT_LIB} PRIVATE VOICE_CLOUD_TASK_STACK_PSRAM=1)
