# ESP32_COMM 通信梳理（2026-05-05）

## 结论

当前“ESP32 ↔ 上位机（tools WebUI）”的主通信方式是 **HTTP**，不是 UDP。

### WebUI 实际在用的接口

- `GET /system/status`：每 1 秒轮询一次，用于在线状态、WiFi、heap、温度、电压、OTA 状态展示
- `GET /logs`：每 1 秒轮询一次，用于拉取并清空日志环形缓冲区
- `POST /can/send`：发送 CAN 请求到 GD32
- `POST /ota/upload` / `POST /ota/relay` / `POST /ota/update`：OTA 相关

## 日志链路

所有 `DEBUG_*` 日志目前会并行写入三处：

1. **UART0**
   - 本地串口调试
2. **UDP 单播** → `DEBUG_LOG_IP:DEBUG_LOG_PORT`
   - 可供独立 UDP 调试接收端使用
3. **内部环形缓冲区**
   - 被 `GET /logs` 读取
   - 这是当前 WebUI 日志面板真正使用的通道

## 为什么 `{"t":"hb"}` 是多余的

之前在 `wifi_ota_task()` 里，每 3 秒会发送一次：

```json
{"t":"hb"}
```

它是通过 `debug_log_send()` 发出的，所以会同时进入：
- UART
- UDP
- `/logs` 对应的环形缓冲区

这会导致 WebUI 把它显示成一条普通日志：

- level: `I`
- tag: `ESP32`
- message: `{"t":"hb"}`

但 WebUI 本来已经每秒轮询 `/system/status`，完全可以判断设备在线，不需要再靠这种“伪日志心跳”。

## 已处理

已删除 `wifi_ota_task()` 中每 3 秒发送一次的 `{"t":"hb"}`。

## 现在是否还有重复/不必要通信

### 对 WebUI 来说

有两条并行链路，但用途不同，不算重复：

- `/system/status`：结构化状态
- `/logs`：文本日志

这两条都保留是合理的。

### 对底层日志输出来说

日志同时发 UART + UDP + ring buffer，属于“一份日志三路镜像”。

这对调试友好，但从“最简设计”角度看，**UDP 在当前 WebUI 场景里不是必需的**。

也就是说：

- **必须保留：** ring buffer + `/logs`
- **建议保留：** UART0
- **可选保留：** UDP 6666

如果后续你想继续精简，可以做“方案2”：把 UDP 也做成可关闭，甚至默认关闭。
