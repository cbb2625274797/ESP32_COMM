/**
 * session.js - 前端 OTA 会话状态机
 *
 * 统一管理一次 OTA 会话的完整生命周期，避免散布的布尔标志
 * (isUploading / currentOtaTarget / activeUploadChip / isEsp32Rebooting)
 * 在网络错误后被轮询错误覆盖、状态混乱的问题。
 *
 * 状态流转：
 *   IDLE → UPLOADING → (ESP32: VERIFYING → REBOOTING) → DONE
 *   IDLE → UPLOADING → (GD32:  RELAYING)              → DONE
 *   任意活跃态 ──(failure/abort)──→ FAILED
 *   FAILED/DONE ──(startUpload)──→ UPLOADING
 *   FAILED/DONE ──(device idle detected by poll)──→ IDLE
 *
 * 共享状态（通过 window.* 引用，入口模块统一挂载）：
 *   window.isUploading / window.isEsp32Rebooting / window.rebootStartTime
 *   window.rebootObservedDown / window.activeUploadChip / window.currentOtaTarget
 *   window.esp32OtaPhase / window.activePhase
 *   window.gd32CompleteAnnounced / window.relayWatchLastSeq / window.gd32RelaySessionLive
 *   window.lastSysStateCode / window.lastRelayState
 *
 * 复用的函数（通过 window.* 引用）：
 *   window.stopRelayProgressWatch / window.setProgress / window.addLog
 */
import { addLog } from '../common/log.js';

// ==================== 全局状态 ====================
// 文件句柄与上传进度等状态保留为模块级，被 file-handle / upload-esp32 等模块通过 window.* 读写
window.isUploading = false;
window.uploadProgress = 0;
window.isEsp32Rebooting = false;
window.rebootStartTime = 0;
window.rebootObservedDown = false;
window.pollingPausedForUpload = false;
window.uploadPollChip = null;
window.activeXhr = null;
window.uploadBtnOrigText = '';
window.activeUploadChip = null;

// 心跳定时器（模块级，仅本模块使用）
let uploadWaitHeartbeatId = null;
let uploadWaitStartMs = 0;

/** @description 停止「等待设备 HTTP 响应」心跳，避免残留定时器改进度条。 */
export function stopUploadWaitHeartbeat() {
  if (uploadWaitHeartbeatId) {
    clearInterval(uploadWaitHeartbeatId);
    uploadWaitHeartbeatId = null;
  }
  uploadWaitStartMs = 0;
}

/**
 * @description 浏览器 body 已发完但尚未收到 HTTP 响应时启动心跳。
 *              上传期轮询已暂停，若无此反馈会像“卡在 100%”。
 */
export function startUploadWaitHeartbeat() {
  stopUploadWaitHeartbeat();
  uploadWaitStartMs = Date.now();
  uploadWaitHeartbeatId = setInterval(function () {
    if (!window.activeXhr) {
      stopUploadWaitHeartbeat();
      return;
    }
    var sec = Math.round((Date.now() - uploadWaitStartMs) / 1000);
    window.setProgress(100, '数据已发完，设备处理中… 已等待 ' + sec + 's');
    // 每 10s 打一条日志，便于区分“仍在处理”与“真死锁”
    if (sec > 0 && sec % 10 === 0) {
      addLog('I', '上传', '仍在等待设备 HTTP 响应（已 ' + sec +
        's；设备可能仍在写 Flash / 算 SHA，或连接异常）');
    }
  }, 5000);
}

// ==================== 前端 OTA 会话状态机 ====================
export const OtaSessionState = {
  IDLE: 'idle',          // 无会话
  UPLOADING: 'uploading', // 固件上传中（XHR 上传阶段）
  VERIFYING: 'verifying', // ESP32 SHA-256 校验 + 应用中
  RELAYING: 'relaying',   // GD32 中继转发中（triggerGd32Relay 发起后）
  REBOOTING: 'rebooting', // ESP32 重启等待中
  DONE: 'done',           // 会话成功完成
  FAILED: 'failed'        // 会话失败/中止
};
window.OtaSessionState = OtaSessionState;

let otaSessionState = OtaSessionState.IDLE;
let otaSessionTarget = null;  // 'esp32' | 'gd32' | null — 本会话的目标芯片
let otaFailReason = '';       // FAILED 时的失败原因（用于 UI 显示）
// 同步到 window 供 polling.js / stages.js / upload-gd32.js 等通过 window.* 读取
window.otaSessionState = otaSessionState;
window.otaSessionTarget = otaSessionTarget;

export { otaSessionState, otaSessionTarget, otaFailReason };

/** @description 查询当前 OTA 会话是否处于活跃态（非 IDLE/DONE/FAILED）。 */
export function otaSessionActive() {
  return otaSessionState === OtaSessionState.UPLOADING ||
         otaSessionState === OtaSessionState.VERIFYING ||
         otaSessionState === OtaSessionState.RELAYING ||
         otaSessionState === OtaSessionState.REBOOTING;
}

/** @description 查询当前 OTA 会话是否已终结（DONE 或 FAILED）。 */
export function otaSessionTerminated() {
  return otaSessionState === OtaSessionState.DONE ||
         otaSessionState === OtaSessionState.FAILED;
}

