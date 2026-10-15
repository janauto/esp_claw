set(ble_host_bridge_requires
    bt
    cap_lua
    cjson
    freertos
    mbedtls
    nvs_flash
    system_ui
    local_app_store
)

# The HID services are registered into this component's GATT server, so the
# bridge needs the HID module's attach API when that module is enabled.
if(CONFIG_APP_CLAW_LUA_MODULE_BLE_HID)
    list(APPEND ble_host_bridge_requires lua_module_ble_hid)
endif()

idf_component_register(
    SRCS
        "src/ble_host_bridge.c"
        "src/ble_host_apps.c"
        "src/voice_frame.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "$ENV{IDF_PATH}/components/bt/host/nimble/esp-hci/include"
    REQUIRES
        ${ble_host_bridge_requires}
)
