/**
 * can.js - CAN 工具页面入口（ESM 组装层）
 *
 * 本文件对应原 tools/can.js 中未拆分到子模块的编排逻辑：
 *   - cacheElements：缓存 DOM 元素到 el（dbc.js 导出的共享 el 对象）
 *   - setConnectionState：CAN 页面专用连接状态更新（canConnPill 等元素）
 *   - testConnection：CAN 页面专用连接检测（GET /system/status，操作 el.connectionNote 等）
 *   - clearDbc / handleDbcFile / handleDbcError：DBC 文件读取与解析流程编排
 *   - bindEvents：绑定所有事件监听器（调用子模块的 bindNodeFilterEvents 等）
 *   - init：页面初始化入口
 *
 * state / el 由 dbc.js 导出（ES Module live binding），cacheElements() 填充 el 后，
 * 各子模块通过 import 引用同一对象，可见最新缓存结果。
 *
 * 注意：原 can.js 的 testConnection 与 common/connection.js 的 testConnection 实现不同
 *       （操作不同的 DOM 元素集），此处保留原 can.js 版本以保证行为一致。
 */

import { state, el, getBaseUrl, parseDbc, readDbcText, buildSummary } from './can/dbc.js';
import {
  updateDbcFileInfo,
  filterMessages,
  bindNodeFilterEvents,
  renderMessageList,
  selectMessage
} from './can/messages.js';
import {
  ensureMessageDefaults,
  renderSelectedMessage,
  updateEncodedPreview
} from './can/signals.js';
import {
  renderHistory,
  sendSelectedMessage,
  sendCheckedMessages,
  refreshManualHint,
  sendManualFrame,
  resetManualFields,
  loadPreviewToManual
} from './can/send.js';
import { escapeHtml as sanitizeHtml, formatUptime } from './common/utils.js';
import { showToast } from './common/toast.js';

// ==================== 本地工具 ====================
// 原 can.js 的 formatUptime 输出格式与 common/utils.js 略有差异（d>0 时只到 h），
// 为保持原行为，此处复用 common/utils.js 的 formatUptime（已在 import 中引入）。
// 若后续需要对齐原 can.js 输出，可在此处覆盖本地实现。

// ==================== 元素缓存 ====================
function cacheElements() {
  [
    'toolIp', 'defaultChannel', 'manualChannel', 'manualId', 'manualDlc', 'manualData',
    'testConnBtn', 'connIndicator', 'connIndicatorTxt', 'connectionNote',
    'dbcDropZone', 'dbcFileInput', 'dbcFileInfo', 'dbcFileName', 'dbcMessageCount',
    'dbcSignalCount', 'dbcNodeCount', 'dbcFileSize', 'dbcSearch', 'dbcHint', 'clearDbcBtn',
    'messageList', 'sendSelectedBtn', 'generatedHex', 'generatedBytes', 'encodeHint',
    'dbcResp', 'dbcRespText', 'loadPreviewToRawBtn', 'manualHint', 'sendManualBtn',
    'resetManualBtn', 'manualResp', 'manualRespText', 'historyList', 'toast', 'toastIco',
    'toastMsg', 'canConnPill', 'canConnTxt', 'canVersionPill', 'canChannelPill',
    'dbcSummaryPill', 'messageSummaryPill',
    'selectedCountPill', 'selectAllBox', 'expandAllBtn', 'collapseAllBtn', 'sendCheckedBtn',
    'nodeFilterWrap', 'nodeFilterBtn', 'nodeFilterLabel', 'nodeFilterPanel', 'nodeFilterSearch',
    'nodeFilterAllBtn', 'nodeFilterNoneBtn', 'nodeFilterList'
  ].forEach(function (id) {
    el[id] = document.getElementById(id);
  });
}

// ==================== 连接状态 ====================
function setConnectionState(connected, text, buildVersion, buildTime) {
  if (el.canConnPill) {
    el.canConnPill.className = 'pill ' + (connected ? 'online' : 'offline');
  }
  if (el.canConnTxt) el.canConnTxt.textContent = text;
  if (el.connIndicator) {
    el.connIndicator.className = 'conn-indicator ' + (connected ? 'ok' : 'err');
  }
  if (el.connIndicatorTxt) el.connIndicatorTxt.textContent = text;
  if (el.canVersionPill) {
    el.canVersionPill.textContent = buildVersion ? ('v' + buildVersion) : 'v--';
    el.canVersionPill.dataset.tooltip = buildTime ? ('编译: ' + buildTime) : '';
  }
}

