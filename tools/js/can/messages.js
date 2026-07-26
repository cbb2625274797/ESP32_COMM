/**
 * messages.js - 报文列表管理（渲染、过滤、节点过滤、批量 UI）
 *
 * 包含：DBC 文件信息、报文过滤、节点过滤面板、报文列表渲染、报文选择/展开、批量 UI。
 *
 * 依赖：dbc.js（state/el/padHex/normalizeWhitespace/cssEscape）、
 *       common/utils.js（escapeHtml/formatBytes）、
 *       signals.js（renderInlineSignalGrid/ensureMessageDefaults/updateEncodedPreview）。
 */
import { state, el, padHex, normalizeWhitespace, cssEscape, findMessageByKey } from './dbc.js';
import { escapeHtml, formatBytes } from '../common/utils.js';
import { renderInlineSignalGrid, ensureMessageDefaults, updateEncodedPreview } from './signals.js';
import { scheduleCanPersist } from './persist.js';

// ==================== DBC 文件信息 ====================
/** @description 更新 DBC 文件信息卡片（文件名/报文数/信号数/节点数/大小），并刷新多 DBC 摘要条与文件 chip 列表。 */
export function updateDbcFileInfo() {
  var sources = state.dbcSources || [];
  var has = sources.length > 0;
  var dbc = state.dbc;

  if (el.dbcFileInfo) {
    el.dbcFileInfo.hidden = !has;
    el.dbcFileInfo.classList.toggle('show', has);
  }
  /* 有文件时仍显示「添加」，文案改成添加；「全部清除」紧挨添加按钮 */
  if (el.importDbcBtn) {
    el.importDbcBtn.hidden = false;
    el.importDbcBtn.textContent = has ? '添加 DBC' : '导入 DBC';
  }
  if (el.clearDbcBtn) el.clearDbcBtn.hidden = !has;
  if (el.dbcDropZone) el.dbcDropZone.classList.toggle('has-dbc', has);

  if (!has || !dbc) {
    if (el.dbcFileCount) el.dbcFileCount.textContent = '0';
    if (el.dbcMessageCount) el.dbcMessageCount.textContent = '0';
    if (el.dbcSignalCount) el.dbcSignalCount.textContent = '0';
    if (el.dbcNodeCount) el.dbcNodeCount.textContent = '0';
    if (el.dbcFileSize) el.dbcFileSize.textContent = '0 B';
    if (el.dbcFileChips) el.dbcFileChips.innerHTML = '';
    return;
  }

  var signalCount = dbc.messages.reduce(function (sum, msg) { return sum + msg.signals.length; }, 0);
  var totalSize = sources.reduce(function (sum, s) { return sum + (s.size || 0); }, 0);
  if (el.dbcFileCount) el.dbcFileCount.textContent = String(sources.length);
  if (el.dbcMessageCount) el.dbcMessageCount.textContent = String(dbc.messages.length);
  if (el.dbcSignalCount) el.dbcSignalCount.textContent = String(signalCount);
  if (el.dbcNodeCount) el.dbcNodeCount.textContent = String(dbc.nodeNames.length);
  if (el.dbcFileSize) el.dbcFileSize.textContent = formatBytes(totalSize);

  if (el.dbcFileChips) {
    el.dbcFileChips.innerHTML = sources.map(function (s) {
      var nMsg = (s.dbc && s.dbc.messages) ? s.dbc.messages.length : 0;
      return '' +
        '<span class="dbc-file-chip" data-source-id="' + escapeHtml(s.id) + '" title="' + escapeHtml(s.name) + ' · ' + nMsg + ' 报文">' +
          '<span class="dbc-file-chip-name">' + escapeHtml(s.name) + '</span>' +
          '<button type="button" class="dbc-file-chip-remove" data-remove-id="' + escapeHtml(s.id) + '" title="移除此文件">×</button>' +
        '</span>';
    }).join('');
  }
}

