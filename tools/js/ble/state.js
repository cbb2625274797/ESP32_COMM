// ==================== BLE OTA 状态与元素缓存 ====================
import { TARGETS } from './constants.js';
import { addLog } from '../common/log.js';
import { updateBleStageLabels, resetBleStages } from './stages.js';
import { hideBleVerifyInfo } from './verify.js';
export const state = {
  device: null,
  server: null,
  service: null,
  ctrlChar: null,
  dataInChar: null,
  dataOutChar: null,
  versionChar: null,
  notificationsEnabled: false,
  selectedFile: null,
  selectedFileHandle: null,  // FileSystemFileHandle for hot reload
  abortRequested: false,
  uploading: false,
  autoFlashing: false,
  lastUploadStatus: 'idle',
  lastNotificationHex: '',
  lastAckCmd: null,
  lastAckSeq: -1,
  lastAckBytes: 0,
  ackWaiter: null,
  otaResultWaiter: null,
  otaRunId: 0,
  lastEsp32DoneSession: null,
  fileSha256: null,       // 当前 BLE 会话用的 SHA（与对应芯片槽同步）
  deviceSha256: null,
  expectedReboot: false,        // 发送 0xFA 后置 true，重连后清
  reconnecting: false,          // 自动重连中
  reconnectAttempts: 0,         // 重连尝试次数
  uploadTargetKey: null,
  activeTotalSize: 0,
  activePacketTotal: 0,
};

export const $ = (id) => document.getElementById(id);
export const els = {
  connectBtn: $('connectBtn'),
  disconnectBtn: $('disconnectBtn'),
  autoFlashBtn: $('autoFlashBtn'),
  abortBtn: $('abortBtn'),
  uploadZone: $('uploadZone'),
  firmwareFile: $('firmwareFile'),
  connState: $('connState'),
  connBadge: $('connBadge'),
  deviceName: $('deviceName'),
  deviceId: $('deviceId'),
  gattState: $('gattState'),
  versionText: $('versionText'),
  fileInfo: $('fileInfo'),
  clearFileBtn: $('clearFileBtn'),
  progressFill: $('progressFill'),      // 共享进度条
  progressTxt: $('progressTxt'),        // 共享进度文本
  progressPct: $('progressPct'),        // 共享进度百分比
  fileName: $('fileName'),
  fileType: $('fileType'),
  fileSize: $('fileSize'),
  packetCount: $('packetCount'),
  sentCount: $('sentCount'),
  logScroll: $('logScroll'),            // 共享日志面板
};

export function currentTarget() {
  return TARGETS[window.flashTarget] || TARGETS.esp32;
}

/** BLE 通道是否正在上传/自动刷写。集中判定，避免各处重复 state.uploading || state.autoFlashing。 */
export function isBleUploading() {
  return !!(state.uploading || state.autoFlashing);
}

export function currentPayloadSize() {
  const cfg = currentTarget();
  return Math.max(32, Math.min(498, Number(cfg.recommendedPayload) || 480));
}

export function flowTarget() {
  return TARGETS[state.uploadTargetKey] || currentTarget();
}

export function bleLog(tag, message, level) {
  level = level || 'I';
  if (typeof addLog === 'function') {
    addLog(level, 'BLE-' + tag, message);
  }
}

export function toHex(uint8) {
  return Array.from(uint8).map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
}

export function hexToBytes(hex) {
  const clean = hex.replace(/[^0-9a-fA-F]/g, '');
  if (clean.length === 0 || clean.length % 2 !== 0) throw new Error('HEX 长度必须为偶数');
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < clean.length; i += 2) out[i / 2] = parseInt(clean.slice(i, i + 2), 16);
  return out;
}

export async function sleep(ms) {
  return new Promise(r => setTimeout(r, ms));
}

export function setReadyHint(html) {
  const el = $('readyHint');
  if (el) el.innerHTML = html;
}

export function setConnectionState(connected) {
  const idle = connected && state.selectedFile && !state.uploading;
  // BLE 模式：连接状态只写顶栏胶囊，避免下方重复文案
  if (typeof connectionMode !== 'undefined' && connectionMode === 'ble') {
    var pc = document.getElementById('pillConn');
    var pct = document.getElementById('pillConnTxt');
    if (pc) pc.className = connected ? 'pill online' : 'pill offline';
    if (pct) pct.textContent = connected ? 'BLE 已连接' : 'BLE 未连接';
    if (els.connState) {
      els.connState.textContent = connected ? '已连接' : '未连接';
      if (els.connState.parentElement) {
        els.connState.parentElement.className = connected ? 'pill online' : 'pill offline';
      }
    }
    if (els.connBadge) {
      els.connBadge.textContent = connected ? 'online' : 'offline';
      els.connBadge.className = `v ${connected ? 'ok' : 'bad'}`;
    }
    if (els.gattState) els.gattState.textContent = connected ? 'GATT 已就绪' : '未就绪';
  }
  els.connectBtn.disabled = connected;
  els.disconnectBtn.disabled = !connected;
  els.autoFlashBtn.disabled = !idle;
  els.abortBtn.disabled = !state.uploading;

  if (!connected) {
    setReadyHint('当前未连接设备。建议顺序：连接设备 → 选择固件 → 开始刷写。');
  } else if (!state.notificationsEnabled) {
    setReadyHint('设备已连接，正在自动准备刷写通道。若长时间无响应，请断开后重连。');
  } else if (!state.selectedFile) {
    setReadyHint('设备已连接。下一步请选择目标芯片与固件文件。');
  } else {
    setReadyHint('设备与文件都已就绪，可以直接开始刷写或执行一键刷写。');
  }
}

export function resetUiAfterDisconnect() {
  state.server = null;
  state.service = null;
  state.ctrlChar = null;
  state.dataInChar = null;
  state.dataOutChar = null;
  state.versionChar = null;
  state.notificationsEnabled = false;
  state.uploading = false;
  state.autoFlashing = false;
  state.lastUploadStatus = 'idle';
  state.deviceSha256 = null;
  if (typeof connectionMode === 'undefined' || connectionMode === 'ble') {
    els.deviceName.textContent = '-';
    els.deviceId.textContent = '-';
    if (els.versionText) els.versionText.textContent = '未读取';
    var pv = document.getElementById('pillVer');
    if (pv) pv.textContent = 'v--';
  }
  resetBleStages();
  hideBleVerifyInfo();
  setConnectionState(false);
}

export function refreshTargetUi() {
  const cfg = currentTarget();
  updateBleStageLabels(cfg);
  /* 与 WiFi 一致：文件由 syncBleFileFromSelectedTarget 按 flashTarget 绑定；
   * 此处只刷新标签/包数/阶段说明。 */
  if (state.selectedFile) {
    const payloadSize = currentPayloadSize();
    const packets = Math.ceil(state.selectedFile.size / payloadSize);
    if (els.packetCount) els.packetCount.textContent = String(packets);
  }
  setConnectionState(!!state.server);
  var espTab = document.getElementById('targetEsp32Tab');
  var gdTab = document.getElementById('targetGd32Tab');
  var isEsp = cfg.label === 'ESP32';
  if (espTab) espTab.classList.toggle('active', isEsp);
  if (gdTab) gdTab.classList.toggle('active', !isEsp);
  var autoBtn = document.getElementById('autoFlashBtn');
  if (autoBtn && !state.uploading && !state.autoFlashing) {
    autoBtn.textContent = isEsp ? '一键刷写 ESP32' : '一键刷写 GD32';
  }
}
