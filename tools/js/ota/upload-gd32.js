/**
 * upload-gd32.js - GD32 中继 OTA 触发 + relay 态跟踪
 *
 * 包含：relay 进度观察定时器、markGd32OtaUiComplete、triggerGd32Relay。
 *
 * 共享状态（通过 window.* 引用）：
 *   window.gd32CompleteAnnounced / window.relayWatchLastSeq / window.gd32RelaySessionLive
 *   window.pollingPausedForUpload / window.statusPollIntervalMs
 *   window.otaSessionState / window.lastSysStateCode / window.lastRelayState
 *
 * 复用的函数（通过 window.* 引用）：
 *   window.otaSessionTransition / window.OtaSessionState / window.otaSessionActive
 *   window.markPhaseDone / window.setProgress / window.updateSystemStatusUi
 *   window.cleanupAfterOtaFailure / window.scheduleStatusPoll / window.pollLogs
 *   window.resumePollingAfterUpload / window.getBaseUrl / window.showToast / window.addLog
 */
import { showToast } from '../common/toast.js';
import { addLog } from '../common/log.js';
import {
  otaSessionTransition, otaSessionActive, OtaSessionState
} from './session.js';
import { markPhaseDone, setProgress } from './progress.js';

/**
 * GD32 中继进度观察：以 /system/status 为唯一真相源。
 * 日志环只有 64 条，中继/擦除期间 INFO 会被挤掉，时间戳还是浏览器本地时间——
 * 绝不能用「最后一条日志停在 SEND_SIZE」判断卡死。
 *
 * 时序约定（避免「设备已 FINISH / 完成，UI 还在 94%」）：
 *   1) 包进度变化先刷进度条 + 日志；
 *   2) 一旦 rseq>=rtot 或 rs>=4(FINISHING)，先强制打出 100% 再进收尾/完成；
 *   3) markGd32OtaUiComplete 前再兜底一次 100%。
 */
let relayWatchTimerId = null;
window.gd32CompleteAnnounced = false;
window.relayWatchLastSeq = -1;
/** 本会话是否已观察到真实中继态（1~4），避免旧 relay_state=5 残留导致一点击就“完成” */
window.gd32RelaySessionLive = false;

/**
 * 包进度变更时更新进度条并写一条「转发」日志（同 seq 不重复）。
 * @param {number} rseq
 * @param {number} rtot
 * @param {{force100?: boolean}} [opts]
 * @returns {boolean} 是否实际输出了进度
 */
export function emitRelayProgress(rseq, rtot, opts) {
  opts = opts || {};
  rseq = Number(rseq) || 0;
  rtot = Number(rtot) || 0;
  if (rtot <= 0) return false;
  if (opts.force100 || rseq >= rtot) {
    rseq = rtot;
  }
  if (rseq <= 0) return false;
  if (rseq === window.relayWatchLastSeq) return false;
  window.relayWatchLastSeq = rseq;
  var pct = Math.min(100, Math.round(rseq * 100 / rtot));
  /* 进度只走进度条/阶段文案，不写调试日志（避免 ring/网页刷屏） */
  setProgress(60 + Math.round(pct * 0.30),
    '已转发 ' + rseq + ' / ' + rtot + ' 包 (' + pct + '%)');
  return true;
}

/** 完成/收尾前：若尚未打出 100%，用 status 里的包数补一条 */
export function ensureRelayProgressComplete() {
  var data = window.latestSystemStatus;
  var o = (data && data.ota) || {};
  var rtot = Number(o.relay_total_packets) || 0;
  var rseq = Number(o.relay_seq) || 0;
  if (rtot <= 0) return;
  /* FINISHING/DONE 时后端 seq 已到 total；status 滞后时也强制 100% */
  if (rseq < rtot && (Number(o.relay_state) >= 4 || Number(o.sys_state_code) === 4)) {
    rseq = rtot;
  }
  if (rseq >= rtot) {
    emitRelayProgress(rtot, rtot, { force100: true });
  }
}

