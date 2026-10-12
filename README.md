# ESP32_COMM · DCU 通信与 OTA 中枢固件

> **文档同步版本：v0.5.2（git 770ae14）· 2026-10-11**
> 本 README 是 [docs/project_introduction.html](docs/project_introduction.html)（完整白皮书，含 21 幅 SVG 原理图）的**浓缩版**，两者须同步更新（见文末「文档同步说明」）。

ESP32-C3FH4 固件，作为 GD32E508 DCU 板的通信与 OTA 中枢：

```
[上位机 PC/手机网页] --WiFi HTTP / BLE GATT--> [ESP32-C3FH4] --SPI Link v2 (13.33MHz, 5线)--> [GD32E508]
                                                    |                                              |
                                          gd32_ota 252KB 固件缓存                        3×CANFD 收发器(电机总线)
                                          app0/app1 AB 自升级                            电机 OTA CAN 二次中继
```

- **OTA 中继**：GD32 固件（六阶段安全握手 + AES-128 逐包加密转发）、ESP32 自身 AB 分区升级、电机节点经 GD32 CAN 二次烧写
- **SPI Link v2 主站**：会话 / 序号 / 信用流控 / CRC32 / HELLO 重同步 / 透明重试
- **CAN 控制面**：单帧、批量、跨通道同步突发（0xC7）、只听模式（0xC8）、遥测回收（0xC4/C5/C6）

## 硬件与分区

| 项 | 值 |
|------|------|
| 芯片 | ESP32-C3FH4（RISC-V 单核 160MHz）· 4MB Flash（DIO） |
| SPI | MOSI=5, MISO=7, SCLK=6, CS=4, DATA_READY=10；13.33MHz Mode 0（`CONFIG_DCU_SPI_CLOCK_HZ`） |
| 调试串口 | UART0 TX=21/RX=20，**3 Mbps**（115200 为 sdkconfig 控制台项）；UDP 日志 192.168.31.3:6666 |
| 欠压 | LVL7（≈2.8V，防长断电半复位） |

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| nvs | 0x9000 | 24KB | 配置 + OTA 元数据（`ota_meta`：total/stored/last_seq/checksum） |
| otadata | 0xF000 | 8KB | AB 启动标志 |
| gd32_ota | 0x11000 | 252KB | GD32/电机固件缓存（互斥共用） |
| app0 / app1 | 0x50000 / 0x190000 | 各 1.25MB | ESP32 AB 分区（回滚保护） |
| spiffs | 0x2D0000 | 1.06MB | 用户数据 |

## 架构与任务

```
接入层  server/: wifi_ota(STA/AP+路由表) · http_handlers(22 路由) · ble_ota*(GATT/窗口ACK/reorder/池)
协调层  ota/:    OtaCoordinator(9 态单例, 唯一权威入口, 忙态看门狗 150s, epoch 保护) · Esp32OtaCore
执行层  relay/ storage/: OtaRelay(7 态事件状态机) · Gd32FlashStore(擦除水位/NVS meta/SHA/包身份校验)
        monitor/: MotorOtaRelay(0xD0/D1/D2 喂数)
底层    transport/: SpiMaster(Link v2+worker 串行) · protocol(AES/nonce) · canfd_bridge(0xC0~C8)
横切    monitor/: health_monitor · debug_log(3 路) · can_feedback_task · cpu_stats · task_stack 等
```

| 任务 | 优先级 | 栈 | 职责 |
|------|-------|------|------|
| ota_relay | 23 | 2560 | GD32 转发状态机 |
| wifi_ota / ble_ota | 22 | 3072/2560 | WiFi/HTTP 与 NimBLE |
| health_mon | 21 | 3072 | 温度 1s + 5s 巡检 |
| ble_data_worker | 12 | 2560 | BLE 数据排空 |
| spi_worker | 8 | 5120 | **唯一持 SPI**，串行 Job 池(7) |
| motor_feed | 7 | 4096 | 电机 OTA 0xD1 喂数 |
| wifi_reconnect / can_fb | 5/4 | 2560/4096 | 重连退避 / 0xC5(250ms)·0xC4(1s) 周期查询 |
| cpu_stats / uart_rx | 1 | 3072 | 1Hz 统计 / 串口行→/logs |

## 核心机制速记（细节见白皮书对应章节）

