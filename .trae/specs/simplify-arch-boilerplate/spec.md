# 架构精简：去样板与重复加锁 Spec

## Why
`docs/esp32_architecture_review_2026-06-25.md` 指出当前代码存在大量零/低风险的重复样板（HTTP URI 注册粘贴、OtaCoordinator 纯转发 wrapper、OtaRelay 事件分发头部、SpiMaster *Unlocked 镜像）以及状态查询时的重复加锁。这些不影响运行时行为，但增加维护成本、放大锁争用，并留下两路代码不同步的隐患。本变更按评审推荐的顺序落地 A~F + 快速清理，预期 `main/` 减约 350~450 行，且不改运行时语义。

## What Changes
- **快速清理**：删除仓库根的临时/历史中间产物（`*.tmp`、`*.log`、`appjs_diff.txt`）。
- **A.1**：把 `OtaCoordinator` 中 9 个纯转发到 `Gd32FlashStore` 的 wrapper 改为 `inline` 并移入头文件，`.cpp` 删除对应实现。
- **A.2**：`OtaCoordinator::isIdle / isGd32Busy / isEsp32Busy` 改为一次 `getSystemState()` 取值后比较，消除多次 `lock()`。
- **B**：`wifi_ota.cpp::httpServerStart()` 的 9×{POST/GET, OPTIONS} 复制粘贴改为路由表 + 循环注册。
- **C**：`OtaRelay::handle*` 头部重复的 ABORT/TIMEOUT/HEALTH_CRIT 拒绝逻辑提取为 `commonReject` helper；`forwardFirmware()` 的 5 次顺序 `processEvent(START)` 改为状态驱动循环，去掉 `goto cleanup`。
- **D**：`SpiMaster` 的 `syncTransfer/syncTransferUnlocked`、`send/sendUnlocked`、`otaPollResponse/otaPollResponseUnlocked` 三对镜像折叠为「公共 API 加锁 → 调内部 Impl」单一实现。
- **F**：`HealthMonitor` 的 `getCurrent/getState/setState/getLastTemperature/getLastWifiRssi/tempMonitorCallback` 中重复的 `if (m_mutex) xSemaphoreTake/Give` 改为 RAII `LockGuard`（与 SpiMaster 一致）。

**不在本次范围**（评审标记为低优先级/可选/行为变更）：
- E（退役 UDP 日志通道）：涉及调试工作流行为变更，评审标为"低优先级""可考虑"，留作后续。
- G（BLE OTA finish ctx malloc→静态）：评审明确"不是精简，是健壮性"，位于 OTA 关键路径，需单独回归，留作后续。
- H（README 同步）：仅当采用 A 的"移除 wrapper"方案时才需要；本次采用 A.1（inline 保留语义），README 现有描述仍然成立，无需改动。

## Impact
- 受影响代码：
  - `main/ota/ota_coordinator.h` / `.cpp`（A.1, A.2）
  - `main/server/wifi_ota.cpp`（B）
  - `main/relay/ota_relay.h` / `.cpp`（C）
  - `main/transport/spi_master.h` / `.cpp`（D）
  - `main/monitor/health_monitor.h` / `.cpp`（F）
- 受影响能力：OTA 协调器状态查询、HTTP 路由注册、OTA 中继状态机、SPI 总线访问、健康监控状态读写。
- **不改**运行时行为：状态机 5 步驱动顺序、健康检查时机、watchdog 自愈、`forceCleanup` 仅在 `isIdle()` 启动、`Esp32OtaCore` 双层 mutex 语义均保持不变（评审"不建议改的地方"）。

## ADDED Requirements

### Requirement: HTTP 路由表化注册
系统 SHALL 使用静态路由表 + 循环注册所有 HTTP URI handler，新增端点仅需在表中追加一行。

#### Scenario: 路由注册等价
- **WHEN** `httpServerStart()` 执行
- **THEN** 所有原 9 个端点（`/ota/upload`、`/ota/relay`、`/ota/update`、`/restart`、`/logs`、`/system/status`、`/system/probe`、`/can/send`、`/spi/send`）及其 OPTIONS 预检全部注册成功
- **AND** 注册行为与原逐条 `httpd_register_uri_handler` 调用完全等价

### Requirement: OtaRelay 事件拒绝逻辑收敛
系统 SHALL 通过 `commonReject(event, allowHealth)` helper 统一各状态 handler 头部的 ABORT/TIMEOUT/HEALTH_CRIT 拒绝分支。

#### Scenario: 拒绝分支等价
- **WHEN** 任一 `handle*` 收到 `RELAY_EVT_ABORT`
- **THEN** 状态转为 `RELAY_ERROR` 并返回 `OTA_RESULT_ERR_USER_ABORT`
- **WHEN** 收到 `RELAY_EVT_TIMEOUT`
- **THEN** 状态转为 `RELAY_ERROR` 并返回 `OTA_RESULT_ERR_TIMEOUT`
- **WHEN** `handleTransferring` 收到 `RELAY_EVT_HEALTH_CRIT`
- **THEN** 状态转为 `RELAY_ERROR` 并返回 `OTA_RESULT_ERR_HEALTH`

### Requirement: SpiMaster 单一加锁路径
系统 SHALL 通过单一内部实现（`*Impl`）承载加锁与无锁两路调用，公共 API 包一层锁，已持锁路径直调 Impl，消除两份镜像逻辑。

#### Scenario: 加锁语义不变
- **WHEN** 外部调用 `syncTransfer/send/otaPollResponse`
- **THEN** 内部先获取 `m_mutex` 再执行传输
- **WHEN** 已持锁的内部路径调用 Impl 版本
- **THEN** 不重复加锁，避免递归死锁

## MODIFIED Requirements

### Requirement: OtaCoordinator GD32 存储操作入口
传输层通过 `OtaCoordinator` 的统一入口操作 GD32 Flash（解决 H3 绕过协调器问题）。9 个纯转发 wrapper（`writeGd32Chunk/saveGd32Meta/loadGd32Meta/updateGd32Meta/clearGd32Meta/verifyGd32Firmware/finalizeGd32Checksum/eraseGd32Partition`）改为 `inline` 定义在头文件中，直接委托 `Gd32FlashStore::instance()`，签名与调用方语义不变。

### Requirement: OtaCoordinator 状态查询
`isIdle / isGd32Busy / isEsp32Busy` SHALL 各仅调用一次 `getSystemState()`（即一次 `lock()`），在局部变量上完成所有比较。`getSystemStateString` 同样仅取值一次。

### Requirement: OtaRelay forwardFirmware 状态驱动
`forwardFirmware()` SHALL 以状态驱动循环推进 IDLE→HANDSHAKING→SIZE_NEG→TRANSFERRING→FINISHING→DONE，任一步返回非 OK 即跳出并复位，不再使用 `goto cleanup`。5 步驱动顺序与超时检查时机保持不变。

### Requirement: HealthMonitor 状态访问加锁
`HealthMonitor` 所有访问 `m_healthState / m_lastTemperature / m_lastWifiRssi / m_lastWifiRssiMs` 的方法 SHALL 通过 RAII `LockGuard`（构造 take、析构 give）保护，与 SpiMaster 的 LockGuard 风格一致。

## REMOVED Requirements
（无移除）
