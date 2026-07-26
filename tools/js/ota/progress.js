/**
 * progress.js - 折叠面板 + 刷写进度 + 阶段节点 UI
 *
 * 操作 DOM: #esp32Stages / #gd32Stages / #stageStatusBar / #progressFill / #progressTxt / #progressPct
 *
 * 共享状态（通过 window.* 引用）：
 *   window.activePhase / window.currentOtaTarget / window.esp32OtaPhase
 *
 * 复用的函数（通过 window.* 引用）：
 *   window.updateStagesFromRelayState / window.cleanupAfterOtaFailure
 *   window.resumePollingAfterUpload / window.addLog
 */
import { addLog } from '../common/log.js';

// ==================== 常量 ====================
// ESP32 OTA 阶段 DOM 节点 id（es1..es6 + esd1..esd6）
var ESP32_STAGE_IDS = ['es1', 'es2', 'es3', 'es4', 'es5', 'es6'];
var ESP32_DETAIL_IDS = ['esd1', 'esd2', 'esd3', 'esd4', 'esd5', 'esd6'];
// 顶部步骤 pill → phase 名映射
var STEP_MAP = { esp32Ota: 'ppEsp32', gd32Ota: 'ppGd32Ota' };
// 进度条 DOM 元素懒缓存（首次访问后复用，DOM 未就绪时回退到 getElementById）
var progressFillEl = null;
var progressTxtEl = null;
var progressPctEl = null;

// ==================== 折叠面板 ====================
export function toggleCollapse(headEl) {
  headEl.closest('.card').classList.toggle('collapsed');
}

// ==================== 刷写进度 ====================
window.activePhase = null;  // 'esp32Ota' | 'gd32Ota' | null
window.currentOtaTarget = null;  // 'esp32' | 'gd32' | null — 当前 OTA 目标，决定显示哪套阶段

// 切换 OTA 阶段面板显示（原始版本，会被 console-mode.js 覆盖以支持 BLE）
export function switchStageFlow(target) {
  window.currentOtaTarget = target;
  var esp32El = document.getElementById('esp32Stages');
  var gd32El = document.getElementById('gd32Stages');
  if (target === 'esp32') {
    if (esp32El) esp32El.style.display = '';
    if (gd32El) gd32El.style.display = 'none';
  } else if (target === 'gd32') {
    if (esp32El) esp32El.style.display = 'none';
    if (gd32El) gd32El.style.display = '';
  } else {
    // null / 'ble' / 未知：隐藏两套面板（reset 语义，避免残留显示）
    if (esp32El) esp32El.style.display = 'none';
    if (gd32El) gd32El.style.display = 'none';
  }
  resetEsp32StageNodes();
  resetGd32StageNodes();
}

/** @description 把 ESP32 阶段节点（es1..es6 / esd1..esd6）复位到初始态。 */
function resetEsp32StageNodes() {
  ESP32_STAGE_IDS.forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
  ESP32_DETAIL_IDS.forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.textContent = '';
  });
}

/** @description 把 GD32 阶段节点（s1..s6 / sd1..sd6）复位到初始态。 */
function resetGd32StageNodes() {
  ['s1', 's2', 's3', 's4', 's5', 's6'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
  ['sd1', 'sd2', 'sd3', 'sd4', 'sd5', 'sd6'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.textContent = '';
  });
}

// ESP32 OTA 阶段状态: 0=待机, 1=上传中, 2=OTA写入, 3=SHA-256校验, 4=应用固件, 5=重启中, 6=启动完成
window.esp32OtaPhase = 0;
export const esp32PhaseLabels = ['', '上传中', '写入中', '校验中', '应用中', '重启中', '已完成'];

