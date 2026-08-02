/**
 * polling.js - 轮询状态 + Page Visibility 补偿 + ESP32 实时日志拉取
 *
 * 递归 setTimeout 调度 /system/status + 递归 setTimeout 调度 /logs，避免 setInterval 慢响应被 abort / 重叠抢 socket。
 *
 * 模块级状态（仅本模块使用，不导出）：
 *   statusTimerId / logsTimerId / statusAbortCtrl / statusPollInFlight / logsInFlight / statusPollIntervalMs
 *   uploadPollChip / pageVisible
 *
 * 共享状态（通过 window.* 引用）：
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
 * 复用的函数（通过 window.* 引用）：
 *   window.getEspIp / window.getBaseUrl
 *   window.updateSystemStatusUi / window.updateEsp32StagesFromPhase
 *   window.applyRebootingRuntimeState / window.applyDisconnectedRuntimeState / window.applyUploadingRuntimeState
 *   window.applyDeviceBusyRuntimeState
 *   window.otaSessionActive / window.otaSessionTransition / window.otaSessionReset
 *   window.updateUploadButtonLabel
 */
import { addLog } from './log.js';
import { showToast } from './toast.js';
import { updateConnectionUi, diagnoseNetwork } from './connection.js';

// ==================== 状态码常量（与后端 ota_coordinator.h / ota_relay.h 枚举对齐） ====================
// sys_state_code
const SYS_GD32_STORING = 1;
const SYS_GD32_RELAYING = 3;
const SYS_ESP32_RECEIVING = 5;
const SYS_ESP32_FINALIZING = 6;
const SYS_ESP32_REBOOTING = 7;
// relay_state（1~4 为握手/协商/传输/收尾，非 IDLE）
const RELAY_ACTIVE_MIN = 1;
const RELAY_ACTIVE_MAX = 4;

// ==================== 轮询状态 ====================
// 共享状态（初始化到 window，供 connection.js 等其他模块读写）
window.consecutivePollFailures = 0;
window.lastSysStateCode = -1;  // 上次成功的 sys_state_code，用于失败时判断设备是否在忙
window.lastRelayState = 0;     // 上次成功的 relay_state

// 模块级状态（仅本模块使用）
var statusTimerId = null;
var logsTimerId = null;
var statusAbortCtrl = null;  // 当前 /system/status 请求的 AbortController（5s 超时用）
var statusPollIntervalMs = 1000;  // 状态轮询目标间隔（中继期可由 setStatusPollIntervalMs 降到 400ms）
var statusPollInFlight = false;   // 是否正在请求 /system/status，防止重叠
var logsInFlight = false;          // 是否正在请求 /logs，防止重叠抢 socket
var pageVisible = !document.hidden;   // 页面可见性：切到后台/最小化/切标签会变 false
var uploadPollChip = null; // 当前暂停轮询的上传目标，用于区分 WiFi/日志策略

/**
 * 调整 /system/status 目标间隔。GD32 中继期用 400ms 缩小「转发 N/M」相对设备日志的滞后；
 * 结束后恢复 1000ms，避免常态堆/栈卡片过刷。
 */
export function setStatusPollIntervalMs(ms) {
  var n = Number(ms);
  if (!Number.isFinite(n) || n < 200) n = 1000;
  if (n > 5000) n = 5000;
  statusPollIntervalMs = n;
}

/** @description 调度下一次 /system/status 轮询（递归 setTimeout）。
 *  默认 delay = statusPollIntervalMs；pollStatus 完成后会按「固定周期」
 *  传入 max(0, interval - 本轮耗时)，使 CPU 采样窗口 sample_ms ≈ interval，
 *  而不是「完成后 + interval」（会稳定偏到 1100+ ms）。
 *  仍保证 in-flight 不重叠，避免 setInterval 慢响应被 abort。
 *  ★ 上传暂停期间（window.pollingPausedForUpload）不重挂定时器，
 *    避免在途请求的 finally 把刚清掉的轮询又挂回来。
 */
export function scheduleStatusPoll(delayMs) {
  if (window.pollingPausedForUpload) return;
  if (statusTimerId) clearTimeout(statusTimerId);
  var interval = statusPollIntervalMs;
  statusTimerId = setTimeout(function() {
    statusTimerId = null;
    pollStatus();
  }, delayMs !== undefined ? delayMs : interval);
}

/** @description 调度下一次 /logs 轮询（递归 setTimeout，仿 scheduleStatusPoll）。 */
function scheduleLogsPoll(delayMs) {
  if (logsTimerId) clearTimeout(logsTimerId);
  logsTimerId = setTimeout(function() {
    logsTimerId = null;
    pollLogs();
  }, delayMs !== undefined ? delayMs : statusPollIntervalMs);
}

/** @description 启动定时轮询（状态 + 日志）。 */

