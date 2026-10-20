# ESP32 软件架构扫描报告（2026-06-27）

## 背景
上一轮评审（2026-06-25，见 `docs/esp32_architecture_review_2026-06-25.md`）已完成 8 项优化：根目录清理、OtaCoordinator 状态查询单次取值、9 个 GD32 存储 wrapper inline 化、HTTP URI 路由表化、OtaRelay commonReject helper、SpiMaster *Unlocked 折叠、HealthMonitor RAII LockGuard、编译验证。

本轮扫描基于上述优化完成后的代码，新发现约 74 项可优化点，分三类：
- **新发现**（约 65 项）：上轮完全未涉及
- **遗留**（3 项）：上轮评审已提到但当时标"不在本次范围"
- **残留**（3 项）：上轮已做但未完全覆盖

按风险/收益分 P0 / P1 / P2 三档。

---

## P0 优先（功能正确性 / 安全性 bug）

### P0-1 metaVerifyChecksum 全零跳过校验是隐性后门
- **文件**：[main/storage/gd32_flash_store.cpp:299-306](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L299-L306)
- **问题**：checksum 全零时直接返回 `ESP_OK`，注释"旧数据兼容"。meta 被意外清零时校验会"通过"，掩盖固件损坏
- **建议**：返回 `ESP_ERR_INVALID_STATE`，由调用方决定是否容忍

### P0-2 RSSI 状态机 CRITICAL→NORMAL 无法降级
- **文件**：[main/monitor/health_monitor.cpp:333-357](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L333-L357)
- **问题**："恢复到 NORMAL"只在 `cur == SYS_WARNING` 时触发，CRITICAL 状态下 RSSI 恢复正常不会降级
- **建议**：明确状态转换表，补 CRITICAL → NORMAL 直接降级路径

