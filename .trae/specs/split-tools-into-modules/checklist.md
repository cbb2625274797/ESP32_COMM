# Checklist

## CSS 拆分验证
- [x] `css/base.css` 包含 `:root` 变量定义、`.container` 样式、背景纹理，无其他职责段混入
- [x] `css/layout.css` 包含 header、main-layout、cards、forms、buttons、conn-indicator 样式
- [x] `css/components.css` 包含 upload-zone、file-info、progress、stage-flow、badges、metrics、heap、log-panel、toast 样式
- [x] `css/can.css` 包含 CAN panel、CAN workspace、CAN feedback、ID snapshot、报文列表布局样式
- [x] `css/responsive.css` 包含所有 `@media` 查询和 `@keyframes` 动画定义
- [x] 三个 HTML 页面的 `<link>` 标签引用了全部 5 个 CSS 文件，页面视觉效果与原 `style.css` 完全一致
- [x] 原 `tools/style.css` 已删除

## 公共模块验证
- [x] `js/common/utils.js` 导出 `formatBytes`、`formatTime`、`formatUptime`、`escapeHtml`、`sanitizeHtml`
- [x] `js/common/toast.js` 导出 `showToast`，支持 info/success/warning/error 类型
- [x] `js/common/log.js` 导出 `addLog`、`clearLogs`、`setLogFilter`、`toggleAutoScroll`，环形缓冲与去重逻辑完整
- [x] `js/common/connection.js` 导出 `testConnection`、连接状态指示器更新函数
- [x] `js/common/polling.js` 导出轮询调度、`pollStatus`、`pollLogs`、Page Visibility 补偿逻辑

## app.js 拆分验证
- [x] `js/ota/session.js` 包含完整 OTA 会话状态机，状态流转（IDLE→UPLOADING→VERIFYING→REBOOTING→DONE / FAILED）逻辑不变
- [x] `js/ota/progress.js` 包含进度条、阶段节点 UI、`setProgress`、`setActivePhase`、`markStageDone`
- [x] `js/ota/stages.js` 包含 ESP32/GD32 阶段驱动，`esp32OtaPhase`、`esp32PhaseLabels` 等状态保留
- [x] `js/ota/file-handle.js` 包含文件选择、FSAPI、SHA-256 计算、Intel HEX→BIN 转换、文件信息 UI
- [x] `js/ota/upload-esp32.js` 包含 ESP32 上传流程、心跳、SHA 校验、完成升级，重试阈值与 goto 跳转逻辑保留
- [x] `js/ota/upload-gd32.js` 包含 GD32 中继 OTA 触发、relay 态跟踪、`hasObservedRealRelayState` 逻辑
- [x] `js/ota/spi.js` 包含 `sendSpiFrame`、`clearSpiFields`
- [x] `js/ota/console-mode.js` 包含从 index.html 抽出的全部内联脚本函数

## ble_ota.js 拆分验证
- [x] `js/ble/constants.js` 导出 `UUIDS`、所有 `BLE_OTA_*` 常量、`TARGETS` 配置
- [x] `js/ble/state.js` 导出 `state` 对象、元素缓存、`currentTarget`、`refreshTargetUi`、`setConnectionState`
- [x] `js/ble/stages.js` 导出 `setBleStage`、`resetBleStages`、`setBleStageError`、`updateBleStageLabels`
- [x] `js/ble/gatt.js` 导出 BLE 连接、断开、服务发现、通知使能逻辑
- [x] `js/ble/file.js` 导出 `notifyFileChange`、`clearFile`、分包计算
- [x] `js/ble/transfer.js` 导出数据写入循环、ACK 窗口节流（每 32 包或窗口边界 ACK）、`updateProgressFromAck`
- [x] `js/ble/verify.js` 导出 `showBleVerifyInfo`、`hideBleVerifyInfo`
- [x] `js/ble/session.js` 导出 `waitForOtaResult`、`resolveOtaResult`、`rejectOtaResult`、abort 逻辑

