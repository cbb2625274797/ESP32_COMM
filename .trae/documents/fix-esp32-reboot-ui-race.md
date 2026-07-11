# 修复 ESP32 OTA 重启 UI 竞态问题

## 问题描述

前端显示 `SHA-256 校验通过，设备自动重启中...` 消息后，"重启中"阶段 UI 不会稳定触发（仅偶发触发一次）。

## 根因分析

**后端时序**（[http_handlers.cpp](file:///g:/Robot_v2/DCU/ESP32_COMM/main/server/http_handlers.cpp#L148-L152) + [ota_coordinator.cpp](file:///g:/Robot_v2/DCU/ESP32_COMM/main/ota/ota_coordinator.cpp#L550-L556)）：
1. `httpd_resp_send()` 发送 HTTP 响应（L148）
2. `vTaskDelay(500ms)` 等待 TCP 刷新（L151）
3. `restartEsp32()` → `notifyEsp32Rebooting()` + `vTaskDelay(500ms)` + `esp_restart()`（L152）

即：**HTTP 响应发出后，设备仍存活约 1000ms 才真正重启**。

**前端竞态**（[app.js](file:///g:/Robot_v2/DCU/ESP32_COMM/tools/app.js#L1496-L1498)）：
1. `verifyEsp32Firmware()` 收到 `/ota/update` 成功响应后，在 L1496 设置 `isEsp32Rebooting = true`，L1497 推进阶段到 5"重启中"。
2. 此时轮询以 1s 间隔运行（上传结束后 `resumePollingAfterUpload()` 已恢复 1s 间隔）。
3. **在 ~1000ms 的 pre-reboot 窗口内，设备仍能响应 `/system/status` 轮询**。
4. `pollStatus()` 成功路径（[app.js:1740-1745](file:///g:/Robot_v2/DCU/ESP32_COMM/tools/app.js#L1740-L1745)）发现 `isEsp32Rebooting === true`，**立即清除标志并推进到阶段 6"启动完成"+ 弹出"ESP32 已恢复在线"** —— 全部错误。
5. 当设备 ~1s 后真正重启时，轮询失败，但 `isEsp32Rebooting` 已被清为 false，失败分支走"设备忙"或"未连接"，**永远不会再显示"重启中"**。

**偶发触发的原因**：仅当轮询时序恰好使得 pre-reboot 窗口内没有成功的轮询（例如轮询正在 in-flight 且设备重启打断它）时，"重启中"UI 才能正确显示。

## 修复方案

引入 `rebootObservedDown` 标志，要求"先观察到设备下线，再接受其恢复在线"，避免在 pre-reboot 窗口内被成功轮询误清除重启状态。

### 改动文件：[tools/app.js](file:///g:/Robot_v2/DCU/ESP32_COMM/tools/app.js)

#### 1. 新增全局标志（L28 附近）

在 `let rebootStartTime = 0;` 下方新增：
```js
let rebootObservedDown = false;  // 是否已观察到设备实际下线（重启）
```

#### 2. arming 点 1：`verifyEsp32Firmware()` 成功路径（L1496-L1498）

```js
isEsp32Rebooting = true;
rebootObservedDown = false;   // ← 新增
updateEsp32StagesFromPhase(5);  // 重启中
rebootStartTime = Date.now();
```

#### 3. arming 点 2：`restartEsp32()` 手动重启（L1531-L1532）

```js
isEsp32Rebooting = true;
rebootObservedDown = false;   // ← 新增
rebootStartTime = Date.now();
```

#### 4. `pollStatus()` 成功路径（L1740-L1746）—— 核心修复

将无条件清除改为"先确认设备下线过再清除"：

```js
if (isEsp32Rebooting) {
  var rbElapsed = Math.round((Date.now() - rebootStartTime) / 1000);
  if (!rebootObservedDown && rbElapsed < 10) {
    // pre-reboot 窗口：设备尚未实际重启（后端有 ~1s 延迟才 esp_restart）
    // 保持"重启中"状态，不清除标志，不推进阶段
    addLog('I', '系统', '设备尚未实际重启，等待 esp_restart...');
  } else {
    // 设备已重启并恢复在线（rebootObservedDown=true）
    // 或超 10s 安全阈值仍未观察到下线（重启过快未捕获或未重启）——接受恢复
    isEsp32Rebooting = false;
    rebootObservedDown = false;
    updateEsp32StagesFromPhase(6);  // 启动完成
    addLog('I', '系统', 'ESP32 已恢复在线');
    showToast('ESP32 已恢复在线', 'success');
  }
}
try { updateSystemStatusUi(data); } catch (e2) { console.warn('updateSystemStatusUi 异常:', e2); }
// pre-reboot 窗口内 updateSystemStatusUi 会把连接胶囊刷成"已连接"，需覆盖回"重启中"
if (isEsp32Rebooting) {
  applyRebootingRuntimeState();
}
```

#### 5. `pollStatus()` 失败路径 isEsp32Rebooting 分支（L1781-L1791）—— 标记已下线

```js
} else if (isEsp32Rebooting) {
  rebootObservedDown = true;  // ← 新增：确认设备已下线（正在重启）
  var elapsed = Math.round((Date.now() - rebootStartTime) / 1000);
  if (elapsed > 60) {
    isEsp32Rebooting = false;
    rebootObservedDown = false;  // ← 新增
    addLog('E', '系统', 'ESP32 重启超时，设备未恢复');
    showToast('ESP32 重启超时，请检查设备', 'error');
    updateConnectionUi(false, '未连接');
    applyDisconnectedRuntimeState();
  } else {
    applyRebootingRuntimeState();
  }
}
```

## 时序验证（修复后）

| 时间 | 事件 | isEsp32Rebooting | rebootObservedDown | UI |
|------|------|------------------|--------------------|-----|
| t=0 | 收到 /ota/update 响应，arming | true | false | 阶段5"重启中" |
| t=0~1s | pre-reboot 窗口，轮询成功 | true | false | 保持"重启中"（不清除）✓ |
| t=1s | esp_restart()，设备下线 | true | false | — |
| t=1~2s | 轮询失败 | true | **true** | "重启中 Ns" ✓ |
| t=3~4s | 设备重启完成，轮询成功 | → false | → false | 阶段6"启动完成" ✓ |

## 假设与决策

- **10s 安全阈值**：ESP32 重启（boot ROM + bootloader + app init + WiFi init）通常 1-3s，1s 间隔轮询至少 1-2 次失败，`rebootObservedDown` 必然被设置。10s 阈值仅用于兜底（设备未实际重启或重启极快未捕获到下线）。
- **不修改后端时序**：后端 1000ms 延迟（500ms TCP 刷新 + 500ms reboot 通知窗口）是必要的（确保响应送达 + 状态上报），问题在前端竞态处理。
- **仅修改 WiFi 路径**：BLE 路径使用事件驱动（0x07 REBOOT_NOW + 断连重连），不受此竞态影响。

## 验证步骤

1. 通过 WiFi 上传 ESP32 固件并完成 SHA-256 校验
2. 确认显示 `SHA-256 校验通过，设备自动重启中...` 后，阶段稳定停留在 5"重启中"，连接胶囊显示"重启中 Ns"计数
3. 不应出现阶段跳到 6"启动完成"再回退的情况
4. 等待设备实际重启完成，确认阶段推进到 6"启动完成"并弹出"ESP32 已恢复在线"
5. 多次重复验证（此前为偶发问题，修复后应 100% 稳定）
