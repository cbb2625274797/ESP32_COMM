# ESP32-C3FH4 OTA 中继架构设计

> **⚠️ v1 已废弃**（2026-07-14 标注）
>
> 本文档描述的是 v1 OTA 中继设计（旧帧格式 0xEB/6B/CRC16、旧引脚 GPIO6-9、旧命令 0xF0-0xF7），
> 与当前代码实现不符。当前 SPI 通信协议以 **`SPEC/spi_link_v2.md`** 为单一权威来源
> （SPI Link v2：16B 头帧、SESSION/SEQ/CREDIT、5-wire SPI、HELLO 握手）。
>
> 本文档保留作历史参考，不再维护。所有新代码与文档改动请参照 SPEC。

## 1. 系统概述

```
[手机APP/PC]  --WiFi-->  [ESP32-C3FH4]  --SPI-->  [GD32E508VET6]
                      |
                      +--BLE--->  [手机APP/PC]
```

- **ESP32 角色**：WiFi/BLE 接收端 + SPI 透传（主机）
- **GD32 角色**：OTA 执行端（从机），已有完整 OTA 流程
- **ESP32 不负责 OTA 逻辑**，只做数据转发

---

## 2. 硬件连接

### SPI 接口

| ESP32 (Master) | GD32 (Slave) | 说明 |
|---|---|---|
| GPIO6 (SPIIO0) | PA4 (NSS) | 片选 |
| GPIO7 (SPIQ) | PA5 (SCK) | 时钟 |
| GPIO8 (SPIWP) | PA6 (MOSI) | 主发从收 |
| GPIO9 (SPIHD) | PA7 (MISO) | 主收从发 |

> ⚠️ GD32E508 使用标准 SPI（PA4-NSS, PA5-SCK, PA6-MOSI, PA7-MISO）
> GD32 已内置 BOOT+APP 模式，ESP32 通过 SPI 命令直接控制进入 Bootloader，无需额外 BOOT/RST 控制引脚。

---

## 3. 通信协议设计

ESP32 与 GD32 之间的 SPI 协议完整映射 GD32 原有 CANFD OTA 流程，分为四个阶段。

### 3.0 帧格式总览

所有 SPI 帧格式统一为：

```
| SOF (1B) | CMD (1B) | LEN (2B, LE) | PAYLOAD (LEN bytes) | CRC16 (2B, LE) |
```

- **SOF**: 0xEB 固定帧头
- **CMD**: 命令码（见下表）
- **LEN**: Payload 长度（不含帧头和 CRC）
- **CRC16**: Modbus CRC16，低字节在前

> 注意：CANFD 侧（原协议）的分包为 48 bytes/packet；SPI 中继时，ESP32 接收 APP/PC 的分包可能更大，内部透传时不做分包合并，只做协议映射。

---

### 3.1 第一阶段：安全握手与认证（步骤 1-6）

目的是建立加密安全通道，防止非法刷写。

#### Step 1 & 2 — 握手发起与响应

| 方向 | Signal | 说明 |
|---|---|---|
| ESP32 → GD32 | `0x00 AC` | 发起 OTA 请求 |
| GD32 → ESP32 | `0x00 AC` | 响应信号，表示准备好进入 OTA 模式 |

**对应 SPI 帧：**

```
CMD=0xF0 (HANDSHAKE_REQ), Payload=[]       → ESP32 发送 0x00 AC
CMD=0xF0 (HANDSHAKE_REQ), Payload=[]       ← GD32 回复 0x00 AC
```

#### Step 3 — 主机挑战（发送随机数）

| 方向 | Signal | 说明 |
|---|---|---|
| ESP32 → GD32 | `nonce_server` (16 bytes) | 16字节随机数，用于生成会话密钥 |

**搅伴算法**（时间 + 内存地址 + rand → 3轮单字节 → 2轮4字节）由 ESP32 实现，生成方法需与 GD32 预置的主密钥匹配。

**对应 SPI 帧：**

```
CMD=0xF1 (CHALLENGE), Payload=[16 bytes nonce_server]
```

#### Step 4 — 从机响应与身份验证

| 方向 | Signal | 说明 |
|---|---|---|
| GD32 → ESP32 | 握手数据包 | 使用主密钥 AES 加密随机数 + 会话密钥加密自身信息 |

GD32 获取芯片唯一码，使用主密钥通过 **AES 算法**加密随机数，并使用会话密钥加密自身信息，打包发送。

**对应 SPI 帧：**

