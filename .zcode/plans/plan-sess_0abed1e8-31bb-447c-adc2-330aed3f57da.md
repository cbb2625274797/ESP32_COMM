## 优化 toast.js + polling.js（不改用户可见行为，不破坏时序）

### A. `tools/js/common/toast.js`

1. **空值保护**：`showToast` 开头加 `var toast = document.getElementById('toast'); if (!toast) return;`，避免 toast 元素未挂载时调用方崩。其余 `toastIco` / `toastMsg` 用 `var ico = ...; var msg = ...;` 取出后判空再用。
2. **`info` 类型语义化**：保持视觉与现状一致，但修正注释。文件头注释从「支持 info / success / warning / error」改为如实描述「支持 success / error / warning（info 等同 warning）」，避免注释误导。不新增 CSS（无 caller 用 info，引入新色只会造成不一致）。

### B. `tools/js/common/polling.js`

5. **`pollLogs` 改递归 `setTimeout` + 在途保护**（修复 socket 抢占）：
   - 新增模块级 `logsInFlight` 标志和 `logsTimerId`（已有）。
   - 抽出 `scheduleLogsPoll(delayMs)`（仿 `scheduleStatusPoll`）。
   - `pollLogs` 体内首行 `if (logsInFlight) return;`；`finally` 内 `logsInFlight=false` + `scheduleLogsPoll(statusPollIntervalMs)`。
   - `startPolling` / `resumePollingAfterUpload` / `visibilitychange` 改为 `scheduleLogsPoll(0)` 或保留 `pollLogs()` 直接触发（in-flight 守卫会跳过重叠）。
   - 注意：`stages.js`、`upload-gd32.js` 的手动 `window.pollLogs()` 调用保持不变，in-flight 守卫下若正飞则跳过、由下一轮定时补上，行为安全。

### C. `tools/js/app.js`

6. **新增 `applyDeviceBusyRuntimeState(text)` helper**（放在 `applyUploadingRuntimeState` 之后，第 148 行后）：
   - 仿现有三兄弟的 DOM 模式：`#pillConn`/`#pillConnTxt`/`#connStatus`/`#connStatusTxt`/`#bConn`。
   - 第 225 行后追加 `window.applyDeviceBusyRuntimeState = applyDeviceBusyRuntimeState;`。

### D. 改动后行为一致性自检（写进 commit message，不写测试）

- 暂停上传：`pollStatus.finally` 不再重挂轮询 → 修正竞态，符合既有注释意图。
- `pollLogs` 重叠：被 in-flight 守卫挡掉 → 减少与 `/ota/upload` 抢 socket，符合既有 `pollStatus` 的设计意图。
- 重启恢复：两处统一走 `markEsp32RebootComplete`，重复 toast/log 由 `isEsp32Rebooting` 幂等守卫消除。
- 设备忙 UI：从 polling.js 内联 DOM 改为调 helper，视觉完全一致（同样的 class、同样的文案）。
- toast `info`：视觉不变（仍为 warn 色），仅注释修正。

### 不做的事

- 不改后端协议、不改 `/system/status`、`/logs` URL 或请求字段。
- 不改 `pollStatus` 的成功分支业务逻辑（重启恢复仅抽出函数，条件不变）。
- 不新增 toast CSS / 不引入 `info` 专属配色。
- 不动 `connection.js`、`log.js`、`ota/*`、`stages.js`、`upload-gd32.js`（仅 app.js 加一个 helper）。
- 不改 `scheduleStatusPoll` 公共签名、不改导出列表。

### 验证

- `git diff` 人工核对 polling.js 重构后两处重启恢复分支与原逻辑逐行等价。
- 浏览器加载 `tools/index.html`，走一遍 ESP32 OTA（上传→中继→重启→恢复），确认：
  - 上传期间无 `/system/status` 请求（DevTools Network）。
  - `/logs` 不出现重叠并发请求。
  - 重启恢复只弹一次「ESP32 已恢复在线」。
  - 设备忙时连接胶囊显示「设备忙」，与改前一致。
- 跑一次 GD32 OTA（中继期 400ms 轮询），确认 `setStatusPollIntervalMs` 路径未受影响。
- 无单元测试框架，不新增测试。

### 影响面

- 修改文件：`tools/js/common/toast.js`、`tools/js/common/polling.js`、`tools/js/app.js`（仅 +1 helper + 1 window 挂载）。
- 风险点：`pollLogs` 改递归 setTimeout 后，若某轮抛出未走 finally 会停转——`pollLogs` 全程 `try/catch/finally`，与 `pollStatus` 同模式，风险等价。