// ==================== 报文过滤 ====================
/** @description 根据 DBC 搜索关键字 + 发送节点过滤报文，并重新渲染列表。 */
export function filterMessages() {
  var keyword = normalizeWhitespace(el.dbcSearch ? el.dbcSearch.value : '').toLowerCase();
  var messages = state.dbc ? state.dbc.messages.slice() : [];
  if (!state.transmitterFilterEmpty) {
    messages = messages.filter(function (msg) {
      var tx = msg.transmitter || 'Vector__XXX';
      return !!state.transmitterFilter[tx];
    });
  }
  if (keyword) {
    messages = messages.filter(function (msg) {
      var idHex = '0x' + padHex(msg.id, msg.isExtended ? 8 : 3);
      if (msg.name.toLowerCase().indexOf(keyword) >= 0) return true;
      if (idHex.toLowerCase().indexOf(keyword) >= 0) return true;
      if (String(msg.idRaw).indexOf(keyword) >= 0) return true;
      if ((msg.transmitter || '').toLowerCase().indexOf(keyword) >= 0) return true;
      for (var i = 0; i < msg.signals.length; i++) {
        if (msg.signals[i].name.toLowerCase().indexOf(keyword) >= 0) return true;
      }
      return false;
    });
  }
  state.filteredMessages = messages;
  renderMessageList();
  updateNodeFilterLabel();
}

// ==================== 发送节点过滤 ====================
/** @description 收集 DBC 中所有发送节点（去重 + 排序）。 */
function collectTransmitters() {
  var set = Object.create(null);
  var list = [];
  if (!state.dbc) return list;
  state.dbc.messages.forEach(function (msg) {
    var tx = msg.transmitter || 'Vector__XXX';
    if (!set[tx]) { set[tx] = 1; list.push(tx); }
  });
  list.sort(function (a, b) { return a.localeCompare(b); });
  return list;
}

/** @description 渲染发送节点过滤面板的复选框列表。 */
function renderNodeFilterList() {
  if (!el.nodeFilterList) return;
  var txs = collectTransmitters();
  var kw = (el.nodeFilterSearch ? (el.nodeFilterSearch.value || '') : '').trim().toLowerCase();
  if (kw) txs = txs.filter(function (n) { return n.toLowerCase().indexOf(kw) >= 0; });
  if (!txs.length) {
    el.nodeFilterList.innerHTML = '<div class="status-empty">无匹配节点</div>';
    return;
  }
  var counts = Object.create(null);
  if (state.dbc) {
    state.dbc.messages.forEach(function (msg) {
      var tx = msg.transmitter || 'Vector__XXX';
      counts[tx] = (counts[tx] || 0) + 1;
    });
  }
  el.nodeFilterList.innerHTML = txs.map(function (name) {
    var checked = !!state.transmitterFilter[name] ? ' checked' : '';
    return '<label class="can-node-filter-item">' +
      '<input type="checkbox" data-node="' + escapeHtml(name) + '"' + checked + '>' +
      '<span class="can-node-filter-name">' + escapeHtml(name) + '</span>' +
      '<span class="can-node-filter-count">' + (counts[name] || 0) + '</span>' +
      '</label>';
  }).join('');
  var inputs = el.nodeFilterList.querySelectorAll('input[type="checkbox"]');
  for (var i = 0; i < inputs.length; i++) {
    inputs[i].addEventListener('change', function () {
      var name = this.getAttribute('data-node');
      if (this.checked) state.transmitterFilter[name] = true;
      else delete state.transmitterFilter[name];
      recomputeTransmitterFilterEmpty();
      filterMessages();
      scheduleCanPersist();
    });
  }
}

/** @description 重新计算 transmitterFilterEmpty 标志（无任何勾选时为 true）。 */
export function recomputeTransmitterFilterEmpty() {
  var any = false;
  for (var k in state.transmitterFilter) {
    if (state.transmitterFilter[k]) { any = true; break; }
  }
  state.transmitterFilterEmpty = !any;
}

