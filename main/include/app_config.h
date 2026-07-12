/*
 * app_config.h - 统一配置文件（C++ 版本）
 * ESP32-C3 OTA 中继项目
 *
 * 使用 constexpr 代替 #define，提供类型安全
 * 版本号和编译时间由 version.h 自动生成
 */
#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "version.h"

// ============================================================
// 固件版本（由 version.h 自动生成，此处提供 C++ constexpr 访问）
// ============================================================
inline constexpr const char *FW_VERSION = FW_VERSION_STRING;     ///< 固件版本字符串（自动生成）
inline constexpr const char *FW_BUILD_TIME = BUILD_TIME;         ///< 编译时间字符串（自动生成）
inline constexpr const char *FW_GIT_HASH = GIT_HASH;             ///< Git 提交哈希（自动生成）

// ============================================================
// WiFi 工作模式选择
// ============================================================
// 0 = AP 模式（设备当热点）
// 1 = STA 模式（连接路由器）
inline constexpr uint8_t CONFIG_WIFI_MODE = 1;

// ============================================================
// WiFi STA 模式配置（连接路由器）
// ============================================================
inline constexpr const char* WIFI_STA_SSID = "004舰桥";            ///< STA 模式目标 WiFi 名称
inline constexpr const char* WIFI_STA_PASSWORD = "q13362875622";   ///< STA 模式 WiFi 密码
inline constexpr uint32_t WIFI_STA_CONNECT_TIMEOUT_MS = 30000;     ///< STA 连接超时（毫秒）
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
inline constexpr const char* WIFI_AP_SSID     = "ESP32_OTA_RELAY"; ///< AP 模式热点名称
inline constexpr const char* WIFI_AP_PASSWORD = "12345678";        ///< AP 模式热点密码
inline constexpr uint8_t     WIFI_AP_CHANNEL = 6;                  ///< AP 模式工作信道
inline constexpr uint8_t     WIFI_AP_MAX_CONNECT = 4;              ///< AP 模式最大连接数

// ============================================================
// HTTP 服务器配置
// ============================================================
inline constexpr uint16_t HTTP_SERVER_PORT      = 80;              ///< HTTP 服务器监听端口
inline constexpr uint32_t HTTP_SERVER_STACK_SIZE = 10240;          ///< HTTP 服务器任务栈大小（字节）
// ★ 历史：6144 栈溢出；8192 在上传收尾仍紧。曾提到 12288 但 ESP32-C3 堆极紧
//   （OTA 后 free~5KB、largest~1KB）。SHA context 已改 static，httpd 栈 10KB 折中。
//   真·栈溢出钩子会 abort；卡 100% 更多是堆碎导致回包/后续分配失败。

// ============================================================
// OTA 配置
// ============================================================
inline constexpr size_t  OTA_BLOCK_SIZE         = 2048;            ///< 上传接收块：2KB 降峰值堆（原 4/8KB）
inline constexpr size_t  OTA_MAX_FIRMWARE_SIZE  = 216 * 1024;  // 216KB = 221184B = 0x35F90，固件大小上限
inline constexpr size_t  GD32_OTA_PACKET_SIZE   = 48;          // GD32 OTA 实际转发包载荷
inline constexpr uint32_t OTA_POLL_RETRIES      = 40;          // 每包 ACK 轮询次数
inline constexpr uint32_t OTA_RESEND_RETRIES    = 10;           // 每包重发次数上限
inline constexpr uint32_t OTA_PACKET_DELAY_MS   = 5;            // 每包发送后延时(ms)，给 GD32 处理时间
inline constexpr uint32_t OTA_POST_SEND_DELAY_MS    = 3;        // 发送请求后首次轮询前的延迟(ms)，无 READY 拉低时防止读到 stale 数据
inline constexpr uint32_t OTA_POLL_NO_FRAME_DELAY_MS = 8;       // 轮询未找到帧时的重试延迟(ms)，无 READY 拉低时需更长

// ============================================================
// 健康监控阈值
// ============================================================
inline constexpr int32_t WIFI_RSSI_CRITICAL = -85;                 ///< WiFi 信号严重警告阈值（dBm）
inline constexpr int32_t WIFI_RSSI_WARNING  = -80;                 ///< WiFi 信号一般警告阈值（dBm）
inline constexpr int32_t OTA_WIFI_RSSI_CRITICAL = -88;             ///< OTA 传输期间 WiFi 信号临界阈值
inline constexpr uint32_t OTA_WIFI_RSSI_SAMPLE_MAX_AGE_MS = 15000; ///< RSSI 采样最大有效期（毫秒）
inline constexpr uint32_t OTA_PACKET_HEALTH_CHECK_INTERVAL = 32;   ///< 每 N 包执行一次健康检查
inline constexpr int32_t TEMP_WARNING       = 80;                  ///< 芯片温度警告阈值（℃）
inline constexpr int32_t TEMP_CRITICAL      = 100;                 ///< 芯片温度严重警告阈值（℃）
inline constexpr size_t  HEAP_WARNING_MIN   = 10240;               ///< 堆内存警告阈值（字节）

