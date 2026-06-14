# ESP32-C3 OTA Relay

**固件版本: v0.3.12** | 构建时间: 2026-06-14 | 基于 ESP-IDF v6.0

## 项目概述

ESP32-C3FH4 OTA 中继固件，作为 **GD32E508VET6** 的 OTA 升级桥梁。通过 **WiFi (HTTP)** 或 **BLE (GATT)** 从上位机（手机/PC）接收固件，存储在本地 SPI Flash 的 `gd32_ota` 分区，再通过 **SPI 总线** 经四阶段安全握手协议转发给 GD32。

```
[手机/PC 上位机] --WiFi/BLE--> [ESP32-C3FH4] --SPI--> [GD32E508VET6]
                                  |
                                  +-- gd32_ota 分区 (252KB)
                                  +-- 健康自检 (温度 + WiFi RSSI)
                                  +-- 本机 OTA (ESP32 自升级)
```

## 硬件平台

| 项目 | 规格 |
|------|------|
| **芯片** | ESP32-C3FH4 (RISC-V, 单核, 160MHz) |
| **Flash** | 4MB |
| **SPI 引脚** | MOSI=GPIO5, MISO=GPIO7, SCLK=GPIO6, CS=GPIO4 |
| **SPI 速率** | 4MHz, Mode 0 |
| **UART 调试** | TX=GPIO21, RX=GPIO20, 115200 baud |
| **编译工具链** | ESP-IDF v6.0, riscv32-esp-elf |

### 分区表 (`config/partitions.csv`)

| 分区 | 类型 | 偏移 | 大小 | 用途 |
|------|------|------|------|------|
| nvs | data/nvs | 0x9000 | 24KB | NVS 存储 |
| otadata | data/ota | 0xF000 | 8KB | OTA 启动信息 |
| gd32_ota | data/spiffs | 0x11000 | 252KB | GD32 固件缓存 |
| app0 | app/ota_0 | 0x50000 | 1.25MB | ESP32 OTA 槽位 0 |
| app1 | app/ota_1 | 0x190000 | 1.25MB | ESP32 OTA 槽位 1 |
| spiffs | data/spiffs | 0x2D0000 | 1.06MB | 文件系统 |

## 功能模块

| 模块 | 文件 | 描述 |
|------|------|------|
| **core** | `core/main.cpp` | 程序入口，初始化各模块并创建 4 个 FreeRTOS 任务 |
| **health_monitor** | `monitor/health_monitor.{h,cpp}` | 温度 + WiFi RSSI 监控，OTA 前健康检查 |
| **debug_log** | `monitor/debug_log.{h,cpp}` | 三路日志输出：UART0 + UDP + 环形缓冲区 |
| **ota_relay** | `relay/ota_relay.{h,cpp}` | GD32 OTA 中继核心状态机（四阶段 15 状态） |
| **esp32_ota_core** | `ota/esp32_ota_core.{h,cpp}` | ESP32 本机 OTA 核心（begin/write/finalize/abort，WiFi/BLE 共用） |
| **wifi_ota** | `server/wifi_ota.{h,cpp}` | WiFi AP/STA + HTTP OTA 服务器 + CAN 帧转发 |
| **ble_ota** | `server/ble_ota.{h,cpp}` | BLE GATT 服务器（NimBLE 协议栈） |
| **spi_master** | `transport/spi_master.{h,cpp}` | SPI 主站驱动 + CANFD 3 通道 + OTA 帧协议 |
| **protocol** | `transport/protocol.{h,cpp}` | 协议层（nonce 搅伴生成、AES 会话密钥管理） |

## 目录结构