/** @description 更新发送节点过滤按钮的标签文本。 */
export function updateNodeFilterLabel() {
  if (!el.nodeFilterLabel) return;
  if (state.transmitterFilterEmpty) {
    el.nodeFilterLabel.textContent = '发送节点：全部';
    if (el.nodeFilterBtn) el.nodeFilterBtn.classList.remove('active');
  } else {
    var picked = Object.keys(state.transmitterFilter).filter(function (k) { return state.transmitterFilter[k]; });
    var preview = picked.length === 1 ? picked[0] : ('已选 ' + picked.length + ' 个');
    el.nodeFilterLabel.textContent = '发送节点：' + preview;
    if (el.nodeFilterBtn) el.nodeFilterBtn.classList.add('active');
  }
}

/** @description 打开/关闭发送节点过滤面板。 */
function setNodeFilterPanelOpen(open) {
  if (!el.nodeFilterPanel || !el.nodeFilterBtn) return;
  if (open) {
    renderNodeFilterList();
    el.nodeFilterPanel.hidden = false;
    el.nodeFilterBtn.setAttribute('aria-expanded', 'true');
    if (el.nodeFilterWrap) el.nodeFilterWrap.classList.add('is-open');
  } else {
    el.nodeFilterPanel.hidden = true;
    el.nodeFilterBtn.setAttribute('aria-expanded', 'false');
    if (el.nodeFilterWrap) el.nodeFilterWrap.classList.remove('is-open');
  }
}

/** @description 绑定发送节点过滤面板的事件（按钮/搜索/全选/全不选/外部点击/Esc）。 */
export function bindNodeFilterEvents() {
  if (!el.nodeFilterBtn) return;
  el.nodeFilterBtn.addEventListener('click', function (event) {
    event.stopPropagation();
    setNodeFilterPanelOpen(el.nodeFilterPanel.hidden);
  });
  document.addEventListener('click', function (event) {
    if (!el.nodeFilterWrap || el.nodeFilterPanel.hidden) return;
    if (!el.nodeFilterWrap.contains(event.target)) setNodeFilterPanelOpen(false);
  });
  document.addEventListener('keydown', function (event) {
    if (event.key === 'Escape' && el.nodeFilterPanel && !el.nodeFilterPanel.hidden) {
      setNodeFilterPanelOpen(false);
    }
  });
  if (el.nodeFilterSearch) {
    el.nodeFilterSearch.addEventListener('input', renderNodeFilterList);
  }
  if (el.nodeFilterAllBtn) el.nodeFilterAllBtn.addEventListener('click', function () {
    collectTransmitters().forEach(function (n) { state.transmitterFilter[n] = true; });
    recomputeTransmitterFilterEmpty();
    renderNodeFilterList();
    filterMessages();
    scheduleCanPersist();
  });
  if (el.nodeFilterNoneBtn) el.nodeFilterNoneBtn.addEventListener('click', function () {
    state.transmitterFilter = Object.create(null);
    state.transmitterFilterEmpty = true;
    renderNodeFilterList();
    filterMessages();
    scheduleCanPersist();
  });
}