export function startPolling() {
  window.consecutivePollFailures = 0;
  statusPollIntervalMs = 1000;
  if (statusTimerId) clearTimeout(statusTimerId);
  if (logsTimerId) clearTimeout(logsTimerId);
  scheduleStatusPoll(0);   // 立即开始第一次，完成后按 interval 递归调度
  scheduleLogsPoll(0);
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
    clearTimeout(logsTimerId);
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
  if (logsTimerId) clearTimeout(logsTimerId);
  // 上传结束立即刷新一次状态；若当前已在请求中，则让它完成后按新间隔继续
  if (statusPollInFlight) {
    if (statusTimerId) clearTimeout(statusTimerId);
  } else {
    scheduleStatusPoll(0);
  }
  scheduleLogsPoll(0);
}

/**
 * @description 统一的 ESP32 重启完成处理：清重启标志 → 推进阶段 6 → 状态机转 DONE → 刷新按钮。
 *  抽出来供 pollStatus 成功分支与 pollLogs 启动 banner 分支复用，避免两处逻辑漂移。
 *  以 isEsp32Rebooting 作幂等守卫：已处理过的重启不再重复弹 toast / 写日志 / 重复转 DONE。
 *  @param {string} source 日志来源描述（如 '启动 banner 收到'），用于区分日志文案。
 */
function markEsp32RebootComplete(source) {
  if (!window.isEsp32Rebooting) return;
  window.isEsp32Rebooting = false;
  window.rebootObservedDown = false;
  /* 重启后重新拉 memory.layout（换固件后静态段可能变化） */
  window._memLayout = null;
  window.updateEsp32StagesFromPhase(6);  // 启动完成
  addLog('I', '系统', source ? 'ESP32 已恢复在线（' + source + '）' : 'ESP32 已恢复在线');
  showToast('ESP32 已恢复在线', 'success');
  // ★ 状态机：ESP32 重启完成 → DONE
  if (window.otaSessionState === window.OtaSessionState.REBOOTING) {
    window.otaSessionTransition(window.OtaSessionState.DONE);
  }
  if (typeof window.updateUploadButtonLabel === 'function') {
    window.updateUploadButtonLabel();
  }
}

/** @description 判断设备最近一次成功轮询时是否处于「忙」状态（OTA 存储/中继/接收/收尾/重启 或 relay 握手~收尾）。 */
function isLastDeviceBusy() {
  var s = window.lastSysStateCode;
  var r = window.lastRelayState;
  return s === SYS_GD32_STORING || s === SYS_GD32_RELAYING ||
         s === SYS_ESP32_RECEIVING || s === SYS_ESP32_FINALIZING ||
         s === SYS_ESP32_REBOOTING ||
         (r >= RELAY_ACTIVE_MIN && r <= RELAY_ACTIVE_MAX);
}