/**
 * @description 转换 OTA 会话状态。
 *              统一在此处维护状态机一致性，同时同步旧标志位
 *              (isUploading / currentOtaTarget)，保证渐进式迁移期间两套标志不冲突。
 * @param {string} newState 目标状态。
 * @param {string} [reason] 失败原因（仅转入 FAILED 时使用）。
 */
export function otaSessionTransition(newState, reason) {
  var prev = otaSessionState;
  otaSessionState = newState;
  window.otaSessionState = newState;
  if (newState === OtaSessionState.FAILED) {
    otaFailReason = reason || '未知原因';
  } else if (newState === OtaSessionState.UPLOADING) {
    otaFailReason = '';
  }
  // 同步旧标志（渐进式迁移）：UPLOADING 对应 isUploading=true
  if (newState === OtaSessionState.UPLOADING) window.isUploading = true;
  else if (newState === OtaSessionState.IDLE ||
           newState === OtaSessionState.DONE ||
           newState === OtaSessionState.FAILED) window.isUploading = false;
  // REBOOTING 复用 isEsp32Rebooting（pollStatus 重启检测依赖）
  if (newState === OtaSessionState.REBOOTING) window.isEsp32Rebooting = true;
  else if (newState !== OtaSessionState.VERIFYING) window.isEsp32Rebooting = false;
}

/**
 * @description 清掉本会话残留 UI/标志（失败、超时、中止、新会话开始前都要调）。
 *              不改变 otaSessionState（由调用方负责 FAILED/IDLE 转换）。
 * @param {object} [opts]
 * @param {boolean} [opts.keepErrorStages]  true=保留错误阶段样式；false=节点复位到待机
 * @param {string} [opts.statusText]    状态栏文案；null 表示「待机」
 * @param {boolean} [opts.errorStyle]   状态栏是否红色
 */
export function clearOtaSessionResiduals(opts) {
  opts = opts || {};
  window.stopRelayProgressWatch();
  stopUploadWaitHeartbeat();

  window.gd32CompleteAnnounced = false;
  window.relayWatchLastSeq = -1;
  window.gd32RelaySessionLive = false;
  window.lastSysStateCode = -1;
  window.lastRelayState = 0;
  window.isEsp32Rebooting = false;
  window.rebootObservedDown = false;
  window.rebootStartTime = 0;
  window.isUploading = false;

  /* 先摘掉引用再 abort，避免 abort 事件回调再次进入 cleanup 递归 */
  var xh = window.activeXhr;
  window.activeXhr = null;
  if (xh) {
    try { xh.abort(); } catch (_) {}
  }

  // 校验面板
  var verifyInfo = document.getElementById('stageVerifyInfo');
  if (verifyInfo) verifyInfo.style.display = 'none';
  var sviFront = document.getElementById('sviFrontSha');
  var sviDevice = document.getElementById('sviDeviceSha');
  var sviResult = document.getElementById('sviResult');
  if (sviFront) sviFront.textContent = '-';
  if (sviDevice) sviDevice.textContent = '-';
  if (sviResult) {
    sviResult.textContent = '-';
    sviResult.className = 'svi-val';
  }

  // 阶段节点
  if (!opts.keepErrorStages) {
    ['es1','es2','es3','es4','es5','es6','s1','s2','s3','s4','s5','s6'].forEach(function(id) {
      var el = document.getElementById(id);
      if (el) el.className = 'stage-node';
    });
    ['esd1','esd2','esd3','esd4','esd5','esd6','sd1','sd2','sd3','sd4','sd5','sd6'].forEach(function(id) {
      var el = document.getElementById(id);
      if (el) el.textContent = '';
    });
    window.esp32OtaPhase = 0;
  }

  // 顶部步骤 pill
  ['ppEsp32', 'ppGd32Ota'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.classList.remove('active', 'done');
  });
  var fill = document.getElementById('progressFill');
  if (fill) fill.classList.remove('done');
  window.activePhase = null;

  // 状态栏
  var sb = document.getElementById('stageStatusBar');
  var st = document.getElementById('stageStatusText');
  if (sb) {
    sb.className = opts.errorStyle ? 'stage-status-bar err' : 'stage-status-bar';
  }
  if (st) {
    st.textContent = (opts.statusText !== undefined && opts.statusText !== null)
      ? opts.statusText
      : '待机';
  }

  // 设备侧缓存展示：失败后不要继续显示“已满缓存像成功”
  var mOtaCache = document.getElementById('mOtaCache');
  if (mOtaCache && opts.clearCacheDisplay) {
    mOtaCache.textContent = '0 / 216 KB';
    mOtaCache.className = 'v';
  }
  var sOtaStored = document.getElementById('sOtaStored');
  if (sOtaStored && opts.clearCacheDisplay) {
    sOtaStored.textContent = '--';
  }
}

/** @description 将会话从 FAILED/DONE 复位到 IDLE（用户发起新上传或设备空闲时调用）。 */
export function otaSessionReset() {
  clearOtaSessionResiduals({
    keepErrorStages: false,
    statusText: '待机',
    errorStyle: false,
    clearCacheDisplay: false
  });
  if (otaSessionState !== OtaSessionState.IDLE) {
    otaSessionState = OtaSessionState.IDLE;
    otaSessionTarget = null;
    otaFailReason = '';
    window.otaSessionState = OtaSessionState.IDLE;
    window.otaSessionTarget = null;
    window.currentOtaTarget = null;
    window.isUploading = false;
  }
  window.activeUploadChip = null;
}