```
CMD=0xF1 (CHALLENGE), Payload=[握手数据包]  ← GD32 响应
```

Payload 结构（AES 加密后的数据）：

```
| AES_encrypt(nonce_server, master_key) (16B) | AES_encrypt(device_info, K_session) (16B) |
```

#### Step 5 — 主机验证与会话密钥分发

| 方向 | Signal | 说明 |
|---|---|---|
| ESP32 → GD32 | 变换后的随机数 | 使用会话密钥加密后发送 |

ESP32 验证随机数，解密获取会话密钥 (K_session)，对随机数进行变换后用会话密钥加密发送。

**对应 SPI 帧：**

```
CMD=0xF2 (SESSION_KEY), Payload=[AES_encrypt(transformed_nonce, K_session)]
```

#### Step 6 — 认证完成

| 方向 | Signal | 说明 |
|---|---|---|
| GD32 → ESP32 | `0xBA C0` | 安全通道建立完成 |

**对应 SPI 帧：**

```
CMD=0xF2 (SESSION_KEY), Payload=[]  ← GD32 回复 0xBA C0
```

---

### 3.2 第二阶段：传输准备（步骤 7-8）

#### Step 7 — 发送大小

| 方向 | Signal | 说明 |
|---|---|---|
| ESP32 → GD32 | `Data Size` (4 bytes, LE) | APP 总数据大小 |

**对应 SPI 帧：**

```
CMD=0xF3 (SEND_SIZE), Payload=[size(4B, LE)]
```

#### Step 8 — 确认大小

| 方向 | Signal | 说明 |
|---|---|---|
| GD32 → ESP32 | `0xBA C0` + 包序号 | 确认收到大小信息 |

**对应 SPI 帧：**

```
CMD=0xF3 (SEND_SIZE), Payload=[seq(2B)]  ← GD32 回复 0xBA C0 + seq
```

---

### 3.3 第三阶段：数据分包传输循环（步骤 9-12）

刷写核心部分，数据加密分包发送。

#### Step 9 — 发送数据包

| 方向 | Signal | 说明 |
|---|---|---|
| ESP32 → GD32 | OTA 程序数据包 | 经会话密钥加密，含 CRC 校验码 |

分包大小约 **48 bytes/packet**（与原 CANFD 协议一致）。

**对应 SPI 帧：**

```
CMD=0xF4 (DATA_PACKET), Payload=[encrypted_data(48B) + CRC]
```

#### Step 10 — 从机校验与确认

| 方向 | Signal | 说明 |
|---|---|---|
| GD32 → ESP32 | `0xBA C0` + 包序号 | 校验通过，请求下一包 |
| GD32 → ESP32 | `0xBA C0` + 错误指示 | 校验失败，要求重发当前包（限次重试） |

**对应 SPI 帧：**

```
CMD=0xF4 (DATA_PACKET), Payload=[seq(2B)]              ← 正常 ACK
CMD=0xF4 (DATA_PACKET), Payload=[seq(2B) + 0xFF]    ← 错误重发请求
```

#### Step 11 — 继续发送

主机发送下一包加密数据。

循环机制：`总循环次数 = 1 + 总APP大小 // 48 Byte`

#### Step 12 — 最后一包

| 方向 | Signal | 说明 |
|---|---|---|
| ESP32 → GD32 | 最后一包数据 | IFEND（结束标志位）置位 |

**对应 SPI 帧：**

```
CMD=0xF5 (DATA_END), Payload=[encrypted_data(N) + CRC + IFEND flag]
```

---

### 3.4 第四阶段：结束与跳转（步骤 13-17）

#### Step 13 — 传输完成确认

| 方向 | Signal | 说明 |
|---|---|---|
| GD32 → ESP32 | `0xBA C0` + `FF FF FF FF` | 所有数据包传输完成并校验通过 |

**对应 SPI 帧：**

```
CMD=0xF6 (TRANSFER_DONE), Payload=[0xFF, 0xFF, 0xFF, 0xFF]  ← GD32 完成确认
```

#### Step 14 — 刷写结束指令

| 方向 | Signal | 说明 |
|---|---|---|
| ESP32 → GD32 | `0x0A AC` | 刷写结束信号 |

**对应 SPI 帧：**

```
CMD=0xF7 (FINISH_OTA), Payload=[]  → ESP32 发送 0x0A AC
```

#### Step 15 — 内部跳转

GD32 内部执行动作：尝试跳转 APP。

> ESP32 无需等待此步骤完成，由 GD32 自行处理。

