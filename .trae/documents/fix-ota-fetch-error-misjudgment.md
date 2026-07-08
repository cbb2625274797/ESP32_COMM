# 修复 OTA 校验请求异常误判为"设备已重启"Plan

## Summary
当前端在 OTA 校验/应用阶段（`POST /ota/update`）发生 fetch 网络异常（如 "Failed to fetch"）时，代码无条件假设"设备已重启"并进入 60 秒重启等待。但服务端实际要先完成 `finalizeForReboot`（SHA 计算可能耗时数十秒）才发送响应、再延迟 500ms 重启。fetch 异常发生时设备可能仍在 finalize、可能崩溃、也可能只是网络抖动——并非一定是正常重启。这种误判掩盖了真实失败原因，让用户长时间等待一个不会发生的"重启完成"。

需将 catch 块改为：**如实报告请求失败，不臆测设备状态，由后续 `pollStatus` 探测设备真实状态**。

## Current State Analysis

### 问题代码位置
- `tools/app.js` L1616-1623：`verifyEsp32Firmware()` 的 catch 块
- `tools/app.js` L1676-1681：`applyEsp32Firmware()` 的 catch 块（相同误判模式）

### 当前 catch 块行为（两处一致）
```javascript
} catch (e) {
  addLog('W', 'OTA', '应用请求出现异常（设备可能已重启）: ' + e.message);
  // 设备在收到请求后即会重启，网络中断属于正常现象，直接进入重启等待
  showVerifyActions('none');
  isEsp32Rebooting = true;            // ← 无条件假设重启
  updateEsp32StagesFromPhase(5);      // ← UI 切到"重启中"
  rebootStartTime = Date.now();
}
```

### 服务端实际时序（`http_handlers.cpp` L137-153）
1. 收到 `/ota/update` → 调用 `finalizeForReboot()`（含 `computeShaFromPartition` 读回整分区算 SHA，大固件耗时数秒到十几秒）
2. finalize 成功 → 发送 HTTP 响应 `{"status":"ok",...}`
3. `vTaskDelay(500ms)` → `restartEsp32()` → `esp_restart()`

**只有第 2 步成功发送响应后设备才会重启**。fetch 异常说明响应未到达前端，设备大概率未正常重启。

### 误判的连锁影响
- 设置 `isEsp32Rebooting = true` → `pollStatus`（L1953-1963）进入 60 秒"重启中"等待分支
- 跳过 L1939 的"5 次失败诊断"逻辑，不会降速、不会显示"未连接"
- 60 秒后才报"ESP32 重启超时"——用户白等一分钟，且看不到真实失败原因
- 若设备实际仍在 finalize 或已崩溃，`/system/status` 也无法响应，但 UI 一直显示"重启中"

### 对照：上传阶段的正确处理（L1532-1537）
```javascript
xhr.addEventListener('error', function() {
  cleanupAfterOtaFailure('网络错误');
});
```
上传阶段网络错误会调用 `cleanupAfterOtaFailure`，如实标记失败。校验/应用阶段应保持一致。

## Proposed Changes

### 文件：`g:\Robot_v2\DCU\ESP32_COMM\tools\app.js`

#### 改动 1：`verifyEsp32Firmware()` catch 块（L1616-1623）

**What**: 移除"假设设备已重启"的逻辑，改为如实报告请求失败，并启动轮询探测设备真实状态。

**Why**: fetch 异常只代表请求未成功完成，不能推断设备状态。设备可能仍在 finalize、可能已崩溃、可能网络抖动。应由 `pollStatus` 探测后给出准确结论。

**How**: 替换为：
```javascript
} catch (e) {
  // fetch 异常仅代表请求未完成，不能推断设备已重启：
  // 设备可能仍在 finalize（SHA 计算耗时）、可能已崩溃、可能网络抖动。
  // 如实报告失败，由 pollStatus 探测设备真实状态。
  addLog('E', 'OTA', '应用请求失败: ' + e.message + '（设备状态未知，等待轮询探测）');
  showToast('应用请求失败: ' + e.message, 'error');
  var resultEl = document.getElementById('sviResult');
  if (resultEl) { resultEl.textContent = '✗ 请求失败: ' + e.message; resultEl.className = 'svi-val err'; }
  showVerifyActions('retry');
  cleanupAfterOtaFailure('应用请求失败');
}
```

