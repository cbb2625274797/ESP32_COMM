## 删除"断线后切换慢速轮询"机制

**文件：** `G:\Robot_v2\DCU\ESP32_COMM\tools\app.js`

**范围：** 只删除"降速到 10s"的行为，保留 5 次失败触发的网络诊断（diagnoseNetwork）和断线 UI 提示。删除后断线时保持 1s 轮询，仅显示"未连接"并按需做网络诊断。

### 改动点

1. **删除变量 `pollSlowMode`（1600 行）**
   - 移除 `var pollSlowMode = false;   // 降速模式（10s 一次）`

2. **删除函数 `slowDownPolling()`（1671-1681 行）**
   - 整个函数移除

3. **删除函数 `restorePolling()`（1682-1695 行）**
   - 整个函数移除（保留 `startPolling`、`pausePollingForUpload`、`resumePollingAfterUpload`）

4. **简化 `startPolling()`（1662-1670 行）**
   - 删除 `pollSlowMode = false;` 这一行（其余保留）

5. **简化 `resumePollingAfterUpload()`（1714 行）**
   - `statusPollIntervalMs = pollSlowMode ? 10000 : 1000;` → `statusPollIntervalMs = 1000;`

6. **简化 `pollStatus()` 成功分支（1738-1747 行）**
   - 移除 `wasSlow` / `restorePolling()` 调用，仅保留 `consecutivePollFailures = 0;`（保留"恢复在线"的 toast 体验需要另判断；改为：若 `consecutivePollFailures > 0` 则提示已恢复并清零）

7. **简化 `pollStatus()` 失败分支的 5-fail 块（1789-1800 行）**
   - 删除 `!pollSlowMode` 条件判断中的 `pollSlowMode` 部分
   - 删除 `slowDownPolling();` 调用和"已切换为慢速轮询"日志
   - 保留 `diagnoseNetwork()` 诊断、`updateConnectionUi(false, '未连接')`、`applyDisconnectedRuntimeState()` 和诊断文本显示

8. **简化 `pollStatus()` 失败分支末尾（1828 行）**
   - `} else if (!pollSlowMode) {` → `} else {`（该 else 分支总是显示未连接状态）

9. **清理 `testConnection()`（817-820 行）**
   - 删除 `if (pollSlowMode) { addLog(...); restorePolling(); }` 块

10. **清理 `refreshSystemStatusNow()`（874 行）**
    - 删除 `if (pollSlowMode) restorePolling();`

11. **清理 `visibilitychange` 监听（1854-1855 行）**
    - 删除 `// 之前若在慢速模式...` 注释和 `if (pollSlowMode) restorePolling();`

12. **清理 IP 输入 change 监听（1887-1892 行）**
    - 简化为：IP 变化时清零失败计数并立即拉一次状态（不再依赖 restorePolling）
    - 或直接移除 `if (pollSlowMode) { ... restorePolling(); }` 块，保留监听器做基本的失败计数清零

### 不删除的部分
- `consecutivePollFailures` 变量及其计数逻辑（诊断仍需要）
- `diagnoseNetwork()` 网络诊断函数
- `lastDiagTime` / `diagResult` 诊断状态变量
- `applyDisconnectedRuntimeState()` 断线 UI 更新
- `statusPollIntervalMs` 变量（被 upload 暂停/恢复机制复用，保持 1000 默认值）

### 验证
- 浏览器打开 `tools/index.html`，确认页面加载无 JS 错误
- 模拟 ESP32 断线：连续 5 次失败后应仍触发网络诊断并显示"未连接"，但轮询间隔保持 1s
- 恢复 ESP32：应清零失败计数并提示"恢复在线"