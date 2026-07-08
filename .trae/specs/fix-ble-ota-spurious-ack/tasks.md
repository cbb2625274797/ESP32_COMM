# Tasks

- [x] Task 1: 修复 ESP32 路径虚假 ACK（process_esp32_packet）
  - [x] SubTask 1.1: 在 `process_esp32_packet()` 中，调用 `esp32_write_ordered()` 前记录 `beforeReceived = getReceived()`
  - [x] SubTask 1.2: 调用后记录 `afterReceived = getReceived()`，计算 `progressMade = (afterReceived > beforeReceived)`
  - [x] SubTask 1.3: 修改 ACK 触发条件：窗口边界 ACK 仅在 `progressMade` 为 true 时触发；`transferComplete` 和 `isLastPacket` 的 ACK 也需 `progressMade` 为 true（完成判定依赖实际写入字节数，无写入则不可能完成）
  - [x] SubTask 1.4: 使用 `afterReceived` 替换 ACK 中的 `getReceived()` 调用，避免重复查询

- [x] Task 2: 修复 GD32 路径重复包 ACK（process_gd32_packet）
  - [x] SubTask 2.1: 移除重复包分支（seq <= last_seq）中的窗口边界 ACK 发送逻辑（L1233-1235）
  - [x] SubTask 2.2: 保留重复包的 DEBUG_DBG 日志和 `return false` 跳过逻辑

- [x] Task 3: 调整发送端首轮 ACK 超时（ble_ota.js）
  - [x] SubTask 3.1: 将首轮窗口 ACK 等待超时从 3000ms 改为 5000ms（L1305 的 waitForAck 调用）
  - [x] SubTask 3.2: 续发重试路径的 ACK 超时保持 3000ms 不变（L1370 的 waitForAck 调用）

# Task Dependencies
- Task 1, Task 2, Task 3 相互独立，可并行实现