// ==================== 报文列表渲染 ====================
/** @description 渲染报文列表表格（表头 + 行 + 展开区）。 */
export function renderMessageList() {
  if (!el.messageList) return;
  if (!state.dbc || !state.dbcSources.length) {
    el.messageList.innerHTML =
      '<div class="dbc-empty-drop" id="dbcEmptyDrop">' +
        '<div class="upload-ico">📥</div>' +
        '<h4>导入 DBC 开始编辑报文</h4>' +
        '<p>支持一次选择或拖入 <b>多个</b> <code>.dbc</code> 文件</p>' +
      '</div>';
    updateBatchUi();
    return;
  }
  if (!state.filteredMessages.length) {
    el.messageList.innerHTML = '<div class="status-empty">没有匹配的报文。</div>';
    updateBatchUi();
    return;
  }
  var channelTxt = el.defaultChannel ? el.defaultChannel.value : '0';
  var rows = state.filteredMessages.map(function (msg, index) {
    var active = msg.key === state.selectedMessageKey ? ' active' : '';
    var checked = state.checkedKeys[msg.key] ? ' checked' : '';
    var expanded = !!state.expandedKeys[msg.key];
    var caret = expanded ? '\u25BE' : '\u25B8';
    var idHex = '0x' + padHex(msg.id, msg.isExtended ? 8 : 3);
    var comment = msg.comment || '';
    if (comment.length > 36) comment = comment.slice(0, 36) + '...';
    var rowHtml = '' +
      '<div class="message-row' + active + '" data-key="' + escapeHtml(msg.key) + '" role="button" tabindex="0">' +
      '<span class="message-col message-col-check"><input type="checkbox" class="message-check" data-key="' + escapeHtml(msg.key) + '"' + checked + '></span>' +
      '<span class="message-col message-col-toggle"><button type="button" class="message-toggle' + (expanded ? ' expanded' : '') + '" data-key="' + escapeHtml(msg.key) + '" aria-label="展开/收起信号">' + caret + '</button></span>' +
      '<span class="message-col message-col-index">' + (index + 1) + '</span>' +
      '<span class="message-col message-col-dbc" title="' + escapeHtml(msg.sourceName || '-') + '">' +
        escapeHtml(msg.sourceName ? String(msg.sourceName).replace(/\.dbc$/i, '') : '-') + '</span>' +
      '<span class="message-col message-col-name" title="' + escapeHtml(msg.name) + '">' +
        escapeHtml(msg.name) + '</span>' +
      '<span class="message-col message-col-id">' + escapeHtml(idHex) + '</span>' +
      '<span class="message-col message-col-channel">CH' + escapeHtml(channelTxt) + '</span>' +
      '<span class="message-col message-col-type">' + escapeHtml(msg.isExtended ? '扩展' : '标准') + '</span>' +
      '<span class="message-col message-col-dlc">' + escapeHtml(msg.dlc) + '</span>' +
      '<span class="message-col message-col-signal">' + escapeHtml(msg.signals.length) + '</span>' +
      '<span class="message-col message-col-node" title="' + escapeHtml(msg.transmitter || 'Vector__XXX') + '">' + escapeHtml(msg.transmitter || 'Vector__XXX') + '</span>' +
      '<span class="message-col message-col-comment" title="' + escapeHtml(msg.comment || '') + '">' + escapeHtml(comment || '-') + '</span>' +
      '</div>';
    var expandHtml = '<div class="message-row-expand" data-key="' + escapeHtml(msg.key) + '"' + (expanded ? '' : ' hidden') + '>' +
      '<div class="signal-table-host"></div>' +
      '</div>';
    return rowHtml + expandHtml;
  }).join('');
  el.messageList.innerHTML =
    '<div class="message-table">' +
    '<div class="message-table-head">' +
    '<span class="message-col message-col-check"></span>' +
    '<span class="message-col message-col-toggle"></span>' +
    '<span class="message-col message-col-index">#</span>' +
    '<span class="message-col message-col-dbc">DBC名称</span>' +
    '<span class="message-col message-col-name">报文名称</span>' +
    '<span class="message-col message-col-id">标识符</span>' +
    '<span class="message-col message-col-channel">通道</span>' +
    '<span class="message-col message-col-type">类型</span>' +
    '<span class="message-col message-col-dlc">DLC</span>' +
    '<span class="message-col message-col-signal">信号</span>' +
    '<span class="message-col message-col-node">发送节点</span>' +
    '<span class="message-col message-col-comment">注释</span>' +
    '</div>' +
    '<div class="message-table-body">' + rows + '</div>' +
    '</div>';

  var rowEls = el.messageList.querySelectorAll('.message-row');
  for (var i = 0; i < rowEls.length; i++) {
    rowEls[i].addEventListener('click', handleMessageRowClick);
    rowEls[i].addEventListener('keydown', function (event) {
      if (event.key === 'Enter' || event.key === ' ') {
        event.preventDefault();
        selectMessage(this.getAttribute('data-key'));
      }
    });
  }
  var checks = el.messageList.querySelectorAll('.message-check');
  for (var j = 0; j < checks.length; j++) {
    checks[j].addEventListener('click', function (event) { event.stopPropagation(); });
    checks[j].addEventListener('change', function () {
      var k = this.getAttribute('data-key');
      if (this.checked) state.checkedKeys[k] = true;
      else delete state.checkedKeys[k];
      updateBatchUi();
      scheduleCanPersist();
    });
  }
  var toggles = el.messageList.querySelectorAll('.message-toggle');
  for (var t = 0; t < toggles.length; t++) {
    toggles[t].addEventListener('click', function (event) {
      event.stopPropagation();
      toggleMessageExpand(this.getAttribute('data-key'));
    });
  }

  state.filteredMessages.forEach(function (msg) {
    if (state.expandedKeys[msg.key]) {
      var host = el.messageList.querySelector('.message-row-expand[data-key="' + cssEscape(msg.key) + '"] .signal-table-host');
      if (host) renderInlineSignalGrid(msg, host);
    }
  });
  updateBatchUi();
}