#### Step 16 — 最终确认

| 方向 | Signal | 说明 |
|---|---|---|
| GD32 → ESP32 | `0x0A AC` | 确认 APP 运行有效，回复刷写结束信号 |

**对应 SPI 帧：**

```
CMD=0xF7 (FINISH_OTA), Payload=[]  ← GD32 回复 0x0A AC
```

#### Step 17 — 清理

流程结束，GD32 清理会话密钥、RAM 缓存等。

---

### 3.5 命令码汇总

| CMD | 名称 | 方向 | Payload | 对应 OTA 阶段 |
|---|---|---|---|---|
| 0xF0 | `HANDSHAKE_REQ` | Bidirectional | 见 Step 1-2 | 阶段一 |
| 0xF1 | `CHALLENGE` | Bidirectional | nonce(16B) / 握手数据包 | 阶段一 |
| 0xF2 | `SESSION_KEY` | Bidirectional | 变换后随机数 / 0xBA C0 | 阶段一 |
| 0xF3 | `SEND_SIZE` | ESP32 → GD32 | size(4B, LE) | 阶段二 |
| 0xF4 | `DATA_PACKET` | ESP32 → GD32 | seq(2B) + data(48B) + CRC | 阶段三 |
| 0xF5 | `DATA_END` | ESP32 → GD32 | seq(2B) + data(N) + CRC + IFEND | 阶段三 |
| 0xF6 | `TRANSFER_DONE` | GD32 → ESP32 | 0xFF 0xFF 0xFF 0xFF | 阶段四 |
| 0xF7 | `FINISH_OTA` | Bidirectional | 0x0A AC | 阶段四 |
| 0x80 | `ACK` | GD32 → ESP32 | cmd(1B) + status(1B) | 通用 |
| 0x81 | `ERROR` | GD32 → ESP32 | cmd(1B) + err_code(1B) | 通用 |

---

### 3.6 时序图

```
ESP32                    GD32
  |                       |
  |---- HANDSHAKE_REQ ---->|  0x00 AC (Step 1)
  |<---- HANDSHAKE_REQ ---|  0x00 AC (Step 2)
  |                       |
  |---- CHALLENGE ------->|  nonce_server 16B (Step 3)
  |<---- CHALLENGE ------|  握手数据包 (Step 4)
  |                       |
  |---- SESSION_KEY ----->|  变换后随机数 (Step 5)
  |<---- SESSION_KEY -----|  0xBA C0 (Step 6) ← 安全通道建立
  |                       |
  |---- SEND_SIZE -------->|  Data Size (Step 7)
  |<---- SEND_SIZE -------|  0xBA C0 + seq (Step 8)
  |                       |
  |==== DATA LOOP ========>|  (Step 9-11, 每包 48B + CRC)
  |<==== DATA LOOP =======|  0xBA C0 + seq (正常) / 错误重发
  |                       |
  |---- DATA_END --------->|  最后一包 + IFEND (Step 12)
  |                       |
  |<---- TRANSFER_DONE ---|  0xBA C0 + FF FF FF FF (Step 13)
  |                       |
  |---- FINISH_OTA ------->|  0x0A AC (Step 14)
  |<---- FINISH_OTA ------|  0x0A AC (Step 16) ← GD32 跳转 APP
  |                       |
  * 清理会话密钥 & 缓存 *      (Step 17)
```

---

### 3.7 ACK 状态码

| status | 含义 |
|---|---|
| 0x00 | OK |
| 0x01 | 命令不支持 |
| 0x02 | 认证失败 |
| 0x03 | 大小超出范围 |
| 0x04 | CRC 校验失败 |
| 0x05 | 重发次数超限 |
| 0x06 | 会话密钥无效 |
| 0x07 | Flash 写入失败 |

---

### 3.8 重发机制

- GD32 检测到 CRC 错误时，回复当前包序号 + 错误指示
- ESP32 重新发送当前包
- 最大重发次数由 GD32 固件定义（建议 3 次）
- 超限后 ESP32 终止传输并上报 APP

---

## 4. ESP32 软件架构

### 4.1 目录结构

```
esp32_ota_relay/
├── main/
│   ├── main.c
│   ├── app_main.c
│   ├── ota_relay.c         # OTA 中继核心逻辑（含四阶段状态机）
│   ├── spi_master.c        # SPI 驱动
│   ├── wifi_ota.c          # WiFi OTA 接收
│   ├── ble_ota.c           # BLE OTA 接收
│   └── protocol.c          # 帧封装/解析/CRC/AES
└── CMakeLists.txt
```

