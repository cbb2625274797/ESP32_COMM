idf_component_register(
    SRCS
        "core/main.cpp"
        "ota/esp32_ota_core.cpp"
        "ota/ota_coordinator.cpp"
        "relay/ota_relay.cpp"
        "transport/spi_master.cpp"
        "transport/protocol.cpp"
        "server/wifi_ota.cpp"
        "server/ble_ota.cpp"
        "monitor/health_monitor.cpp"
        "monitor/debug_log.cpp"
    INCLUDE_DIRS
        "."
        "core"
        "ota"
        "relay"
        "transport"
        "server"
        "monitor"
        "include"
    REQUIRES
        app_update
        cjson
        esp_driver_spi
        esp_driver_tsens
        esp_driver_uart
        esp_wifi
        esp_netif
        esp_event
        esp_partition
        esp_timer
        esp_http_server
        esp_blockdev
        nvs_flash
        freertos
        mbedtls
        bt
)

# 每次构建时自动刷新 version.h（BUILD_TIME / GIT_HASH）
add_custom_target(version_header ALL
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/include/version.h
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_version_script.cmake
    COMMENT "Refreshing version.h"
    VERBATIM
)