```
ESP32_COMM/
├── CMakeLists.txt                    # 顶层 CMake 项目定义
├── sdkconfig.defaults                # SDK 配置默认值 (WiFi RX/TX BA=6)
├── config/
│   └── partitions.csv                # 4MB Flash 分区表
├── cmake/
│   └── gen_version_script.cmake      # 构建时自动生成 version.h
├── main/
│   ├── CMakeLists.txt                # 注册 11 个源文件 + 依赖
│   ├── include/
│   │   ├── app_config.h              # 统一配置 (constexpr: WiFi/BLE/OTA/阈值/密钥)
│   │   ├── version.h                 # 自动生成版本信息 (v0.3.12)
│   │   └── ota_mutex.h               # BLE/WiFi OTA 全局互斥
│   ├── core/main.cpp                 # app_main 入口，4 个 FreeRTOS 任务
│   ├── relay/ota_relay.{h,cpp}       # GD32 OTA 中继状态机
│   ├── ota/esp32_ota_core.{h,cpp}    # ESP32 本机 OTA 核心
│   ├── transport/
│   │   ├── spi_master.{h,cpp}        # SPI 主站 + CANFD 3 通道
│   │   └── protocol.{h,cpp}          # 协议层 (CRC16 / AES / nonce)
│   ├── server/
│   │   ├── wifi_ota.{h,cpp}          # WiFi OTA HTTP 服务器
│   │   └── ble_ota.{h,cpp}           # BLE GATT OTA 服务器
│   └── monitor/
│       ├── health_monitor.{h,cpp}    # 温度 + RSSI 健康监控
│       └── debug_log.{h,cpp}         # UART0 + UDP + 环形缓冲区日志
├── components/
│   └── cjson/                        # cJSON 组件
├── managed_components/
│   └── espressif__mdns/              # mDNS 组件
├── tools/
│   ├── index.html                    # WiFi OTA WebUI 控制台
│   ├── ble_ota.html                  # BLE OTA WebUI 控制台 (Web Bluetooth)
│   ├── app.js                        # WebUI JavaScript
│   ├── style.css                     # WebUI 样式
│   ├── serve.bat                     # Python HTTP 服务器启动脚本 (端口 8080)
│   ├── serial_monitor.py             # 串口日志采集工具 (COM15)
│   └── ota_upload_test.py            # OTA 上传测试脚本
├── docs/
│   ├── esp32_ota_relay_design.md     # 架构设计文档
│   ├── DEVELOPMENT_GUIDE.md          # 开发者指南 (T0-T8)
│   └── COMMUNICATION_AUDIT_2026-05-05.md  # 通信审计
├── .vscode/                          # VS Code 调试/编译配置 (COM15, clangd)
└── .devcontainer/                    # Docker 开发容器 (ESP-IDF 6.0)
```

## FreeRTOS 任务

4 个主任务全部固定运行在 ESP32-C3 单核（核 0），通过 FreeRTOS 队列通信：

| 任务名 | 函数 | 优先级 | 栈大小 | 职责 |
|--------|------|--------|--------|------|
| `health_mon` | `HealthMonitor::taskEntry` | configMAX_PRIORITIES - 4 | 4096 | 温度监控 (1s 定时器) + 堆巡检 (5s) + 健康状态管理 |
| `wifi_ota` | `WifiOta::taskEntry` | configMAX_PRIORITIES - 3 | 8192 | WiFi (AP/STA) + HTTP 服务器 (端口 80) + 状态轮询 |
| `ble_ota` | `BleOta::taskEntry` | configMAX_PRIORITIES - 3 | 8192 | BLE GATT 服务器初始化 + NimBLE Host 任务 + 心跳保活 |
| `ota_relay` | `OtaRelay::taskEntry` | configMAX_PRIORITIES - 2 | 12288 | 等待调度信号 → 四阶段状态机 → SPI 转发 |

额外后台任务：

| 任务名 | 优先级 | 栈大小 | 职责 |
|--------|--------|--------|------|
| `wifi_reconnect` | 5 | 4096 | STA 断线重连（退避算法，独立任务避免死锁）|
| `ble_ota_finish` | 5 | 4096 | BLE ESP32 OTA 异步收尾（SHA 校验 + end + reboot）|
| `nimble_host` | NimBLE 内部 | NimBLE 内部 | BLE 协议栈事件循环 |