### 4.2 模块职责

```
┌─────────────────────────────────────────────┐
│              ESP32 OTA Relay                 │
│                                             │
│  ┌──────────┐   ┌──────────┐               │
│  │ WiFi OTA │   │  BLE OTA │               │
│  │  Server  │   │   GATT   │               │
│  └────┬─────┘   └────┬─────┘               │
│       │              │                       │
│       └──────┬───────┘                       │
│              ▼                               │
│      ┌──────────────┐                       │
│      │ OTA Relay    │ ← 四阶段状态机        │
│      │  Dispatcher  │   透传加密数据        │
│      └──────┬───────┘                       │
│             │                               │
│      ┌──────▼──────┐                       │
│      │  Protocol   │ ← 帧封装/解析/CRC/AES  │
│      │   Layer     │                       │
│      └──────┬──────┘                       │
│             │                               │
│      ┌──────▼──────┐                       │
│      │SPI Master   │                       │
│      └─────────────┘                       │
└─────────────────────────────────────────────┘
```

### 4.3 OTA Relay Core 状态机

```c
typedef enum {
    // 阶段一：安全握手
    RELAY_HANDSHAKE_IDLE,      // 等待 HANDSHAKE_REQ
    RELAY_HANDSHAKE_WAIT,      // 等待 GD32 响应 0x00 AC
    RELAY_CHALLENGE_SENT,      // 已发送挑战随机数
    RELAY_CHALLENGE_RESPOND,    // 等待 GD32 握手数据包
    RELAY_SESSION_KEY_SENT,     // 已发送会话密钥分发
    RELAY_AUTH_COMPLETE,       // 认证完成，安全通道建立

    // 阶段二：传输准备
    RELAY_SIZE_SENT,           // 已发送 Data Size
    RELAY_SIZE_ACK,            // 收到大小确认

    // 阶段三：数据传输
    RELAY_RECEIVING,           // 接收固件数据
    RELAY_PACKET_SENT,         // 等待 GD32 ACK
    RELAY_RECEIVE_DONE,        // 传输完成（收到 DATA_END ACK）

    // 阶段四：结束跳转
    RELAY_TRANSFER_DONE,        // 收到传输完成确认
    RELAY_FINISH_SENT,         // 已发送刷写结束信号
    RELAY_DONE,                // 全部完成

    RELAY_ERROR
} relay_state_t;
```

### 4.4 安全相关说明

- **AES 加密**：由 ESP32 实现（需预置主密钥），GD32 解密验证
- **会话密钥 K_session**：由 Step 4/5 双方协商生成
- **搅伴算法**：ESP32 生成 nonce_server 时使用（时间 + 内存地址 + rand → 3轮单字节 → 2轮4字节）
- **芯片唯一码**：GD32 提供，ESP32 用于密钥派生

> ⚠️ 主密钥和 AES 算法细节需由 GD32 固件侧提供，本设计仅做协议映射。

### 4.6 数据缓冲策略

**方案 A：存储后转发（先存 ESP32 Flash，再发 SPI）**

ESP32 将 APP 发送的固件完整存入 Flash，传输完成后再通过 SPI 转发给 GD32。

#### Flash 分区设计

假设固件最大 256KB，预留足够空间：

```yaml
# Partitions
# Name,     Type, SubType, Offset,   Size
otadata,    data, ota,      0x9000,   0x2000     # OTA 数据区标记
gd32_ota,   data, spiffs,  0xB000,   0x40000    # GD32 固件存储区 (256KB)
app0,       app,  ota_0,   0x4B000,  0x330000   # ESP32 自身 APP
app1,       app,  ota_1,   0x3DC000, 0x330000
```

> `gd32_ota` 分区使用 spiffs 或自定义 data 分区，存放原始固件 binary。

#### 接收与存储流程

```
APP                        ESP32                      GD32
 |                          |                          |
 |--- DATA_PACKET (4KB) --->|                          |
 |                          |--- write to Flash ------>|  (存 Flash，不发 SPI)
 |<--- ACK (stored) --------|                          |
 |                          |                          |
 |--- DATA_PACKET (4KB) --->|                          |
 |                          |--- write to Flash ------>|  (存 Flash，不发 SPI)
 |<--- ACK (stored) --------|                          |
 |                          |                          |
 |         ... 循环存完所有包 ...                       |
 |                          |                          |
 |--- DATA_END ------------->|                          |
 |                          |                          |
 |                          |--- ENTER_BOOT ----------->|  (开始刷写)
 |                          |<--- ACK ------------------|  (GD32 就绪)
 |                          |                          |
 |                          |=== 流式转发固件 =========>|  (按 48B/packet 逐包发送)
 |                          |<=== ACK per packet =======|  (每包 GD32 ACK)
 |                          |                          |
 |                          |--- FINISH_OTA ---------->|  (刷写结束)
 |                          |<--- FINISH_OTA ----------|  (GD32 跳转 APP)
```

