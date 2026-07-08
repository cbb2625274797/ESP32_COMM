# 修复 BLE OTA 虚假 ACK 导致刷写极慢 Spec

## Why
BLE 刷写 ESP32 自身固件时速度极慢，每个 32 包窗口都报"丢失 32 包"并触发重发。根因是 `process_esp32_packet()` 中 ACK 触发条件仅检查 `(seq+1)%32==0`，未区分包是否真正写入 flash。重复包（来自发送端超时重发）在窗口边界会触发虚假 ACK，携带陈旧的 `getReceived()` 值，被发送端误判为丢包，导致每个窗口都产生不必要的重发。

## What Changes
- **ESP32 路径** (`process_esp32_packet`)：在调用 `esp32_write_ordered()` 前后对比 `getReceived()`，仅在实际写入新字节时才触发窗口边界 ACK，避免重复包/乱序缓存包发送陈旧 ACK
- **GD32 路径** (`process_gd32_packet`)：移除重复包在窗口边界的 ACK 发送（L1233-1235），重复包静默跳过，发送端已持有上次有效 ACK 的进度
- **ACK 超时调整**：发送端首次窗口 ACK 超时从 3000ms 提升至 5000ms，适应设备端 flash 写入速度（实测约 3.4s/窗口），避免首轮超时产生重复包

## Impact
- Affected code:
  - `main/server/ble_ota.cpp` — `process_esp32_packet()` ACK 触发逻辑 (L1146-1172)
  - `main/server/ble_ota.cpp` — `process_gd32_packet()` 重复包 ACK 逻辑 (L1229-1236)
  - `tools/ble_ota.js` — `uploadFirmware()` ACK 超时参数 (L1305, L1370)

## ADDED Requirements

### Requirement: ACK 触发需基于实际写入进度
ESP32 OTA 接收端在处理数据包时，窗口边界 ACK 的触发必须以"实际写入 flash 的字节数增长"为前提，而非仅依据包序号是否落在窗口边界。

#### Scenario: 重复包到达窗口边界
- **WHEN** 发送端因超时重发，重复包（seq < expected）到达设备端，且该 seq 处于窗口边界（(seq+1)%32==0）
- **THEN** 设备端不发送 ACK（该包未产生新的 flash 写入）

#### Scenario: 乱序缓存包到达窗口边界
- **WHEN** 乱序包（seq > expected）被缓存到 reorder buffer，且该 seq 处于窗口边界
- **THEN** 设备端不发送 ACK（该包未产生 flash 写入，仅暂存）

#### Scenario: 正常写入触发 flush 跨越窗口边界
- **WHEN** 按序包到达后触发 reorder buffer flush，`getReceived()` 增长
- **THEN** 设备端正常发送 ACK，携带最新的 `getReceived()` 值

## MODIFIED Requirements

### Requirement: GD32 重复包处理
GD32 OTA 接收端遇到重复包（seq <= last_seq）时，静默跳过，不发送 ACK。发送端已通过上一窗口的有效 ACK 获知设备端进度，重复包的 ACK 只会干扰当前窗口的 waitForAck 判断。

#### Scenario: 重复包到达（GD32）
- **WHEN** GD32 OTA 收到重复包（seq <= last_seq）
- **THEN** 跳过写入，不发送任何 ACK，继续处理队列中的后续包

### Requirement: BLE OTA 发送端 ACK 超时
发送端首轮窗口 ACK 等待超时从 3000ms 调整为 5000ms，以适应设备端 flash 写入速度（实测约 3.4s/32 包窗口），减少首轮不必要的超时重发。续发重试路径的超时保持 3000ms 不变（此时设备端已在处理，ACK 会更快返回）。

#### Scenario: 首轮窗口 ACK 等待
- **WHEN** 发送端发送一个新窗口的 32 个包并等待 ACK
- **THEN** 等待超时为 5000ms，若设备端在 5s 内完成处理并返回 ACK，无需重发

#### Scenario: 续发重试 ACK 等待
- **WHEN** 发送端因丢包检测而续发部分包并等待 ACK
- **THEN** 等待超时保持 3000ms