### 跨任务通信

- **OTA 调度**: WiFi/BLE 任务通过 `OtaRelay::dispatchQueue` (FreeRTOS 队列, 深度 4) 发送调度信号给 `ota_relay` 任务；结果通过 `resultQueue` 回传
- **OTA 互斥**: `g_esp32OtaMutex` (全局 Mutex) + `g_esp32OtaOccupied` 标志，防止 BLE 和 WiFi 同时操作 ESP32 OTA 分区
- **中继状态**: `OtaRelay::s_stateMutex` 保护 relayState/relaySeq/relayTotalPackets 等跨任务状态（`StateLockGuard` RAII 封装）
- **WiFi 状态**: `WifiOta::m_stateMutex` + `std::atomic` 保护 WiFi 连接状态和重连计数器
- **健康状态**: `HealthMonitor::m_mutex` 保护 healthState/temperature/RSSI
- **UDP 日志**: `DebugLog::m_logRingMutex` 保护 HTTP `/logs` 环形缓冲区

## OTA 核心流程

### 一、GD32 OTA 中继（四阶段 + APP 验证）

上位机上传 GD32 固件到 ESP32 本地 `gd32_ota` 分区 → 收到触发命令后启动状态机：

```
阶段1 ─ 安全握手 (5 状态)
  RELAY_HANDSHAKE_SENT → RELAY_CHALLENGE_SENT → RELAY_CHALLENGE_RESPOND
  → RELAY_SESSION_KEY_SENT → RELAY_AUTH_COMPLETE

阶段2 ─ 固件大小协商 (2 状态)
  RELAY_SIZE_SENT → RELAY_SIZE_ACK

阶段3 ─ 数据包转发 (4 状态)
  RELAY_RELAY_ACTIVE → RELAY_PACKET_SENT → RELAY_PACKET_ACK_OK
  [重试] RELAY_PACKET_ACK_RETRY

阶段4 ─ 完成与跳转 (3 状态)
  RELAY_RECEIVE_DONE → RELAY_FINISH_SENT → RELAY_DONE

阶段5 ─ APP 存活验证
  RELAY_APP_VERIFY (PING GD32 APP, 最多 30 次 × 500ms)
```

#### 安全握手协议

1. **握手请求**: ESP32 发送 `0x00 0xAC`（最多重试 10 次，200ms 超时）
2. **Challenge**: GD32 回复 36B 数据包（12B UID + 8B nonce 片段 + 16B 加密会话密钥）
3. **验证**: ESP32 验证 nonce 匹配，用 MasterKey 派生 `K_session = AES(MasterKey, nonce_server)` 并比对
4. **确认**: ESP32 发送 `nonce_server ^ 0x55` 经 `K_session` AES-ECB 加密
5. **完成**: GD32 回复 `0xBA 0xC0`，建立 AES 加密通道

#### 数据包转发

- 每包 48 字节固件载荷，使用 `K_session` AES-ECB 加密
- 包结构: `session(4B) + seq(4B) + encrypted48(48B) + if_end(4B) + crc32(4B)` = 64B
- 每 32 包执行一次 OTA 健康检查（温度/堆）
- 支持重试（单包最多 10 次），结构化 ACK 含 seq 校验，残留 ACK 自动过滤
- 全局超时: 动态计算 5~10 分钟

### 二、ESP32 本机 OTA（WiFi + BLE 共用）

核心逻辑封装在 `Esp32OtaCore` 单例中，WiFi 和 BLE 通道共用：

| 步骤 | 方法 | 说明 |
|------|------|------|
| 1 | `begin(total_size)` | 选择非当前 OTA 分区，获取全局互斥，`esp_ota_begin` |
| 2 | `write(data, len)` | 循环调用 `esp_ota_write`（WiFi 通过 HTTP 流，BLE 通过 GATT 块） |
| 3 | `finalize(sha_hex, reboot)` | `esp_ota_end` → 从分区读回计算 SHA-256 → 比对前端 SHA → `esp_ota_set_boot_partition` → 可选重启 |
| - | `abort()` | 中止并清理（释放 mutex + 复位状态） |

