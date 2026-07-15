# Tasks

## Phase 1: CSS 拆分（无依赖，可独立完成）

- [x] Task 1: 拆分 `style.css` 为 5 个职责文件
  - [ ] SubTask 1.1: 创建 `css/base.css`（:root 变量、reset、container、背景纹理，对应原 1~53 行）
  - [ ] SubTask 1.2: 创建 `css/layout.css`（header、main-layout、cards、forms、buttons、conn-indicator，对应原 55~210 行）
  - [ ] SubTask 1.3: 创建 `css/components.css`（upload-zone、file-info、progress、stage-flow、badges、metrics、heap、log-panel、toast，对应原 211~1401 行中非 CAN 部分）
  - [ ] SubTask 1.4: 创建 `css/can.css`（CAN panel、CAN workspace page、CAN 1Hz feedback、ID snapshot table、报文列表布局，对应原 778~1384 + 1726~2146 末尾的 CAN 段）
  - [ ] SubTask 1.5: 创建 `css/responsive.css`（@media 响应式 + @keyframes 动画 + enhanced animations，对应原 1476~1725 + 散落的 keyframes）
  - [ ] SubTask 1.6: 更新 `index.html`、`can.html`、`can_monitor.html` 的 `<link>` 为 5 个 `css/*.css` 引用，删除原 `style.css` 引用

## Phase 2: 公共模块抽取（app.js / can.js / can_monitor.js 共同依赖）

- [x] Task 2: 创建 `js/common/` 公共工具模块
  - [ ] SubTask 2.1: 创建 `js/common/utils.js` — 导出 `formatBytes`、`formatTime`、`formatUptime`、`escapeHtml`、`sanitizeHtml`（从 app.js 426~516 行 + can.js 重复实现提取）
  - [ ] SubTask 2.2: 创建 `js/common/toast.js` — 导出 `showToast`（从 app.js 484~516 行提取）
  - [ ] SubTask 2.3: 创建 `js/common/log.js` — 导出 `addLog`、`clearLogs`、`setLogFilter`、`toggleAutoScroll` 及日志环形缓冲状态（从 app.js 517~635 行提取）
  - [ ] SubTask 2.4: 创建 `js/common/connection.js` — 导出 `testConnection`、`setConnectionState`、连接指示器更新逻辑（从 app.js 636~823 行 + 1076~1138 行提取）
  - [ ] SubTask 2.5: 创建 `js/common/polling.js` — 导出 `schedulePolling`、`pollStatus`、`pollLogs`、`pausePollingForUpload`、Page Visibility 补偿（从 app.js 2051~2371 行提取）

## Phase 3: app.js 拆分（依赖 Phase 2）

- [x] Task 3: 拆分 `app.js` OTA 相关模块到 `js/ota/`
  - [ ] SubTask 3.1: 创建 `js/ota/session.js` — OTA 会话状态机（`OtaSessionState`、`otaSessionTransition`、`otaSessionReset`、`clearOtaSessionResiduals`、`otaSessionActive`、`otaSessionTerminated`），对应原 69~237 行
  - [ ] SubTask 3.2: 创建 `js/ota/progress.js` — 进度条与阶段 UI（`toggleCollapse`、`switchStageFlow`、`setProgress`、`setActivePhase`、`markStageDone`、`resetProgress`、`clearOtaResultWaiter`），对应原 239~425 行
  - [ ] SubTask 3.3: 创建 `js/ota/stages.js` — ESP32/GD32 阶段驱动（`updateEsp32StagesFromPhase`、GD32 阶段更新、`esp32OtaPhase`、`esp32PhaseLabels`），对应原 824~1138 行
  - [ ] SubTask 3.4: 创建 `js/ota/file-handle.js` — 文件选择 / FSAPI / SHA-256 / Intel HEX→BIN 转换 / 文件信息 UI（`handleFileSelect`、`clearFile`、`pickFileViaFSAPI`、`notifyFileChange`、`finalizeFileInfo`、`updateFileInfo`），合并原 1228~1466 行两个"文件处理"段
  - [ ] SubTask 3.5: 创建 `js/ota/upload-esp32.js` — ESP32 上传流程（`startUpload('esp32')`、`uploadFirmware`、`startUploadWaitHeartbeat`、`stopUploadWaitHeartbeat`、`verifyEsp32Sha256`、ESP32 完成升级），对应原 1467~1859 行
  - [ ] SubTask 3.6: 创建 `js/ota/upload-gd32.js` — GD32 中继 OTA 触发（`startUpload('gd32')`、`triggerGd32Ota`、relay 态跟踪、`hasObservedRealRelayState`），对应原 1860~2050 行
  - [ ] SubTask 3.7: 创建 `js/ota/spi.js` — SPI 帧构建与发送（`sendSpiFrame`、`clearSpiFields`），对应原 1139~1227 行

