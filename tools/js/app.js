/**
 * app.js - ESP32 OTA 中继控制台入口（WiFi OTA 通道）
 *
 * 薄入口：import 所有 ota/* + common/* 模块，将 HTML onclick 引用的函数
 * 显式挂载到 window 对象，保留原有内联事件处理器兼容。
 *
 * 模块通过 window.* 共享状态，模块间依赖通过 import 解耦。
 * 此入口只负责挂载 + 顶层副作用（拖拽区绑定、SPI 输入监听、启动轮询）。
 */
import { formatBytes, formatTime, formatUptime, escapeHtml } from './common/utils.js';
import { showToast } from './common/toast.js';
import { addLog, clearLogs, setLogFilter, toggleAutoScroll } from './common/log.js';
import {
  updateConnectionUi, updateHealthUi, updateHeapAndTaskStatusUi,
  testConnection, refreshSystemStatusNow
} from './common/connection.js';
import {
  scheduleStatusPoll, startPolling, pausePollingForUpload,
  resumePollingAfterUpload, pollStatus, pollLogs
} from './common/polling.js';

import {
  OtaSessionState, otaSessionState, otaSessionTarget, otaFailReason,
  otaSessionActive, otaSessionTerminated, otaSessionTransition,
  otaSessionReset, clearOtaSessionResiduals,
  stopUploadWaitHeartbeat, startUploadWaitHeartbeat
} from './ota/session.js';
import {
  toggleCollapse, switchStageFlow, updateEsp32StagesFromPhase,
  updateEsp32StagesError, cleanupAfterOtaFailure, setActivePhase,
  markPhaseDone, setProgress, resetProgress, esp32PhaseLabels
} from './ota/progress.js';
import {
  updateStagesFromRelayState, updateSystemStatusUi
} from './ota/stages.js';
import {
  handleFileSelect, pickFileViaFSAPI, setupDropZone,
  parseIntelHex, isIntelHexText, convertHexFileToBin,
  processFile, clearFile
} from './ota/file-handle.js';
import { sendSpiFrame, clearSpiFields, updateSpiHint } from './ota/spi.js';
import { startUpload, verifyEsp32Firmware, restartEsp32 } from './ota/upload-esp32.js';
import { triggerGd32Relay, markGd32OtaUiComplete, stopRelayProgressWatch, startRelayProgressWatch } from './ota/upload-gd32.js';

// ==================== 工具函数（部分未拆分的辅助函数） ====================
function formatPercent(value) {
  if (!Number.isFinite(value)) return '--';
  return value.toFixed(1).replace(/\.0$/, '') + '%';
}
function formatHttpErrorDetail(text) {
  var detail = (text || '').replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ').trim();
  if (detail.length > 180) detail = detail.slice(0, 177) + '...';
  return detail;
}
function isOtaBusyError(status, detail) {
  var text = (detail || '').toLowerCase();
  return status === 403 ||
    status === 409 ||
    status === 423 ||
    text.indexOf('busy') >= 0 ||
    text.indexOf('occupied') >= 0 ||
    text.indexOf('another ota') >= 0 ||
    text.indexOf('invalid state') >= 0 ||
    text.indexOf('占用') >= 0 ||
    text.indexOf('互斥') >= 0 ||
    text.indexOf('系统忙') >= 0;
}
function getEspIp() {
  return document.getElementById('espIp').value.trim() || '192.168.31.127';
}
function getBaseUrl() {
  return 'http://' + getEspIp();
}
function updateUploadButtonLabel() {
  var esp32Btn = document.getElementById('uploadEsp32Btn');
  var gd32Btn = document.getElementById('uploadGd32Btn');
  if (esp32Btn) esp32Btn.disabled = !window.esp32File || window.isUploading;
  if (gd32Btn) gd32Btn.disabled = !window.gd32File || window.isUploading;
}
function resetStageFlow() {
  ['s1', 's2', 's3', 's4', 's5', 's6'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
  ['sd1', 'sd2', 'sd3', 'sd4', 'sd5', 'sd6'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.textContent = '';
  });
  var statusBar = document.getElementById('stageStatusBar');
  if (statusBar) statusBar.className = 'stage-status-bar';
  var statusText = document.getElementById('stageStatusText');
  if (statusText) statusText.textContent = '待机';
}
function applyDisconnectedRuntimeState() {
  document.getElementById('pillIP').textContent = '--';
  document.getElementById('pillRSSI').textContent = '--';
  // ★ 状态机守卫：会话活跃时不重置进度/阶段，避免覆盖进行中的 OTA 进度
  if (otaSessionActive()) return;
  // FAILED 态：保留失败进度显示，只清连接胶囊（让用户看到失败原因）
  if (otaSessionState === OtaSessionState.FAILED) return;
  resetProgress();
  resetStageFlow();
}
function applyRebootingRuntimeState() {
  var pc = document.getElementById('pillConn');
  var pct = document.getElementById('pillConnTxt');
  pc.className = 'pill processing';
  var elapsed = Math.round((Date.now() - window.rebootStartTime) / 1000);
  pct.textContent = '重启中 ' + elapsed + 's';
  var cs = document.getElementById('connStatus');
  var cst = document.getElementById('connStatusTxt');
  if (cs) cs.className = 'conn-indicator warn';
  if (cst) cst.textContent = 'ESP32 正在重启...';
  var bc = document.getElementById('bConn');
  if (bc) bc.textContent = '重启中';
  if (bc) bc.className = 'v warn';
}
function applyUploadingRuntimeState() {
  // ★ 状态机守卫：会话已失败/完成时，绝不重新应用"上传中"运行时态，
  //   防止网络抖动后的轮询错误分支把 UI 拉回"上传中"。
  if (otaSessionState === OtaSessionState.FAILED ||
      otaSessionState === OtaSessionState.DONE) {
    return;
  }
  var pc = document.getElementById('pillConn');
  var pct = document.getElementById('pillConnTxt');
  pc.className = 'pill processing';
  pct.textContent = '上传中';
  var cs = document.getElementById('connStatus');
  var cst = document.getElementById('connStatusTxt');
  if (cs) cs.className = 'conn-indicator warn';
  if (cst) cst.textContent = '固件上传中，设备忙...';
  var bc = document.getElementById('bConn');
  if (bc) bc.textContent = '上传中';
  if (bc) bc.className = 'v warn';
}