要点：
- 日志级别 W → E（请求失败是错误，不是警告）
- 日志文案去掉"设备可能已重启"臆测，改为"设备状态未知，等待轮询探测"
- 不再设置 `isEsp32Rebooting = true`，让 `pollStatus` 走正常失败诊断路径
- 调用 `cleanupAfterOtaFailure` 标记失败态（与上传阶段错误处理一致）
- 显示 retry 按钮（`showVerifyActions('retry')`）让用户可重试

#### 改动 2：`applyEsp32Firmware()` catch 块（L1676-1681）

**What**: 同样的修复，移除重启假设。

**Why**: 与改动 1 相同的误判逻辑，需保持一致。

**How**: 替换为：
```javascript
} catch (e) {
  // fetch 异常仅代表请求未完成，不能推断设备已重启。
  addLog('E', 'OTA', '应用请求失败: ' + e.message + '（设备状态未知，等待轮询探测）');
  showToast('应用请求失败: ' + e.message, 'error');
  cleanupAfterOtaFailure('应用请求失败');
}
```

要点：
- 日志级别 W → E
- 不再设置 `isEsp32Rebooting = true`
- 调用 `cleanupAfterOtaFailure` 标记失败态

## Assumptions & Decisions

1. **不保留任何"假设重启"的快速路径**：即使用户确实遇到设备正常重启的场景，`pollStatus` 也会在设备恢复后正确识别（L1912-1917 的 `isEsp32Rebooting` 分支虽然不再被这两处设置，但其他正常路径如 `data.status === 'ok'` 仍会设置）。去掉误判路径不会影响正常重启的检测。

2. **依赖 `pollStatus` 探测真实状态**：`pollStatus` 已有完善的设备状态探测逻辑（L1939 的 5 次失败诊断、L1964 的 `lastDeviceBusy` 判断）。fetch 异常后让轮询接管，能在设备真重启时通过 `lastSysStateCode === 7`（ESP32_REBOOTING）或 `lastDeviceBusy` 保持 1s 轮询，在设备真断联时降速诊断。

3. **`showVerifyActions('retry')`**：调研报告提到 L179-182 的 `showVerifyActions('retry')` 函数体可能已空（按钮移除）。即使按钮不存在，调用也是安全的（DOM 查询返回 null 时不会报错）。保留调用以备未来恢复按钮。

4. **不修改服务端**：服务端的 finalize→响应→重启时序是合理的（先响应再重启，确保前端收到结果）。问题在前端错误推断。

5. **不改动 `verifyEsp32Firmware()` 成功路径（L1583-1606）的 `isEsp32Rebooting = true`**：成功路径下设备确实会重启（服务端已发送 ok 响应并执行 restart），此时设置重启等待是正确的。

## Verification

1. **代码审查**：确认两处 catch 块均已移除 `isEsp32Rebooting = true`、`rebootStartTime = Date.now()`、`updateEsp32StagesFromPhase(5)`，改为调用 `cleanupAfterOtaFailure`。

2. **成功路径不受影响**：确认 L1583-1606（校验通过）和 L1663-1669（应用成功）的成功路径仍正常设置 `isEsp32Rebooting`，设备正常重启时前端仍能正确显示重启等待。

3. **运行时验证**：
   - 正常 OTA 升级：校验通过 → 设备重启 → `pollStatus` 检测到设备恢复 → 显示"ESP32 已恢复在线"（验证成功路径未受影响）
   - 模拟网络中断（如 OTA 期间拔网线/关 WiFi）：fetch 异常 → 日志显示"应用请求失败: ...（设备状态未知，等待轮询探测）" → `pollStatus` 走失败诊断路径 → 不再显示误导性的"重启中 Xs"
   - 模拟设备 finalize 阶段崩溃：fetch 异常 → 同上 → `pollStatus` 探测到设备无响应 → 降速诊断 → 用户看到"未连接"而非"重启中"
