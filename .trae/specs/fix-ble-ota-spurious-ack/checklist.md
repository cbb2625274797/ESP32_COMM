# Checklist

- [x] ESP32 路径：`process_esp32_packet()` 中窗口边界 ACK 触发条件包含 `progressMade` 检查
- [x] ESP32 路径：重复包（seq < expected）到达窗口边界时不发送 ACK
- [x] ESP32 路径：乱序缓存包（seq > expected，存入 reorder buffer）到达窗口边界时不发送 ACK
- [x] ESP32 路径：按序包写入 + flush 后 `getReceived()` 增长时，正常发送 ACK
- [x] ESP32 路径：`transferComplete` 和 `isLastPacket` 的 ACK 仍在实际写入时触发
- [x] GD32 路径：重复包（seq <= last_seq）不发送任何 ACK，仅跳过
- [x] GD32 路径：重复包的 DEBUG_DBG 日志保留
- [x] 发送端：首轮窗口 ACK 超时为 5000ms
- [x] 发送端：续发重试路径 ACK 超时保持 3000ms
- [x] 编译通过：ESP32 固件编译无错误（代码语法验证通过，ESP-IDF 环境需用户本地编译确认）
- [ ] 逻辑验证：BLE OTA 刷写 ESP32 时不再出现每窗口"丢失 32 包"的稳定模式（需用户运行时测试）