- **SPI Link v2**（§5）：帧 = 16B 头（SOF A5 5A/VER 02/FLAGS/SESSION 4LE/TX_SEQ/ACK_SEQ/CREDIT/TYPE/LEN）+ 载荷 ≤236B + CRC32-MPEG2。延迟一拍全双工：transact 两步法（发送丢弃陈旧 RX → poll 按 DATA_READY 边沿读真实响应）。credit 8/8 背压，credit=0 死锁有 PB6 强制放行 + `resetCredit()` 双破除。透明重试 `kTxRetryMax=3`（同 TX_SEQ，GD32 DUP-SEQ 去重兜底）。CS 事务间隔 ≥1ms + 快速类型（0xA1/0xA3/0xC0~C5/C7/D1/F7）禁盲读防撕裂；3 连败熔断 2s；session=0 自动 HELLO 重同步（5s 冷却）。
- **安全握手**（§6）：GD32 OTA 数据面——0x00 0xAC → 36B challenge（UID 12B + nonce 回显 8B + AES(MASTER_KEY,K_session) 16B）→ `K_session = AES-128-ECB(MASTER_KEY, nonce_server)` → AES(K_session, nonce^0x55) → 0xBA 0xC0。nonce 由 scrambleGenerate（tick/栈址/0x12345678 ⊗ 每开机 RNG 扰码常数）生成。
- **GD32 OTA 中继**（§7）：RELAY_IDLE→HANDSHAKING(20×1.6s)→SIZE_NEG(poll 500ms×120，GD32 擦 Flash)→TRANSFERRING(64B 包=4B session+2B seq+48B AES+4B if_end+4B CRC32(key 混入)；预算 10s+25ms/包 ≤120s；每 32 包健康检查；单包 5 次 DATA×50ms，末包 ACK=0xF0F0F0F0)→FINISHING(0x0A 0xAC×10 重试 + PING 实证 APP 存活)。失败必发 SPI CANCEL 清 GD32 半会话。**包级防呆**：Cortex-M 向量表判 kApp 才放行（BOOT 包禁入 APP 区）。
- **ESP32 本机 OTA**（§7.4）：`OTA_WITH_SEQUENTIAL_WRITES` 边写边擦；finalize 分区读回算 SHA-256 与前端比对，不符回 `ESP_ERR_INVALID_VERSION`+设备 SHA；`mark_app_valid_cancel_rollback()` 回滚保护；期间 `setSpiPaused(true)`。
- **BLE OTA**（§8）：GATT 0xFFFF/0x0001~0x0004，MTU 512，载荷 480B；**窗口 ACK 64**（累计字节校验）+ reorder 64 槽 + 负 ACK 续传（64 包/200ms 节流）；DATA_IN 池 8×512B（曾 32KB 致收尾 OOM）；CTRL 0xF7 触发 GD32 中继 / 0xF8 ESP32 收尾 / 0xFA 重启 / 0xFB 中止（0xF9 预留未实现）；BLE 与 WiFi OTA 软挂起共存（不 deinit 协议栈）。
- **CANFD 桥接**（§10）：0xC0 单帧（最短 5B）/0xC1 统计+ID 快照分页（28B 头+条目 8+dlc，bus_state bit0~5，bit5=Silent）/0xC2 收模式（全收/过滤/CAN0→1/2 桥接）/0xC7 跨通道突发（ISR 内装载全部邮箱后回**一个** ACK，组内偏差 <1ms；单次 ≤16 帧，HTTP ≤24 帧，id=(node<<16)|suffix）/0xC8 收发器 Silent（TCAN1057AV S 脚 PA8/PB14/PB9，接收照常，不持久化，发送回 E6）。错误码 E0~E7：E2/E3(BUS_OFF/TEC>96) fatal 中止批量，E4 邮箱满，E5 电机 OTA 残留（自动 CANCEL→resetCredit→重试 START 自愈），E6 Silent，E7 被电机 OTA 独占。
- **电机 OTA**（§11）：/motor/ota 上传（整区强制擦除）→ 0xD0 START（fire-and-forget）→ feedTask 0xD1 {绝对 offset+≤232B}（重传幂等；120s 整体超时；8 败/150 WAIT → QUERY）→ 0xD2 进度。
- **监控**（§12）：健康 4 级（80°C/100°C；RSSI -80/-85/-88；堆 10KB）门控 OTA；日志三路（UART 3M 非阻塞 / UDP / 环 64→16 槽 →/logs）+ UART0 RX 泵；遥测 0xC5 电机状态 27 槽×14 页、0xC4 ADC（VCC=vin×11）、0xC6 电机版本、0xA3 初始化状态 13 模块故障码；CPU/任务栈/堆低水位快照/启动自检 16 位位图全进 /system/status。

