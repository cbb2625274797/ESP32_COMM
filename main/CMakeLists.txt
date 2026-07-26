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
        "server/ble_ota.cpp"           # BLE OTA 公开 API / 收尾 / 共存
        "server/ble_ota_data.cpp"      # BLE OTA 数据路径（reorder/worker/DATA_IN）
        "server/ble_ota_host.cpp"      # BLE OTA GATT/GAP/广播/Host
        "monitor/health_monitor.cpp"   # 系统健康监控（温度/RSSI/堆内存/栈水位主动巡检）
        "monitor/cpu_stats.cpp"        # 1Hz FreeRTOS CPU 占用采样任务
        "monitor/can_feedback_task.cpp" # CAN 反馈被动监听（0xC1 takeEvent）+ 分发 0xC4
        "monitor/gd32_adc_feedback.cpp" # GD32 ADC 调试反馈缓存（0xC4）
        "monitor/motor_state.cpp"      # GD32 多电机状态缓存（0xC5）
        "monitor/motor_version.cpp"    # GD32 多电机版本缓存（0xC6，事件驱动）
        "monitor/motor_ota_relay.cpp"  # 电机 OTA relay（0xD0~0xD2）
        "monitor/debug_log.cpp"        # 三路调试日志（UART/UDP/环形缓冲）
        "monitor/task_stack.cpp"       # 任务栈水位收集 + 溢出钩子
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