#### 断点续传设计

| 文件 | 内容 | 说明 |
|---|---|---|
| `ota_meta.json` | `{total_size, stored_size, chunk_count, last_seq}` | 记录传输进度 |

- APP 重连后先读 `ota_meta.json`，获取 `stored_size`
- 从 `last_seq + 1` 继续发送剩余包
- ESP32 追加写入 Flash（不需要从头擦写）

#### 存储完成判断

- APP 发送 `DATA_END` 后，ESP32 更新 `ota_meta.json` 的 `stored_size = total_size`
- ESP32 检测到 `stored_size == total_size` 后，自动开始向 GD32 转发

#### 内存 Buffer

存储同时使用 2 × 4096 bytes 乒乓 buffer 接收 APP 数据，写入 Flash 后切换 buffer 继续接收。

---

## 5. WiFi OTA 实现

### 5.1 工作模式

- **AP 模式**：ESP32 作为热点，手机直连
- **STA 模式**：ESP32 连接路由器，APP 通过 IP 访问

### 5.2 HTTP OTA 服务器

```
HTTP POST /ota/upload
Content-Type: application/octet-stream
Body: <raw firmware binary>

Response:
{"status": "ok", "progress": 100}
```

### 5.3 APP → ESP32 控制协议

APP 与 ESP32 之间使用简化帧格式：

```
| CMD(1B) | LEN(2B) | PAYLOAD(LEN) | CRC16(2B) |
```

命令码直接对应上述 SPI 命令码（0xF0-0xF7），但数据字段为明文（加密由 ESP32 处理后再转发给 GD32）。

### 5.4 固件分包（APP → ESP32）

```
| CMD(1B) | SEQ(2B) | SIZE(4B) | DATA(SIZE) | CRC32(4B) |
```

---

## 6. BLE OTA 实现

### 6.1 GATT Profile 设计

| Handle | UUID | 属性 | 说明 |
|---|---|---|---|
| 0x0001 | - | Primary Service | OTA Service |
| 0x0010 | `xxxx-xxxx-xxxx-0001` | Control Point | 写命令/通知（控制帧通道） |
| 0x0011 | `xxxx-xxxx-xxxx-0002` | Data In | 写无回应（大规模数据通道） |
| 0x0012 | `xxxx-xxxx-xxxx-0003` | Data Out | 通知 GD32 响应 |
| 0x0013 | `xxxx-xxxx-xxxx-0004` | Version | 只读，ESP32 版本号 |

### 6.2 BLE 传输流程

1. APP 写 `Control Point` (0xF0) → ESP32，发起握手
2. ESP32 通过 SPI 转发给 GD32
3. GD32 响应通过 `Data Out` 通知 APP
4. APP 写 `Data In` 流式发送固件分包（48B/packet）
5. 每包完成后，ESP32 回复 ACK 到 `Data Out`
6. APP 写 `Control Point` (0xF7) → FINISH
7. GD32 响应后，APP 可断开连接

### 6.3 MTU 设置

- 建议 MTU ≥ 512 bytes
- BLE 每次 `Data In` 最大传输 512 bytes（减去 ATT 头）
- 大文件分包多次写入

---

## 7. SPI Master 配置

```c
// ESP32 SPI2 配置 (VSPI)
spi_device_handle_t spi;

// 建议时钟: 40MHz
spi_bus_config_t bus_cfg = {
    .mosi_io_num = 8,
    .miso_io_num = 7,
    .sclk_io_num = 6,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
};

spi_device_interface_config_t dev_cfg = {
    .clock_speed_hz = 40 * 1000 * 1000,  // 40MHz
    .mode = 0,           // CPOL=0, CPHA=0
    .spics_io_num = 4,   // GPIO4 as CS
    .queue_size = 1,
    .command_bits = 0,
    .address_bits = 0,
};
```

---

## 8. 错误处理

