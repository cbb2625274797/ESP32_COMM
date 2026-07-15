/**
 * polling.js - 轮询状态 + Page Visibility 补偿 + ESP32 实时日志拉取
 *
 * 从 app.js 的「轮询状态」「Page Visibility 补偿」「ESP32 实时日志拉取 (增量)」段提取。
 * 递归 setTimeout 调度 /system/status + setInterval 调度 /logs，避免 setInterval 慢响应被 abort。
 *
 * 模块级状态（仅本模块使用，不导出）：
 *   statusTimerId / logsTimerId / statusAbortCtrl / statusPollInFlight / statusPollIntervalMs
 *   lastDiagTime / diagResult / uploadPollChip / lastLogTimestamp / pageVisible
 *
 * 共享状态（通过 window.* 引用，后续 app.js 拆分时会统一挂载）：
 *   window.pollingPausedForUpload   — 上传期间暂停轮询标志
 *   window.consecutivePollFailures  — 连续轮询失败计数（connection.js 也写入）
 *   window.lastSysStateCode         — 上次成功的 sys_state_code（connection.js 写入）
 *   window.lastRelayState           — 上次成功的 relay_state（connection.js 写入）
 *   window.isEsp32Rebooting         — ESP32 重启中标志
 *   window.rebootStartTime          — 重启开始时间戳
 *   window.rebootObservedDown       — 已观察到设备下线
 *   window.otaSessionState          — OTA 会话状态机当前状态
 *   window.OtaSessionState          — OTA 会话状态枚举
 *   window.activeUploadChip         — 当前上传目标芯片
 *   window.currentOtaTarget         — 当前 OTA 目标
 *
 * 未拆分的 app.js 函数（通过 window.* 引用）：
 *   window.getEspIp / window.getBaseUrl
 *   window.updateSystemStatusUi / window.updateEsp32StagesFromPhase
 *   window.applyRebootingRuntimeState / window.applyDisconnectedRuntimeState / window.applyUploadingRuntimeState
 *   window.otaSessionActive / window.otaSessionTransition / window.otaSessionReset
 *   window.updateUploadButtonLabel / window.updateSpiHint
 */
import { addLog } from './log.js';
import { showToast } from './toast.js';
import { updateConnectionUi } from './connection.js';

// ==================== 轮询状态 ====================
// 共享状态（初始化到 window，供 connection.js 等其他模块读写）
window.consecutivePollFailures = 0;
window.lastSysStateCode = -1;  // 上次成功的 sys_state_code，用于失败时判断设备是否在忙
window.lastRelayState = 0;     // 上次成功的 relay_state

// 模块级状态（仅本模块使用）
var statusTimerId = null;
var logsTimerId = null;
var lastDiagTime = 0;       // 上次诊断时间
var diagResult = '';        // 最近一次诊断结果
var statusAbortCtrl = null;  // 当前 /system/status 请求的 AbortController（3s 超时用）
var statusPollIntervalMs = 1000;  // 状态轮询目标间隔
var statusPollInFlight = false;   // 是否正在请求 /system/status，防止重叠
var pageVisible = !document.hidden;   // 页面可见性：切到后台/最小化/切标签会变 false
var uploadPollChip = null; // 当前暂停轮询的上传目标，用于区分 WiFi/日志策略

