# CAN 通信协议文档

> 更新日期：2026-07-16  
> 涉及固件：GD32 DCU_APP + ESP32 ESP32_COMM  
> SPI：**Link v2 only** · 时钟 4 MHz  
> APP 最大业务 payload：`kTxBufSize(256) - V2_OVERHEAD(20) = 236`  
> BOOT 最大业务 payload：`kTxBufSize(128) - V2_OVERHEAD(20) = 108`

---

## 1. 概述

ESP32 通过 SPI Link v2 与 GD32 通信，实现三路 CAN/CANFD 的收发控制。CAN 相关 SPI 命令占用 `0xC0` ~ `0xC3`。

| 帧类型 | 名称 | 方向 | 用途 |
|--------|------|------|------|
| `0xC0` | kTypeCanSend | ESP32 → GD32 | 发送一帧 CAN 数据 |
| `0xC1` | kTypeCanFeedback | 双向 | 通道统计 + ID 快照页；GD32 亦可 1Hz 主动推送 |
| `0xC2` | kTypeCanSetMode | ESP32 → GD32 | 切换三路 CAN 接收过滤模式 |
| `0xC3` | kTypeIdSnapshot | ESP32 → GD32 | 完整 ID 条目（含 seq/rx_count） |

---

## 2. SPI 协议帧格式（v2）

所有 SPI 帧遵循 SPI Link v2（`spi_link/spi_link_v2.h`）：

```
[SOF0 A5][SOF1 5A][VER 02][FLAGS][SESSION 4LE][TX_SEQ][ACK_SEQ][CREDIT][TYPE][LEN 2LE][RSV 2][PAYLOAD][CRC32 4LE]
```

- 开销 `V2_OVERHEAD = 20`
- CRC32 MPEG-2（`0x04C11DB7`，无反射）
- **无** v1 单字节 SOF + CRC8 路径

应答 TYPE 常用：`0x80` ACK · `0x81` WAIT · `0x82` ERROR

---

## 3. 0xC0 — CAN 发送

### 请求 payload

```
[0]     channel     0/1/2
[1..4]  ext_id      扩展帧 ID（Big-Endian，29 位）
[5..N]  data        CAN 数据负载（0~64 字节；可无，对应 DLC=0）
```

最短 payload 为 **5** 字节（仅 ch+id，DLC=0，例如 `CANFD_BOOT`）。

### 应答 payload

```
[0]     0x00（ACK）或错误码 E0~E5
```

| 码 | 含义 |
|----|------|
| E0 | payload 过短（&lt; 5 字节） |
| E1 | 通道非法 |
| E2 | BUS_OFF |
| E3 | TEC 过高（&gt;96，对端 ACK 失败率高） |
| E4 | 邮箱满 |
| E5 | 参数无效 |

---

## 4. 0xC1 — 通道统计 + ID 快照（核心）

GD32 可 1Hz 主动 `requestEsp32Read(0xC1)` 推送；ESP32 亦支持被动查询。  
打包上限：`kFeedbackPayloadLen = kTxBufSize - V2_OVERHEAD = 236`。

### 请求 payload（被动）

```
[0]     channel     0/1/2
[1]     page        0 = 条目 0~7，1 = 条目 8~15
```

### 应答 / 推送 payload（变长）

固定头 **28** 字节 + 条目区：

```
[0]      channel
[1]      page
[2]      total_count
[3]      entry_count
[4]      mode             0=全收 1=过滤 2=桥接
[5..8]   snapshot_seq     LE
[9..12]  rx_count         LE
[13..16] tx_count         LE
[17..20] rx_drop_count    LE
[21..24] error_count      LE
[25]     tec
[26]     rec
[27]     bus_state        bit0=init … bit3=bus_off bit4=EW
[28..]   条目
```

### 单条目（8 + dlc）

```
[+0..+3]  id LE
[+4]      dlc
[+5]      flags     bit0=FD bit1=BRS bit2=ESI
[+6..+7]  reserved
[+8..]    data
```

---

## 5. 0xC2 — 模式切换

```
req[0]  mode   0=全收 1=过滤 2=CAN0→1/2 桥接
rsp[0]  actual 生效 mode
```

---

## 6. 0xC3 — ID 快照（完整字段）

固定头 4B + 条目 `(16 + dlc)`（含 id/seq/rx_count/dlc/flags/rsv/data）。  
上限同 APP：`256 - V2_OVERHEAD = 236`。

---

## 7. 实现索引

| 端 | 文件 |
|----|------|
| GD32 0xC1/C2 | `DCU_APP/.../app_spi_cmd_can_feedback.cpp` |
| GD32 0xC3 | `app_spi_cmd_id_snapshot.cpp` |
| GD32 0xC0 | `app_spi_cmd_can_bridge.cpp` |
| ESP32 解析/桥 | `ESP32_COMM/main/transport/canfd_bridge.*` |
| ESP32 读预算 | `spi_master.cpp` → `spiRespReadBudget()` |
| 帧格式 | `spi_link/spi_link_v2.h` |
| 图示文档 | `SPEC/communication_ids.html` · `SPEC/spi_flow_diagram.html` |
