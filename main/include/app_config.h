/*
 * app_config.h - 统一配置文件（C++ 版本）
 * ESP32-C3 OTA 中继项目
 *
 * 使用 constexpr 代替 #define，提供类型安全
 * 版本号和编译时间在 version.h 中手动维护
 */
#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "version.h"
#include "build_info.h"

// ============================================================
// 功能开关（ESP32 本机；GD32 侧开关随 HELLO/PING 身份块上报，无需镜像）
// ============================================================
// 0 = AP 热点，1 = STA 连路由器
inline constexpr uint8_t CONFIG_WIFI_MODE = 1;

// ============================================================
// 固件版本（在 version.h 中手动维护，此处提供 C++ constexpr 访问）
// ============================================================
inline constexpr const char *FW_VERSION = FW_VERSION_STRING;     ///< 固件版本字符串（手动维护）
/* FW_BUILD_TIME 已删：手写死值停在旧时间，刷新固件仍显示 20:25 被误判回退。
 * 改用 fwBuildTime()（build_info.h，构建系统每次自动生成真实编译时间）。 */
inline constexpr const char *FW_GIT_HASH = GIT_HASH;             ///< Git 提交哈希（手动维护）

// ============================================================
// WiFi STA 模式配置（连接路由器）
// ============================================================
inline constexpr const char* WIFI_STA_SSID = "004舰桥";            ///< STA 模式目标 WiFi 名称
inline constexpr const char* WIFI_STA_PASSWORD = "q13362875622";   ///< STA 模式 WiFi 密码
inline constexpr uint32_t WIFI_STA_CONNECT_TIMEOUT_MS = 30000;     ///< STA 连接超时（毫秒）
inline constexpr uint8_t  WIFI_STA_MAX_RETRY = 5;  // 最大重连次数，超过后切换 AP 模式

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

// ============================================================
// 任务栈大小（字节）— 2026-07 按板端 task_stack 水位收敛
// 规则：S ≈ peak_used + 1.5～2KB；偏松的收、偏紧的不砍。
// 板测峰值量级（常规/OTA）：
//   http~5.2KB, spi~1.9KB, wifi_ota~1.2KB, ble_ota~0.9KB,
//   ble_worker 1004B, health/cpu~1.8KB, ota_relay 轻载~1.3KB。
// 2026-07 余量优化：收 http/spi/wifi/ble* 偏松项，health/cpu/relay 保持。
// ============================================================
inline constexpr uint32_t HTTP_SERVER_STACK_SIZE   = 7168;  ///< httpd：peak~5.2KB + ~2KB → 7KB（原 8/10KB）
inline constexpr uint32_t TASK_STACK_WIFI_OTA      = 3072;  ///< wifi_ota：peak~1.2KB + ~1.8KB → 3KB（原 4/8KB）
inline constexpr uint32_t TASK_STACK_OTA_RELAY     = 2560;  ///< ota_relay：2.5KB（轻载 peak~1.3KB；转发突发不砍）
inline constexpr uint32_t TASK_STACK_SPI_WORKER    = 4096;  ///< spi_worker：peak~1.9KB + ~2KB → 4KB（原 5KB）
inline constexpr uint32_t TASK_STACK_BLE_OTA       = 2560;  ///< ble_ota：peak~0.9KB + ~1.5KB → 2.5KB（原 3KB）
inline constexpr uint32_t TASK_STACK_BLE_WORKER    = 2560;  ///< ble_ota_worker：peak 1004B + ~1.5KB → 2.5KB（原 3KB）
inline constexpr uint32_t TASK_STACK_HEALTH_MON    = 3072;  ///< health_mon：peak~1.8KB，余量~1.2KB，不砍
inline constexpr uint32_t TASK_STACK_CPU_STATS     = 3072;  ///< cpu_stats：peak~1.8KB，余量~1.2KB，不砍
inline constexpr uint32_t TASK_STACK_WIFI_RECONNECT = 2560; ///< wifi_reconnect：轻载，2.5KB 保持
inline constexpr uint32_t TASK_STACK_CAN_FB        = 3072;  ///< can_fb：被动 takeEvent(0xC1)
inline constexpr uint32_t TASK_STACK_BLE_FINISH    = 4096;  ///< 按需：SHA finalize（保持）
inline constexpr uint32_t TASK_STACK_BLE_GD32_WAIT = 3072;  ///< 按需：GD32 中继等待
inline constexpr uint32_t TASK_STACK_BLE_RESTART   = 2048;  ///< 按需：延时重启

