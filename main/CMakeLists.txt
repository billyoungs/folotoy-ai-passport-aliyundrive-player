idf_component_register(
    SRCS "main.c"
         "bp_ui.c"
         "bp_ui_reel.c"
         "bp_aliyun.c"
         "bp_keys.c"
         "bp_player.c"
         "bp_media.c"
         "bp_mp4.c"
         "bp_console.c"
         "bp_wifi.cc"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_wifi esp-wifi-connect esp_http_client json mbedtls libhelix-aac
    PRIV_REQUIRES esp_netif esp_event esp_timer esp-tls lwip lvgl
)
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/bp_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/bp_font_24.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/images/bp_reel_disc.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/images/bp_fish.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/images/bp_char_discs.c"
)

# 应用凭据:构建时从被 Git 忽略的 bp_baidu_keys.h 生成编码后的 bp_keys_obf.h,
# C 代码只包含生成的头文件,固件中不出现明文凭据。缺失时用占位符模板。
set(BP_KEYS_SRC "${CMAKE_CURRENT_LIST_DIR}/bp_baidu_keys.h")
if(NOT EXISTS "${BP_KEYS_SRC}")
    message(WARNING "main/bp_baidu_keys.h missing: building with placeholder Baidu credentials")
    set(BP_KEYS_SRC "${CMAKE_CURRENT_LIST_DIR}/bp_baidu_keys.example.h")
endif()
idf_build_get_property(python PYTHON)
set(BP_KEYS_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/obfuscate_keys.py")
set(BP_KEYS_OUT "${CMAKE_CURRENT_BINARY_DIR}/bp_keys_obf.h")
add_custom_command(
    OUTPUT "${BP_KEYS_OUT}"
    COMMAND ${python} "${BP_KEYS_TOOL}" --in "${BP_KEYS_SRC}" --out "${BP_KEYS_OUT}"
    DEPENDS "${BP_KEYS_SRC}" "${BP_KEYS_TOOL}"
    VERBATIM)
add_custom_target(bp_keys_obf DEPENDS "${BP_KEYS_OUT}")
add_dependencies(${COMPONENT_LIB} bp_keys_obf)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# 上游 demo_*.c / ui_pixel*.c 保留在磁盘上供主机测试(tools/validate.sh 直接编译),
# 不参与固件构建:本应用是全新界面(强制 UI 重新设计规则)。
