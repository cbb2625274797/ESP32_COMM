# CAN 通信协议文档

> 更新日期：2026-06-29
> 涉及固件：GD32 DCU_APP + ESP32 ESP32_COMM
> SPI 时钟：4 MHz | 应答帧最大 payload：123 字节（kTxBufSize=128 - overhead=5）

---

## 1. 概述

ESP32 通过 SPI 协议与 GD32 通信，实现三路 CAN/CANFD 的收发控制。CAN 相关 SPI 命令占用 `0xC0` ~ `0xC2` 帧类型。

| 帧类型 | 名称 | 方向 | 用途 |
|--------|------|------|------|
| `0xC0` | kTypeCanSend | ESP32 → GD32 | 发送一帧 CAN 数据 |
| `0xC1` | kTypeCanFeedback | ESP32 → GD32 | 拉取不重复 ID 快照表（分页） |
| `0xC2` | kTypeCanSetMode | ESP32 → GD32 | 切换三路 CAN 接收过滤模式 |

---

## 2. SPI 协议帧格式

所有 SPI 帧遵循统一格式：

```
[SOF:1B][TYPE:1B][LEN_LO:1B][LEN_HI:1B][PAYLOAD:NB][CRC8:1B]
```

- SOF = `0xA5`
- LEN = payload 长度（小端 16 位）
- CRC8 = 对 SOF+TYPE+LEN+PAYLOAD 的校验

应答帧 TYPE：
- `0x80` = ACK（成功）
- `0x81` = WAIT（OTA 中间态）
- `0x82` = ERROR

---

## 3. 0xC0 — CAN 发送

### 请求 payload

```
[0]     channel     0/1/2
[1..4]  ext_id      扩展帧 ID（Big-Endian，29 位）
[5..N]  data        CAN 数据负载（1~64 字节）
```

### 应答 payload

```
[0]     0x00（ACK）或错误码
```

---

## 4. 0xC1 — 不重复 ID 快照表（核心协议）

每秒 ESP32 对每通道发 2 次请求（page=0 和 page=1），拉取该通道所有**不重复 CAN ID** 的最新帧快照。

### 请求 payload

```
[0]     channel     0/1/2
[1]     page        页号：0 = 条目 0~7，1 = 条目 8~15
```

### 应答 payload（变长，最大 123 字节）

```
[0]      channel          回显通道号
[1]      page             回显页号
[2]      total_count      该通道实际 ID 条目总数（0~16）
[3]      entry_count      本页实际条目数
[4]      mode             当前 CAN 接收模式（0=全收, 1=过滤）
[5..8]   snapshot_seq     快照序号（LE，每秒 +1）
[9..12]  rx_count         通道累计接收帧数（LE）
[13..16] tx_count         通道累计发送帧数（LE）
[17..20] rx_drop_count    通道累计丢帧（LE）
[21..24] error_count      通道累计错误（LE）
[25..]   条目逐条拼接（见下）
```

### 单条目格式（变长）

```
[+0..+3]  id        CAN 报文 ID（LE）
[+4]      dlc       数据长度（1~64）
[+5]      flags     bit0=FD, bit1=BRS, bit2=ESI
[+6..+7]  reserved  保留（0x00）
[+8..+N]  data      报文数据（dlc 字节）
```

条目总大小 = 8 + dlc 字节。
- 标准 CAN（dlc=8）：14 字节/条 → 每页最多 8 条
- CANFD（dlc=64）：72 字节/条 → 每页最多 1 条

### 拉取时序

```
ESP32                          GD32
  |--- 0xC1 [ch=0, page=0] --->|
  |<-- ACK [统计+条目0~7] -----|
  |--- 0xC1 [ch=0, page=1] --->|
  |<-- ACK [条目8~15] ---------|
  |--- 0xC1 [ch=1, page=0] --->|
  |<-- ACK [统计+条目0~7] -----|
  |--- 0xC1 [ch=1, page=1] --->|
  |<-- ACK [条目8~15] ---------|
  |--- 0xC1 [ch=2, page=0] --->|
  |<-- ACK [统计+条目0~7] -----|
  |--- 0xC1 [ch=2, page=1] --->|
  |<-- ACK [条目8~15] ---------|
  |            (1 秒后重复)
```

每轮 6 次 SPI 事务，SPI 总线占用 ~5ms（0.5%）。

### total_count 与 entry_count 的关系

- `total_count` = 该通道当前所有不重复 ID 的总数（0~16）
- `entry_count` = 本页实际返回的条目数（≤8）
- 当 `total_count ≤ 8` 时，page=1 的 `entry_count=0`，ESP32 不再拉第二页
- 当 `total_count = 0` 时，表示该通道未收到任何 CAN 帧

---

## 5. 0xC2 — 切换接收过滤模式

### 请求 payload

```
[0]     mode        0 = 全部接收（kModeAllPass）
                    1 = 按 ID 过滤（kModeFilterId）
```

### 应答 payload

```
[0]     actual_mode GD32 实际生效的模式
```

模式切换影响三路 CAN 通道（全局生效，不是单通道）。