// 从设备 IP 推导网关 IP（取前三段 + .1）
function getGatewayIp() {
  var parts = window.getEspIp().split('.');
  if (parts.length === 4) return parts[0] + '.' + parts[1] + '.' + parts[2] + '.1';
  return '192.168.31.1';
}
// 网络诊断：区分浏览器离线 / 网关不通 / 仅设备不通
async function diagnoseNetwork() {
  lastDiagTime = Date.now();
  var result = [];
  // 1) 浏览器是否在线
  if (navigator.onLine === false) {
    result.push('浏览器无网络连接');
    diagResult = result.join('；');
    return diagResult;
  }
  // 2) 尝试触达网关（超时 2s）
  var gwIp = getGatewayIp();
  var gwOk = false;
  try {
    await fetch('http://' + gwIp + '/', { mode: 'no-cors', signal: AbortSignal.timeout(2000) });
    gwOk = true; // no-cors 模式下不抛异常即视为可达
  } catch (_) {}
  if (!gwOk) {
    // 再试一次 favicon（有些路由器不响应 /）
    try {
      await fetch('http://' + gwIp + '/favicon.ico', { mode: 'no-cors', signal: AbortSignal.timeout(1500) });
      gwOk = true;
    } catch (_) {}
  }
  if (!gwOk) {
    result.push('网关 ' + gwIp + ' 不可达（本地网络不通）');
  } else {
    result.push('网关 ' + gwIp + ' 可达');
    // 3) 网关通但设备不通 → 设备自身问题
    result.push('设备 ' + window.getEspIp() + ' 不响应（可能断电、重启中或 WiFi 断开）');
  }
  diagResult = result.join('；');
  return diagResult;
}
/** @description 调度下一次 /system/status 轮询（递归 setTimeout）。
 *  上一次请求完成（或 3s 超时）后才调度下一次，避免 setInterval 把慢响应
 *  直接 abort 而导致堆/任务栈/CPU 等卡片刷新率不稳。
 */
export function scheduleStatusPoll(delayMs) {
  if (statusTimerId) clearTimeout(statusTimerId);
  // 中继期加快：进度条依赖 status.relay_seq，1s 间隔体感像卡死
  var interval = statusPollIntervalMs;
  // 防御性守卫：window.OtaSessionState 可能尚未挂载（模块加载早期），此时不加速
  if (window.OtaSessionState && window.otaSessionState === window.OtaSessionState.RELAYING) {
    interval = Math.min(interval, 400);
  }
  statusTimerId = setTimeout(function() {
    statusTimerId = null;
    pollStatus();
  }, delayMs !== undefined ? delayMs : interval);
}

/** @description 启动定时轮询（状态 + 日志）。 */

