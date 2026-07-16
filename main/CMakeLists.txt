# ESP32-C3 OTA 中继项目 - 主组件构建配置
idf_component_register(
    SRCS
        "core/main.cpp"                # 程序入口 (app_main)
        "core/system_boot.cpp"         # 启动模块位图与汇总
        "ota/esp32_ota_core.cpp"       # ESP32 本机 OTA 核心（分区写入/SHA验证）
        "ota/ota_coordinator.cpp"      # OTA 多通道协调器（状态机/互斥）
        "relay/ota_relay.cpp"          # GD32 SPI OTA 中继状态机
        "storage/gd32_flash_store.cpp" # GD32 Flash 存储抽象层（H1 提取）
        "server/wifi_ota.cpp"          # WiFi HTTP OTA 服务器
        "server/http_handlers.cpp"     # HTTP 路由处理（H2 从 wifi_ota 提取）
        "server/ble_ota.cpp"           # BLE GATT OTA 服务器
        "monitor/health_monitor.cpp"   # 系统健康监控（温度/RSSI/堆内存/栈水位主动巡检）
        "monitor/cpu_stats.cpp"        # 1Hz FreeRTOS CPU 占用采样任务
        "monitor/can_feedback_task.cpp" # CAN 1Hz 反馈轮询任务（0xC1）
        "monitor/debug_log.cpp"        # 三路调试日志（UART/UDP/环形缓冲）
        "monitor/task_stack_check.cpp" # 任务栈水位指标收集（health_mon 巡检 + /system/status 共享）
        "monitor/stack_overflow_hook.cpp" # FreeRTOS 栈溢出钩子（canary 触发时 UART 硬写告警）
    INCLUDE_DIRS
        "."
        "core"
        "ota"
        "relay"
        "storage"                     # GD32 Flash 存储抽象层头文件
        "server"
        "monitor"
        "include"                     # 公共头文件（app_config.h, version.h）
    REQUIRES
        transport                     # M5: 传输层组件（SPI/CANFD/协议）
        app_update                    # ESP32 OTA API
        cjson                         # JSON 解析/生成
        esp_driver_tsens              # 温度传感器驱动
        esp_driver_uart               # UART 驱动（调试日志）
        esp_wifi                      # WiFi STA/AP 模式
        esp_netif                     # 网络接口层
        esp_event                     # 事件循环
        esp_partition                 # Flash 分区操作
        esp_timer                     # 高精度定时器
        esp_http_server               # HTTP OTA 服务器
        esp_blockdev                  # Flash 块设备
        nvs_flash                     # NVS 存储（OTA 元数据）
        freertos                      # RTOS 内核
        mbedtls                       # SHA-256 / AES 加密
        bt                            # NimBLE BLE 协议栈
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