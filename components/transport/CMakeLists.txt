# transport 组件 - SPI 总线驱动、CANFD 桥接、协议层
# 提供底层硬件通信能力，被 main 组件依赖。
#
# PRIV_INCLUDE_DIRS 仅对本组件编译可见，
# 不会将 main/ 的子目录路径泄漏给依赖本组件的其他组件，
# 从而在编译层面强制模块边界。

idf_component_register(
    SRCS
        "../../main/transport/spi_master.cpp"
        "../../../spi_link/spi_link_v2.cpp"
        "../../main/transport/canfd_bridge.cpp"
        "../../main/transport/protocol.cpp"
    INCLUDE_DIRS
        "../../main/transport"       # spi_master.h, canfd_bridge.h, protocol.h
        "../../../spi_link"           # spi_link_v2.h（三工程共享协议头）
    PRIV_INCLUDE_DIRS
        "../../main"                 # monitor/debug_log.h（仅本组件编译时使用）
        "../../main/include"         # app_config.h
    REQUIRES
        esp_driver_spi               # SPI 主设备驱动
        esp_driver_gpio              # GPIO 驱动（SPI Ready 握手引脚）
        freertos                     # FreeRTOS 内核
        mbedtls                      # AES 加密
        esp_timer                    # 高精度定时器
)