export function updateEsp32StagesFromPhase(phase) {
  window.esp32OtaPhase = phase;
  var all = ESP32_STAGE_IDS.map(function (id) { return document.getElementById(id); });
  var details = ESP32_DETAIL_IDS.map(function (id) { return document.getElementById(id); });
  all.forEach(function (s) { if (s) s.className = 'stage-node'; });
  details.forEach(function (d) { if (d) d.textContent = ''; });
  var statusBar = document.getElementById('stageStatusBar');
  var statusText = document.getElementById('stageStatusText');
  if (phase <= 0) {
    if (statusBar) statusBar.className = 'stage-status-bar';
    if (statusText) statusText.textContent = '待机';
    return;
  }
  /* phase=N：前 N-1 步 done；第 N 步进行中 live。
   * phase=6（启动完成/已完成）是终态：全部 done（绿），最后一步不要 live（蓝）。 */
  for (var i = 0; i < 6; i++) {
    if (!all[i]) continue;
    if (i < phase - 1) {
      all[i].className = 'stage-node done';
    } else if (i === phase - 1) {
      all[i].className = (phase >= 6) ? 'stage-node done' : 'stage-node live';
      if (details[i]) details[i].textContent = esp32PhaseLabels[phase] || '';
    }
  }
  if (statusBar && statusText) {
    if (phase >= 6) {
      statusBar.className = 'stage-status-bar ok';
      statusText.textContent = 'ESP32 刷写完成';
    } else {
      statusBar.className = 'stage-status-bar active';
      statusText.textContent = (esp32PhaseLabels[phase] || '进行中') + ' (' + phase + '/6)';
    }
  }
}

export function updateEsp32StagesError() {
  var all = ESP32_STAGE_IDS.map(function (id) { return document.getElementById(id); });
  var details = ESP32_DETAIL_IDS.map(function (id) { return document.getElementById(id); });
  all.forEach(function (s) { if (s) s.className = 'stage-node'; });
  // ★ 失败态：阶段下方文案也要清空，避免残留"上传中/校验中"与红色样式并存
  details.forEach(function (d) { if (d) d.textContent = ''; });
  var statusBar = document.getElementById('stageStatusBar');
  var statusText = document.getElementById('stageStatusText');
  if (statusBar) statusBar.className = 'stage-status-bar err';
  if (statusText) statusText.textContent = '校验/应用失败';
}

/**
 * @description OTA 失败后的统一清理函数。
 * 所有上传失败、校验失败、中继触发失败等异常路径都应调用此函数，
 * 确保 UI 状态完整重置，不会残留"上传中"或中间态。
 *
 * ★ 状态机改造：转入 FAILED 态，置位 otaFailReason。
 *   之后轮询（pollStatus/updateSystemStatusUi）不得覆盖此失败态，
 *   直到用户发起新上传（otaSessionReset）或检测到设备已回 IDLE。
 * @param {string} reason 失败原因描述
 */
export function cleanupAfterOtaFailure(reason) {
  // 幂等：已在 FAILED 且无活跃 XHR 时只刷新文案，避免 abort 事件二次递归清状态
  var alreadyFailed = window.otaSessionState === window.OtaSessionState.FAILED;
  /* chip 判定优先级（权威来源顺序）：
   *   activeUploadChip — XHR 上传开始时由 startUpload 写入，WiFi 路径权威
   *   otaSessionTarget — OTA 会话状态机目标（仅 WiFi 走 OtaSessionState，BLE 不走）
   *   currentOtaTarget — switchStageFlow 设置的当前面板目标
   *   flashTarget      — 用户选择的目标（BLE 路径未设上述两者时兜底）
   *   'esp32'          — 最终默认值 */
  var chip = window.activeUploadChip || window.otaSessionTarget ||
             window.currentOtaTarget || window.flashTarget || 'esp32';

  // 0. 状态机：转入 FAILED（先于标志重置）
  window.otaSessionTransition(window.OtaSessionState.FAILED, reason);

  // 1. 清掉本会话全部残留标志 / 定时器 / 校验面板 / 旧 progress 语义
  window.clearOtaSessionResiduals({
    keepErrorStages: true,
    statusText: reason || '失败',
    errorStyle: true
  });

  // 2. 阶段指示器标错
  if (chip === 'esp32') {
    updateEsp32StagesError();
  } else {
    window.updateStagesFromRelayState(255);
  }

  // 3. 进度条失败文案（clear 已把 fill 的 done 去掉）
  setProgress(0, reason || '刷写失败');
  setActivePhase(null);
  window.currentOtaTarget = null;

  // 4. 恢复上传按钮（两枚都解禁并恢复各自原文案，避免 chip 判错或一侧残留"上传中"）
  //    ★ 按钮文案是固定的，绝不能用共享的 uploadBtnOrigText 回写两枚，否则最后
  //      刷 GD32 后 ESP32 按钮会被错写成 "GD32 OTA"（反之亦然）。
  var uploadBtnLabels = { uploadEsp32Btn: '上传并升级 ESP32', uploadGd32Btn: 'GD32 OTA' };
  Object.keys(uploadBtnLabels).forEach(function (btnId) {
    var btn = document.getElementById(btnId);
    if (!btn) return;
    btn.disabled = false;
    btn.textContent = uploadBtnLabels[btnId];
  });
  window.activeUploadChip = null;
  window.otaSessionTarget = null;
  window.uploadBtnOrigText = '';

  if (typeof window.updateUploadButtonLabel === 'function') {
    window.updateUploadButtonLabel();
  }

  // 5. 恢复轮询（之后 status 不得覆盖 FAILED UI，直至用户新上传或设备 idle 复位）
  if (typeof window.resumePollingAfterUpload === 'function') {
    window.resumePollingAfterUpload();
  }

  if (!alreadyFailed) {
    addLog('E', 'OTA', '刷写流程已中止，已清理残留状态: ' + (reason || '未知原因'));
  }
}