**SHA-256 校验**: 固件写入完成后从 OTA 分区读回计算，不做增量 SHA。前端与设备端比对一致才 set_boot，否则返回 `ESP_ERR_INVALID_VERSION`。

**回滚保护**: `esp_ota_mark_app_valid_cancel_rollback()` 在启动时标记当前固件有效。启用 `CONFIG_APP_ROLLBACK_ENABLE` 后，新固件崩溃自动回退。

## 通信协议

### SPI OTA 帧格式（底层传输）

`esp32_ota_core.cpp` 中的 `otaBuildFrame` / `otaParseFrame`：

```
| SOF(0xA5, 1B) | TYPE(1B) | LEN(2B LE) | PAYLOAD(LEN) | CRC8(1B) |
```

| TYPE | 名称 | 用途 |
|------|------|------|
| 0x01 | OTA_CMD_REQ | OTA 请求帧 |
| 0x02 | OTA_CMD_DATA | OTA 数据帧 |
| 0x03 | OTA_CMD_CANCEL | 取消 OTA |
| 0xB0 | APP_ENTER_BOOT_CMD | 通知 GD32 APP 进入 Bootloader |
| 0x80 | RSP_ACK | 响应：数据就绪 |
| 0x81 | RSP_WAIT | 响应：GD32 忙，请等待 |
| 0x82 | RSP_ERROR | 响应：错误 |

### CANFD 3 通道 SPI 协议

ESP32 通过 SPI 转发 CAN 帧到 GD32（GD32 端驱动外部 CAN 收发器）：

```
| 通道编号(1B) | 扩展帧ID(4B BE) | 数据(1~64B) |
```

| 通道 | 用途 |
|------|------|
| CH0 | 主通信通道 |
| CH1 | 备用通道 |
| CH2 | 备用通道 |

### GD32 命令 ID 定义

| 域 | 命令 | ID |
|------|------|------|
| 系统 | PING / RESET / GET_VERSION / GET_STATUS / SET_STATUS | 0x0001~0x0005 |
| Flash | ERASE / WRITE / READ / VERIFY | 0x1001~0x1004 |
| OTA | START / DATA / END / ABORT | 0x2001~0x2004 |
| 传感器 | READ / CONFIG | 0x3001~0x3002 |
| Bootloader | ENTER / JUMP_APP | 0xF001~0xF002 |

## WiFi 工作模式

支持 AP/STA 双模式，启动时优先 STA，超时自动回退 AP。

### AP 模式（设备当热点）

| 参数 | 值 |
|------|------|
| SSID | `ESP32_OTA_RELAY` |
| 密码 | `12345678` |
| IP | 192.168.31.1 |
| 信道 | 6 |
| 最大客户端 | 4 |

### STA 模式（连接路由器）

| 参数 | 值 |
|------|------|
| SSID | `004舰桥` |
| 连接超时 | 30 秒 |
| 最大重试 | 5 次（超限后自动切换 AP 模式） |
| IP 分配 | DHCP（可切换静态 IP 192.168.31.32）|
| 国家码 | CN（允许信道 1-13）|

**冷启动优化**: 复位原因为 POWERON/BROWNOUT 时，清除 WiFi NVS 缓存 + PHY 校准数据，增加 500ms 启动延迟，防止长时间断电后校准残留导致连接失败。

### mDNS 发现

设备在局域网内通过 `esp32-ota-relay.local` 可发现（需 `managed_components/espressif__mdns` 组件）。

## BLE GATT 服务

基于 **NimBLE** 协议栈，设备名 `ESP32-OTA-RELAY`，ATT MTU 512 字节，支持 GD32 和 ESP32 两种 OTA 目标。