async function testConnection() {
  var baseUrl = getBaseUrl();
  if (el.connectionNote) {
    el.connectionNote.className = 'helper-note';
    el.connectionNote.innerHTML = '正在请求 <code>/system/status</code> 检查设备状态...';
  }
  try {
    var resp = await fetch(baseUrl + '/system/status', { signal: AbortSignal.timeout(3000) });
    if (!resp.ok) {
      setConnectionState(false, 'HTTP ' + resp.status, '', '');
      if (el.connectionNote) {
        el.connectionNote.className = 'helper-note err';
        el.connectionNote.innerHTML = '状态接口返回 HTTP <b>' + resp.status + '</b>，请确认设备地址和 HTTP 服务。';
      }
      showToast('设备响应异常: HTTP ' + resp.status, 'error');
      return;
    }
    var data = await resp.json();
    state.latestStatus = data;
    var wifi = data.wifi || {};
    var connected = !!wifi.connected;
    var mode = wifi.mode || 'STA';
    var text = connected ? (mode + ' 已连接') : (mode + ' 未连接');
    setConnectionState(connected, text, data.version, data.build_time);
    if (el.connectionNote) {
      el.connectionNote.className = 'helper-note ' + (connected ? 'ok' : 'warn');
      el.connectionNote.innerHTML =
        'IP: <code>' + sanitizeHtml(wifi.ip || '--') + '</code>，RSSI: <b>' +
        sanitizeHtml(wifi.rssi !== undefined && wifi.rssi !== -127 ? (wifi.rssi + ' dBm') : '--') +
        '</b>，运行: <b>' + sanitizeHtml(formatUptime(data.uptime_sec)) + '</b>';
    }
    showToast(connected ? 'ESP32 当前在线' : '设备已响应，但 WiFi 未连接', connected ? 'success' : 'warning');
  } catch (err) {
    setConnectionState(false, '连接失败', '', '');
    if (el.connectionNote) {
      el.connectionNote.className = 'helper-note err';
      el.connectionNote.innerHTML = '无法访问 <code>/system/status</code>：' + sanitizeHtml(err.message);
    }
    showToast('连接失败: ' + err.message, 'error');
  }
}

// ==================== DBC 文件流程 ====================
async function handleDbcFile(file) {
  if (!file) return;
  var text = await readDbcText(file);
  var dbc = parseDbc(text);
  state.dbcFile = file;
  state.dbcText = text;
  state.dbc = dbc;
  state.selectedMessageKey = '';
  state.signalValues = {};
  state.checkedKeys = {};
  state.expandedKeys = {};
  state.transmitterFilter = Object.create(null);
  state.transmitterFilterEmpty = true;
  updateDbcFileInfo(file, dbc);
  buildSummary();
  filterMessages();
  if (dbc.messages.length) selectMessage(dbc.messages[0].key);
  else renderSelectedMessage();
  if (el.dbcHint) {
    el.dbcHint.className = 'helper-note ok';
    el.dbcHint.innerHTML = 'DBC 已解析：<b>' + dbc.messages.length + '</b> 条报文，<b>' +
      dbc.messages.reduce(function (sum, msg) { return sum + msg.signals.length; }, 0) +
      '</b> 个信号。';
  }
  showToast('DBC 导入成功', 'success');
}

function clearDbc() {
  state.dbcFile = null;
  state.dbcText = '';
  state.dbc = null;
  state.filteredMessages = [];
  state.selectedMessageKey = '';
  state.signalValues = {};
  state.checkedKeys = {};
  state.expandedKeys = {};
  state.transmitterFilter = Object.create(null);
  state.transmitterFilterEmpty = true;
  state.generatedData = new Uint8Array(0);
  if (el.dbcFileInput) el.dbcFileInput.value = '';
  if (el.dbcSearch) el.dbcSearch.value = '';
  updateDbcFileInfo(null, null);
  buildSummary();
  renderMessageList();
  renderSelectedMessage();
  if (el.generatedHex) el.generatedHex.textContent = '--';
  if (el.generatedBytes) el.generatedBytes.innerHTML = '';
  if (el.dbcHint) {
    el.dbcHint.className = 'helper-note';
    el.dbcHint.innerHTML = '当前解析器会自动识别常见 DBC 的 <code>BO_</code>、<code>SG_</code>、<code>VAL_</code>、<code>CM_</code> 和 <code>SIG_VALTYPE_</code>。';
  }
}