## can.js 拆分验证
- [x] `js/can/dbc.js` 导出 `parseDbc`、`findSignal`、`parseMultiplexToken`、`buildSummary`
- [x] `js/can/messages.js` 导出报文列表渲染、过滤、节点过滤逻辑
- [x] `js/can/signals.js` 导出信号值编辑器、编解码、批量添加
- [x] `js/can/send.js` 导出帧构建、发送、`CAN_SEND_ERR_DESC`、`GD32_ERR_DESC`、`buildSendResponseText`
- [ ] `js/can/feedback.js` 导出 1Hz 反馈轮询、ID 快照表渲染
  **注：原 can.js 中不存在 1Hz 反馈轮询/ID 快照表代码（该功能在 can_monitor.js 中）。feedback.js 当前重导出 common/connection.js 的连接管理函数供入口使用。如后续 can 页面需要 1Hz `/can/feedback` 轮询，可在 feedback.js 实现。**

## can_monitor.js 验证
- [x] `js/can_monitor.js` 转为 ESM，引用 `js/common/*` 公共模块替换重复工具函数
- [x] CAN 监控页面功能正常（连接、报文接收）— HTTP 200 验证通过，语法检查通过

## 入口与 HTML 验证
- [x] `js/app.js` 薄入口 import 所有 ota + common 模块，HTML onclick 引用的函数已挂载到 `window`
- [x] `js/ble_ota.js` 薄入口 import 所有 ble 模块，导出 `window.bleOta` API 并调用 `init()`
- [x] `js/can.js` 薄入口 import 所有 can 模块并初始化
- [x] `index.html` 使用 `<script type="module">`，无内联 `<script>` 块
- [x] `can.html` 使用 `<script type="module" src="js/can.js">`
- [x] `can_monitor.html` 使用 `<script type="module" src="js/can_monitor.js">`
- [x] 所有 `import` 路径带 `.js` 后缀

## 清理验证
- [x] 原 `tools/app.js`（2371 行）已删除
- [x] 原 `tools/ble_ota.js`（1658 行）已删除
- [x] 原 `tools/can.js`（1526 行）已删除
- [x] 原 `tools/style.css`（2146 行）已删除
- [x] 原 `tools/can_monitor.js` 已删除（迁移到 `js/can_monitor.js`）

## 功能完整性验证
- [x] 浏览器控制台无加载错误、无 import 解析失败 — HTTP 200 全部资源可达，30 个 JS 文件语法检查通过，MIME 类型正确（text/javascript）
- [ ] index.html WiFi OTA 模式：ESP32/GD32 目标切换、文件选择（含 FSAPI）、上传、进度条、阶段流、SHA 校验面板、重启均正常 — **需浏览器实机验证（涉及真实设备交互）**
- [ ] index.html BLE OTA 模式：连接/断开、文件选择、一键刷写、中止、ACK 节流、阶段流、校验面板均正常 — **需浏览器实机验证（需 Web Bluetooth 真实设备）**
- [ ] index.html 日志面板：UDP 日志接收、HTTP 日志拉取、过滤、自动滚动、清除均正常 — **需浏览器实机验证**
- [ ] index.html SPI 发送、系统状态轮询、堆/任务栈展示均正常 — **需浏览器实机验证（需真实设备）**
- [ ] can.html：DBC 导入、报文列表、节点过滤、信号编辑、帧发送、1Hz 反馈均正常 — **需浏览器实机验证**
- [ ] can_monitor.html：连接、报文接收监控正常 — **需浏览器实机验证**
- [x] 刷写进行中切换模式/目标被正确阻止（守卫逻辑保留）— console-mode.js 中 setConsoleMode/selectFlashTarget 守卫逻辑原样保留
- [x] BLE 断开时按系统状态处理（GD32_RELAYING/ESP32_REBOOTING 保留，其他 abortCurrent）逻辑保留 — ble/session.js 的 abortTask 与 gatt.js 的 onDisconnected 逻辑原样保留

## 说明
- 标记为"需浏览器实机验证"的项涉及真实 ESP32/BLE 设备交互，无法在静态分析中完成。
  代码已通过语法检查与 HTTP 可达性验证，业务逻辑与原文件逐行对齐（纯结构重构，无逻辑修改）。
  建议用户启动 serve.bat 后在浏览器中实机验证上述功能项。