# ESP32-C3FH4 OTA 中继 — 开发任务清单

> 每完成一条就在前面打 `-[x]`（手机/PC 编辑器支持 Markdown checkbox）

---

## T0 — 项目基础设施

- [x] **T0.1** 搭建 ESP32-C3 工程
  - 基于 ESP-IDF v5.x，创建 CMake 项目骨架
  - 配置 chip=esp32c3

- [x] **T0.2** 配置 Flash 分区表
  - 定义 gd32_ota (256KB)、otadata、spiffs、app0、app1 分区

- [x] **T0.3** 配置 GPIO 引脚
  - SPI2 (VSPI) 引脚映射到 GPIO6-9
  - CS=GPIO4

- [x] **T0.4** FreeRTOS 任务框架
  - 建立 System Monitor / OTA Relay / WiFi Server / BLE GATT 四个任务
  - 配置任务优先级和栈大小

- [x] **T0.5** 日志系统
  - 统一 printf 接口
  - Flash 日志写入接口（system_log 分区）

---

## T1 — SPI 通信层

- [x] **T1.1** SPI2 总线初始化
  - VSPI, clock=40MHz, mode=0, queue_size=1

- [x] **T1.2** SPI 同步读写接口
  - `spi_sync_transfer(cmd, payload, &recv_buf)` 阻塞式

- [x] **T1.3** 协议帧封装/解析
  - SOF (0xEB) 检测
  - CMD 路由分发
  - CRC16 Modbus 校验/生成

- [x] **T1.4** SPI 超时与重发
  - 发送后等待 ACK 超时 3s
  - ≥3 次则报错并通知 APP

---

## T2 — 安全握手与认证（阶段一）

- [x] **T2.1** 搅伴算法实现
  - 输入：时间戳 + 内存地址 + rand
  - 处理：3轮单字节 + 2轮4字节
  - 输出：16 bytes nonce_server

- [x] **T2.2** AES 加解密实现
  - 预置主密钥，AES-128 CBC 模式
  - 使用 ESP32 mbedTLS 库
  - 实现 `aes_encrypt()` / `aes_decrypt()`

- [x] **T2.3** 握手状态机
  - RELAY_HANDSHAKE_IDLE → HANDSHAKE_WAIT → CHALLENGE_SENT → CHALLENGE_RESPOND → SESSION_KEY_SENT → AUTH_COMPLETE

- [x] **T2.4** 会话密钥管理
  - K_session 存储到 RAM
  - 握手完成后生效，OTA 结束时清除

---

## T3 — OTA 中继核心（阶段二～四）

- [x] **T3.1** SEND_SIZE 发送
  - 发送固件总大小 (4B LE) 给 GD32
  - 等待 0xBA C0 + seq 确认

- [x] **T3.2** Flash 写入接口
  - `gd32_ota_partition` 追加写入
  - 支持断点续传（追加不擦写）

- [x] **T3.3** `ota_meta.json` 管理
  - 读写元数据：total_size / stored_size / last_seq / chunk_count
  - 初始写入 / 中途更新 / 完成后校验

- [x] **T3.4** 固件流式转发
  - 存储完成后，从 Flash 按 48B/packet 逐包发给 GD32
  - 每包等待 GD32 ACK

- [x] **T3.5** DATA_END 处理
  - 最后一包带 IFEND 标志
  - 发送后等待 TRANSFER_DONE (0xBA C0 + FF FF FF FF)

- [x] **T3.6** FINISH_OTA
  - 发送 0x0A AC，等待 GD32 跳转确认
  - 清理会话密钥和 RAM 缓存

- [x] **T3.7** 四阶段状态机整合
  - 串联 T3.1～T3.6，覆盖全部 15 个状态
  - 与 System Monitor 健康状态联动

---

## T4 — WiFi OTA 服务器

- [x] **T4.1** WiFi 连接管理
  - AP 模式（ESP32 作为热点）
  - STA 模式（连接路由器）
  - 连接失败自动重试

- [x] **T4.2** HTTP 服务器
  - POST `/ota/upload` 接收固件 binary
  - 返回 JSON 响应：`{"status": "ok", "progress": xx}`

- [x] **T4.3** 固件分包存储
  - 接收 APP 分包（APP→ESP32 协议：CMD + SEQ + SIZE + DATA + CRC32）
  - 写入 Flash，更新 meta

- [x] **T4.4** 存储完成后自动触发
  - 检测 `stored_size == total_size`
  - 自动开始阶段一～四

- [x] **T4.5** 进度上报
  - HTTP 响应附带当前进度百分比
  - 存储进度实时更新

---

## T5 — BLE OTA 服务器

- [x] **T5.1** BLE GATT Service 创建
  - Primary Service (OTA)
  - Control Point (UUID 0001): 写命令/通知
  - Data In (UUID 0002): 写无回应，大数据通道
  - Data Out (UUID 0003): Notify，透传 GD32 响应
  - Version (UUID 0004): 只读，ESP32 版本号

- [x] **T5.2** Control Point 写处理
  - 解析 0xF0～0xF7 命令帧
  - 转发给 OTA Relay 任务处理