// ==================== 挂载 ota/* 函数到 window（供其他模块通过 window.* 调用） ====================
window.OtaSessionState = OtaSessionState;
window.otaSessionActive = otaSessionActive;
window.otaSessionTerminated = otaSessionTerminated;
window.otaSessionTransition = otaSessionTransition;
window.otaSessionReset = otaSessionReset;
window.clearOtaSessionResiduals = clearOtaSessionResiduals;
window.stopUploadWaitHeartbeat = stopUploadWaitHeartbeat;
window.startUploadWaitHeartbeat = startUploadWaitHeartbeat;

window.toggleCollapse = toggleCollapse;
window.switchStageFlow = switchStageFlow;
window.updateEsp32StagesFromPhase = updateEsp32StagesFromPhase;
window.updateEsp32StagesError = updateEsp32StagesError;
window.cleanupAfterOtaFailure = cleanupAfterOtaFailure;
window.setActivePhase = setActivePhase;
window.markPhaseDone = markPhaseDone;
window.setProgress = setProgress;
window.resetProgress = resetProgress;

window.updateStagesFromRelayState = updateStagesFromRelayState;
window.updateSystemStatusUi = updateSystemStatusUi;

window.handleFileSelect = handleFileSelect;
window.pickFileViaFSAPI = pickFileViaFSAPI;
window.parseIntelHex = parseIntelHex;
window.isIntelHexText = isIntelHexText;
window.convertHexFileToBin = convertHexFileToBin;
window.processFile = processFile;
window.clearFile = clearFile;

window.sendSpiFrame = sendSpiFrame;
window.clearSpiFields = clearSpiFields;
window.updateSpiHint = updateSpiHint;

window.startUpload = startUpload;
window.verifyEsp32Firmware = verifyEsp32Firmware;
window.restartEsp32 = restartEsp32;

window.triggerGd32Relay = triggerGd32Relay;
window.markGd32OtaUiComplete = markGd32OtaUiComplete;
window.stopRelayProgressWatch = stopRelayProgressWatch;
window.startRelayProgressWatch = startRelayProgressWatch;

// ==================== 挂载 common/* 函数到 window（供 polling.js 等通过 window.* 调用） ====================
window.formatBytes = formatBytes;
window.formatTime = formatTime;
window.formatUptime = formatUptime;
window.escapeHtml = escapeHtml;
window.formatPercent = formatPercent;
window.showToast = showToast;
window.addLog = addLog;
window.clearLogs = clearLogs;
window.setLogFilter = setLogFilter;
window.toggleAutoScroll = toggleAutoScroll;
window.updateConnectionUi = updateConnectionUi;
window.updateHealthUi = updateHealthUi;
window.updateHeapAndTaskStatusUi = updateHeapAndTaskStatusUi;
window.testConnection = testConnection;
window.refreshSystemStatusNow = refreshSystemStatusNow;
window.scheduleStatusPoll = scheduleStatusPoll;
window.startPolling = startPolling;
window.pausePollingForUpload = pausePollingForUpload;
window.resumePollingAfterUpload = resumePollingAfterUpload;
window.pollStatus = pollStatus;
window.pollLogs = pollLogs;

// ==================== 挂载本入口的辅助函数到 window ====================
window.getEspIp = getEspIp;
window.getBaseUrl = getBaseUrl;
window.updateUploadButtonLabel = updateUploadButtonLabel;
window.resetStageFlow = resetStageFlow;
window.applyDisconnectedRuntimeState = applyDisconnectedRuntimeState;
window.applyRebootingRuntimeState = applyRebootingRuntimeState;
window.applyUploadingRuntimeState = applyUploadingRuntimeState;
window.formatHttpErrorDetail = formatHttpErrorDetail;
window.isOtaBusyError = isOtaBusyError;

// ==================== 顶层副作用（原 app.js 末尾的初始化） ====================
setupDropZone('uploadZoneEsp32', 'esp32');
setupDropZone('uploadZoneGd32', 'gd32');
var spiTypeEl = document.getElementById('spiType');
var spiDataEl = document.getElementById('spiData');
if (spiTypeEl) spiTypeEl.addEventListener('input', updateSpiHint);
if (spiDataEl) spiDataEl.addEventListener('input', updateSpiHint);

// 启动轮询（必须在所有模块加载完成、window.OtaSessionState 已挂载后调用，
//   否则 polling.js 的 scheduleStatusPoll 会因 window.OtaSessionState 未定义而抛 TypeError）
startPolling();

// IP 输入变化时清零失败计数并立即刷新状态
var ipInput = document.getElementById('espIp');
if (ipInput) {
  ipInput.addEventListener('change', function () {
    window.consecutivePollFailures = 0;
    addLog('I', '系统', 'IP 已修改，重新启动轮询...');
    if (!window.statusPollInFlight) {
      scheduleStatusPoll(0);
    }
  });
}

// 初始化按钮状态与提示
setTimeout(function () {
  updateUploadButtonLabel();
  addLog('I', '系统', '控制台初始化完成，等待固件文件...');
  updateSpiHint();
}, 500);