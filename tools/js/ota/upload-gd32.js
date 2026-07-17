/**
 * upload-gd32.js - GD32 中继 OTA 触发 + 进度辅助
 *
 * 进度/完成/失败 UI 的唯一应用路径：pollStatus → updateSystemStatusUi（stages.js）。
 * 本文件只负责：
 *   - 触发 /ota/relay
 *   - 中继期加速 status 轮询（400ms）
 *   - emitRelayProgress / markGd32OtaUiComplete 等被 stages 调用的辅助函数
 *
 * 共享状态（通过 window.* 引用）：
 *   window.gd32CompleteAnnounced / window.relayWatchLastSeq / window.gd32RelaySessionLive
 *   window.pollingPausedForUpload / window.statusPollIntervalMs
 *   window.otaSessionState / window.lastSysStateCode / window.lastRelayState
 *
 * 复用的函数（通过 window.* 引用）：
 *   window.otaSessionTransition / window.OtaSessionState
 *   window.markPhaseDone / window.setProgress / window.updateSystemStatusUi
 *   window.cleanupAfterOtaFailure / window.scheduleStatusPoll / window.pollLogs
 *   window.resumePollingAfterUpload / window.getBaseUrl / window.showToast / window.addLog
 */
import { showToast } from '../common/toast.js';
import { addLog } from '../common/log.js';
import {
  otaSessionTransition, OtaSessionState
} from './session.js';
import { markPhaseDone, setProgress } from './progress.js';

/** 中继期 status 轮询间隔（ms）；结束后由 stopRelayProgressWatch 恢复 1000 */
const RELAY_STATUS_POLL_MS = 400;
const NORMAL_STATUS_POLL_MS = 1000;

window.gd32CompleteAnnounced = false;
window.relayWatchLastSeq = -1;
/** 本会话是否已观察到真实中继态（1~4），避免旧 relay_state=5 残留导致一点击就“完成” */
window.gd32RelaySessionLive = false;

/**
 * 包进度变更时更新进度条（同 seq 不重复）。
 * 由 stages.js 在 status 轮询路径调用；勿在本文件再起独立定时器刷进度。
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
  // 已公告过：禁止再改 className / setProgress / toast，否则 status 重触发动效会「疯狂跳 ✓」
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

/**
 * 结束中继加速轮询，恢复常态 1s status 间隔。
 * 不再持有独立 setInterval（进度/完成/失败统一由 stages 在 pollStatus 路径处理）。
 */
export function stopRelayProgressWatch() {
  if (typeof window.setStatusPollIntervalMs === 'function') {
    window.setStatusPollIntervalMs(NORMAL_STATUS_POLL_MS);
  }
}

/**
 * 中继进度跟踪：只加速 /system/status 轮询，不再另起 400ms 定时器。
 * UI 真相源仍是 pollStatus → updateSystemStatusUi（stages.js）。
 */
export function startRelayProgressWatch() {
  if (window.pollingPausedForUpload) {
    window.resumePollingAfterUpload();
  }
  if (typeof window.setStatusPollIntervalMs === 'function') {
    window.setStatusPollIntervalMs(RELAY_STATUS_POLL_MS);
  }
  if (typeof window.scheduleStatusPoll === 'function') {
    window.scheduleStatusPoll(0);
  }
  if (typeof window.pollLogs === 'function') {
    window.pollLogs();
  }
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
  addLog('I', 'OTA', '触发 GD32 中继…');
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
      addLog('I', 'OTA', 'GD32 OTA 已触发；进度看状态栏包数');
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
          addLog('E', 'OTA', 'GD32 SHA-256 不匹配: 设备=' + err.device +
            ' 前端=' + sha256);
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