function handleMessageRowClick(event) {
  if (event.target.closest('.message-check') || event.target.closest('.message-toggle')) return;
  var key = this.getAttribute('data-key');
  selectMessage(key);
}

function toggleMessageExpand(key) {
  if (!state.dbc) return;
  var msg = findMessageByKey(key);
  if (!msg) return;
  var willExpand = !state.expandedKeys[key];
  if (willExpand) {
    state.expandedKeys[key] = true;
    ensureMessageDefaults(msg);
  } else {
    delete state.expandedKeys[key];
  }
  var panel = el.messageList.querySelector('.message-row-expand[data-key="' + cssEscape(key) + '"]');
  var toggle = el.messageList.querySelector('.message-toggle[data-key="' + cssEscape(key) + '"]');
  if (panel) {
    if (willExpand) {
      panel.hidden = false;
      var host = panel.querySelector('.signal-table-host');
      if (host) renderInlineSignalGrid(msg, host);
    } else {
      panel.hidden = true;
    }
  }
  if (toggle) {
    toggle.classList.toggle('expanded', willExpand);
    toggle.textContent = willExpand ? '\u25BE' : '\u25B8';
  }
  scheduleCanPersist();
}

// ==================== 批量 UI ====================
/** @description 更新批量发送按钮 / 全选框 / 已选计数 pill。 */
export function updateBatchUi() {
  var keys = Object.keys(state.checkedKeys);
  var count = keys.length;
  if (el.selectedCountPill) el.selectedCountPill.textContent = '已勾选 ' + count;
  if (el.sendCheckedBtn) {
    el.sendCheckedBtn.disabled = count === 0 || state.batchInFlight;
    el.sendCheckedBtn.textContent = state.batchInFlight ? '发送中...' : ('批量发送勾选 (' + count + ')');
  }
  if (el.selectAllBox) {
    var visible = state.filteredMessages || [];
    var allChecked = visible.length > 0 && visible.every(function (m) { return !!state.checkedKeys[m.key]; });
    var someChecked = visible.some(function (m) { return !!state.checkedKeys[m.key]; });
    el.selectAllBox.checked = allChecked;
    el.selectAllBox.indeterminate = !allChecked && someChecked;
  }
}

// ==================== 报文选择 ====================
/** @description 选中指定 key 的报文，并刷新列表/信号编辑/编码预览。 */
export function selectMessage(key) {
  if (!state.dbc) return;
  var msg = findMessageByKey(key);
  if (!msg) return;
  state.selectedMessageKey = key;
  ensureMessageDefaults(msg);
  renderMessageList();
  updateEncodedPreview();
  scheduleCanPersist();
}