## HTTP API（22 条路由 / 21 路径，无认证，CORS 全开）

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | /ota/upload | 固件上传（JSON 头 `{"type":"esp32"\|"gd32","total_size":N}` + 二进制流式） |
| POST | /ota/update | ESP32 OTA 收尾 `{sha256, verify_only}`（SHA 比对→set_boot→重启） |
| POST | /ota/relay | 触发 GD32 中继（前端 SHA + kApp 身份校验） |
| POST | /restart | 重启 |
| GET | /logs | 日志环 chunked 流式 |
| GET | /system/status | 全量状态（?lite=1 / ?layout=1；含 spi_link 统计、启动自检、GD32 版本/ADC） |
| GET | /system/probe · /system/heap_low_log | GD32 存活探测 / 堆低水位快照 |
| POST | /can/send_batch · /can/send_burst | 批量发送（≤128 帧）/ 跨通道突发（≤24 帧） |
| GET·POST | /can/feedback · /can/mode · /can/silent | 快照查询 / 收模式 / Silent 只听 |
| POST | /spi/send | 原始 SPI 帧透传 |
| GET | /motor/state(?bin=1) · /motor/version | 电机状态二进制协议 / 版本 |
| POST·GET | /motor/ota · /motor/ota/relay · /motor/ota/status · /motor/ota/cancel | 电机 OTA 全流程 |
| GET | /spi/speedtest | SPI 性能测试（临时） |

WiFi：STA 优先（SSID「004舰桥」，FAST_SCAN，CN 1-13），冷启动退避 2s×retry≤3 / 否则 1s，5 次耗尽或 30s 超时降级 AP（`ESP32_OTA_RELAY`/`12345678`，192.168.31.1，信道 6，≤4 台）。**无 mDNS**，访问靠固定 IP。

## BLE GATT 速查

| UUID | 特征 | 说明 |
|------|------|------|
| 0xFFFF | Service | 设备名 ESP32-OTA-RELAY |
| 0x0001 | CTRL (Write) | 0xF7/0xF8/0xFA/0xFB（0xF9 预留） |
| 0x0002 | DATA_IN (WriteNoRsp) | cmd 1B(0x00 GD32/0x01 ESP32)+seq u16+total u32+payload ≤480B |
| 0x0003 | DATA_OUT (Notify) | 0xA1 ACK 8B / 0xB1 FINISH 6B / 0xC1 SHA 35B |
| 0x0004 | VERSION (Read) | 版本+构建时间 |

## 构建与烧录

```powershell
# ESP-IDF v6.0 环境（IDF_TOOLS_PATH/IDF_PATH/PATH，或用 .devcontainer）
Set-Location "G:\Robot_v2\DCU\ESP32_COMM"
python "F:\.espressif\v6.0\esp-idf\tools\idf.py" build    # 产物 build/ESP32_COMM.bin
& "F:\.espressif\v6.0\esp-idf\tools\idf.py" -p COM15 flash monitor   # 串口烧录（恢复手段）
```

- **发版**：手动改 `main/include/version.h`（版本号+变更摘要注释+GIT_HASH）；构建时间自动生成
- **网页升级**：控制台上传 `build/ESP32_COMM.bin` → SHA 比对 → 重启（前端按 banner「OTA Relay … 启动」+ uptime<120s 判定恢复）
- **GD32**：上传 .bin/.hex → /ota/relay → 六阶段转发 → PING 确认；**电机**：/motor/ota → /motor/ota/relay

## 网页控制台

`tools/` 已移出仓库（快照 `.cache/ws_era_frontend_backup/`），`tools/serve.bat`（http.server:8080）托管，跨源调 REST API，改网页无需重编固件。页面：**index.html** OTA 控制台（双通道、阶段条、启动自检面板、堆/任务抽屉、日志）；**motor.html** 电机控制台（三轮询链路互不阻塞、波形）；**can.html / can_monitor.html**（DBC、报文编辑、收发监控）。

## 安全注意与已知限制

- ⚠️ **HTTP 无认证**（Bearer token 方案已设计未实施）；WiFi 凭据与 MASTER_KEY 占位值明文入仓（2026-08 审计项，待迁 NVS/Kconfig）
- ⚠️ AES-ECB 确定性模式；无固件签名（仅 SHA-256 完整性 + GD32 侧挑战-应答）
- Flash 必须锁 DIO（QIO 会致 OTA 读回 SHA 必错）；SPI ≥16MHz 全双工不可行
- tools/ 不在仓库；`spi_link/` 为三工程共享兄弟目录（`../../../spi_link/`），协议改动需三侧同步

## 文档同步说明（HTML ↔ README）

- [docs/project_introduction.html](docs/project_introduction.html) = 完整白皮书（算法字节级细节、状态机、SVG 图解、事故沉淀）；本 README = 浓缩速查版，**内容同源**
- **同步规则**：凡影响功能清单 / 协议与常量数值 / 状态机 / 端点表 / 实现状态的代码变更，须**同时更新两份文档**；变更摘要写 `main/include/version.h` 注释；细节以 HTML 为准；两文件顶部「文档同步版本」随变更递增（当前 v0.5.2 / 2026-10-11）