---

## 6. GD32 端实现要点

### 不重复 ID 快照表（canfd.h）

```cpp
struct IdSnapshotEntry {
    uint32_t id;
    uint32_t seq;           // 更新序号
    uint32_t rx_count;      // 该 ID 累计接收
    uint8_t  dlc, fd_flag, brs, esi;
    uint8_t  data[64];
    bool     occupied;
};

struct IdSnapshotTable {
    IdSnapshotEntry entries[16]; // CANFD_ID_SNAPSHOT_MAX
    uint8_t         count;
    uint32_t        seq;
};
```

### 中断更新逻辑（canfd.cpp::handleRxInterrupt）

每收到一帧 CAN：
1. 遍历 `s_idSnapshot[channel].entries[0..count-1]`，查找 ID 匹配的条目
2. 找到 → 更新 data/dlc/flags/rx_count++
3. 未找到且 count < 16 → 新建条目
4. 未找到且 count = 16 → FIFO 淘汰最旧条目（index 0），整体左移，新条目插入末尾

### SPI 处理器（app_spi_cmd_can_feedback.cpp）

- `kFeedbackPayloadLen = 123`（kTxBufSize 128 - overhead 5）
- 固定头 25 字节 + 变长条目逐条拼接
- 条目不足时 `entry_count < kPageEntries`，ESP32 侧据此判断是否需要拉下一页

---

## 7. ESP32 端实现要点

### 数据流

```
GD32 CAN 中断
  → s_idSnapshot[ch].entries[匹配ID]
    ↓ (1Hz 快照刷新)
  refreshFeedbackSnapshot → seq++
    ↓ (ESP32 1Hz 轮询)
  0xC1 × 3通道 × 2页 = 6次 SPI/s
    ↓
  can_feedback_task 缓存到 s_latest[ch]
    ↓
  /can/feedback HTTP → JSON
    ↓
  can_monitor.js 表格渲染
```

### SPI 轮询（can_feedback_task.cpp）

- 固定 1Hz 循环，每通道发 2 次 0xC1（page=0 和 page=1）
- OTA 进行中时暂停（`s_paused` 标志，由 OtaCoordinator 和 OtaRelay 设置）
- 结果缓存在 `s_latest[3]`（`can_feedback_result_t`），线程安全（互斥锁）

### HTTP 接口

| 端点 | 方法 | 用途 |
|------|------|------|
| `/can/feedback` | GET | 三路 CAN 通道统计 + ID 快照表 |
| `/can/mode` | GET | 查询当前接收模式 |
| `/can/mode` | POST | 切换模式 `{"mode":0}` 或 `{"mode":1}` |
| `/can/send` | POST | 发送一帧 CAN 数据 |

### /can/feedback 响应格式

```json
{
  "channels": [
    {
      "ch": 0,
      "valid": true,
      "mode": 0,
      "seq": 1234,
      "rx": 5678,
      "tx": 0,
      "drop": 0,
      "err": 0,
      "count": 5,
      "entries": [
        {
          "id": "0x18DA00F1",
          "dlc": 8,
          "fd": 0,
          "brs": 0,
          "esi": 0,
          "data": "0102030405060708"
        }
      ]
    },
    { "ch": 1, "valid": false },
    { "ch": 2, "valid": false }
  ]
}
```

---

## 8. 前端（tools/can_monitor.html）

### 页面结构

1. **顶部状态栏**：连接状态、当前模式、轮询状态
2. **三路通道面板**（CH0/CH1/CH2）：
   - 状态（活跃/无帧）
   - rx/tx/drop/err 统计
   - seq（活性指示）
   - 当前 ID 数量
3. **每 ID 快照表**：
   - 通道下拉选择
   - 每行一个不重复 ID
   - 列：CAN ID | DLC | FD | BRS | ESI | 数据 (HEX)
   - 1Hz 自动刷新

### 导航

- OTA 控制台 (`index.html`) → "CAN 接收监控" 按钮
- CAN 发送控制台 (`can.html`) → header "CAN 接收监控" 链接
- CAN 接收监控 (`can_monitor.html`) → header 可跳回 CAN 发送或 OTA 控制台

---

## 9. 资源消耗

| 资源 | 消耗 |
|------|------|
| GD32 SRAM（快照表） | 3.8KB / 128KB = 3% |
| GD32 中断开销（16 条遍历） | < 1μs |
| SPI 事务/秒 | 6 次（3 通道 × 2 页） |
| SPI 总线占用 | ~5ms/s = 0.5% |
| ESP32 RAM（缓存） | ~4KB |
| OTA 期间 CAN 轮询 | 自动暂停 |

---

## 10. 已废弃

| 项目 | 状态 | 说明 |
|------|------|------|
| `0xC3 kTypeIdSnapshot` | 保留定义但不使用 | 功能已合并到 0xC1 |
| `/can/idsnap` | 已移除 | 数据由 `/can/feedback` 统一提供 |
| `canFdPullIdSnapshot()` | 已移除 | 改用 `canFdPollFeedback()` 分页拉取 |