export function markGd32OtaUiComplete(msg) {
  // 已公告过：禁止再改 className / setProgress / toast，否则定时器重触发动效会「疯狂跳 ✓」
  if (window.gd32CompleteAnnounced) {
    return;
  }
  /* 仅本会话 RELAYING/FAILED 可宣布完成；IDLE/UPLOADING 时 status 里的旧 relay 残留一律忽略 */
  if (window.otaSessionState !== OtaSessionState.RELAYING &&
      window.otaSessionState !== OtaSessionState.FAILED) {
    return;
  }
  /* 先 100% 转发进度，再宣布刷写完成 —— 避免日志里「完成」早于「1089/1089」 */
  ensureRelayProgressComplete();
  window.gd32CompleteAnnounced = true;
  stopRelayProgressWatch();

  markPhaseDone('gd32Ota');
  window.currentOtaTarget = null;
  setProgress(100, msg || 'GD32 刷写完成');
  var sbDone = document.getElementById('stageStatusBar');
  var stDone = document.getElementById('stageStatusText');
  if (sbDone) sbDone.className = 'stage-status-bar ok';
  if (stDone) stDone.textContent = msg || 'GD32 刷写完成';
  if (window.otaSessionState === OtaSessionState.RELAYING ||
      window.otaSessionState === OtaSessionState.FAILED) {
    otaSessionTransition(OtaSessionState.DONE);
  }
  if (typeof window.updateUploadButtonLabel === 'function') {
    window.updateUploadButtonLabel();
  }
  addLog('I', 'OTA', msg || 'GD32 刷写完成');
  showToast(msg || 'GD32 刷写完成', 'success');
}

export function stopRelayProgressWatch() {
  if (relayWatchTimerId) {
    clearInterval(relayWatchTimerId);
    relayWatchTimerId = null;
  }
  /* 中继结束：恢复常态 1s status 轮询 */
  if (typeof window.setStatusPollIntervalMs === 'function') {
    window.setStatusPollIntervalMs(1000);
  }
}

/**
 * 中继进度观察：只读 pollStatus 写入的 latestSystemStatus，不再另起 /system/status。
 * 中继期 status 加速到 400ms，使「转发 N/M」更贴近设备真实进度。
 */
export function startRelayProgressWatch() {
  stopRelayProgressWatch();
  if (window.pollingPausedForUpload) {
    window.resumePollingAfterUpload();
  }
  /* 中继期加速 status，缩小 UI 进度相对设备日志的滞后 */
  if (typeof window.setStatusPollIntervalMs === 'function') {
    window.setStatusPollIntervalMs(400);
  }
  window.scheduleStatusPoll(0);
  window.pollLogs();
  addLog('I', 'OTA', '跟踪中继进度：看进度条与 relay_seq（/system/status），勿只看日志');
  relayWatchTimerId = setInterval(function () {
    var active = window.otaSessionState === OtaSessionState.RELAYING ||
                 (window.otaSessionState === OtaSessionState.FAILED &&
                  (window.lastSysStateCode === 3 || window.lastRelayState >= 1 && window.lastRelayState <= 4));
    if (!active) {
      if (window.otaSessionState === OtaSessionState.DONE) {
        stopRelayProgressWatch();
      }
      return;
    }
    var data = window.latestSystemStatus;
    if (!data || !data.ota) return;
    var o = data.ota || {};
    var code = Number(o.sys_state_code);
    var rstate = Number(o.relay_state);
    var alive = !!o.gd32_app_alive;
    var rseq = Number(o.relay_seq) || 0;
    var rtot = Number(o.relay_total_packets) || 0;
    if (rstate >= 1 && rstate <= 4) {
      window.gd32RelaySessionLive = true;
    }
    /* 中继失败：必须把前端会话从 RELAYING 拉到 FAILED，否则无法新开上传 */
    if (code === 8 || rstate === 255) {
      stopRelayProgressWatch();
      window.pollLogs();
      if (window.otaSessionState === OtaSessionState.RELAYING ||
          window.otaSessionState === OtaSessionState.UPLOADING) {
        var failMsg = 'GD32 中继失败（relay_state=' + rstate +
          ', sys=' + (o.sys_state || code) + ')';
        if (typeof window.cleanupAfterOtaFailure === 'function') {
          window.cleanupAfterOtaFailure(failMsg);
        } else {
          otaSessionTransition(OtaSessionState.FAILED, failMsg);
        }
      }
      return;
    }
    /*
     * ERROR 可能在 status 路径被软清为 IDLE，前端收不到 code=8。
     * 本会话已见过真实中继，设备却已回 IDLE 且未宣布完成 → 按失败清理。
     */
    var idleAfterRelay = (code === 0 || code === 2) &&
                         (rstate === 0 || rstate === 5) &&
                         window.gd32RelaySessionLive &&
                         !window.gd32CompleteAnnounced &&
                         window.otaSessionState === OtaSessionState.RELAYING;
    if (idleAfterRelay) {
      stopRelayProgressWatch();
      window.pollLogs();
      if (typeof window.cleanupAfterOtaFailure === 'function') {
        window.cleanupAfterOtaFailure('GD32 中继已结束但未成功（设备已回 IDLE）');
      } else {
        otaSessionTransition(OtaSessionState.FAILED, 'GD32 中继已结束但未成功');
      }
      return;
    }
    /*
     * 顺序：先刷包进度（含强制 100%），再判定完成。
     * FINISHING/DONE 时若 status 滞后未到 total，仍按 total 打 100%。
     */
    var transferDone = rtot > 0 && (rseq >= rtot || rstate >= 4 || code === 4);
    if (transferDone) {
      emitRelayProgress(rtot, rtot, { force100: true });
    } else if (rtot > 0 && rseq > 0) {
      emitRelayProgress(rseq, rtot);
    }
    if (window.gd32RelaySessionLive &&
        (code === 4 || rstate === 5 ||
         (alive && rtot > 0 && rseq >= rtot && rstate >= 3))) {
      markGd32OtaUiComplete('GD32 刷写完成');
      window.pollLogs();
    }
  }, 400);
}

