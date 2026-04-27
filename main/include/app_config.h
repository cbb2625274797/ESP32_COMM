/*
 * app_config.h - 统一配置文件（C++ 版本）
 * ESP32-C3 OTA 中继项目
 *
 * 使用 constexpr 代替 #define，提供类型安全
 */
#pragma once
#include <cstdint>

// ============================================================
// WiFi 工作模式选择
// ============================================================
// 0 = AP 模式（设备当热点）
// 1 = STA 模式（连接路由器）
inline constexpr uint8_t CONFIG_WIFI_MODE = 0;

// ============================================================
// WiFi STA 模式配置（连接路由器）
// ============================================================
inline constexpr const char* WIFI_STA_SSID = "004舰桥";
inline constexpr const char* WIFI_STA_PASSWORD = "q13362875622";
inline constexpr uint32_t WIFI_STA_CONNECT_TIMEOUT_MS = 30000;

// STA 静态 IP
inline constexpr uint8_t WIFI_STA_USE_STATIC_IP = 1;  // 设为 1 启用静态 IP

#if WIFI_STA_USE_STATIC_IP
inline constexpr const char* WIFI_STA_STATIC_IP      = "192.168.1.100";
inline constexpr const char* WIFI_STA_STATIC_GW      = "192.168.1.1";
inline constexpr const char* WIFI_STA_STATIC_NETMASK = "255.255.255.0";
inline constexpr const char* WIFI_STA_STATIC_DNS     = "192.168.1.1";
#endif

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
inline constexpr size_t  OTA_BLOCK_SIZE         = 4096;
inline constexpr size_t  OTA_MAX_FIRMWARE_SIZE   = 2 * 1024 * 1024;  // 2MB

// ============================================================
// 健康监控阈值
// ============================================================
inline constexpr int32_t WIFI_RSSI_CRITICAL = -75;
inline constexpr int32_t WIFI_RSSI_WARNING  = -65;
inline constexpr int32_t TEMP_WARNING       = 80;
inline constexpr int32_t TEMP_CRITICAL      = 100;
inline constexpr size_t  HEAP_WARNING_MIN   = 10240;

// ============================================================
// 调试配置（UDP 日志）
// ============================================================
inline constexpr uint8_t  DEBUG_LOG_ENABLED = 1;       // 设为 0 禁用调试日志
inline constexpr const char* DEBUG_LOG_IP   = "192.168.31.3";  // 上位机 IP
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

// ============================================================
// 安全常量（需与 GD32 固件团队确认）
// ⚠️ 请填入实际密钥
// ============================================================
inline constexpr uint8_t MASTER_KEY[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

inline constexpr uint8_t SCRAMBLE_CONST[] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00
};

// ⚠️ 请填入实际的 GD32 芯片唯一码（12字节）
inline constexpr uint8_t GD32_CHIP_UID[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B
};