| UUID | 特征 | 属性 | 用途 |
|------|------|------|------|
| 0xFFFF | Service | - | OTA GATT 服务 |
| 0x0001 | CTRL | Write | 控制命令：`0xF7`=触发GD32中继, `0xF8+32B SHA`=ESP32 OTA, `0xFA`=重启ESP32 |
| 0x0002 | DATA_IN | Write | 固件数据块：`cmd(1)+seq(2)+total_size(4)+payload` |
| 0x0003 | DATA_OUT | Notify | 推送：ACK(窗口确认, 0xA1+8B) / FINISH(阶段事件, 0xB1+6B) / SHA256(0xC1+33B) |
| 0x0004 | VERSION | Read | 固件版本字符串 |

### BLE 数据传输

- **ACK 窗口**: 每 32 包等待一次 DATA_OUT ACK（带累计字节数验证）
- **窗口重传**: ACK 超时或字节数不足时重发整个窗口（最多 3 次）
- **ESP32 收尾**: BLE 传输完成后由前端发 `0xF8 + 32B 文件 SHA-256`，设备端创建异步任务执行 finalize（含 SHA 比对），结果通过 FINISH 事件通知前端
- **GD32 触发**: 传输完成后自动发 `0xF7` 触发 SPI 中继
- **合并包**: NimBLE 可能将多个 writeWithoutResponse 合并到同一 mbuf，接收端支持一次解析多个数据包

## HTTP API (WiFi WebUI)

| 方法 | 端点 | 功能 | 请求体 | 响应 |
|------|------|------|--------|------|
| POST | `/ota/upload` | 上传固件 | JSON 头 + binary 数据 | `{"status":"ok", "target":"esp32"}` |
| POST | `/ota/update` | 完成 ESP32 OTA | `{"sha256":"...","verify_only":true/false}` | `{"status":"ok","sha256":"..."}` |
| POST | `/ota/relay` | 触发 GD32 中继 | - | `{"status":"ok"}` |
| GET | `/system/status` | 系统状态 JSON | - | 含版本/WiFi/OTA/温度/堆/GD32 版本等 |
| GET | `/logs` | 拉取调试日志 | - | JSON 数组字符串 |
| POST | `/can/send` | 发送 CAN 帧 | `{"ch":0,"id":123,"dlc":8,"data":"0102..."}` | `{"status":"ok","recv":"..."}` |
| POST | `/spi/send` | SPI 直接发送 | `{"type":"01","data":"00AC","timeout_ms":1000}` | `{"status":"ok","resp_type":".."}` |
| POST | `/restart` | 重启 ESP32 | - | 500ms 后 `esp_restart()` |

### POST /ota/upload 协议

通过 JSON 头中的 `type` 字段自动区分目标：

```json
// ESP32 OTA
{"type": "esp32", "total_size": 524288}
<binary firmware data...>

// GD32 OTA
{"type": "gd32", "total_size": 131072}
<binary firmware data...>
```

- **ESP32 模式**: 直接写入 OTA 分区，进度上报，完成后调用 `/ota/update` 收尾
- **GD32 模式**: 写入 `gd32_ota` 分区 → 擦除 → 计算 SHA-256 → 自动触发 SPI 中继

### curl 示例

```bash
# 查询系统状态
curl http://192.168.31.1/system/status | jq

# 发送 CAN 帧
curl -X POST http://192.168.31.1/can/send \
  -H "Content-Type: application/json" \
  -d '{"ch":0,"id":0x18DA00F1,"dlc":8,"data":"0102030405060708"}'

# SPI 直接发送
curl -X POST http://192.168.31.1/spi/send \
  -H "Content-Type: application/json" \
  -d '{"type":"01","data":"00AC","timeout_ms":1000}'

# 获取日志
curl http://192.168.31.1/logs
```

## WebUI 工具

### WiFi OTA 控制台 (`tools/index.html`)