- [x] Task 4: 抽取 `index.html` 内联脚本到 `js/ota/console-mode.js`
  - 将原 431~684 行的内联 `<script>` 迁移为 ES Module
  - 包含 `setConsoleMode`、`selectFlashTarget`、`handleSharedFileSelect`、`pickSharedFirmware`、`clearSharedFirmware`、`setBleFirmwareInputFile`、`syncBleFileFromSelectedTarget`、`applySharedModeUi`、`switchStageFlow` 覆盖逻辑
  - 导出需要挂载到 `window` 的函数供 HTML onclick 使用

## Phase 4: ble_ota.js 拆分（与 Phase 3 无依赖，可并行）

- [x] Task 5: 拆分 `ble_ota.js` 为 `js/ble/` 模块
  - [ ] SubTask 5.1: 创建 `js/ble/constants.js` — 导出 `UUIDS`、`BLE_OTA_*` 协议常量、`TARGETS` 配置、`UI_BUILD`（原 5~55 行）
  - [ ] SubTask 5.2: 创建 `js/ble/state.js` — 导出 `state` 对象、`$` 元素查询辅助、`els` 元素缓存、`currentTarget`、`currentPayloadSize`、`flowTarget`、`refreshTargetUi`、`setConnectionState`、`resetUiAfterDisconnect`（原 57~430 行）
  - [ ] SubTask 5.3: 创建 `js/ble/stages.js` — 导出 `updateBleStageLabels`、`resetBleStages`、`setBleStage`、`setBleStageText`、`setBleStageError`、`bleStageIds`（原 141~206 行）
  - [ ] SubTask 5.4: 创建 `js/ble/gatt.js` — 导出 `connectBle`、`disconnect`、`onDisconnected`、服务发现、通知使能（原 431~560 行附近 connect/disconnect 段）
  - [ ] SubTask 5.5: 创建 `js/ble/file.js` — 导出 `notifyFileChange`、`clearFile`、文件准备、分包计算、`setReadyHint`（原文件准备相关段）
  - [ ] SubTask 5.6: 创建 `js/ble/transfer.js` — 导出数据写入循环、ACK 窗口节流、`updateProgressFromAck`、重传逻辑（原数据写入段）
  - [ ] SubTask 5.7: 创建 `js/ble/verify.js` — 导出 `showBleVerifyInfo`、`hideBleVerifyInfo`、SHA-256 校验比对逻辑（原 276~302 行）
  - [ ] SubTask 5.8: 创建 `js/ble/session.js` — 导出 `waitForOtaResult`、`resolveOtaResult`、`rejectOtaResult`、`abortCurrent`、`nextOtaSessionId`、`finishBelongsToWaiter`（原 207~261 + 1568~1657 行）

## Phase 5: can.js 拆分（与 Phase 3/4 无依赖，可并行）

- [x] Task 6: 拆分 `can.js` 为 `js/can/` 模块
  - [ ] SubTask 6.1: 创建 `js/can/dbc.js` — 导出 `parseDbc`、`findSignal`、`parseMultiplexToken`、`buildSummary`、`normalizeWhitespace`（原 182~335 行）
  - [ ] SubTask 6.2: 创建 `js/can/messages.js` — 导出 `filterMessages`、`renderMessageList`、`collectTransmitters`、`renderNodeFilterList`、`bindNodeFilterEvents`、`updateDbcFileInfo`（原 357~700 行附近）
  - [ ] SubTask 6.3: 创建 `js/can/signals.js` — 导出信号值编辑器、编解码、`renderSignalEditor`、批量添加逻辑、旧版 shim（原 700~1100 行附近）
  - [ ] SubTask 6.4: 创建 `js/can/send.js` — 导出 `buildSendResponseText`、`CAN_SEND_ERR_DESC`、`GD32_ERR_DESC`、帧构建与发送（原发送相关段）
  - [ ] SubTask 6.5: 创建 `js/can/feedback.js` — 导出 1Hz 反馈轮询、ID 快照表渲染（原反馈轮询段）