- [x] **T5.3** Data In 大数据通道
  - 接收 APP 固件分包，写入 Flash
  - 与 T3.2 / T3.3 共用存储逻辑

- [x] **T5.4** Data Out 通知
  - 将 GD32 ACK 透传给 APP（BLE Notify）

- [x] **T5.5** MTU 协商
  - 设置 MTU ≥ 512 bytes
  - 支持大数据分包多次写入

---

## T6 — ESP32 自检与保护

- [x] **T6.1** System Monitor 任务
  - FreeRTOS 高优先级任务
  - 通过 mutex 与 OTA Relay 共享 health_state

- [x] **T6.2** 芯片温度检测
  - 使用 `esp_temp_sensor` API
  - 1s 轮询，三级阈值判断（NORMAL < 80°C / WARNING 80-100°C / CRITICAL > 100°C）

- [x] **T6.3** 供电电压检测
  - 使用 `esp_efuse` / `esp_adc`
  - 5s 轮询，三级阈值判断（NORMAL 2.5-3.6V / WARNING < 2.5V / CRITICAL < 2.3V）

- [x] **T6.4** WiFi RSSI 监测
  - `esp_wifi_sta_get_rssi()`
  - 10s 轮询，三级阈值（NORMAL > -70 / WARNING -70～-85 / CRITICAL < -85 dBm）

- [x] **T6.5** BLE 连接状态回调
  - BLE GAP Event 驱动更新连接状态
  - 断开时通知 APP 重连

- [x] **T6.6** 健康状态联动
  - OTA 每包发送前检查 health_state
  - WARNING：降低 SPI 速率 + 记录日志
  - CRITICAL：暂停 OTA + 通知 APP
  - SAFE_SHUTDOWN：发送 ABORT + 清除 meta + 保护性复位

- [x] **T6.7** Flash 日志写入
  - 异常事件追加写入 system_log 分区
  - 格式：`[TIMESTAMP] EVENT_TYPE: value`

---

## T7 — 错误处理与恢复

- [x] **T7.1** GD32 超时处理
  - SPI 无响应 3s → 重发同一包
  - ≥3 次 → 报错并通知 APP

- [x] **T7.2** CRC 重发机制
  - GD32 要求重发时，从 Flash 重新读取当前包并重发
  - 限次重试（建议 3 次），超限则中止 OTA

- [x] **T7.3** WiFi/BLE 断连恢复
  - WiFi 断开自动重连（STA 模式）
  - BLE 断开通知 APP，保留 meta 支持断点续传

- [x] **T7.4** OTA 中止与清理
  - SAFE_SHUTDOWN 时发送 ABORT 到 GD32
  - 清除 meta，重置状态机，释放内存

---

## T8 — 测试与验证

- [ ] **T8.1** SPI 回环测试
  - 短接 MOSI/MISO（GPIO7↔GPIO9）
  - 自发自收验证帧协议正确性

- [ ] **T8.2** 协议 Mock 测试
  - 不接 GD32，用逻辑分析仪抓 SPI 数据
  - 验证 SOF/CMD/LEN/CRC16 帧格式正确

- [ ] **T8.3** Flash 读写验证
  - 写入 256KB 数据后读出校验
  - 确认存储逻辑正确

- [ ] **T8.4** 单包刷写测试
  - 发送小固件（< 1KB）给 GD32
  - 验证 GD32 写入正确

- [ ] **T8.5** 完整 OTA 测试
  - 传输一个完整固件（≥ 100KB）
  - 验证四阶段（握手→大小→数据→结束）全流程

- [ ] **T8.6** 断点续传测试
  - 模拟传输中断（断开连接）
  - 恢复后从断点继续，验证 meta 正确恢复

- [ ] **T8.7** 自检触发测试
  - 升温验证 WARNING/CRITICAL 状态正确触发
  - 降压验证 SAFE_SHUTDOWN 正确中止 OTA

---

## 完成总结

**T0-T7 已完成代码实现：**
- T0: 项目骨架、CMake、FreeRTOS任务框架、日志系统
- T1: SPI2 (VSPI) 40MHz master驱动、同步传输、超时重试
- T2: 搅伴算法(3轮单字节+2轮4字节)、AES-128 CBC、握手状态机、会话密钥管理
- T3: 四阶段OTA状态机、Flash追加写入、ota_meta.json断点续传、固件转发
- T4: WiFi AP/STA模式、HTTP服务器(POST /ota/upload)、进度上报
- T5: BLE GATT Service、Control Point/Data In/Data Out特性、MTU≥512
- T6: System Monitor任务、温度/电压/WiFi RSSI三级监测、健康状态联动
- T7: GD32超时重试、CRC重发机制、断连恢复、SAFE_SHUTDOWN中止清理

**待验证(T8)：**
- T8.1-T8.7 需要硬件在环测试（SPI连接GD32、WiFi/BLE连接APP）

**待确认参数（代码中已TODO标注）：**
- MASTER_KEY_HERE: AES-128 主密钥（16 bytes）
- SCRAMBLE_CONST_HERE: 搅伴算法种子
- CHIP_UID_HERE: GD32芯片唯一ID
- OTA Service UUID: 需与APP侧协商