| 场景 | 处理 |
|---|---|
| GD32 无响应 ACK | 超时 3s → 重发同一包，最多 3 次 → 报错 |
| CRC 校验失败 | GD32 要求重发 → ESP32 重发当前包 |
| 重发次数超限 | ESP32 终止传输，通知 APP |
| WiFi 断开 | 暂停传输，等待重连 |
| BLE 断开 | 通知 APP 重新连接 |
| 认证失败 | 终止流程，通知 APP |

---


## 9. ESP32 自检与保护机制

ESP32-C3FH4 利用芯片内置传感器实现运行安全监控，不外挂额外传感器。

### 9.1 检测项

| 检测项 | 来源 | 阈值范围 | 处理动作 |
|---|---|---|---|
| 芯片温度 | 内部温度传感器 | 正常: < 80°C / 警告: 80-100°C / 停机: > 100°C | 警告时降频，停机时中止 OTA |
| Flash 空间 | 系统 API | 可用 > 16KB（存储分区元数据） | 空间不足时拒绝 OTA 请求 |
| WiFi 信号强度 | WiFi RSSI | 正常: > -70 dBm / 弱: -70 ~ -85 dBm / 断开: < -85 dBm | 弱时降低 OTA 速度，断开时暂停 |
| BLE 连接状态 | BLE stack | 连接中 / 断开 | 断开时通知 APP 重连 |

### 9.2 检测频率

| 检测项 | 频率 | 方式 |
|---|---|---|
| 芯片温度 | 每 1 秒 | FreeRTOS Timer |
| Flash 空间 | OTA 开始前检查 | 一次性 |
| WiFi RSSI | 每 10 秒 | 轮询 |
| BLE 连接 | 事件驱动 | BLE GAP 回调 |

### 9.3 本地判断逻辑

```c
typedef enum {
    SYS_NORMAL,         // 全部正常
    SYS_WARNING,        // 任一检测项触发警告阈值
    SYS_CRITICAL,      // 任一检测项触发停机阈值
    SYS_SAFE_SHUTDOWN  // 强制停机
} sys_health_state_t;

// 状态机优先级：SAFE_SHUTDOWN > CRITICAL > WARNING > NORMAL
// 高优先级状态覆盖低优先级状态
```

### 9.4 异常处理联动

| ESP32 健康状态 | 对 OTA 的影响 |
|---|---|
| `SYS_NORMAL` | 正常进行 |
| `SYS_WARNING` | 降低 SPI 速率，继续 OTA，记录警告日志 |
| `SYS_CRITICAL` | 暂停 OTA 传输，通知 APP 当前状态，等待恢复 |
| `SYS_SAFE_SHUTDOWN` | 中止 OTA，发送 ABORT 到 GD32，保护性复位 |

### 9.5 日志记录

异常事件写入 Flash 日志分区（`system_log`），包含：

```
[TIMESTAMP] TEMP_WARNING: 85°C
[TIMESTAMP] OTA_PAUSED: temp_critical
[TIMESTAMP] OTA_RESUMED
```

日志可由 APP 通过 WiFi/BLE 查询。

### 9.6 模块架构

```
┌──────────────────────────────────┐
│        System Monitor            │
│  (FreeRTOS Task, Priority High) │
├──────────────────────────────────┤
│  temp_monitor   (1s timer)       │
│  wifi_monitor   (10s timer)     │
│  ble_monitor    (event-driven)   │
├──────────────────────────────────┤
│  health_state_t state           │
│  → shared with OTA task via mutex│
└──────────────────────────────────┘
                │
                ▼
┌──────────────────────────────────┐
│   OTA Relay Task                 │
│   - checks health_state before   │
│     each GD32 DATA_PACKET send   │
│   - pauses/aborts on WARNING/    │
│     CRITICAL/SAFE_SHUTDOWN       │
└──────────────────────────────────┘
```



## 10. ESP32 固件分区（参考）

```yaml
# Partitions
# Name,     Type, SubType, Offset,   Size
otadata,    data, ota,      0x9000,   0x2000     # OTA 数据区标记
gd32_ota,   data, spiffs,  0xB000,   0x40000    # GD32 固件存储区 (256KB)
app0,       app,  ota_0,   0x4B000,  0x330000   # ESP32 自身 APP
app1,       app,  ota_1,   0x3DC000, 0x330000
spiffs,     data, spiffs,  0x670000, 0x190000   # 文件系统（日志等）
```

> **注意：** `gd32_ota` 分区大小根据实际 GD32 固件最大体积调整，建议预留 256KB 以上。