/** @description 触发 GD32 中继 OTA 流程（通过 HTTP 接口）。 */
export async function triggerGd32Relay() {
  var sha256 = window._gd32FileSha256;
  if (!sha256) {
    addLog('E', 'OTA', '缺少 GD32 文件 SHA-256，请先上传文件');
    showToast('SHA-256 缺失', 'error');
    window.cleanupAfterOtaFailure('SHA-256 缺失');
    return;
  }
  addLog('I', 'OTA', 'GD32 SHA-256: ' + sha256 + ' → 触发中继...');
  showToast('正在触发 GD32 OTA', 'success');
  setProgress(48, '校验 SHA 并触发 GD32 中继…');
  try {
    // 仅触发中继（设备端异步跑），不阻塞到刷写结束；SHA 读分区可能数秒
    var resp = await fetch(window.getBaseUrl() + '/ota/relay', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sha256: sha256 }),
      signal: AbortSignal.timeout(120000)
    });
    var text = await resp.text();
    if (resp.ok) {
      try {
        var data = JSON.parse(text);
        if (data.sha256) addLog('I', 'OTA', '设备端 SHA-256: ' + data.sha256);
      } catch (_) {}
      addLog('I', 'OTA', 'GD32 OTA 已触发；进度看状态栏包数（勿只看日志时间戳）');
      showToast('GD32 OTA 已触发', 'success');
      setProgress(50, '中继已启动，同步进度…');
      window.gd32CompleteAnnounced = false;
      window.relayWatchLastSeq = -1;
      startRelayProgressWatch();
    } else {
      // 尝试解析错误响应中的 device SHA
      try {
        var err = JSON.parse(text);
        if (err.device) {
          addLog('E', 'OTA', 'GD32 SHA-256 不匹配: 设备=' + err.device + ' 前端=' + sha256);
          showToast('SHA-256 不匹配', 'error');
          window.cleanupAfterOtaFailure('SHA-256 不匹配');
          return;
        }
      } catch (_) {}
      addLog('E', 'OTA', 'GD32 触发失败: HTTP ' + resp.status + ' ' + text);
      showToast('GD32 触发失败', 'error');
      window.cleanupAfterOtaFailure('触发失败 HTTP ' + resp.status);
    }
  } catch (e) {
    // 请求超时/断连 ≠ 设备未启动中继：先探测 status 再决定是否标失败
    try {
      var probe = await fetch(window.getBaseUrl() + '/system/status', {
        signal: AbortSignal.timeout(3000)
      });
      if (probe.ok) {
        var st = await probe.json();
        var o = st.ota || {};
        var code = Number(o.sys_state_code);
        var rstate = Number(o.relay_state);
        if (code === 3 || (rstate >= 1 && rstate <= 4)) {
          addLog('W', 'OTA', '触发请求中断(' + e.message + ')，但设备已在中继，继续跟踪进度');
          showToast('设备已在刷写，同步进度中', 'success');
          if (window.otaSessionState !== OtaSessionState.RELAYING) {
            otaSessionTransition(OtaSessionState.RELAYING);
          }
          try { window.updateSystemStatusUi(st); } catch (_) {}
          startRelayProgressWatch();
          return;
        }
      }
    } catch (_) { /* probe 失败再走失败清理 */ }
    addLog('E', 'OTA', 'GD32 触发失败: ' + e.message);
    showToast('GD32 触发失败', 'error');
    window.cleanupAfterOtaFailure('触发异常');
  }
}