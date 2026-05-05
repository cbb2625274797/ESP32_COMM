# ESP32-C3 OTA Relay

ESP32-C3FH4 OTA 中继固件，通过 WiFi 接收上位机 OTA 固件包，通过 SPI 转发给下位机（GD32）。

## 硬件平台

- **芯片:** ESP32-C3FH4 (RISC-V, 单核)
- **Flash:** 4MB
- **编译工具链:** ESP-IDF v6.0, riscv32-esp-elf

## 功能模块

| 模块 | 描述 |
|------|------|
| `health_monitor` | 温度 + WiFi RSSI 监控 |
| `ota_relay` | OTA 数据包接收与转发 |
| `wifi_ota` | WiFi OTA HTTP 服务器 + CAN 帧转发 |
| `spi_master` | SPI 主站，与 GD32 通信 |
| `protocol` | CRC16 校验 + AES-128-CBC 解密（硬件加速） |

## 目录结构

```
ESP32_COMM/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── app_config.h      # WiFi / OTA 参数配置
│   ├── core/main.cpp         # 任务创建入口
│   ├── relay/ota_relay.cpp   # OTA 中继逻辑
│   ├── transport/
│   │   ├── spi_master.cpp    # SPI 主站 + CANFD 3通道发送
│   │   └── protocol.cpp      # 协议层 (CRC16 / AES-128-CBC)
│   ├── server/
│   │   └── wifi_ota.cpp      # WiFi OTA HTTP 服务器
│   └── monitor/
│       ├── health_monitor.cpp  # 健康监控
│       └── debug_log.cpp       # UDP 调试日志
└── tools/
    └── ota_host_web.html     # 上位机 WebUI（CAN 发送 + OTA 上传）
```

## WiFi 工作模式

支持 AP / STA 两种模式，通过 `app_config.h` 配置：

```c
// AP 模式（ESP32 当热点）
CONFIG_WIFI_MODE_IS_STA = 0
WIFI_AP_SSID = "ESP32_OTA_RELAY"   // 默认热点名称
WIFI_AP_PASSWORD = "12345678"      // 默认密码
// 上位机连接热点后访问: http://192.168.31.1

// STA 模式（连接路由器）
CONFIG_WIFI_MODE_IS_STA = 1
WIFI_STA_SSID = "004舰桥"          // 路由器名称
WIFI_STA_PASSWORD = "q13362875622"  // 路由器密码
// 上位机和 ESP32 在同一局域网，通过 DHCP 分配的 IP 访问
```

## CANFD 3通道 SPI 协议

GD32 通信帧格式：`[通道编号: 1B] [扩展帧ID: 4B BE] [数据: 1~64B]`

| 通道 | 用途 |
|------|------|
| CH0 | 主通信通道 |
| CH1 | 备用通道 1 |
| CH2 | 备用通道 2 |

### GD32 命令 ID 定义

| 域 | 命令 | ID |
|---|---|---|
| 系统 | PING / RESET / GET_VERSION / GET_STATUS / SET_STATUS | 0x0001~0x0005 |
| Flash | ERASE / WRITE / READ / VERIFY | 0x1001~0x1004 |
| OTA | START / DATA / END / ABORT | 0x2001~0x2004 |
| 传感器 | READ / CONFIG | 0x3001~0x3002 |
| Bootloader | ENTER / JUMP_APP | 0xF001~0xF002 |

## 上位机 WebUI

`tools/ota_host_web.html` — 浏览器打开即可使用，无需安装。

### HTTP API

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/ota/upload` | 上传固件 (.bin) |
| POST | `/ota/relay` | 触发 OTA 中继 |
| GET | `/ota/status` | 查询 OTA 状态 |
| POST | `/can/send` | 发送 CAN 帧到 GD32 |
| GET | `/can/status` | CAN 通道状态 |
| GET | `/health` | ESP32 健康状态 |
| GET | `/wifi/status` | WiFi 连接状态 |

### CAN 帧发送示例

```bash
curl -X POST http://192.168.31.1/can/send \
  -H "Content-Type: application/json" \
  -d '{"ch": 0, "id": 12345678, "dlc": 8, "data": "0102030405060708"}'
```

## 任务配置 (单核 ESP32-C3)

所有任务固定运行在 CPU 核 0：

| 任务 | 优先级 | 栈大小 | 说明 |
|------|--------|--------|------|
| health_mon | configMAX_PRIORITIES - 4 | 4096 | 健康监控 |
| ota_relay | configMAX_PRIORITIES - 2 | 8192 | OTA 中继 |
| wifi_ota | configMAX_PRIORITIES - 3 | 8192 | WiFi OTA |

## 调试日志与上位机通信

当前与上位机相关的通信分成两类：

1. **业务通信：HTTP**
   - WebUI 通过 HTTP 接口与 ESP32 通信
   - 典型接口：`/system/status`、`/logs`、`/can/send`、`/ota/*`
   - 其中日志面板实际通过 **轮询 `GET /logs`** 获取日志

2. **调试镜像：UART0 + UDP**
   - 所有 `DEBUG_*` 日志会同时写到 UART0、UDP、以及内部环形缓冲区
   - UDP 目标端口为 `6666`，适合独立调试接收器使用
   - 但 **当前 tools WebUI 不直接监听 UDP 6666**

因此，当前 WebUI 场景下：
- `/system/status` 用于在线状态/健康状态展示
- `/logs` 用于日志展示
- 不再需要额外的 `{"t":"hb"}` 假日志心跳包

## 编译

```bash
idf.py set-target esp32c3
idf.py fullclean
idf.py build
idf.py flash monitor
```

## 待确认参数

- AES-128 主密钥 (MASTER_KEY_HERE)
- 搅伴算法种子 (SCRAMBLE_CONST_HERE)
- 芯片唯一码 (CHIP_UID_HERE)
- OTA Service UUID (需与 APP 侧协商)