### P0-3 OTA_MAX_FIRMWARE_SIZE 注释与 0x36000 不一致
- **文件**：[main/include/app_config.h:62](file:///g:/Robot_v2/DCU/ESP32_COMM/main/include/app_config.h#L62)
- **问题**：`216*1024 = 221184`，注释"对齐 GD32 APP_MAX_SIZE (0x36000)"，但 `0x36000 = 221296`，相差 112 字节，并非对齐
- **建议**：确认正确值，用十六进制表达 `0x36000` 或修正注释

### P0-4 凭证硬编码入仓库
- **文件**：[main/include/app_config.h:31,48,112-115](file:///g:/Robot_v2/DCU/ESP32_COMM/main/include/app_config.h#L31)
- **问题**：WiFi 密码、AP 密码、MASTER_KEY 明文入仓库，README 也标注"⚠️ 占位值，生产环境需更换"
- **建议**：迁移到 NVS/Kconfig + env 文件，CI 加凭证扫描规则

### P0-5 BleEsp32FinishCtx 三处 free 易漏
- **文件**：[main/server/ble_ota.cpp:854,158,190,210](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L854)
- **问题**：ctx 在 3 个不同分支释放，`if (ctx) free(ctx)` 防御式写法暗示历史 bug；第 190 行后 `restartEsp32()` 不返回，理论上 free 不执行
- **建议**：RAII 封装 `BleFinishCtxDeleter`，任务入口 `BleFinishCtxDeleter guard{ctx};`，所有 return 路径自动释放
- **备注**：上轮评审 G 项已提到（"不是精简，是健壮性，留后续"），本轮方案改为 RAII deleter（上轮原方案是改静态单例）

### P0-6 GD32 固件大小校验逻辑双通道重复
- **文件**：[main/server/ble_ota.cpp:1115-1121](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L1115-L1121) vs [main/server/http_handlers.cpp:302-308](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L302-L308)
- **问题**：两处完全相同的校验：`uint32_t rounded = ((total + PKT - 1) / PKT) * PKT; if (total == 0 || total > MAX || rounded > MAX) {...}`，但 BLE 用 `BLE_OTA_FINISH_ERR_GD32_SIZE` 上报，HTTP 用 `HTTPD_500` 返回，错误码语义不对齐
- **建议**：抽取 `OtaCoordinator::validateGd32Size(total_size) -> esp_err_t`，两路调用方各自翻译为通道特定错误回报

### P0-7 doRelayPackets 180 行 + goto + 重试阈值混乱
- **文件**：[main/relay/ota_relay.cpp:936-1115](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L936-L1115)
- **问题**：约 180 行，`for` → 内嵌 `while(1)` → 内嵌 `for` 重试 → 2 处 `goto resend_packet`（行 1057、1091）；重试计数器阈值不一致：行 1015 `>=10U`、行 1032 `>=3U`、行 1050 `>=3U`、行 1075 `>=10U`、行 1084 `>=10U`，同一 `retry_count` 既限 3 又限 10
- **建议**：拆分为 `preparePacket(seq)` + `sendPacketWithRetry(seq, tx_buf)` + 外层 `doRelayPackets`；重试阈值提取常量 `kMaxResendOnNack`/`kMaxResendOnStructError`/`kMaxResendOnPollExhaust`

### P0-8 ble_ota_process_data_batch 245 行
- **文件**：[main/server/ble_ota.cpp:975-1219](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L975-L1219)
- **问题**：单函数处理 ESP32 与 GD32 两条独立数据流，ESP32 分支内嵌 6 个子逻辑，GD32 分支内嵌 9 个子逻辑，循环内多处 `offset += packet_len; if (offset + 7 > recv_len) break;` 重复推进
- **建议**：拆分为 `process_esp32_packet(...)` 与 `process_gd32_packet(...)`，主循环只负责解包 + 分发

---

## P1 优先（数据竞争 / 资源泄漏 / 双通道一致性）

### P1-A OtaRelay 状态成员封装破坏（一组原子改动）

**头文件**：[main/relay/ota_relay.h:228-237](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.h#L228-L237)

**问题**：把 `relayState/relaySeq/relayTotalPackets/gd32AppAlive/gd32Version/taskHandle/s_relayListener/s_stateMutex` 全部声明为 `public static`，但 cpp 内部多处直接访问绕过带锁的 setter/getter，与外部带锁读取形成数据竞争：

| 位置 | 直接访问 |
|---|---|
| [ota_relay.cpp:331-332](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L331-L332) | `handleSizeNeg` 直接写 `relayTotalPackets/relaySeq` 无锁 |
| [ota_relay.cpp:1102,1104-1105](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L1102-L1105) | `doRelayPackets` 直接写读 `relaySeq` 无锁 |
| [ota_relay.cpp:1189,1197-1198,1209](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L1189-L1209) | `doFinishAndVerify` 直接写读 `gd32AppAlive/gd32Version` 无锁 |
| [ota_relay.cpp:470,478](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L470-L478) | `forwardFirmware` 直接读 `relayState` 无锁 |
| [ota_relay.cpp:1272,1288](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L1272) | `taskEntry` 直接访问 `s_relayListener` 绕过 getter |

**建议**：所有静态状态成员改 `private`，统一通过 setter/getter；relay task 内部若已持锁，在 `StateLockGuard` 块内访问。`s_stateMutex` 改 private，`StateLockGuard` 作为友元或保留 public 嵌套类。

### P1-9 getSnapshot 双重加锁且可能不一致
- **文件**：[main/ota/ota_coordinator.cpp:571-591](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L571-L591)
- **问题**：行 576-581 持锁读取 `m_sysState` 填入 `snap.sys_state` 后释放；行 582 调 `getSystemStateString()` 又内部调 `getSystemState()` 二次加锁。两次取值之间状态可能变化，导致 `snap.sys_state`（第一次取的值）与 `snap.sys_state_str`（第二次取的值）描述不同时刻状态，前端展示自相矛盾
- **建议**：让 `getSystemStateString` 接受 `ota_system_state_t` 参数（重载 `static const char* stateToString(ota_system_state_t)`），`getSnapshot` 持锁时一次性取值并就地转字符串
- **备注**：上轮 A.2 只改了 `isIdle/isGd32Busy/isEsp32Busy`，`getSnapshot` 未覆盖

### P1-10 getSystemStateString 等仍独立加锁
- **文件**：[main/ota/ota_coordinator.cpp:509-549](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L509-L549)
- **问题**：`isIdle`、`isGd32Busy`、`isEsp32Busy`、`getSystemStateString` 各自调 `getSystemState()`（加锁取值再解锁）。`if (!isIdle() && !isGd32Busy())` 这类连续调用会加锁 2~3 次
- **建议**：提供"取一次状态后多次判断"的接口，或让调用方先 `getSystemState()` 取一次再传给各判断函数
- **备注**：上轮 A.2 残留

### P1-11 esp32_ota_core finalize 3 处清理序列重复
- **文件**：[main/ota/esp32_ota_core.cpp:281-288,312-318,332-339](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/esp32_ota_core.cpp#L281-L339)
- **问题**：3 处几乎相同：`esp_ota_abort(m_handle); m_ended = false; releaseMutex(); resetState(); return xxx;`（SHA 计算失败 / SHA 不匹配 / esp_ota_end 失败）
- **建议**：提取私有方法 `finalizeCleanupAndReturn(esp_err_t code)`，或用 `goto cleanup`

### P1-12 esp32_ota_core begin 3 处清理序列重复
- **文件**：[main/ota/esp32_ota_core.cpp:126-130,135-141,151-158](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/esp32_ota_core.cpp#L126-L158)
- **问题**：3 处清理序列：分区找不到 / 固件太大 / `esp_ota_begin` 失败，模式高度相似，差异仅在是否清零 `m_handle` 和返回码
- **建议**：提取 `beginCleanupAndReturn(esp_err_t code)`，或统一出错点设置返回码后 `goto begin_fail`

### P1-13 doFinishAndVerify 100 行双职责
- **文件**：[main/relay/ota_relay.cpp:1118-1216](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L1118-L1216)
- **问题**：单函数承担两个协议阶段：(1) 发送 FINISH_OTA 并等 ACK（1120-1176）；(2) APP 存活轮询验证（1184-1213）。函数名含 "Verify"，但两个阶段职责不同、超时参数不同、重试策略不同，行 1191 `for (attempt < 30)` 与 FINISH_OTA 的 `for (retry < 10)` 嵌套语义混淆
- **建议**：拆分为 `doSendFinishOta()` + `doWaitForAppAlive()`，`doFinishAndVerify` 改为顺序调用两者并做 session 清理

### P1-14 APP 存活探测模式三处重复
- **文件**：[main/relay/ota_relay.cpp:1191-1207,1240-1261](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L1191-L1207) + [main/ota/ota_coordinator.cpp:596-615](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L596-L615)
- **问题**：同一"调 `SpiMaster::appPing` + 写 `gd32AppAlive`/`gd32Version` + 日志版本号"模式在 3 处重复：(1) `doFinishAndVerify` 轮询（30 次重试）；(2) `taskEntry` 启动探测（1 次）；(3) `OtaCoordinator::probeGd32AppOnce`（1 次）。日志格式、版本号字段打印、成功/失败处理高度相似但不完全相同
- **建议**：提取 `static void OtaRelay::probeAndCacheAppAlive(uint32_t timeout_ms, uint32_t* out_uptime)`，三处调用方分别指定重试次数和超时

### P1-15 setWifiConnected 未加锁
- **文件**：[main/monitor/health_monitor.cpp:320-323,248](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L320-L323)
- **问题**：直接写 `volatile bool m_wifiConnected`，未走 `m_mutex`。虽然 `volatile` 保证可见性，但与 `m_healthState` 等其他状态的访问模式不一致。`checkBeforeOta` 行 248 读 `m_wifiConnected` 也未加锁，存在读写竞态
- **建议**：统一通过 LockGuard 保护，或显式说明为何此字段例外（如使用 `std::atomic<bool>`）
- **备注**：上轮 F 给 HealthMonitor 加了 LockGuard，但 `m_wifiConnected` 漏了

### P1-16 温度 esp_timer_handle_t 是局部变量，资源无法回收
- **文件**：[main/monitor/health_monitor.cpp:182-194](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L182-L194)
- **问题**：`esp_timer_handle_t temp_timer` 是局部变量未存为成员。任务若被删除，定时器持续运行，回调仍会访问 `instance()` 的成员
- **建议**：将 `temp_timer` 存为成员，提供 `deinit()` 在任务删除时 `esp_timer_stop` + `esp_timer_delete`

### P1-17 metaUpdate 读-改-写非原子
- **文件**：[main/storage/gd32_flash_store.cpp:188-198](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L188-L198)
- **问题**：`metaLoad` → 修改 → `metaSave` 中间无锁，若两个任务同时调用 `metaUpdate`，会丢失更新
- **建议**：在 Gd32FlashStore 内增加 NVS 互斥锁，或将"由调用方串行化"约束以运行时断言强制

### P1-18 metaLoad NVS open 失败一律当 OK
- **文件**：[main/storage/gd32_flash_store.cpp:142-145](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L142-L145)
- **问题**：`nvs_open` 失败时直接 `return ESP_OK;`（注释说"命名空间不存在时返回全零结构"），但 NVS open 失败原因可能不止"不存在"（如 NVS 损坏），一律当 OK 会掩盖真实错误
- **建议**：区分 `ESP_ERR_NVS_NOT_FOUND`（OK + 全零）与其他错误（返回 err）

### P1-19 DebugLog init 不检查 uartInit 返回值
- **文件**：[main/monitor/debug_log.cpp:252-272](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L252-L272)
- **问题**：`uartInit();`（行 258）返回值被丢弃，UART 初始化失败仍设 `m_initialized = true` 并打印"调试日志已初始化"
- **建议**：检查返回值，失败时降级（仅 ring buffer）并在日志中标注 UART 不可用

### P1-20 ringDrainJson 容量估算与边界检查
- **文件**：[main/monitor/debug_log.cpp:116-151](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L116-L151)
- **问题**：`capacity = count * (LOG_RING_SIZE * 2) + 16`，count=64、LOG_RING_SIZE=256 → 最大 32784 字节，单次 malloc 在 ESP32-C3（~160KB heap）上风险较大；`for` 循环 `pos < capacity - 4` 的边界检查在 `first` 分支后逗号写入（行 133）未做边界检查，可能溢出
- **建议**：分块 realloc，或对单条日志超长时截断并标记

### P1-21 关键任务创建失败不停止初始化
- **文件**：[main/core/main.cpp:69-73,101-105,117-121,133-137](file:///g:/Robot_v2/DCU/ESP32_COMM/main/core/main.cpp#L69-L137)
- **问题**：任务创建失败仅 `DEBUG_ERR`，不停止后续初始化，可能导致部分功能缺失但系统"看起来正常启动"
- **建议**：关键任务（health_mon、ota_relay）失败时 `ESP_ERROR_CHECK(ESP_FAIL)` 或进入 safe state

### P1-22 s_ota_recv_buf 全局静态被双 handler 共享
- **文件**：[main/server/http_handlers.cpp:36,249,337](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L36)
- **问题**：`static uint8_t s_ota_recv_buf[OTA_BLOCK_SIZE];` 在 ESP32 与 GD32 两条路径间共享。httpd 默认单线程不会并发，但若未来 `max_open_sockets > 1` 启用并发请求，两个 `ota_upload_handler` 同时跑会互相覆盖 buffer
- **建议**：改为 handler 内栈分配（若 `OTA_BLOCK_SIZE < 4KB` 可行），或改为 per-request 分配

### P1-23 一次性任务创建失败上报不一致
- **文件**：[main/server/ble_ota.cpp:816-821,875-878,896-899](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L816-L899)
- **问题**：
  - ble_gd32_relay_wait_task 创建失败：`free(ctx); sendFinishEvent(ERR_GD32_RELAY, ...);` — 完整上报
  - ble_esp32_finish_and_reboot_task 创建失败：`free(finish_ctx); m_esp32RebootPending = false;` — **未上报 FINISH 错误事件**，前端会一直等
  - ble_esp32_restart_task 创建失败：`m_esp32RebootPending = false;` — **未上报任何错误**
- **建议**：统一"任务创建失败 → 释放 ctx → 复位 pending 标志 → 上报 FINISH 错误事件"三步

### P1-24 reorder slot malloc 失败静默丢弃
- **文件**：[main/server/ble_ota.cpp:513-518](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L513-L518)
- **问题**：malloc 失败时返回 `ESP_OK` 让上层认为已处理，但实际数据丢失。若客户端窗口已用完（无重传预算），固件写入会永久卡在缺口。注释说"靠客户端重传"但客户端无法感知设备端 malloc 失败
- **建议**：malloc 失败时至少发一次 `sendWindowAck` 报告当前 `expected`，让客户端立即重传缺口；或返回错误触发 abort

### P1-25 protocol.h 死代码
- **文件**：[main/transport/protocol.h:16-18,25-29](file:///g:/Robot_v2/DCU/ESP32_COMM/main/transport/protocol.h#L16-L29) + [main/transport/protocol.cpp:96-118](file:///g:/Robot_v2/DCU/ESP32_COMM/main/transport/protocol.cpp#L96-L118)
- **问题**：
  1. `protocol.h` 仍定义 `FRAME_SOF 0xEB`、`FRAME_MAX_PAYLOAD 64`、`FRAME_OVERHEAD 6`，但 `protocol.cpp` 注释（第 8-10 行）明确说"帧构建、AES-CBC 加解密、CRC16 等中间件代码已移除"，这三个常量无任何引用
  2. `session_ctx_t.iv[16]` 在 `sessionKeyInit` 中被 `memset(iv, 0, 16)` 后再无任何写入；`session_ctx_t.used` 初始化为 0 后再无任何代码置 1。整个 `session_ctx_t` 实际只用到 `key[16]`
- **建议**：删除 `FRAME_SOF`/`FRAME_MAX_PAYLOAD`/`FRAME_OVERHEAD`；精简 `session_ctx_t` 为 `{ uint8_t key[16]; }` 或整体移除

### P1-26 GD32 OTA 写入序列两路径重复
- **文件**：[main/server/ble_ota.cpp:1113-1208](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L1113-L1208)（BLE）vs [main/server/http_handlers.cpp:314-413](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L314-L413)（HTTP）
- **问题**：两路径都执行相同序列：`beginGd32Storage` → `saveGd32Meta` → 循环 `writeGd32Chunk` + `updateGd32Meta` → `completeGd32Storage` + `finalizeGd32Checksum`。但 HTTP 路径多了 `eraseGd32Partition` 和 `clearGd32Meta`（行 327、335），BLE 路径没有。这暗示 BLE 路径可能依赖前次 HTTP 上传后的已擦除状态，或在 `beginGd32Storage` 内隐式擦除——这种隐式假设是 bug 温床
- **建议**：抽取 `OtaCoordinator::storeGd32FirmwareFromStream(...)` 接收"读块回调"，BLE 与 HTTP 各自实现回调，统一擦除/写入/meta/完成序列

---

## P2 优先（样板抽取 / 配置化 / 风格统一）

### P2-A 跨模块样板抽取（高 ROI）

| # | 主题 | 涉及位置 |
|---|---|---|
| P2-27 | Hex 编解码工具统一 | [http_handlers.cpp:460-463,558-562,629-633](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L460-L633)、[ble_ota.cpp:174-178,642-649,861-863](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L174-L863) 共 6+ 处。**关键 bug**：SHA-256 在 BLE 路径大小写混用（`shaBytesToHexUpper` 大写 vs 行 861-863 `%02x` 小写），HTTP 路径用大写 `%02X`，前端比对 SHA 时大小写敏感会出错 |
| P2-28 | SHA-256 → hex 循环 | [esp32_ota_core.cpp:230-234,254-257,290-293,323-325](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/esp32_ota_core.cpp#L230-L325) 4 处 + [gd32_flash_store.cpp:283-286,316-320](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L283-L320) 2 处。提取 `static void sha256ToHex(const uint8_t in[32], char out[65])` |
| P2-29 | HTTP JSON body 接收+解析 | [http_handlers.cpp:71-95,436-452,522-535,595-608](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L71-L608) 4 处。缓冲区上限不一（256/512）。提取 `http_recv_json(httpd_req_t*, char*, size_t, cJSON**)` |
| P2-30 | HTTP 错误响应样板 | [http_handlers.cpp](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp) 17 处 `httpd_resp_send_err + return ESP_FAIL`。提取 `http_send_json_error/http_send_json_ok` |
| P2-31 | 忙状态判断 if 链 | [ota_coordinator.cpp:136-140](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L136-L140)（beginGd32Storage） vs [ota_coordinator.cpp:346-350](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L346-L350)（beginEsp32Ota）。提取 `bool isAnyChannelBusy() const` |
| P2-32 | 三个校验函数重复 metaLoad 样板 | [gd32_flash_store.cpp:265-275,292-298,331-351](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L265-L351) |

### P2-B 函数过长 / 高圈复杂度

| # | 函数 | 行数 | 文件 |
|---|---|---|---|
| P2-33 | ble_ota_process_data_batch | 245 行 | [ble_ota.cpp:975-1219](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L975-L1219)（见 P0-8） |
| P2-34 | ota_upload_handler | 240 行 | [http_handlers.cpp:180-420](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L180-L420)。ESP32/GD32 双路径混杂，共享 `pre_read_buf`/`pre_read_len`。拆为 `handle_esp32_upload`/`handle_gd32_upload` |
| P2-35 | ble_ota_ctrl_access | 190 行 | [ble_ota.cpp:717-907](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L717-L907)。3 命令分支长度极不均（0xF7 90 行 / 0xF8 54 行 / 0xFA 19 行），`session_id` 提取重复 |
| P2-36 | doRelayPackets | 180 行 + goto | [ota_relay.cpp:936-1115](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L936-L1115)（见 P0-7） |
| P2-37 | doFinishAndVerify | 100 行双职责 | [ota_relay.cpp:1118-1216](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L1118-L1216)（见 P1-13） |
| P2-38 | app_main | 124 行 | [main.cpp:30-154](file:///g:/Robot_v2/DCU/ESP32_COMM/main/core/main.cpp#L30-L154)。拆为 `initEarlyServices` + `createOtaTasks` + `markAppValid` |

### P2-C 配置化（magic number → app_config.h）

| # | 主题 | 位置 |
|---|---|---|
| P2-39 | 任务栈/优先级散落硬编码 | [main.cpp:63,65,95,97,111,113,127,129](file:///g:/Robot_v2/DCU/ESP32_COMM/main/core/main.cpp#L63)、[ble_ota.cpp:812,871,892,1723](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L812)、[wifi_ota.cpp:623](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/wifi_ota.cpp#L623)、[http_handlers.cpp:905-913](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L905-L913)（task_metrics 必须手工同步，已出现一处不对齐风险） |
| P2-40 | HTTP server config | [wifi_ota.cpp:419-424](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/wifi_ota.cpp#L419-L424)（`max_uri_handlers=32` 实际只用 18） |
| P2-41 | BLE 连接参数 | [ble_ota.cpp:1395-1401](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L1395-L1401)（itvl/latency/supervision） |
| P2-42 | AP 静态 IP | [wifi_ota.cpp:234-236,240,258-259](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/wifi_ota.cpp#L234-L259) |
| P2-43 | BLE_OTA_ACK_WINDOW 位置 | [ble_ota.cpp:58](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L58) 用 `#define` 应移入 app_config.h 作 `inline constexpr` |
| P2-44 | debug_log magic number | [debug_log.cpp:199,317,352,365,369](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L199)（1024/512/600/32） |
| P2-45 | 健康巡检周期 | [health_monitor.cpp:190,208](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L190)（1000000/5000） |
| P2-46 | 温度采样间隔 | [health_monitor.cpp:190](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L190) |

### P2-D 死代码 / 未使用配置

| # | 项 | 位置 |
|---|---|---|
| P2-47 | `DEBUG_LOG_ENABLED` 全代码库零引用 | [app_config.h:83](file:///g:/Robot_v2/DCU/ESP32_COMM/main/include/app_config.h#L83) |
| P2-48 | `GD32_CHIP_UID[]` 占位 | [app_config.h:120-128](file:///g:/Robot_v2/DCU/ESP32_COMM/main/include/app_config.h#L120-L128) |
| P2-49 | `BLE_OTA_PACKET_OVERHEAD` 零引用 | [app_config.h:104](file:///g:/Robot_v2/DCU/ESP32_COMM/main/include/app_config.h#L104) |
| P2-50 | `WIFI_RSSI_WEAK` 零引用 | [health_monitor.h:33](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.h#L33) |
| P2-51 | `DEBUG_LOG_NONE/ALL` 零引用 | [debug_log.h:35,40](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.h#L35) |
| P2-52 | `WIFI_STA_USE_STATIC_IP` 临时调试开关 | [app_config.h:36](file:///g:/Robot_v2/DCU/ESP32_COMM/main/include/app_config.h#L36) |
| P2-53 | `ota_snapshot.h` 仅聚合无价值 | [ota_snapshot.h](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_snapshot.h) 全文 |

### P2-E 其他低风险风格类

| # | 项 | 位置 |
|---|---|---|
| P2-54 | SpiMaster 公共接口混合多个抽象层 | [spi_master.h:60-100](file:///g:/Robot_v2/DCU/ESP32_COMM/main/transport/spi_master.h#L60-L100)（上轮 D 残留） |
| P2-55 | DebugLog 未用 RAII（与 HealthMonitor 风格不一） | [debug_log.cpp:53-60,78-108,162-174](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L53-L174) |
| P2-56 | DebugLog logLevel 暴露在 public | [debug_log.h:86](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.h#L86) |
| P2-57 | DebugLog 三路 sink 抽象为列表 | [debug_log.cpp:300-333](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L300-L333)（上轮 E 项遗留） |
| P2-58 | DebugLog send() 返回值无人检查，可改 void | [debug_log.cpp:329-332](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L329-L332) |
| P2-59 | ensureUdpSocket 失败后每条日志都重试 | [debug_log.cpp:278-292](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L278-L292) |
| P2-60 | DebugLog log() 三次缓冲区拼接 | [debug_log.cpp:342-373](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L342-L373) |
| P2-61 | DebugLog 头注释与实现不一致（UDP 无开关） | [debug_log.h:6-7](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.h#L6-L7) vs [debug_log.cpp:311-324](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L311-L324) |
| P2-62 | DebugLog 重复 include | [debug_log.h:23-24](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.h#L23-L24) vs [debug_log.cpp:20,23](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/debug_log.cpp#L20) |
| P2-63 | health_monitor WIFI_RSSI_NORMAL 别名位置错误 | [health_monitor.h:32-33](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.h#L32-L33) |
| P2-64 | health_state_t 与 health_status_t 两套枚举语义重叠 | [health_monitor.h:16-29](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.h#L16-L29) |
| P2-65 | health_monitor 阈值检查样板可抽 applyThresholdAlarm | [health_monitor.cpp:129-156,333-357](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L129-L357) |
| P2-66 | health_monitor checkBeforeOta 与 checkBeforeOtaRelay 分级样板重复 | [health_monitor.cpp:232-311](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L232-L311) |
| P2-67 | health_monitor taskEntry 职责混杂 | [health_monitor.cpp:165-210](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L165-L210) |
| P2-68 | health_monitor logEvent 与相邻 DEBUG 重复日志 | [health_monitor.cpp:220-223](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L220-L223) |
| P2-69 | health_monitor tempMonitorCallback 注释硬编码温度值 | [health_monitor.cpp:293](file:///g:/Robot_v2/DCU/ESP32_COMM/main/monitor/health_monitor.cpp#L293) |
| P2-70 | gd32_flash_store NVS 字段名硬编码 | [gd32_flash_store.cpp:136-185](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L136-L185) |
| P2-71 | gd32_flash_store tag "OTA" 与模块名不一致 | [gd32_flash_store.cpp](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp) 全文 |
| P2-72 | gd32_flash_store SHA 长度 32 裸数字 | [gd32_flash_store.h:26](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.h#L26) 等 |
| P2-73 | gd32_flash_store flashWriteChunk 错误清理重复 | [gd32_flash_store.cpp:74-108](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L74-L108) |
| P2-74 | gd32_flash_store computeFlashSha256 错误清理重复 | [gd32_flash_store.cpp:224-262](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L224-L262) |
| P2-75 | gd32_flash_store 全零 checksum 检查可简化为 memcmp | [gd32_flash_store.cpp:299-302](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L299-L302) |
| P2-76 | gd32_flash_store flashWriteChunk 非对齐警告条件恒真 | [gd32_flash_store.cpp:65-72](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L65-L72) |
| P2-77 | gd32_flash_store m_partition 懒初始化无线程安全 | [gd32_flash_store.cpp:30-40](file:///g:/Robot_v2/DCU/ESP32_COMM/main/storage/gd32_flash_store.cpp#L30-L40) |
| P2-78 | main.cpp 4 处 xTaskCreatePinnedToCore 样板 | [main.cpp:60-137](file:///g:/Robot_v2/DCU/ESP32_COMM/main/core/main.cpp#L60-L137) |
| P2-79 | main.cpp 启动横幅分隔线硬编码 4 次 | [main.cpp:50,56,149,153](file:///g:/Robot_v2/DCU/ESP32_COMM/main/core/main.cpp#L50-L153) |
| P2-80 | main.cpp xRet 变量重复使用 | [main.cpp:59,69,92,101,108,117,124,133](file:///g:/Robot_v2/DCU/ESP32_COMM/main/core/main.cpp#L59-L133) |
| P2-81 | http_handlers.cpp 单文件 TAG 混用 OTA/WIFI/SPI | [http_handlers.cpp](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp) 全文 |
| P2-82 | canFdSend 返回 -1 无法区分失败模式 | [canfd_bridge.cpp:59-68](file:///g:/Robot_v2/DCU/ESP32_COMM/main/transport/canfd_bridge.cpp#L59-L68) |
| P2-83 | otaCrc8 实为 XOR 校验和，命名误导 | [spi_master.cpp:180-187](file:///g:/Robot_v2/DCU/ESP32_COMM/main/transport/spi_master.cpp#L180-L187) |
| P2-84 | otaParseFrame/otaFindFrame 验证逻辑重复 | [spi_master.cpp:225-279](file:///g:/Robot_v2/DCU/ESP32_COMM/main/transport/spi_master.cpp#L225-L279) |
| P2-85 | CAN 帧 vs SPI OTA 帧两层 framing 未抽象 | [canfd_bridge.cpp:12-51](file:///g:/Robot_v2/DCU/ESP32_COMM/main/transport/canfd_bridge.cpp#L12-L51) + [spi_master.cpp:198-279](file:///g:/Robot_v2/DCU/ESP32_COMM/main/transport/spi_master.cpp#L198-L279) |
| P2-86 | BLE OTA WiFi 共存标志位冗余 | [ble_ota.cpp:226-250](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/ble_ota.cpp#L226-L250) |
| P2-87 | http_handlers task_metrics 硬编码任务栈大小 | [http_handlers.cpp:905-913](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L905-L913) |
| P2-88 | notifyRelayStarted/Complete 纯转发中间层 | [ota_coordinator.cpp:264-307](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L264-L307) |
| P2-89 | notifyEsp32FinalizeComplete 与 notifyEsp32Rebooting 几乎相同 | [ota_coordinator.cpp:409-428](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L409-L428) |
| P2-90 | doFinishAndVerify 成功路径与 reset() session 清理重复 | [ota_relay.cpp:1178-1182 vs 487-504](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L1178-L1182) |
| P2-91 | metaLoad 单次中继被调 3 次 | [ota_coordinator.cpp:213-221](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L213-L221) + [ota_relay.cpp:311-316,438-440](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L311-L440) |
| P2-92 | forwardFirmware 中 expected_ack=0xF0F0F0F0 魔法数字 | [ota_relay.cpp:1024](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L1024) |
| P2-93 | doRelayPackets:918 WARN 级别输出纯配置信息（调试残留） | [ota_relay.cpp:918](file:///g:/Robot_v2/DCU/ESP32_COMM/main/relay/ota_relay.cpp#L918) |
| P2-94 | app_config 配置项类型不一致（uint8_t 应为 bool） | [app_config.h:25,36,83](file:///g:/Robot_v2/DCU/ESP32_COMM/main/include/app_config.h#L25) |

---

## 不建议改的地方（保留上轮结论）

- OTA 事件状态机的 5 步驱动顺序、健康检查时机、`OtaCoordinator` 的 watchdog 自愈、`forceCleanup` 仅在 `isIdle()` 启动。
- `Esp32OtaCore::s_mutex + s_occupied` 双层 mutex 语义（"持锁查占用" + "持续占用期"）。
- 单例模式已统一（`static XXX inst; return inst;` + `= delete` 拷贝构造/赋值）。
- 无 TODO/FIXME/XXX/HACK 标记，无注释掉的死代码。

---

## 预估收益

- 修复 4 项功能性 bug（校验后门 / RSSI 状态机降级 / 固件大小注释 / 凭证安全）。
- 消除 6 处数据竞争（OtaRelay 状态访问 + setWifiConnected）。
- 6 个超长函数拆分（245/240/190/180/124/100 行），可维护性显著提升。
- `main/` 行数预计再减约 400~500 行（去重 + 死代码）。
- 双通道（BLE/HTTP）一致性通过 `validateGd32Size` / `storeGd32FirmwareFromStream` 抽取彻底对齐。
- protocol.h / app_config.h 死代码清理约减 50+ 行。

---

## 推荐落地顺序

1. **第 1 批（P0）**：P0-1 ~ P0-8。先修功能正确性 / 安全性 bug。P0-7/P0-8 函数拆分风险高，需配合 GD32 全链路 OTA 回归。
2. **第 2 批（P1.A）**：OtaRelay 状态成员封装一组（P1-A + P1-15），消除数据竞争，原子改动。
3. **第 3 批（P1.B）**
4. **第 4 批（P2.A/B）**：样板抽取（P2-27 ~ P2-32）+ 函数拆分（P2-33 ~ P2-38）。
5. **第 5 批（P2.C/D/E）**：配置化（P2-39 ~ P2-46）+ 死代码删除（P2-47 ~ P2-53）+ 风格统一（P2-54 ~ P2-94），可随业务变更顺手做。
