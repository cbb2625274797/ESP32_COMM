# ESP32-C3FH4 OTA 中继项目 — 程序开发说明

**委托开发方：** 请完整执行以下所有任务，完成后停止。不要在中途停止。

**开发范围：** ESP32-C3FH4 固件开发，充当手机 APP/PC 与 GD32E508VET6 之间的 OTA 中继器。

**核心工作模式：** ESP32 通过 WiFi 或 BLE 接收上位机发送的 GD32 固件 → 存入本地 Flash → 通过 SPI 转发给 GD32 执行 OTA 升级。

> **重要：** 开发过程中如需与 GD32 固件侧确认接口（如 AES 密钥长度、搅伴算法细节等），请先按设计文档实现接口定义并预留 hook，实际参数值用占位符标注，最终由委托方补充。

---

## 参考文件

| 文件 | 路径 | 说明 |
|---|---|---|
| 架构设计文档 | `/workzone/esp32_ota_relay/esp32_ota_relay_design.md` | 完整协议定义、模块架构、状态机设计 |
| 开发任务清单 | `/workzone/esp32_ota_relay/tasks.md` | 所有 48 条开发任务的详细描述 |

**开发顺序：** T0 → T1 → T2 → T3 → T4/T5（可并行）→ T6 → T7 → T8

---

## 系统架构（必读）

```
[手机APP/PC]  --WiFi/BLE-->  [ESP32-C3FH4]  --SPI-->  [GD32E508VET6]
                                        |
                                        +-- 存储 GD32 固件到 Flash
                                        +-- 四阶段安全握手协议
                                        +-- 内置温度/电压/WiFi 自检
```

ESP32 的角色是**数据透传 + 存储中继**，GD32 OTA 的加密握手和数据传输逻辑全部由 ESP32 透传到 GD32，ESP32 本身不解析固件内容。

---

## 通信协议要点

### ESP32 ↔ GD32（SPI）

完整映射 GD32 原有 CANFD OTA 四阶段流程：

- **阶段一（安全握手）：** 0x00 AC 握手 → 16B nonce 挑战 → 握手包响应 → 0xBA C0 完成
- **阶段二（传输准备）：** 发送固件总大小 → 0xBA C0 + seq 确认
- **阶段三（数据分包）：** 每包 48B + CRC，循环发送直到最后一包带 IFEND
- **阶段四（结束跳转）：** 0xBA C0 + FF FF FF FF 完成确认 → 0x0A AC 刷写结束 → GD32 跳转 APP

帧格式：`SOF(0xEB) + CMD + LEN(2B LE) + PAYLOAD(LEN) + CRC16(2B LE)`

命令码：0xF0 HANDSHAKE_REQ / 0xF1 CHALLENGE / 0xF2 SESSION_KEY / 0xF3 SEND_SIZE / 0xF4 DATA_PACKET / 0xF5 DATA_END / 0xF6 TRANSFER_DONE / 0xF7 FINISH_OTA

### APP ↔ ESP32（WiFi HTTP / BLE GATT）

- WiFi：HTTP POST `/ota/upload`，JSON 响应进度
- BLE：4 个 GATT Characteristic（Control Point / Data In / Data Out / Version）
- APP→ESP32 使用明文帧格式（加密由 ESP32 在转发给 GD32 时处理）

---

## 存储方案（方案 A）

固件先完整存入 ESP32 Flash，再转发给 GD32：

- 分区：`gd32_ota`（256KB，用于存 GD32 固件 binary）
- 元数据文件：`ota_meta.json`（total_size / stored_size / last_seq）
- 断点续传：APP 重连后从 `last_seq + 1` 继续发送
- 存储完成后自动触发 SPI 转发流程

---

## 自检与保护

不外挂传感器，使用 ESP32 内置传感器：

- **芯片温度**（正常 < 80°C / 警告 80-100°C / 停机 > 100°C）— 1s 轮询
- **供电电压**（正常 2.5-3.6V / 警告 < 2.5V / 停机 < 2.3V）— 5s 轮询
- **WiFi RSSI**（正常 > -70 / 弱 -70~-85 / 断开 < -85 dBm）— 10s 轮询
- **BLE 连接状态** — 事件驱动

OTA 每包发送前检查健康状态，CRITICAL 时暂停，SAFE_SHUTDOWN 时中止并清除 meta。

---

## 开发要求

1. **使用 ESP-IDF v5.x**，基于 CMake 构建系统，目标芯片 `esp32c3`
2. **遵循 FreeRTOS 任务架构**：System Monitor / OTA Relay / WiFi Server / BLE GATT 四个任务
3. **AES 使用 mbedTLS**（ESP32 内置），预置主密钥用占位符 `MASTER_KEY_HERE`
4. **搅伴算法**实现需与 GD32 固件侧匹配，预留参数配置接口
5. **Flash 写入**使用 esp_partition API，追加写入不断点
6. **SPI 使用 VSPI (SPI2)**，GPIO6-9，CS=GPIO4，时钟 40MHz，mode=0
7. **所有任务完成后**，清理会话密钥、释放内存、返回完成信号
8. **不允许中途停止**，必须完成 T0～T8 所有任务后，才能停止运行或退出

---

## 开发产出要求

1. 完整可编译的 ESP32-C3 工程代码（CMake 项目结构）
2. 所有代码放在 `/workzone/esp32_ota_relay/` 目录下
3. 代码中必须有完整的 `main/` 目录（`main.c` / `app_main.c` / `ota_relay.c` / `spi_master.c` / `wifi_ota.c` / `ble_ota.c` / `protocol.c`）
4. 不得使用任何需要外部 API Key 的服务（AES、WiFi、BLE 均使用 ESP-IDF 内置库）
5. 完成后更新 `/workzone/esp32_ota_relay/tasks.md` 的进度记录

---

## GD32 固件侧依赖说明

以下接口需 GD32 固件侧提供参数后才能完整验证，请预留 hook 并用占位符标注：

| 接口 | 占位符 | 说明 |
|---|---|---|
| 主密钥 | `MASTER_KEY_HERE` | AES-128 主密钥，16 bytes |
| 搅伴算法参数 | `SCRAMBLE_CONST_HERE` | 时间戳+内存地址+rand 的固定 seed |
| 芯片唯一码 | `CHIP_UID_HERE` | GD32 芯片唯一 ID 用于密钥派生 |
| 会话密钥 K_session | - | 由 Step 4/5 双方协商生成，存储在 RAM |
| AES 算法模式 | AES-128 CBC | 由 ESP32 和 GD32 双方协商一致即可 |

---

## 联系方式

如需向委托方确认接口参数，请在代码中用 `// TODO: confirm with GD32 firmware team` 标注，不得自行假定。

**开发过程中遇到任何问题，请继续尝试解决，不要停止。**