function handleDbcError(err) {
  if (el.dbcHint) {
    el.dbcHint.className = 'helper-note err';
    el.dbcHint.innerHTML = 'DBC 解析失败：' + sanitizeHtml(err.message);
  }
  showToast('DBC 解析失败: ' + err.message, 'error');
}

// ==================== 事件绑定 ====================
function bindEvents() {
  el.testConnBtn.addEventListener('click', testConnection);
  el.defaultChannel.addEventListener('change', function () {
    el.manualChannel.value = el.defaultChannel.value;
    el.canChannelPill.textContent = 'CH' + el.defaultChannel.value;
    renderMessageList();
    renderSelectedMessage();
    updateEncodedPreview();
  });
  el.dbcDropZone.addEventListener('click', function () { el.dbcFileInput.click(); });
  el.dbcDropZone.addEventListener('dragover', function (event) {
    event.preventDefault();
    el.dbcDropZone.classList.add('dragover');
  });
  el.dbcDropZone.addEventListener('dragleave', function () {
    el.dbcDropZone.classList.remove('dragover');
  });
  el.dbcDropZone.addEventListener('drop', function (event) {
    event.preventDefault();
    el.dbcDropZone.classList.remove('dragover');
    var file = event.dataTransfer && event.dataTransfer.files ? event.dataTransfer.files[0] : null;
    if (file) handleDbcFile(file).catch(handleDbcError);
  });
  el.dbcFileInput.addEventListener('change', function (event) {
    var file = event.target && event.target.files ? event.target.files[0] : null;
    if (file) handleDbcFile(file).catch(handleDbcError);
  });
  el.clearDbcBtn.addEventListener('click', clearDbc);
  el.dbcSearch.addEventListener('input', filterMessages);
  el.sendSelectedBtn.addEventListener('click', sendSelectedMessage);
  el.loadPreviewToRawBtn.addEventListener('click', loadPreviewToManual);
  el.sendManualBtn.addEventListener('click', sendManualFrame);
  el.resetManualBtn.addEventListener('click', resetManualFields);
  el.manualData.addEventListener('input', refreshManualHint);
  el.manualDlc.addEventListener('input', refreshManualHint);
  el.manualId.addEventListener('input', refreshManualHint);
  el.toolIp.addEventListener('change', testConnection);
  if (el.sendCheckedBtn) el.sendCheckedBtn.addEventListener('click', sendCheckedMessages);
  if (el.selectAllBox) el.selectAllBox.addEventListener('change', function () {
    var visible = state.filteredMessages || [];
    if (this.checked) {
      visible.forEach(function (m) { state.checkedKeys[m.key] = true; });
    } else {
      visible.forEach(function (m) { delete state.checkedKeys[m.key]; });
    }
    renderMessageList();
  });
  if (el.expandAllBtn) el.expandAllBtn.addEventListener('click', function () {
    var visible = state.filteredMessages || [];
    visible.forEach(function (m) { state.expandedKeys[m.key] = true; ensureMessageDefaults(m); });
    renderMessageList();
  });
  if (el.collapseAllBtn) el.collapseAllBtn.addEventListener('click', function () {
    var visible = state.filteredMessages || [];
    visible.forEach(function (m) { delete state.expandedKeys[m.key]; });
    renderMessageList();
  });
  bindNodeFilterEvents();
}

// ==================== 初始化 ====================
function init() {
  cacheElements();
  bindEvents();
  setConnectionState(false, '未连接', '', '');
  if (el.canChannelPill) el.canChannelPill.textContent = 'CH' + el.defaultChannel.value;
  if (el.manualChannel) el.manualChannel.value = el.defaultChannel.value;
  buildSummary();
  renderMessageList();
  renderSelectedMessage();
  refreshManualHint();
  renderHistory();
  setTimeout(testConnection, 150);
}

init();