export function startPolling() {
  window.consecutivePollFailures = 0;
  statusPollIntervalMs = 1000;
  if (statusTimerId) clearTimeout(statusTimerId);
  if (logsTimerId) clearInterval(logsTimerId);
  scheduleStatusPoll(0);   // 立即开始第一次，完成后按 interval 递归调度
  logsTimerId = setInterval(pollLogs, 1000);
}
export function pausePollingForUpload() {
  if (window.pollingPausedForUpload) return;
  window.pollingPausedForUpload = true;
  uploadPollChip = window.activeUploadChip || window.flashTarget || null;
  window.consecutivePollFailures = 0;
  // ★ 上传大固件期间必须停掉 status/logs 轮询。
  //   httpd max_open_sockets=4 + lru_purge：轮询会再占 socket，LRU 可能踢掉
  //   正在写 Flash 的 /ota/upload 连接 → 浏览器 upload 进度已 100%（发完），
  //   但永远等不到 HTTP 响应，卡在「已上传 xxx / xxx」。
  if (statusTimerId) {
    clearTimeout(statusTimerId);
    statusTimerId = null;
  }
  if (logsTimerId) {
    clearInterval(logsTimerId);
    logsTimerId = null;
  }
  if (statusAbortCtrl) {
    try { statusAbortCtrl.abort(); } catch (e) { /* ignore */ }
    statusAbortCtrl = null;
    statusPollInFlight = false;
  }
}
export function resumePollingAfterUpload() {
  if (!window.pollingPausedForUpload) return;
  window.pollingPausedForUpload = false;
  uploadPollChip = null;
  window.consecutivePollFailures = 0;
  statusPollIntervalMs = 1000;
  if (logsTimerId) clearInterval(logsTimerId);
  logsTimerId = setInterval(pollLogs, statusPollIntervalMs);
  // 上传结束立即刷新一次状态；若当前已在请求中，则让它完成后按新间隔继续
  if (statusPollInFlight) {
    if (statusTimerId) clearTimeout(statusTimerId);
  } else {
    scheduleStatusPoll(0);
  }
  pollLogs();
}
/** @description 轮询 ESP32 系统状态（GET /status）并更新 UI。 */
export async function pollStatus() {
  // 递归 setTimeout 调度，正常情况下不会重叠；做个保险避免并发请求
  if (statusPollInFlight) return;
  statusPollInFlight = true;
  statusAbortCtrl = new AbortController();
  var pollTimer = setTimeout(function() { try { statusAbortCtrl.abort(); } catch(_e) {} }, 3000);
  try {
    var resp = await fetch(window.getBaseUrl() + '/system/status', {
      signal: statusAbortCtrl.signal
    });
    if (resp.ok) {
      var data = await resp.json();
      // 恢复：之前有失败计数，说明设备曾离线，现在恢复在线
      if (window.consecutivePollFailures > 0) {
        window.consecutivePollFailures = 0;
        addLog('I', '系统', '设备已恢复在线，轮询恢复正常');
        showToast('设备已恢复在线', 'success');
        // 设备离线后恢复在线，清理残留的 OTA 会话状态
        if (window.otaSessionState !== window.OtaSessionState.IDLE) {
          window.otaSessionReset();
          addLog('I', '系统', '已清理 OTA 会话状态');
        }
      }
      if (window.isEsp32Rebooting) {
        var rbElapsed = Math.round((Date.now() - window.rebootStartTime) / 1000);
        if (!window.rebootObservedDown && rbElapsed < 10) {
          // pre-reboot 窗口：后端发响应后约 1s 才 esp_restart，期间设备仍响应轮询
          // 保持"重启中"状态，不清除标志，不推进阶段
          // ★ 真正的"重启完成"由 pollLogs 收到启动 banner（MAIN: ESP32-C3 OTA Relay v1.0 启动）触发
        } else {
          // 设备已重启并恢复在线（rebootObservedDown=true）
          // 或超 10s 安全阈值仍未观察到下线（重启过快未捕获或未重启）——接受恢复
          window.isEsp32Rebooting = false;
          window.rebootObservedDown = false;
          window.updateEsp32StagesFromPhase(6);  // 启动完成
          addLog('I', '系统', 'ESP32 已恢复在线');
          showToast('ESP32 已恢复在线', 'success');
          // ★ 状态机：ESP32 重启完成 → DONE
          if (window.otaSessionState === window.OtaSessionState.REBOOTING) window.otaSessionTransition(window.OtaSessionState.DONE);
        }
      }
      try { window.updateSystemStatusUi(data); } catch (e2) { console.warn('updateSystemStatusUi 异常:', e2); }
      // pre-reboot 窗口内 updateSystemStatusUi 会把连接胶囊刷成"已连接"，需覆盖回"重启中"
      if (window.isEsp32Rebooting) {
        window.applyRebootingRuntimeState();
      }
    }
    clearTimeout(pollTimer);
  } catch (e) {
    clearTimeout(pollTimer);
    // 自身 AbortController 触发的取消不算失败，静默返回
    if (e && e.name === "AbortError") return;
    // 后台期间 setInterval 会被 Chrome/Edge 节流到最低 1 分钟/次，
    // 因此后台累积的失败计数不应递增，避免切回前台瞬间误触发 5-fail 诊断。
    if (pageVisible) window.consecutivePollFailures++;
    // 判断设备是否处于"已知忙/重启"状态（依据最近一次成功轮询的 sys_state_code）：
    //   1=GD32_STORING, 3=GD32_RELAYING, 5=ESP32_RECEIVING,
    //   6=ESP32_FINALIZING, 7=ESP32_REBOOTING
    // 也包含 relay_state 非 IDLE（1-4 表示握手/协商/传输/收尾）
    var lastDeviceBusy =
      window.lastSysStateCode === 1 || window.lastSysStateCode === 3 ||
      window.lastSysStateCode === 5 || window.lastSysStateCode === 6 ||
      window.lastSysStateCode === 7 ||
      (window.lastRelayState >= 1 && window.lastRelayState <= 4);
    // OTA/传输/重启中屏蔽网关无响应的检测功能（设备忙或正在重启，无法响应轮询是正常的）
    // 设备已知忙时：保持 1s 轮询，不触发降速诊断，避免错过设备恢复的即时反馈
    // ★ 状态机：用统一的会话活跃/失败判断替代散布的标志。
    //   FAILED 态不触发诊断降速（让用户尽快看到设备恢复），但也不覆盖失败 UI。
    var sessionActive = window.otaSessionActive();
    var sessionFailed = window.otaSessionState === window.OtaSessionState.FAILED;
    if (pageVisible && window.consecutivePollFailures >= 5 && !sessionActive && !window.isEsp32Rebooting && !window.currentOtaTarget && !lastDeviceBusy && !sessionFailed) {
      addLog('W', '诊断', '设备连续 5 次无响应，正在诊断网络...');
      var diag = await diagnoseNetwork();
      addLog('W', '诊断', diag);
      updateConnectionUi(false, '未连接');
      window.applyDisconnectedRuntimeState();
      var cst = document.getElementById('connStatusTxt');
      if (cst) cst.textContent = '诊断中: ' + diag;
    }
    // ★ 状态机优先：会话已 FAILED → 保持失败 UI，仅标连接胶囊为"设备忙/未连接"
    if (sessionFailed) {
      if (lastDeviceBusy) {
        // 设备端仍在跑（中继没停），标"设备忙"但不覆盖进度/阶段
        var pcF = document.getElementById('pillConn');
        var pctF = document.getElementById('pillConnTxt');
        if (pcF) pcF.className = 'pill processing';
        if (pctF) pctF.textContent = '设备忙';
        var csF = document.getElementById('connStatus');
        var cstF = document.getElementById('connStatusTxt');
        if (csF) csF.className = 'conn-indicator warn';
        if (cstF) cstF.textContent = '设备忙（OTA 后台运行中），等待回 IDLE';
        updateConnectionUi(true, '设备忙');
      } else {
        // 设备已回 IDLE：连接胶囊显示未连接，但失败进度保留
        updateConnectionUi(false, '未连接');
      }
    } else if (window.otaSessionState === window.OtaSessionState.UPLOADING) {
      window.applyUploadingRuntimeState();
    } else if (window.otaSessionState === window.OtaSessionState.REBOOTING || window.isEsp32Rebooting) {
      window.rebootObservedDown = true;  // 确认设备已下线（正在重启）
      var elapsed = Math.round((Date.now() - window.rebootStartTime) / 1000);
      if (elapsed > 60) {
        window.otaSessionTransition(window.OtaSessionState.FAILED, 'ESP32 重启超时');
        window.isEsp32Rebooting = false;
        window.rebootObservedDown = false;
        addLog('E', '系统', 'ESP32 重启超时，设备未恢复');
        showToast('ESP32 重启超时，请检查设备', 'error');
        updateConnectionUi(false, '未连接');
        window.applyDisconnectedRuntimeState();
      } else {
        window.applyRebootingRuntimeState();
      }
    } else if (sessionActive || lastDeviceBusy) {
      // 设备在 OTA/中继/重启期间不响应是正常的：保持 1s 轮询，UI 标记为"设备忙"
      // 不调用 applyRebootingRuntimeState（避免显示"重启中 Xs"误导）
      var pc = document.getElementById('pillConn');
      var pct = document.getElementById('pillConnTxt');
      if (pc) pc.className = 'pill processing';
      if (pct) pct.textContent = '设备忙';
      var cs = document.getElementById('connStatus');
      var cst = document.getElementById('connStatusTxt');
      if (cs) cs.className = 'conn-indicator warn';
      if (cst) cst.textContent = '设备忙（OTA/重启中），保持 1s 轮询';
      updateConnectionUi(true, '设备忙');
    } else {
      updateConnectionUi(false, '未连接');
      window.applyDisconnectedRuntimeState();
    }
  } finally {
    statusPollInFlight = false;
    scheduleStatusPoll();
  }
}
// 注意：startPoll() 由入口模块（app.js / ble_ota.js）在所有模块加载完成后调用，
//   此处不在模块顶层自动启动，避免 ESM 求值顺序导致 window.OtaSessionState 尚未初始化。
// ==================== Page Visibility 补偿 ====================
// Chrome/Edge 在标签隐藏/窗口最小化时会把 setInterval 节流到最低 1 分钟/次。
// 切回前台时立即拉一次状态 + 日志，并清零后台期间累积的失败计数，
// 避免在"刚切回前台那一轮"被误判为 5-fail 离线。
document.addEventListener('visibilitychange', function () {
  pageVisible = !document.hidden;
  if (!pageVisible) return;
  window.consecutivePollFailures = 0;
  // 切回前台立即刷新状态；若当前已在请求中，取消等待中的下一次让它尽快重排
  if (statusPollInFlight) {
    if (statusTimerId) clearTimeout(statusTimerId);
  } else {
    if (statusTimerId) clearTimeout(statusTimerId);
    scheduleStatusPoll(0);
  }
  pollLogs();
});
// ==================== ESP32 实时日志拉取 (增量) ====================
var lastLogTimestamp = 0;
/** @description 轮询 ESP32 日志（GET /logs）并追加到日志面板。 */
export async function pollLogs() {
  // ★ ESP32 上传期间也允许日志轮询
  try {
    var resp = await fetch(window.getBaseUrl() + '/logs', {
      signal: AbortSignal.timeout(2000)
    });
    if (!resp.ok) return;
    var logs = await resp.json();
    if (!Array.isArray(logs) || logs.length === 0) return;
    var levelMap = { E: 'E', W: 'W', I: 'I', D: 'D' };
    logs.forEach(function (line) {
      var m = line.match(/^\[([EWID])\]\[([^\]]+)\]\s*(.*)$/);
      if (m) {
        addLog(levelMap[m[1]] || 'I', m[2], m[3].trimEnd());
        // ★ 复用设备启动 banner 作为 ESP32 重启完成的权威信号。
        //   esp_restart() 是硬件复位，不保证 TCP FIN/RST；靠轮询超时/网络错误判断
        //   "已下线"不可靠。设备启动后会推 [I][MAIN] ESP32-C3 OTA Relay v1.0 启动，
        //   收到这行即说明设备已重启回来，直接推进到阶段 6（启动完成）。
        if (window.isEsp32Rebooting && m[2] === 'MAIN' && /OTA Relay v1\.0 启动/.test(m[3])) {
          window.isEsp32Rebooting = false;
          window.rebootObservedDown = false;
          window.updateEsp32StagesFromPhase(6);  // 启动完成
          addLog('I', '系统', 'ESP32 已恢复在线（启动 banner 收到）');
          showToast('ESP32 已恢复在线', 'success');
          if (window.otaSessionState === window.OtaSessionState.REBOOTING) window.otaSessionTransition(window.OtaSessionState.DONE);
        }
      } else {
        addLog('I', 'ESP32', line.trimEnd());
      }
    });
  } catch (e) { /* 静默 */ }
}
setTimeout(function () {
  window.updateUploadButtonLabel();
  addLog('I', '系统', '控制台初始化完成，等待固件文件...');
  window.updateSpiHint();
  // IP 输入变化时清零失败计数并立即刷新状态
  var ipInput = document.getElementById('espIp');
  if (ipInput) {
    ipInput.addEventListener('change', function () {
      window.consecutivePollFailures = 0;
      addLog('I', '系统', 'IP 已修改，重新启动轮询...');
      if (!statusPollInFlight) {
        if (statusTimerId) clearTimeout(statusTimerId);
        scheduleStatusPoll(0);
      }
    });
  }
}, 500);
