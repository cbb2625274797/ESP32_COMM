/**
 * can.js - CAN 工具页面入口（ESM 组装层）
 *
 * CAN 页面的编排逻辑：
 *   - cacheElements：缓存 DOM 元素到 el（dbc.js 导出的共享 el 对象）
 *   - setConnectionState：顶栏 pills（连接/IP/RSSI/运行/版本）
 *   - testConnection：GET /system/status，静默刷新顶栏（5s）
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

/* JS 间 import 一律不带 ?v=，版本戳只出现在 HTML 入口标签上，
 * 避免同一模块因 query 不同被浏览器实例化两次（state/toast 单例分裂） */
import {
  state, el, getBaseUrl, getDeviceIp, setDeviceIp, parseDbc, readDbcText, buildSummary,
  rebuildMergedDbc, makeDbcSourceId
} from './can/dbc.js';
import {
  updateDbcFileInfo,
  filterMessages,
  bindNodeFilterEvents,
  renderMessageList,
  selectMessage,
  recomputeTransmitterFilterEmpty,
  updateNodeFilterLabel,
  updateBatchUi
} from './can/messages.js';
import {
  ensureMessageDefaults,
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
import {
  scheduleCanPersist,
  loadCanSession,
  bindCanPersistLifecycle
} from './can/persist.js';
import { escapeHtml as sanitizeHtml, formatUptime } from './common/utils.js';
import { showToast } from './common/toast.js';
import { initCanMonitor } from './can_monitor.js';

// ==================== 本地工具 ====================
// 原 can.js 的 formatUptime 输出格式与 common/utils.js 略有差异（d>0 时只到 h），
// 为保持原行为，此处复用 common/utils.js 的 formatUptime（已在 import 中引入）。
// 若后续需要对齐原 can.js 输出，可在此处覆盖本地实现。

// ==================== 元素缓存 ====================
function cacheElements() {
  [
    'toolIp', 'defaultChannel', 'manualChannel', 'manualId', 'manualDlc', 'manualData',
    'dbcDropZone', 'dbcFileInput', 'dbcFileInfo', 'dbcMessageCount',
    'dbcSignalCount', 'dbcNodeCount', 'dbcFileSize', 'dbcFileCount', 'dbcFileChips',
    'dbcSearch', 'dbcHint', 'clearDbcBtn',
    'importDbcBtn',
    'messageList', 'sendSelectedBtn', 'generatedHex', 'generatedBytes', 'encodeHint',
    'dbcResp', 'dbcRespText', 'loadPreviewToRawBtn', 'manualHint', 'sendManualBtn',
    'resetManualBtn', 'manualResp', 'manualRespText', 'historyList', 'toast', 'toastIco',
    'toastMsg', 'canConnPill', 'canConnTxt', 'canIpPill', 'canRssiPill', 'canUptimePill',
    'canVersionPill', 'canChannelPill',
    'dbcSummaryPill', 'messageSummaryPill',
    'selectedCountPill', 'selectAllBox', 'expandAllBtn', 'collapseAllBtn', 'sendCheckedBtn',
    'nodeFilterWrap', 'nodeFilterBtn', 'nodeFilterLabel', 'nodeFilterPanel', 'nodeFilterSearch',
    'nodeFilterAllBtn', 'nodeFilterNoneBtn', 'nodeFilterList'
  ].forEach(function (id) {
    el[id] = document.getElementById(id);
  });
}

// ==================== 连接状态（仅顶栏 pills，与工作台去重） ====================
function setConnectionState(connected, text, buildVersion, buildTime, wifi, uptimeSec) {
  if (el.canConnPill) {
    el.canConnPill.className = 'pill ' + (connected ? 'online' : 'offline');
  }
  if (el.canConnTxt) el.canConnTxt.textContent = text;
  wifi = wifi || {};
  if (el.canIpPill) {
    var ip = wifi.ip || getDeviceIp();
    el.canIpPill.textContent = 'IP ' + (ip || '--');
  }
  if (el.canRssiPill) {
    var rssiOk = wifi.rssi !== undefined && wifi.rssi !== null && wifi.rssi !== -127;
    el.canRssiPill.textContent = rssiOk ? ('RSSI ' + wifi.rssi + ' dBm') : 'RSSI --';
  }
  if (el.canUptimePill) {
    el.canUptimePill.textContent = '运行 ' + (uptimeSec !== undefined && uptimeSec !== null
      ? formatUptime(uptimeSec) : '--');
  }
  if (el.canVersionPill) {
    el.canVersionPill.textContent = buildVersion ? ('v' + buildVersion) : 'v--';
    el.canVersionPill.dataset.tooltip = buildTime ? ('编译: ' + buildTime) : '';
  }
}

async function testConnection() {
  var baseUrl = getBaseUrl();
  try {
    /* 状态端点禁用 HTTP 缓存，避免命中旧响应 */
    var resp = await fetch(baseUrl + '/system/status', {
      signal: AbortSignal.timeout(3000),
      cache: 'no-store'
    });
    if (!resp.ok) {
      setConnectionState(false, 'HTTP ' + resp.status, '', '', {}, null);
      return;
    }
    var data = await resp.json();
    state.latestStatus = data;
    var wifi = data.wifi || {};
    if (wifi.ip) setDeviceIp(wifi.ip);
    var connected = !!wifi.connected;
    var mode = wifi.mode || 'STA';
    var text = connected ? (mode + ' 已连接') : (mode + ' 未连接');
    setConnectionState(connected, text, data.version, data.build_time, wifi, data.uptime_sec);
  } catch (err) {
    setConnectionState(false, '连接失败', '', '', {}, null);
  }
}

// ==================== DBC 文件流程（支持多文件叠加） ====================
function resetSelectionState() {
  state.selectedMessageKey = '';
  state.signalValues = {};
  state.checkedKeys = {};
  state.expandedKeys = {};
  state.transmitterFilter = Object.create(null);
  state.transmitterFilterEmpty = true;
  state.generatedData = new Uint8Array(0);
}

function afterDbcSourcesChanged(opts) {
  opts = opts || {};
  rebuildMergedDbc();
  if (opts.resetSelection) resetSelectionState();
  updateDbcFileInfo();
  buildSummary();
  filterMessages();
  if (state.dbc && state.dbc.messages.length) {
    if (opts.resetSelection || !state.selectedMessageKey) {
      selectMessage(state.dbc.messages[0].key);
    } else {
      updateEncodedPreview();
    }
  } else {
    updateEncodedPreview();
  }
  if (el.dbcHint) {
    el.dbcHint.hidden = true;
    el.dbcHint.textContent = '';
  }
  if (!opts.skipPersist) scheduleCanPersist();
}

/**
 * 添加一个或多个 DBC。同名文件覆盖旧源；不同名叠加。
 * @param {FileList|File[]} fileList
 */
async function handleDbcFiles(fileList) {
  var files = Array.prototype.slice.call(fileList || []).filter(function (f) {
    return f && f.name;
  });
  if (!files.length) return;

  var added = 0;
  var replaced = 0;
  var errors = [];

  for (var i = 0; i < files.length; i++) {
    var file = files[i];
    try {
      var text = await readDbcText(file);
      var dbc = parseDbc(text);
      if (!dbc.messages.length) {
        errors.push(file.name + '（无报文）');
        continue;
      }
      var existingIdx = -1;
      for (var j = 0; j < state.dbcSources.length; j++) {
        if (state.dbcSources[j].name === file.name) {
          existingIdx = j;
          break;
        }
      }
      var entry = {
        id: existingIdx >= 0 ? state.dbcSources[existingIdx].id : makeDbcSourceId(file.name),
        name: file.name,
        size: file.size || 0,
        text: text, /* 原文，供刷新后恢复 */
        dbc: dbc
      };
      if (existingIdx >= 0) {
        state.dbcSources[existingIdx] = entry;
        replaced++;
      } else {
        state.dbcSources.push(entry);
        added++;
      }
    } catch (err) {
      errors.push(file.name + '（' + (err && err.message ? err.message : err) + '）');
    }
  }

  /* 仅在确有新增/覆盖时才 reset 并刷新 UI；
   * 全部解析失败时保留用户当前选中/勾选/信号值，只弹错误提示。 */
  if (added || replaced) {
    afterDbcSourcesChanged({ resetSelection: true });
  }

  if (added || replaced) {
    var msg = 'DBC：';
    if (added) msg += '新增 ' + added;
    if (added && replaced) msg += '，';
    if (replaced) msg += '覆盖 ' + replaced;
    msg += '，合计 ' + state.dbcSources.length + ' 个文件';
    showToast(msg, 'success');
  }
  if (errors.length) {
    showToast('部分失败: ' + errors.slice(0, 2).join('; '), 'warning');
    if (el.dbcHint) {
      el.dbcHint.hidden = false;
      el.dbcHint.className = 'helper-note err';
      el.dbcHint.innerHTML = '导入问题：' + sanitizeHtml(errors.join('；'));
    }
  }
}

function removeDbcSource(sourceId) {
  state.dbcSources = state.dbcSources.filter(function (s) { return s.id !== sourceId; });
  afterDbcSourcesChanged({ resetSelection: true });
  if (el.generatedHex) el.generatedHex.textContent = '--';
  if (el.generatedBytes) el.generatedBytes.innerHTML = '';
  showToast(state.dbcSources.length ? '已移除 1 个 DBC' : '已清除全部 DBC', 'success');
}

function clearDbc() {
  state.dbcSources = [];
  state.dbc = null;
  state.filteredMessages = [];
  resetSelectionState();
  if (el.dbcFileInput) el.dbcFileInput.value = '';
  if (el.dbcSearch) el.dbcSearch.value = '';
  afterDbcSourcesChanged({ resetSelection: true });
  /* 历史/原始帧表单仍保留并写回 localStorage */
  if (el.generatedHex) el.generatedHex.textContent = '--';
  if (el.generatedBytes) el.generatedBytes.innerHTML = '';
}

function handleDbcError(err) {
  if (el.dbcHint) {
    el.dbcHint.hidden = false;
    el.dbcHint.className = 'helper-note err';
    el.dbcHint.innerHTML = 'DBC 解析失败：' + sanitizeHtml(err.message);
  }
  showToast('DBC 解析失败: ' + err.message, 'error');
}

// ==================== 事件绑定 ====================
function bindEvents() {
  if (el.defaultChannel) {
    el.defaultChannel.addEventListener('change', function () {
      if (el.manualChannel) el.manualChannel.value = el.defaultChannel.value;
      if (el.canChannelPill) el.canChannelPill.textContent = 'CH' + el.defaultChannel.value;
      renderMessageList();
      updateEncodedPreview();
      scheduleCanPersist();
    });
  }
  function openDbcPicker() {
    if (el.dbcFileInput) el.dbcFileInput.click();
  }
  if (el.importDbcBtn) el.importDbcBtn.addEventListener('click', openDbcPicker);
  if (el.dbcDropZone) {
    el.dbcDropZone.addEventListener('dragover', function (event) {
      event.preventDefault();
      el.dbcDropZone.classList.add('dragover');
    });
    el.dbcDropZone.addEventListener('dragleave', function (event) {
      if (!el.dbcDropZone.contains(event.relatedTarget)) {
        el.dbcDropZone.classList.remove('dragover');
      }
    });
    el.dbcDropZone.addEventListener('drop', function (event) {
      event.preventDefault();
      el.dbcDropZone.classList.remove('dragover');
      var files = event.dataTransfer && event.dataTransfer.files ? event.dataTransfer.files : null;
      if (files && files.length) handleDbcFiles(files).catch(handleDbcError);
    });
    el.dbcDropZone.addEventListener('click', function (event) {
      var removeBtn = event.target.closest('[data-remove-id]');
      if (removeBtn) {
        event.preventDefault();
        event.stopPropagation();
        removeDbcSource(removeBtn.getAttribute('data-remove-id'));
        return;
      }
      if (state.dbcSources.length) return;
      if (event.target.closest('button, a, input, select, label')) return;
      openDbcPicker();
    });
  }
  if (el.dbcFileInput) {
    el.dbcFileInput.addEventListener('change', function (event) {
      var files = event.target && event.target.files ? event.target.files : null;
      if (files && files.length) handleDbcFiles(files).catch(handleDbcError);
      event.target.value = '';
    });
  }
  if (el.clearDbcBtn) el.clearDbcBtn.addEventListener('click', clearDbc);
  if (el.dbcSearch) {
    el.dbcSearch.addEventListener('input', function () {
      filterMessages();
      scheduleCanPersist();
    });
  }
  if (el.sendSelectedBtn) el.sendSelectedBtn.addEventListener('click', sendSelectedMessage);
  if (el.loadPreviewToRawBtn) el.loadPreviewToRawBtn.addEventListener('click', loadPreviewToManual);
  if (el.sendManualBtn) el.sendManualBtn.addEventListener('click', sendManualFrame);
  if (el.resetManualBtn) el.resetManualBtn.addEventListener('click', function () {
    resetManualFields();
    scheduleCanPersist();
  });
  function onManualFieldChange() {
    refreshManualHint();
    scheduleCanPersist();
  }
  /* 手动帧三字段统一守卫，避免任一元素缺失时后续绑定被中断 */
  ['manualData', 'manualDlc', 'manualId'].forEach(function (id) {
    if (el[id]) el[id].addEventListener('input', onManualFieldChange);
  });
  if (el.manualChannel) el.manualChannel.addEventListener('change', scheduleCanPersist);
  if (el.sendCheckedBtn) el.sendCheckedBtn.addEventListener('click', sendCheckedMessages);
  if (el.selectAllBox) el.selectAllBox.addEventListener('change', function () {
    var visible = state.filteredMessages || [];
    if (this.checked) {
      visible.forEach(function (m) { state.checkedKeys[m.key] = true; });
    } else {
      visible.forEach(function (m) { delete state.checkedKeys[m.key]; });
    }
    renderMessageList();
    scheduleCanPersist();
  });
  if (el.expandAllBtn) el.expandAllBtn.addEventListener('click', function () {
    var visible = state.filteredMessages || [];
    visible.forEach(function (m) { state.expandedKeys[m.key] = true; ensureMessageDefaults(m); });
    renderMessageList();
    scheduleCanPersist();
  });
  if (el.collapseAllBtn) el.collapseAllBtn.addEventListener('click', function () {
    var visible = state.filteredMessages || [];
    visible.forEach(function (m) { delete state.expandedKeys[m.key]; });
    renderMessageList();
    scheduleCanPersist();
  });
  bindNodeFilterEvents();
}

// ==================== 初始化 ====================
function init() {
  cacheElements();
  bindEvents();
  bindCanPersistLifecycle();

  var restoredInfo = loadCanSession();
  setConnectionState(false, '未连接', '', '', { ip: getDeviceIp() }, null);
  if (el.canChannelPill && el.defaultChannel) {
    el.canChannelPill.textContent = 'CH' + el.defaultChannel.value;
  }
  if (el.manualChannel && el.defaultChannel && !restoredInfo.restored) {
    el.manualChannel.value = el.defaultChannel.value;
  }

  recomputeTransmitterFilterEmpty();
  updateNodeFilterLabel();
  updateDbcFileInfo();
  buildSummary();
  filterMessages();
  if (state.dbc && state.dbc.messages.length) {
    if (state.selectedMessageKey) {
      selectMessage(state.selectedMessageKey);
    } else {
      selectMessage(state.dbc.messages[0].key);
    }
  } else {
    updateEncodedPreview();
  }
  updateBatchUi();
  refreshManualHint();
  renderHistory();

  if (restoredInfo.restored && (restoredInfo.dbcCount || restoredInfo.historyCount)) {
    var tip = '已恢复会话';
    if (restoredInfo.dbcCount) tip += '：' + restoredInfo.dbcCount + ' 个 DBC';
    if (restoredInfo.historyCount) {
      tip += (restoredInfo.dbcCount ? '，' : '：') + restoredInfo.historyCount + ' 条发送记录';
    }
    showToast(tip, 'success');
  }

  setTimeout(testConnection, 150);
  /* 周期性刷新顶栏 STA/IP/RSSI/运行时间（与监控 1Hz 解耦，避免重复 pill） */
  setInterval(testConnection, 5000);
  /* 右侧抽屉：CAN 接收监控 */
  initCanMonitor();
}

init();