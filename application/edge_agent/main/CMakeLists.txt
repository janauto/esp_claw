set(main_requires
    ble_host_bridge
    cap_im_platform
    captive_dns
    cmd_wifi
    esp_app_format
)

idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES ${main_requires}
)