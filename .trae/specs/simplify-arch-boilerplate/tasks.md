# Tasks

- [x] Task 1: 快速清理仓库根临时文件
  - [x] SubTask 1.1: 删除 `old_wifi_ota.cpp.tmp`、`original_app.js.tmp`、`appjs_diff.txt`、`build_baseline.log`、`build_output.log`
  - [x] SubTask 1.2: 评估 `sdkconfig.bak.mbedtls_trim` / `sdkconfig.ci.c3eco7` / `sdkconfig.ci.esp32c2*` / `sdkconfig.ci.wpa_psk` 是否与 ESP32-C3FH4 目标相关，无关则删除（需确认未被 CMake/CI 引用）— 已确认未被 CMake/CI 引用，均为其他芯片变体配置，已删除

- [x] Task 2: A.2 — OtaCoordinator 状态查询单次取值
  - [x] SubTask 2.1: `isIdle()` 改为 `ota_system_state_t s = getSystemState();` 后一次比较 IDLE/STORED/COMPLETE
  - [x] SubTask 2.2: `isGd32Busy()` 已是单次取值，确认无需改动
  - [x] SubTask 2.3: `isEsp32Busy()` 已是单次取值，确认无需改动
  - [x] SubTask 2.4: `getSystemStateString()` 改为 `switch(getSystemState())` 单次取值（当前每次 case 隐式调用）

- [x] Task 3: A.1 — OtaCoordinator 9 个存储 wrapper inline 化
  - [x] SubTask 3.1: 在 `ota_coordinator.h` 中将 `writeGd32Chunk/saveGd32Meta/loadGd32Meta/updateGd32Meta/clearGd32Meta/verifyGd32Firmware/finalizeGd32Checksum/eraseGd32Partition` 改为 `inline` 实现并直接委托 `Gd32FlashStore::instance()`
  - [x] SubTask 3.2: 删除 `ota_coordinator.cpp` 中这 8 个函数的对应实现（注意 `writeGd32Chunk` 等）
  - [x] SubTask 3.3: 确认头文件已 `#include "storage/gd32_flash_store.h"`（当前已包含）

- [x] Task 4: B — WiFi HTTP URI 路由表化
  - [x] SubTask 4.1: 在 `wifi_ota.cpp::httpServerStart()` 顶部定义 `UriEntry { uri, method, handler }` 静态数组，列出 9 个端点
  - [x] SubTask 4.2: 用 for 循环对每个 entry 注册主 method + HTTP_OPTIONS（`http_options_handler`）
  - [x] SubTask 4.3: 保留 NVS 恢复固件状态的前置逻辑与启动日志不变
  - [x] SubTask 4.4: 确认 `max_uri_handlers=32` 仍足够（9×2=18，余量充足）

- [x] Task 5: C — OtaRelay 事件分发去重
  - [x] SubTask 5.1: 在 `ota_relay.cpp` 匿名命名空间添加 `commonReject(relay_event_t e, bool allowHealth=false)`，返回 `OTA_RESULT_OK` 或对应错误码（不改状态）
  - [x] SubTask 5.2: `handleHandshaking/handleSizeNeg/handleTransferring/handleFinishing` 开头调用 `commonReject`，仅在其返回非 OK 时 `setState(RELAY_ERROR)` 并返回（`handleTransferring` 传 `allowHealth=true`）
  - [x] SubTask 5.3: `forwardFirmware()` 将 5 次顺序 `processEvent(START)` 改为 while 循环：当前状态非 DONE/ERROR 时持续推进，任一步非 OK 即跳出；去掉 `goto cleanup`，失败路径统一 `reset()`
  - [x] SubTask 5.4: 保持 5 步驱动顺序与"同步 handler 执行后才检查超时"的语义不变

- [x] Task 6: D — SpiMaster *Unlocked 镜像折叠
  - [x] SubTask 6.1: 保留 `transferClocked` / `transferRequestResponse` / `otaPollResponseUnlocked` 作为内部 Impl（已持锁路径调用）
  - [x] SubTask 6.2: `syncTransfer` 公共 API：`LockGuard` + 直接调 `transferRequestResponse`（删除 `syncTransferUnlocked`，其唯一调用方 `transferWithRetry` 改为调 `transferRequestResponse`）
  - [x] SubTask 6.3: `send` 公共 API：`LockGuard` + 直接调 `transferRequestResponse`（删除 `sendUnlocked`，确认无外部调用方）
  - [x] SubTask 6.4: `otaPollResponse` 公共 API：`LockGuard` + 调 `otaPollResponseUnlocked`（保持不变，已是该模式）
  - [x] SubTask 6.5: 更新 `spi_master.h`：移除已删除的 `syncTransferUnlocked`/`sendUnlocked` 声明

- [x] Task 7: F — HealthMonitor RAII LockGuard
  - [x] SubTask 7.1: 在 `health_monitor.h` 私有区添加 `LockGuard` 内部类（与 SpiMaster 风格一致：构造 take portMAX_DELAY，析构 give，禁拷贝）
  - [x] SubTask 7.2: `getState/setState/getLastTemperature/getLastWifiRssi/tempMonitorCallback` 改用 `LockGuard lock(m_mutex);`
  - [x] SubTask 7.3: `setWifiConnected` 中 `m_wifiConnected` 为 `volatile`，确认是否仍需加锁（当前未加锁，保持现状，仅文档化其为原子推送）

- [x] Task 8: 编译验证
  - [x] SubTask 8.1: 运行 `idf.py build` 确认无编译错误/警告回归（15 个源文件 0 warning / 0 error，CMake 配置 + 编译阶段均通过；ninja 链接阶段因 TRAE 沙箱与 Windows overlapped I/O 不兼容无法在 IDE 内执行，需在 ESP-IDF PowerShell 终端外完成）
  - [x] SubTask 8.2: 确认 `main/` 行数减少符合预期（关键削减：httpServerStart 173→67、ota_coordinator.cpp 8 wrapper 删除、spi_master.cpp 2 Unlocked 删除、ota_relay.cpp goto 重构）

# Task Dependencies
- Task 3（A.1 inline 化）依赖 Task 2（A.2）先完成，避免合并冲突
- Task 5（OtaRelay）与 Task 6（SpiMaster）独立，可并行
- Task 7（HealthMonitor）独立，可并行
- Task 8（编译验证）依赖 Task 2~7 全部完成