- ESP32 固件上传与 OTA 升级（含 SHA-256 前端校验）
- GD32 固件上传（支持 .bin/.hex）与 SPI 中继触发
- 实时 OTA 流程阶段显示（6 阶段进度追踪）
- CAN 帧手动发送面板
- SPI 直接发送面板
- 系统状态监控（温度、RSSI、空闲堆、最小堆、GD32 APP/BOOT 版本、OTA 缓存）
- 调试日志面板（HTTP `/logs` 轮询，按级别过滤）
- 一键重启 ESP32

### BLE OTA 控制台 (`tools/ble_ota.html`)

- **Web Bluetooth API** 连接（需 Chrome/Edge/Android Chromium）
- GD32 与 ESP32 双目标切换
- 一键刷写（自动重连）
- 窗口 ACK 重传机制（最多 3 次重试）
- SHA-256 前端+设备端比对校验（校验面板显示）
- FINISH 阶段事件通知
- 设备重启 + 自动重连（退避 1.6x 指数，最多 12 次）

## 健康监控

### 健康状态等级

| 等级 | 名称 | 触发条件 | OTA 影响 |
|------|------|----------|----------|
| 0 | SYS_NORMAL | 正常 | 允许 |
| 1 | SYS_WARNING | 温度 ≥ 80°C 或 RSSI < -75 dBm | 允许（带警告）|
| 2 | SYS_CRITICAL | RSSI < -85 dBm | ESP32 OTA 禁止，SPI 中继允许 |
| 3 | SYS_SAFE_SHUTDOWN | 温度 ≥ 100°C | 所有 OTA 禁止 |

### 监控指标

- **温度**: 1 秒定时器读取内部温度传感器（20°C~80°C 量程）
- **WiFi RSSI**: WiFi 任务每 10 秒检查一次
- **空闲堆**: 每 5 秒巡检，低于 10KB 触发警告

### OTA 前健康检查

- `checkBeforeOta()`: 完整检查（温度 + RSSI + WiFi 连接状态）
- `checkBeforeOtaRelay()`: 仅检查温度（中继阶段不依赖 WiFi）

## 诊断日志

三条并行输出路径：

1. **UART0**: GPIO21(TX), GPIO20(RX), 115200 baud — 本地串口调试
2. **UDP 单播**: `192.168.31.3:6666` — 独立调试接收端
3. **环形缓冲区**: 64 槽 × 256 字节，互斥保护 — 供 HTTP `GET /logs` 轮询拉取

日志级别: `E`(错误) / `W`(警告) / `I`(信息) / `D`(调试)，运行时可调。

UDP socket 延迟创建（首次 `send()` 时才分配），WiFi 未连接时 `sendto()` 静默跳过。

**当前 WebUI 日志面板通过 HTTP `/logs` 轮询获取日志，不直接监听 UDP 6666。**

## 配置参数 (`main/include/app_config.h`)

| 分类 | 参数 | 默认值 | 说明 |
|------|------|--------|------|
| WiFi | `CONFIG_WIFI_MODE` | 1 (STA) | 0=AP, 1=STA |
| WiFi | `WIFI_STA_SSID` | "004舰桥" | 路由器名称 |
| WiFi | `WIFI_AP_SSID` | "ESP32_OTA_RELAY" | 热点名称 |
| OTA | `OTA_BLOCK_SIZE` | 8192 | 读写块大小 |
| OTA | `OTA_MAX_FIRMWARE_SIZE` | 216KB | GD32 固件上限 |
| OTA | `GD32_OTA_PACKET_SIZE` | 48 | 每包载荷字节 |
| OTA | `OTA_RESEND_RETRIES` | 10 | 单包最大重发次数 |
| OTA | `OTA_PACKET_HEALTH_CHECK_INTERVAL` | 32 | 每 N 包健康检查 |
| 阈值 | `TEMP_WARNING` | 80°C | 温度警告阈值 |
| 阈值 | `TEMP_CRITICAL` | 100°C | 温度危险阈值 |
| 阈值 | `WIFI_RSSI_CRITICAL` | -85 dBm | RSSI 危险阈值 |
| 调试 | `DEBUG_LOG_IP` | "192.168.31.3" | UDP 日志目标 IP |
| 调试 | `DEBUG_LOG_PORT` | 6666 | UDP 日志端口 |
| BLE | `BLE_DEVICE_NAME` | "ESP32-OTA-RELAY" | BLE 广播名称 |
| BLE | `BLE_MTU_SIZE` | 512 | ATT MTU |
| BLE | `BLE_OTA_ACK_WINDOW` | 32 | ACK 窗口大小 |
| BLE | `BLE_OTA_CHUNK_PAYLOAD` | 480 | BLE 单包载荷 |