/** @description 轮询 ESP32 系统状态（GET /status）并更新 UI。 */
export async function pollStatus() {
  // 递归 setTimeout 调度，正常情况下不会重叠；做个保险避免并发请求
  if (statusPollInFlight) return;
  statusPollInFlight = true;
  var pollStartedAt = Date.now();
  statusAbortCtrl = new AbortController();
  var pollTimer = setTimeout(function() { try { statusAbortCtrl.abort(); } catch(_e) {} }, 5000);
  try {
    /* 首次/无缓存：?layout=1 取静态内存布局；之后只轮询动态字段 */
    var statusUrl = (window.systemStatusUrl
      ? window.systemStatusUrl(false)
      : (window.getBaseUrl() + '/system/status' + (window._memLayout ? '' : '?layout=1')));
    var resp = await fetch(statusUrl, {
      signal: statusAbortCtrl.signal
    });
    if (resp.ok) {
      var data = await resp.json();
      // 恢复：之前有失败计数，说明设备曾离线，现在恢复在线
      if (window.consecutivePollFailures > 0) {
        window.consecutivePollFailures = 0;
        addLog('I', '系统', '设备已恢复在线，轮询恢复正常');
        showToast('设备已恢复在线', 'success');
        // 仅清理终态残留。禁止对活跃会话 otaSessionReset：
        //   旧逻辑任意非 IDLE 都 reset -> 中继中一次 status 超时恢复后会把 RELAYING
        //   清成 IDLE，UI 不再跟踪；REBOOTING 被清掉后永远进不了启动完成。
        var ssRec = window.otaSessionState;
        if (ssRec === window.OtaSessionState.FAILED ||
            ssRec === window.OtaSessionState.DONE) {
          window.otaSessionReset();
          addLog('I', '系统', '已清理终态 OTA 会话残留');
        }
      }
      if (window.isEsp32Rebooting ||
          window.otaSessionState === window.OtaSessionState.REBOOTING) {
        var rbElapsed = Math.round((Date.now() - window.rebootStartTime) / 1000);
        /* 重启完成的证据（任一即可）：
         *  1) rebootObservedDown：已观察到设备下线后恢复。
         *  2) freshBoot：设备回报 uptime 很小（<120s，与 resolveEsp32VerifyAmbiguous
         *     的 freshBoot 阈值一致）。★ 关键：设备掉线仅 3~5s 时（FAST_SCAN 生效后
         *     常见），在途轮询的 TCP SYN 重传会在设备起来后完成握手、全部成功，
         *     rebootObservedDown 永远不置位，旧逻辑只能干等 10s 兜底——
         *     实测设备 3s 就绪、页面却 10s 才恢复。uptime 是新固件启动的权威信号。
         *  3) rbElapsed >= 10：安全兜底（兼容 uptime 缺失 / 设备未真重启）。
         * pre-restart 窗口（finalize 响应后约 1s 设备仍在跑旧固件）uptime 很大，
         * 不会误判为 freshBoot。 */
        var upS = Number(data && data.uptime_sec);
        var freshBoot = Number.isFinite(upS) && upS >= 0 && upS < 120;
        if (window.rebootObservedDown || freshBoot || rbElapsed >= 10) {
          markEsp32RebootComplete();
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
    // 先判定 OTA 是否进行中：OTA 期间所有失败都不计入 consecutivePollFailures，
    //   也不触发诊断。避免"上传完成→finalize→重启"窗口期失败计数累积，
    //   切回 IDLE 瞬间立即触发 5-fail 诊断，干扰进度显示。
    var lastDeviceBusy0 = isLastDeviceBusy();
    var sessionActive0 = window.otaSessionActive();
    var sessionFailed0 = window.otaSessionState === window.OtaSessionState.FAILED;
    var otaOngoing0 = window.pollingPausedForUpload ||
                      sessionActive0 || sessionFailed0 ||
                      window.isEsp32Rebooting || !!window.currentOtaTarget || lastDeviceBusy0;
    // 后台期间 setInterval 会被 Chrome/Edge 节流到最低 1 分钟/次，
    // 因此后台累积的失败计数不应递增，避免切回前台瞬间误触发 5-fail 诊断。
    // OTA 期间也不递增：设备忙/重启无法响应是正常的，不该计入"诊断触发计数"。
    if (pageVisible && !otaOngoing0) window.consecutivePollFailures++;
    var lastDeviceBusy = lastDeviceBusy0;
    // OTA/传输/重启中屏蔽网关无响应的检测功能（设备忙或正在重启，无法响应轮询是正常的）
    // 设备已知忙时：保持 1s 轮询，不触发降速诊断，避免错过设备恢复的即时反馈
    // ★ 状态机：用统一的会话活跃/失败判断替代散布的标志。
    //   FAILED 态不触发诊断降速（让用户尽快看到设备恢复），但也不覆盖失败 UI。
    var sessionActive = sessionActive0;
    var sessionFailed = sessionFailed0;
    // ★ OTA 期间（含暂停轮询、会话活跃、设备忙、正在重启、FAILED、当前 OTA 目标存在）
    //   一律不触发诊断日志/UI 覆盖。这些 W 级日志会刷在日志区，与"上传 xx/xx" "重启中 Xs"
    //   "中继 N/M" 等进度行混在一起，用户反馈干扰 OTA 进度显示。
    //   之前条件漏了 pollingPausedForUpload，且 sessionActive 可能在 finalize/reboot 短窗口
    //   翻 false，导致 5 次失败后误触发诊断。这里把所有 OTA 相关态统一兜底。
    var otaOngoing = window.pollingPausedForUpload ||
                     sessionActive || sessionFailed ||
                     window.isEsp32Rebooting || !!window.currentOtaTarget || lastDeviceBusy;
    if (pageVisible && window.consecutivePollFailures >= 5 && !otaOngoing) {
      addLog('W', '诊断', '设备连续 5 次无响应，正在诊断网络...');
      var diag = await diagnoseNetwork();
      addLog('W', '诊断', diag);
      updateConnectionUi(false, '诊断中');
      window.applyDisconnectedRuntimeState();
      addLog('W', '诊断', '诊断结果: ' + diag);
    }
    // ★ 状态机优先：会话已 FAILED → 保持失败 UI，仅标连接胶囊为"设备忙/未连接"
    if (sessionFailed) {
      if (lastDeviceBusy) {
        // 设备端仍在跑（中继没停），标"设备忙"但不覆盖进度/阶段
        window.applyDeviceBusyRuntimeState('设备忙', '设备忙（OTA 后台运行中），等待回 IDLE', '设备忙');
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
        // ★ 统一走 cleanupAfterOtaFailure：清 xhr/定时器/按钮/阶段，避免只改 state 留下假死 UI
        addLog('E', '系统', 'ESP32 重启超时，设备未恢复');
        showToast('ESP32 重启超时，请检查设备', 'error');
        if (typeof window.cleanupAfterOtaFailure === 'function') {
          window.cleanupAfterOtaFailure('ESP32 重启超时');
        } else {
          window.otaSessionTransition(window.OtaSessionState.FAILED, 'ESP32 重启超时');
          window.isEsp32Rebooting = false;
          window.rebootObservedDown = false;
        }
        updateConnectionUi(false, '未连接');
        window.applyDisconnectedRuntimeState();
      } else {
        window.applyRebootingRuntimeState();
      }
    } else if (sessionActive || lastDeviceBusy) {
      // 设备在 OTA/中继/重启期间不响应是正常的：保持 1s 轮询，UI 标记为"设备忙"
      // 不调用 applyRebootingRuntimeState（避免显示"重启中 Xs"误导）
      window.applyDeviceBusyRuntimeState('设备忙', '设备忙（OTA/重启中），保持 1s 轮询', '设备忙');
      updateConnectionUi(true, '设备忙');
    } else {
      updateConnectionUi(false, '未连接');
      window.applyDisconnectedRuntimeState();
    }
  } finally {
    statusPollInFlight = false;
    statusAbortCtrl = null;
    /* 固定周期调度：interval 从本轮开始计时扣减请求耗时。
     * 旧逻辑「完成后 + 1000ms」→ 窗口稳定 1100+；现 sample_ms 贴近 1000/400。
     * 本轮超时/很慢时 delay=0，立即补下一轮，仍不重叠。
     * ★ 上传暂停期间不重挂（由 scheduleStatusPoll 内部判断），避免在途请求 abort 后
     *   的 finally 把刚清掉的轮询又挂回来，导致上传中途再打 /system/status 抢 socket。 */
    var elapsed = Date.now() - pollStartedAt;
    var delay = Math.max(0, statusPollIntervalMs - elapsed);
    scheduleStatusPoll(delay);
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
/** @description 轮询 ESP32 日志（GET /logs）并追加到日志面板。 */
export async function pollLogs() {
  // ★ ESP32 上传期间也允许日志轮询；in-flight 守卫避免与 /system/status、/ota/upload 抢 socket
  if (logsInFlight) return;
  logsInFlight = true;
  try {
    var resp = await fetch(window.getBaseUrl() + '/logs', {
      signal: AbortSignal.timeout(5000)
    });
    if (!resp.ok) return;
    var logs = await resp.json();
    if (!Array.isArray(logs) || logs.length === 0) return;
    logs.forEach(function (line) {
      var m = line.match(/^\[([EWID])\]\[([^\]]+)\]\s*(.*)$/);
      if (m) {
        addLog(m[1], m[2], m[3].trimEnd());
        // ★ 复用设备启动 banner 作为 ESP32 重启完成的权威信号。
        //   esp_restart() 是硬件复位，不保证 TCP FIN/RST；靠轮询超时/网络错误判断
        //   "已下线"不可靠。设备启动后会推 [I][MAIN] ESP32-C3 OTA Relay v1.0 启动，
        //   收到这行即说明设备已重启回来，直接推进到阶段 6（启动完成）。
        /* 与 main 启动 banner 对齐：ESP32-C3 OTA Relay <FW_VERSION> 启动（勿写死 v1.0） */
        if (window.isEsp32Rebooting && m[2] === 'MAIN' && /OTA Relay .*启动/.test(m[3])) {
          markEsp32RebootComplete('启动 banner 收到');
        }
        /*
         * 设备已报「全部数据包已转发」时，立刻补 UI 100% 进度。
         * status 轮询可能仍停在旧 relay_seq，否则会出现 FINISH 日志之后还刷 94%。
         */
        if (m[2] === 'OTA' && /全部数据包已转发/.test(m[3]) &&
            typeof window.emitRelayProgress === 'function') {
          var otaSnap = (window.latestSystemStatus && window.latestSystemStatus.ota) || {};
          var tot = Number(otaSnap.relay_total_packets) || 0;
          if (tot > 0) {
            window.emitRelayProgress(tot, tot, { force100: true });
          }
        }
      } else {
        addLog('I', 'ESP32', line.trimEnd());
      }
    });
  } catch (e) { /* 静默 */ } finally {
    logsInFlight = false;
    // 递归调度下一轮：固定周期扣减本轮耗时，避免慢响应时重叠抢 socket
    if (!window.pollingPausedForUpload) {
      scheduleLogsPoll(statusPollIntervalMs);
    }
  }
}
setTimeout(function () {
  window.updateUploadButtonLabel();
  addLog('I', '系统', '控制台初始化完成，等待固件文件...');
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