## Phase 6: can_monitor.js ESM 化（依赖 Phase 2）

- [x] Task 7: 将 `can_monitor.js` 转为 ES Module
  - 创建 `js/can_monitor.js` 入口，引用 `js/common/utils.js`、`js/common/connection.js` 等公共模块替换内部重复实现
  - 原文件仅 244 行，内部逻辑保持不变，仅替换工具函数为 import

## Phase 7: 入口组装与 HTML 更新

- [x] Task 8: 创建薄入口文件并更新 HTML 引用
  - [ ] SubTask 8.1: 创建 `js/app.js` 入口 — import 所有 `js/ota/*` + `js/common/*` 模块，将 HTML onclick 引用的函数挂载到 `window`（`setConsoleMode`、`selectFlashTarget`、`handleSharedFileSelect`、`pickSharedFirmware`、`clearSharedFirmware`、`startUpload`、`restartEsp32`、`sendSpiFrame`、`clearSpiFields`、`testConnection`、`refreshSystemStatusNow`、`setLogFilter`、`toggleAutoScroll`、`clearLogs`、`toggleCollapse` 等）
  - [ ] SubTask 8.2: 创建 `js/ble_ota.js` 入口 — import 所有 `js/ble/*` 模块，组装并导出 `window.bleOta` API 对象（`init`、`isUploading`、`notifyFileChange`、`clearFile`、`refreshButtons`、`refreshTargetUi` 等），调用 `init()`
  - [ ] SubTask 8.3: 创建 `js/can.js` 入口 — import 所有 `js/can/*` 模块，执行初始化
  - [ ] SubTask 8.4: 更新 `index.html` — `<script>` 改为 `type="module" src="js/app.js"` + `type="module" src="js/ble_ota.js"`，删除内联 `<script>` 块
  - [ ] SubTask 8.5: 更新 `can.html` — `<script>` 改为 `type="module" src="js/can.js"`
  - [ ] SubTask 8.6: 更新 `can_monitor.html` — `<script>` 改为 `type="module" src="js/can_monitor.js"`

## Phase 8: 清理与验证

- [x] Task 9: 删除原始大文件
  - 删除 `tools/app.js`（原 2371 行，已被 `js/app.js` + `js/ota/*` 替代）
  - 删除 `tools/ble_ota.js`（原 1658 行，已被 `js/ble_ota.js` + `js/ble/*` 替代）
  - 删除 `tools/can.js`（原 1526 行，已被 `js/can.js` + `js/can/*` 替代）
  - 删除 `tools/style.css`（原 2146 行，已被 `css/*` 替代）
  - 删除原 `tools/can_monitor.js`（已迁移到 `js/can_monitor.js`）

- [x] Task 10: 全功能验证
  - 启动 `serve.bat`，浏览器打开三个页面验证无控制台错误
  - 验证 index.html：WiFi 模式切换、ESP32/GD32 目标切换、文件选择、上传按钮状态、进度条、阶段流、日志面板、SPI 发送、系统状态轮询
  - 验证 BLE 模式：连接/断开、文件选择、一键刷写、中止、阶段流、校验面板
  - 验证 can.html：DBC 导入、报文列表、节点过滤、信号编辑、帧发送、1Hz 反馈
  - 验证 can_monitor.html：连接、报文接收监控

# Task Dependencies
- [Task 2] 依赖 [Task 1] 完成（CSS 拆分先行，避免干扰）
- [Task 3] 依赖 [Task 2] 完成（ota 模块引用 common 公共函数）
- [Task 4] 依赖 [Task 3] 完成（console-mode 引用 ota 模块导出）
- [Task 5] 依赖 [Task 2] 完成（ble 模块可能引用 common 工具）
- [Task 6] 依赖 [Task 2] 完成（can 模块引用 common 工具）
- [Task 7] 依赖 [Task 2] 完成（can_monitor 引用 common）
- [Task 5]、[Task 6] 与 [Task 3] 无依赖，可并行执行
- [Task 8] 依赖 [Task 3]、[Task 4]、[Task 5]、[Task 6]、[Task 7] 全部完成
- [Task 9] 依赖 [Task 8] 完成
- [Task 10] 依赖 [Task 9] 完成