## 安全配置

| 参数 | 位置 | 说明 | 状态 |
|------|------|------|------|
| `MASTER_KEY[16]` | app_config.h:110 | AES-128 主密钥 | ⚠️ 占位值，生产环境需更换 |
| `SCRAMBLE_CONST[16]` | app_config.h:115 | Nonce 搅伴常量 | ⚠️ 占位值，生产环境需更换 |
| `GD32_CHIP_UID[12]` | app_config.h:125 | 运行时从握手包获取 | 仅保留默认占位 |
| `K_session` | 运行时派生 | `AES(MasterKey, nonce_server)` | 每次 OTA 重新生成 |
| SHA-256 | 最终校验 | 从分区读回计算，前端比对 | WiFi/BLE 均已实现 |

## 构建与部署

### 一、本地编译（Windows PowerShell + ESP-IDF v6.0）

首次编译前需设置工具链环境变量（PowerShell）：

```powershell
# ── 设置 ESP-IDF v6.0 工具链环境 ──
$env:IDF_VERSION = "6.0"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.0\venv"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PATH = "F:\.espressif\v6.0\esp-idf"
$env:ESP_ROM_ELF_DIR = "C:\Espressif\tools\esp-rom-elfs\20241011"

# ── 添加工具链到 PATH ──
$env:PATH = "C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;" +
            "C:\Espressif\tools\cmake\4.0.3\bin;" +
            "C:\Espressif\tools\esp-clang\esp-20.1.1_20250829\esp-clang\bin;" +
            "C:\Espressif\tools\idf-exe\1.0.3\;" +
            "C:\Espressif\tools\ninja\1.12.1\;" +
            "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;" +
            "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\riscv32-esp-elf\bin;" +
            "C:\Espressif\tools\python\v6.0\venv\Scripts;" + $env:PATH
```

编译命令：

```powershell
# ── 进入项目目录 ──
Set-Location "G:\Robot_v2\DCU\ESP32_COMM"

# ── 全量编译（自动生成 version.h，确认零 error/warning）──
& "C:\Espressif\tools\python\v6.0\venv\Scripts\python.exe" `
  "F:\.espressif\v6.0\esp-idf\tools\idf.py" build

# ── 目标文件 ──
# build/ESP32_COMM.bin       ← OTA 烧录用
# build/bootloader/bootloader.bin
# build/partition_table/partition-table.bin
```

### 二、烧录方式

#### 方式 A：串口烧录（首次 / 恢复）

连接 ESP32-C3 到串口（默认 COM15），直接在 PowerShell 中烧录：

```powershell
# 全量烧录（固件 + bootloader + 分区表）
& "F:\.espressif\v6.0\esp-idf\tools\idf.py" -p COM15 flash

# 仅烧录应用程序（分区不变时快速更新）
& "F:\.espressif\v6.0\esp-idf\tools\idf.py" -p COM15 app-flash