// ============================================================
// OTA 配置
// ============================================================
inline constexpr size_t  OTA_BLOCK_SIZE         = 4096;            ///< 上传接收块：4KB（吞吐优先）；会话内 malloc，稳态不占 .bss
// OTA_BLOCK_SIZE 演进：8KB→2KB(压堆)→4KB(试吞吐)→2KB(2026-07 回退)→4KB(2026-07-20 再提吞吐)。
// 2026-09：s_ota_upload_recv 从常驻静态改为 OTA 会话内 RAII malloc，稳态省 4KB。
inline constexpr size_t  OTA_MAX_FIRMWARE_SIZE  = 216 * 1024;  // 216KB = 221184B = 0x35F90，固件大小上限
inline constexpr size_t  GD32_OTA_PACKET_SIZE   = 48;          // GD32 OTA 实际转发包载荷

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
// GD32 ADC/CAN 等开关：看 HELLO_ACK/PING 身份块 feat 位（GD32 config 顶部）
// ============================================================
inline constexpr const char* DEBUG_LOG_IP   = "192.168.31.3";      ///< UDP 日志目标 IP
inline constexpr uint16_t DEBUG_LOG_PORT   = 6666;                 ///< UDP 日志目标端口

// ============================================================
// SPI 主机（ESP32 ↔ GD32）可调参数
// 协议帧格式见 spi_link_v2.h；引脚与超时集中在此便于板级调整
// ============================================================
inline constexpr int      SPI_MASTER_CS_GPIO     = 4;     ///< CS 片选 GPIO
inline constexpr int      SPI_READY_GPIO         = 10;    ///< DATA_READY 通知线 GPIO（↔ GD32 PB6）
inline constexpr int      SPI_MODE               = 0;     ///< CPOL=0 CPHA=0
inline constexpr int      SPI_QUEUE_SIZE         = 4;     ///< SPI 事务队列深度
inline constexpr size_t   SPI_OTA_FRAME_MAX      = 256;   ///< 单 CS 最大帧长（APP 满缓冲）
inline constexpr size_t   SPI_BOOT_TX_BUF_SIZE   = 128;   ///< BOOT TX DMA 满缓冲（读预算用）
inline constexpr size_t   SPI_APP_TX_BUF_SIZE    = 256;   ///< APP TX DMA 满缓冲（读预算用）

// ============================================================
// OTA 协调器
// ============================================================
inline constexpr uint32_t OTA_COORD_LOCK_TIMEOUT_MS   = 100;  ///< 协调器 mutex 超时（ms）
/** 忙状态自愈：须 > 中继转发预算上限（约 120s）+ 握手/SIZE 余量。 */
inline constexpr uint32_t OTA_COORD_BUSY_WATCHDOG_MS  = 150000;

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
// 滑动 ACK 窗口（须与前端 tools/js/ble/constants.js BLE_OTA_ACK_WINDOW 保持一致）
// ★ 64：实测 Windows WebBT 窗口内单包间隔 0.4-2.9ms（2M PHY 多包/事件），
//        瓶颈在每窗口的 ACK 往返。窗口 32→64 提速 ~10%（40→44 KB/s），再试 96 无收益
//        （ACK 往返在 64 已摊薄，稳态瓶颈是 Windows GATT 每包 ~10.6ms resolve 延迟）。
//        内存：64×480B ≈ 30KB reorder buffer，分配失败有 fallback 丢弃策略兜底。
inline constexpr size_t      BLE_OTA_ACK_WINDOW = 64;
// Reorder buffer 窗口大小（吸收 BLE 窗口内乱序到达的包，须 >= BLE_OTA_ACK_WINDOW）
inline constexpr size_t      BLE_OTA_REORDER_WINDOW = 64;
// 数据队列深度：略大于窗口，seq=0 预擦期间多扛几批
inline constexpr size_t      BLE_OTA_DATA_QUEUE_LEN = (BLE_OTA_ACK_WINDOW * 2 + 16);

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

inline constexpr uint32_t DEVICE_SERIAL_ADDR_LSB = 0x1FFFF7E8U; ///< GD32 设备序列号低字 Flash 地址
inline constexpr uint32_t DEVICE_SERIAL_ADDR_MID = 0x1FFFF7ECU; ///< GD32 设备序列号中字 Flash 地址
inline constexpr uint32_t DEVICE_SERIAL_ADDR_MSB = 0x1FFFF7F0U; ///< GD32 设备序列号高字 Flash 地址
