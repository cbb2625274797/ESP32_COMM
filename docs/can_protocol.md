# CAN 通信协议文档

> 更新日期：2026-10-10  
> 涉及固件：GD32 DCU_APP + ESP32 ESP32_COMM  
> SPI：**Link v2 only** · 时钟 4 MHz  
> APP 最大业务 payload：`kTxBufSize(256) - V2_OVERHEAD(20) = 236`  
> BOOT 最大业务 payload：`kTxBufSize(128) - V2_OVERHEAD(20) = 108`

---

## 1. 概述

ESP32 通过 SPI Link v2 与 GD32 通信，实现三路 CAN/CANFD 的收发控制。CAN 相关 SPI 命令占用 `0xC0` ~ `0xC3`、`0xC7`、`0xC8`。

| 帧类型 | 名称 | 方向 | 用途 |
|--------|------|------|------|
| `0xC0` | kTypeCanSend | ESP32 → GD32 | 发送一帧 CAN 数据 |
| `0xC1` | kTypeCanFeedback | 双向 | 通道统计 + ID 快照页；GD32 亦可 1Hz 主动推送 |
| `0xC2` | kTypeCanSetMode | ESP32 → GD32 | 切换三路 CAN 接收过滤模式 |
| `0xC3` | kTypeIdSnapshot | ESP32 → GD32 | 完整 ID 条目（含 seq/rx_count） |
| `0xC7` | kTypeCanBurstSend | ESP32 → GD32 | 跨通道突发发送（一次事务背靠背入多邮箱） |
| `0xC8` | kTypeCanSetSilent | ESP32 → GD32 | 切换指定通道收发器 Silent（只听不发） |

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
[0]     0x00（ACK）或错误码 E0~E6
```

| 码 | 含义 |
|----|------|
| E0 | payload 过短（&lt; 5 字节） |
| E1 | 通道非法 |
| E2 | BUS_OFF |
| E3 | TEC 过高（&gt;96，对端 ACK 失败率高） |
| E4 | 邮箱满 |
| E5 | 参数无效 |
| E6 | 通道处于收发器 Silent（只听不发），入口即拒，不计错误计数 |

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
[27]     bus_state        bit0=init bit1=控制器睡眠 bit2=EP bit3=bus_off bit4=EW bit5=只听不发(Silent)
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

## 5.1 0xC8 — 收发器 Silent（只听不发）

GD32 把该通道 TCAN1057AV 的 **S 脚**（CAN0=PA8 / CAN1=PB14 / CAN2=PB9）置高：
驱动器关断，**接收机照常工作、RXD 仍镜像总线**（手册 Table 8-4），所以静默期间接收与
0xC1 上报完全不受影响。S 脚内部上拉，MCU 复位/浮空即为 Silent。

```
req[0]  channel   0/1/2
req[1]  silent    1=进 Silent（只听不发） 0=回 Normal
rsp[0]  channel   回显
rsp[1]  actual    实际生效（1=Silent；与 GD32 发送门控同一个值）
rsp[2]  ch_en     该通道是否使能
```

- ERROR：`E0`=payload 过短 · `E1`=通道非法/未使能 · `E2`=S 脚回读与意图不符（内部标志已回滚，不会留下"报了静默其实没静默"的状态）
- 静默生效期间，该通道所有发送入口（0xC0 / 0xC7 / 桥接 / 电机 relay）回 **E6**，且不推进任何错误计数
- 进 Silent 前 GD32 先 abort 本通道 PENDING 邮箱（驱动器已关，留着只会白涨 TEC）
- 退 Silent 时 GD32 忙等收发器模式建立时间 `tMODE = 20 µs`（手册 6.9）再放开发送门
- **不持久化**：上电 init 一律回 Normal
- 状态读取走 0xC1 的 `bus_state` bit5，ESP32 侧不另开轮询端点
- ESP32 HTTP：`POST /can/silent` body `{"ch":0,"on":1}`；OTA / send_batch 独占 SPI 期间返回 `{"ok":false,"err":"paused"}`

---

## 6. 0xC3 — ID 快照（完整字段）

固定头 4B + 条目 `(16 + dlc)`（含 id/seq/rx_count/dlc/flags/rsv/data）。  
上限同 APP：`256 - V2_OVERHEAD = 236`。

---

## 7. 实现索引

| 端 | 文件 |
|----|------|
| GD32 0xC1/C2/C8 | `DCU_APP/.../app_spi_cmd_can_feedback.cpp`（0xC8 复用同一 handler） |
| GD32 S 脚/发送门控 | `DCU_APP/.../device/canfd.cpp` → `setTxSilent()` / `isTxSilent()` |
| GD32 0xC3 | `app_spi_cmd_id_snapshot.cpp` |
| GD32 0xC0 | `app_spi_cmd_can_bridge.cpp` |
| ESP32 解析/桥 | `ESP32_COMM/main/transport/canfd_bridge.*` |
| ESP32 读预算 | `spi_master.cpp` → `spiRespReadBudget()` |
| 帧格式 | `spi_link/spi_link_v2.h` |
| 图示文档 | `SPEC/communication_ids.html` · `SPEC/spi_flow_diagram.html` |
