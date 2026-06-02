/*
 * app_config.h - 统一配置文件（C++ 版本）
 * ESP32-C3 OTA 中继项目
 *
 * 使用 constexpr 代替 #define，提供类型安全
 * 版本号和编译时间由 version.h 自动生成
 */
#pragma once
#include <cstdint>
#include "version.h"

// ============================================================
// 固件版本（由 version.h 自动生成，此处提供 C++ constexpr 访问）
// ============================================================
inline constexpr const char *FW_VERSION = FW_VERSION_STRING;
inline constexpr const char *FW_BUILD_TIME = BUILD_TIME;
inline constexpr const char *FW_GIT_HASH = GIT_HASH;

// ============================================================
// WiFi 工作模式选择
// ============================================================
// 0 = AP 模式（设备当热点）
// 1 = STA 模式（连接路由器）
inline constexpr uint8_t CONFIG_WIFI_MODE = 1;

// ============================================================
// WiFi STA 模式配置（连接路由器）
// ============================================================
inline constexpr const char* WIFI_STA_SSID = "004舰桥";
inline constexpr const char* WIFI_STA_PASSWORD = "q13362875622";
inline constexpr uint32_t WIFI_STA_CONNECT_TIMEOUT_MS = 30000;
inline constexpr uint8_t  WIFI_STA_MAX_RETRY = 5;  // 最大重连次数，超过后切换 AP 模式

// STA 静态 IP
inline constexpr uint8_t WIFI_STA_USE_STATIC_IP = 0;  // 先关闭静态 IP，使用 DHCP 排查连接问题

// 静态 IP 配置（始终定义，由 WIFI_STA_USE_STATIC_IP 在运行时决定是否使用）
inline constexpr const char* WIFI_STA_STATIC_IP      = "192.168.31.32";
inline constexpr const char* WIFI_STA_STATIC_GW      = "192.168.31.1";
inline constexpr const char* WIFI_STA_STATIC_NETMASK = "255.255.255.0";
inline constexpr const char* WIFI_STA_STATIC_DNS     = "192.168.31.1";

// ============================================================
// WiFi AP 模式配置（设备当热点）
// ============================================================
inline constexpr const char* WIFI_AP_SSID     = "ESP32_OTA_RELAY";
inline constexpr const char* WIFI_AP_PASSWORD = "12345678";
inline constexpr uint8_t     WIFI_AP_CHANNEL = 6;
inline constexpr uint8_t     WIFI_AP_MAX_CONNECT = 4;

// ============================================================
// HTTP 服务器配置
// ============================================================
inline constexpr uint16_t HTTP_SERVER_PORT      = 80;
inline constexpr uint32_t HTTP_SERVER_STACK_SIZE = 8192;

// ============================================================
// OTA 配置
// ============================================================
inline constexpr size_t  OTA_BLOCK_SIZE         = 8192;
inline constexpr size_t  OTA_MAX_FIRMWARE_SIZE  = 216 * 1024;  // 216KB，对齐 GD32 APP_MAX_SIZE (0x36000)
inline constexpr size_t  GD32_OTA_PACKET_SIZE   = 48;          // GD32 OTA 实际转发包载荷
inline constexpr uint32_t OTA_POLL_RETRIES      = 40;          // 每包 ACK 轮询次数
inline constexpr uint32_t OTA_RESEND_RETRIES    = 10;           // 每包重发次数上限
inline constexpr uint32_t OTA_PACKET_DELAY_MS   = 5;            // 每包发送后延时(ms)，给 GD32 处理时间

// ============================================================
// 健康监控阈值
// ============================================================
inline constexpr int32_t WIFI_RSSI_CRITICAL = -85;
inline constexpr int32_t WIFI_RSSI_WARNING  = -75;
inline constexpr int32_t OTA_WIFI_RSSI_CRITICAL = -88;
inline constexpr uint32_t OTA_WIFI_RSSI_SAMPLE_MAX_AGE_MS = 15000;
inline constexpr uint32_t OTA_PACKET_HEALTH_CHECK_INTERVAL = 32;
inline constexpr int32_t TEMP_WARNING       = 80;
inline constexpr int32_t TEMP_CRITICAL      = 100;
inline constexpr size_t  HEAP_WARNING_MIN   = 10240;

// ============================================================
// 调试配置（UDP 日志）
// ============================================================
inline constexpr uint8_t  DEBUG_LOG_ENABLED = 1;
inline constexpr const char* DEBUG_LOG_IP   = "192.168.31.3";
inline constexpr uint16_t DEBUG_LOG_PORT   = 6666;

// ============================================================
// BLE OTA 配置
// ============================================================
inline constexpr const char* BLE_DEVICE_NAME    = "ESP32-OTA-RELAY";
inline constexpr uint16_t    BLE_MTU_SIZE       = 512;
inline constexpr uint16_t    BLE_OTA_SERVICE_UUID = 0xFFFF;
inline constexpr uint16_t    BLE_OTA_CTRL_UUID   = 0x0001;
inline constexpr uint16_t    BLE_OTA_DATA_IN_UUID  = 0x0002;
inline constexpr uint16_t    BLE_OTA_DATA_OUT_UUID = 0x0003;
inline constexpr uint16_t    BLE_OTA_VERSION_UUID  = 0x0004;
inline constexpr uint8_t BLE_OTA_CTRL_TRIGGER_GD32  = 0xF7;
inline constexpr uint8_t BLE_OTA_CTRL_TRIGGER_ESP32 = 0xF8;
inline constexpr uint8_t BLE_OTA_CMD_GD32_DATA  = 0x00;
inline constexpr uint8_t BLE_OTA_CMD_ESP32_DATA = 0x01;
inline constexpr size_t      BLE_OTA_CHUNK_PAYLOAD = 480;
inline constexpr size_t      BLE_OTA_PACKET_OVERHEAD = 11;

// ============================================================
// 安全常量（需与 GD32 固件团队确认）
// ⚠️ 请填入实际密钥
// ============================================================
inline constexpr uint8_t MASTER_KEY[] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
};

inline constexpr uint8_t SCRAMBLE_CONST[] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00
};

// GD32_CHIP_UID 仅保留为默认占位，运行时以握手包解出的芯片地址 UID 为准。
inline constexpr uint32_t DEVICE_SERIAL_ADDR_LSB = 0x1FFFF7E8U;
inline constexpr uint32_t DEVICE_SERIAL_ADDR_MID = 0x1FFFF7ECU;
inline constexpr uint32_t DEVICE_SERIAL_ADDR_MSB = 0x1FFFF7F0U;

inline constexpr uint8_t GD32_CHIP_UID[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B
};