/** @description 设置当前活跃 OTA 阶段并更新 UI 步骤指示器。 */
export function setActivePhase(phase) {
  if (phase === 'gd32Upload' || phase === 'gd32Relay') phase = 'gd32Ota';
  window.activePhase = phase;
  Object.keys(STEP_MAP).forEach(function (p) {
    var el = document.getElementById(STEP_MAP[p]);
    if (!el) return;
    el.classList.remove('active', 'done');
    if (p === phase) el.classList.add('active');
  });
  // Reset progress fill color
  var fill = document.getElementById('progressFill');
  if (fill) fill.classList.remove('done');
}

/** @description 标记指定阶段为完成，更新进度条颜色。 */
export function markPhaseDone(phase) {
  if (phase === 'gd32Upload' || phase === 'gd32Relay') phase = 'gd32Ota';
  var el = document.getElementById(STEP_MAP[phase]);
  if (el) { el.classList.remove('active'); el.classList.add('done'); }
  // All done? turn progress bar green
  if (phase === 'gd32Ota' || phase === 'esp32Ota') {
    var fill = document.getElementById('progressFill');
    if (fill) fill.classList.add('done');
  }
}

/** @description 更新进度条百分比和文本显示。对 pct 做边界校验，过滤 NaN/undefined。 */
export function setProgress(pct, text) {
  // 数值校验：BLE ACK 高频调用路径偶尔传入 undefined/NaN，需兜底避免出现 "undefined%"
  var n = Number(pct);
  if (!Number.isFinite(n)) n = 0;
  if (n < 0) n = 0;
  else if (n > 100) n = 100;
  // 懒缓存 DOM 元素引用；DOM 未就绪时回退到 getElementById（兼容早期调用）
  if (!progressFillEl || !progressFillEl.isConnected) progressFillEl = document.getElementById('progressFill');
  if (!progressTxtEl || !progressTxtEl.isConnected) progressTxtEl = document.getElementById('progressTxt');
  if (!progressPctEl || !progressPctEl.isConnected) progressPctEl = document.getElementById('progressPct');
  if (progressFillEl) progressFillEl.style.width = n + '%';
  if (progressTxtEl) progressTxtEl.textContent = text;
  if (progressPctEl) progressPctEl.textContent = n + '%';
}

/** @description 重置进度条和阶段状态到初始值。 */
export function resetProgress() {
  setProgress(0, '等待操作...');
  window.activePhase = null;
  window.esp32OtaPhase = 0;
  switchStageFlow(null);
  ['ppEsp32', 'ppGd32Ota'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.classList.remove('active', 'done');
  });
  var fill = document.getElementById('progressFill');
  if (fill) fill.classList.remove('done');
}