// ============================================================
// 调试配置（UDP 日志）
// ============================================================
inline constexpr uint8_t  DEBUG_LOG_ENABLED = 1;                   ///< UDP 调试日志开关（1=启用）
inline constexpr const char* DEBUG_LOG_IP   = "192.168.31.3";      ///< UDP 日志目标 IP
inline constexpr uint16_t DEBUG_LOG_PORT   = 6666;                 ///< UDP 日志目标端口

// ============================================================
// BLE OTA 配置
// ============================================================
inline constexpr const char* BLE_DEVICE_NAME    = "ESP32-OTA-RELAY"; ///< BLE 广播设备名
inline constexpr uint16_t    BLE_MTU_SIZE       = 512;               ///< BLE MTU 协商大小
inline constexpr uint16_t    BLE_OTA_SERVICE_UUID = 0xFFFF;          ///< OTA GATT 服务 UUID
inline constexpr uint16_t    BLE_OTA_CTRL_UUID   = 0x0001;           ///< OTA 控制特征 UUID（写入命令）
inline constexpr uint16_t    BLE_OTA_DATA_IN_UUID  = 0x0002;         ///< OTA 数据输入特征 UUID（写固件包）
inline constexpr uint16_t    BLE_OTA_DATA_OUT_UUID = 0x0003;         ///< OTA 数据输出特征 UUID（通知/ACK）
inline constexpr uint16_t    BLE_OTA_VERSION_UUID  = 0x0004;         ///< OTA 版本信息特征 UUID（只读）
inline constexpr uint8_t BLE_OTA_CTRL_TRIGGER_GD32  = 0xF7;         ///< BLE 控制命令：触发 GD32 中继 OTA
inline constexpr uint8_t BLE_OTA_CTRL_TRIGGER_ESP32 = 0xF8;         ///< BLE 控制命令：触发 ESP32 本机 OTA
inline constexpr uint8_t BLE_OTA_CTRL_VERIFY_SHA256 = 0xF9;         ///< BLE 控制命令：SHA-256 哈希验证
inline constexpr uint8_t BLE_OTA_CTRL_RESTART_ESP32 = 0xFA;         ///< BLE 控制命令：重启 ESP32
inline constexpr uint8_t BLE_OTA_CTRL_ABORT_OTA     = 0xFB;         ///< BLE 控制命令：中止当前 OTA（不断开 BLE）
inline constexpr uint8_t BLE_OTA_CMD_GD32_DATA  = 0x00;             ///< BLE 数据通道：GD32 固件包标识
inline constexpr uint8_t BLE_OTA_CMD_ESP32_DATA = 0x01;             ///< BLE 数据通道：ESP32 固件包标识
inline constexpr size_t      BLE_OTA_CHUNK_PAYLOAD = 480;            ///< BLE 单包数据载荷大小（字节）
inline constexpr size_t      BLE_OTA_PACKET_OVERHEAD = 11;           ///< BLE 包头+序号+校验开销（字节）
// Reorder buffer 窗口大小（吸收 BLE 窗口内乱序到达的包，须 >= BLE_OTA_ACK_WINDOW）
inline constexpr size_t      BLE_OTA_REORDER_WINDOW = 32;

// ============================================================
// 安全常量（需与 GD32 固件团队确认）
// ⚠️ 请填入实际密钥
// ============================================================
inline constexpr uint8_t MASTER_KEY[] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
};

// SCRAMBLE_CONST 已移至 Protocol 类，运行时由硬件 RNG 随机生成
inline constexpr size_t SCRAMBLE_CONST_LEN = 16;

// GD32_CHIP_UID 仅保留为默认占位，运行时以握手包解出的芯片地址 UID 为准。
inline constexpr uint32_t DEVICE_SERIAL_ADDR_LSB = 0x1FFFF7E8U; ///< GD32 设备序列号低字 Flash 地址
inline constexpr uint32_t DEVICE_SERIAL_ADDR_MID = 0x1FFFF7ECU; ///< GD32 设备序列号中字 Flash 地址
inline constexpr uint32_t DEVICE_SERIAL_ADDR_MSB = 0x1FFFF7F0U; ///< GD32 设备序列号高字 Flash 地址

inline constexpr uint8_t GD32_CHIP_UID[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B
};
