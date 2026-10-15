# Host preview of the firmware system UI screens (see README.md).
cmake_minimum_required(VERSION 3.16)
project(system_ui_preview C)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(APP "${REPO}/application/edge_agent")
set(LVGL_DIR "${APP}/managed_components/lvgl__lvgl" CACHE PATH "LVGL source (fetched by idf.py build)")
set(UI_SQ "${REPO}/components/common/ui_squareline")
set(SYS_UI "${REPO}/components/common/system_ui")

set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_DIR}" lvgl EXCLUDE_FROM_ALL)
target_include_directories(lvgl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB UI_SQ_SRCS
    "${UI_SQ}/src/*.c" "${UI_SQ}/src/fonts/*.c"
    "${UI_SQ}/src/images/*.c" "${UI_SQ}/src/components/*.c")
list(FILTER UI_SQ_SRCS EXCLUDE REGEX "ui_img_sd\\.c$")

# Image name -> compiled-in descriptor, standing in for the SD-card .bin files.
file(GLOB IMG_SRCS "${UI_SQ}/src/images/*.c")
set(IMG_DECL "")
set(IMG_ROWS "")
foreach(f ${IMG_SRCS})
    file(STRINGS "${f}" lines REGEX "^const lv_image_dsc_t [A-Za-z0-9_]+ = ")
    foreach(l ${lines})
        string(REGEX REPLACE "^const lv_image_dsc_t ([A-Za-z0-9_]+) = .*" "\\1" n "${l}")
        string(APPEND IMG_DECL "extern const lv_image_dsc_t ${n};\n")
        string(APPEND IMG_ROWS "    { \"${n}\", &${n} },\n")
    endforeach()
endforeach()
configure_file(img_table.c.in "${CMAKE_CURRENT_BINARY_DIR}/img_table.c" @ONLY)

add_executable(system_ui_preview
    main.c stubs.c "${CMAKE_CURRENT_BINARY_DIR}/img_table.c"
    ${UI_SQ_SRCS}
    "${SYS_UI}/src/boot_splash.c"
    "${SYS_UI}/src/businesscard_screen.c"
    "${SYS_UI}/src/meeting_minutes_screen.c"
    "${SYS_UI}/src/infrared_remote_screen.c"
    "${SYS_UI}/src/infrared_remote_commands.c"
    "${SYS_UI}/src/workbuddy_screen.c")
target_include_directories(system_ui_preview PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${LVGL_DIR}"
    "${UI_SQ}/include" "${UI_SQ}/src"
    "${SYS_UI}/include" "${SYS_UI}/src"
    "${REPO}/components/claw_modules/claw_core/include"
    "${REPO}/components/common/display_service/include"
    "${REPO}/components/lua_modules/lua_module_audio/src"
    "${APP}/managed_components/espressif__esp_audio_codec/include"
    "${APP}/managed_components/georgik__lua/lua"
    "${APP}/managed_components/georgik__lua/include"
    "${APP}/managed_components/espressif__esp_audio_codec/include/encoder"
    "${APP}/managed_components/espressif__esp_audio_codec/include/decoder")
target_compile_definitions(system_ui_preview PRIVATE
    PREVIEW_IMG_BASE="/preview"
    PREVIEW_FONT_PATH="${APP}/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf")
target_compile_options(system_ui_preview PRIVATE -include stdlib.h -Wno-unused-function -Wno-unused-variable)
target_link_libraries(system_ui_preview PRIVATE lvgl m)