# 烧录 + 监控
& "F:\.espressif\v6.0\esp-idf\tools\idf.py" -p COM15 flash monitor
```

#### 方式 B：WiFi OTA 烧录（ESP32 自升级，无需串口线）

前提：ESP32 已通过 WiFi 联网（或 AP 模式），上位机与 ESP32 在同一网络。

**步骤 1 — 启动 WebUI 服务**：

```powershell
# 在项目目录下运行
tools\serve.bat
# 浏览器打开 http://localhost:8080/tools/index.html
```

**步骤 2 — 连接设备**：

- 在页面顶部输入 ESP32 IP 地址（AP 模式为 `192.168.31.1`，STA 模式查路由器分配 IP）
- 点击 **"测试"** 确认连通，状态指示应变为绿色

**步骤 3 — 上传固件**：

- 在 **"ESP32 自身升级"** 卡片中，拖拽或点击选择 `build/ESP32_COMM.bin`
- 页面自动读取文件大小并计算 SHA-256
- 点击 **"上传并升级 ESP32"**
- WebUI 自动执行：上传 → OTA 写入 → SHA-256 校验 → 设置启动分区 → 重启
- 重启后页面连接断开，ESP32 以新固件启动（约 10~15 秒）

> ⚠️ 如果上传后 ESP32 无法正常启动（反复重启），说明固件有严重 bug，此时需用 **串口烧录** 恢复。

#### 方式 C：BLE OTA 烧录（无需 WiFi）

前提：上位机（手机/PC）支持 Web Bluetooth（Chrome / Edge / Android Chromium）。

- 浏览器打开 `tools/ble_ota.html`
- 点击 **"连接设备"**，在设备选择器中选择 `ESP32-OTA-RELAY`
- 选择固件文件，点击 **"一键刷写"**
- 通过 GATT Notify 实时查看进度和 SHA-256 校验结果

### 三、GD32 SPI OTA 中继烧录

GD32 固件通过 ESP32 中转烧录，不需要串口连接 GD32：

**步骤 1** — 在 ESP32 WebUI 的 **"GD32 SPI 中继"** 卡片中上传 GD32 固件（.bin 或 .hex）

**步骤 2** — 上传完成后，系统自动计算 SHA-256 并触发 SPI 中继转发

**步骤 3** — ESP32 通过四阶段安全握手协议将固件逐包转发给 GD32 Bootloader

**步骤 4** — 转发完成后 GD32 APP 启动，ESP32 轮询 PING 确认 APP 存活（最多 15 秒）

> 也可手动点击 **"触发 GD32 OTA 中继"** 按钮重新触发转发。

### 四、Python 辅助工具

```powershell
# 串口日志采集（COM15, 5 分钟）
python serial_monitor.py

# OTA 上传测试（HTTP）
python ota_upload_test.py <esp32_ip> <firmware.bin>
```

### 五、开发容器

`.devcontainer/` 提供 Docker 容器，含 ESP-IDF v6.0 工具链和 riscv32-esp-elf 编译器，支持 VS Code Remote-Containers 开箱即用。

## SDK 配置 (`sdkconfig.defaults`)

```ini
CONFIG_ESP_WIFI_RX_BA_WIN=6
CONFIG_ESP_WIFI_TX_BA_WIN=6
CONFIG_LWIP_MAX_SOCKETS=10
CONFIG_LWIP_TCP_WND_DEFAULT=11480
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=11520
```

## OTA 回滚保护

启用 `CONFIG_APP_ROLLBACK_ENABLE` 后：
1. 新固件首次启动必须在主循环前调用 `esp_ota_mark_app_valid_cancel_rollback()`
2. 若新固件在标记前崩溃/重启，bootloader 自动回退到上一个有效分区
3. `main.cpp:140` 在任务创建完成后执行此标记

## 固件版本管理

位于 `cmake/gen_version_script.cmake`，在每次 `idf.py build` 时自动执行：
- **版本号**: `0.3.12`（手动修改 CMake 文件中的 MAJOR/MINOR/PATCH）
- **构建时间**: 自动写入 `%Y-%m-%d %H:%M:%S`
- **Git Hash**: 自动从 `git rev-parse --short HEAD` 获取
- 输出到 `main/include/version.h`，仅在内容变化时写入以避免不必要的重编译
