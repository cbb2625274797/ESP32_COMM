/**
 * persist.js - CAN 控制台会话持久化（刷新后恢复）
 *
 * 保存到 localStorage：多 DBC 原文、信号编辑值、勾选/展开、发送节点过滤、
 * 最近发送历史、通道/搜索/原始帧表单。
 *
 * 依赖：dbc.js（state/el/parseDbc/rebuildMergedDbc）
 */
import { state, el, parseDbc, rebuildMergedDbc } from './dbc.js?v=20260717v';

var LS_KEY = 'dcu_can_session_v1';
var MAX_HISTORY = 200;
var SAVE_DEBOUNCE_MS = 350;
var saveTimer = null;
var restoring = false;

function safeJsonParse(raw) {
  try {
    return JSON.parse(raw);
  } catch (_) {
    return null;
  }
}

function transmitterFilterToPlain() {
  var out = {};
  for (var k in state.transmitterFilter) {
    if (Object.prototype.hasOwnProperty.call(state.transmitterFilter, k) || state.transmitterFilter[k]) {
      out[k] = !!state.transmitterFilter[k];
    }
  }
  return out;
}

function collectUiFields() {
  return {
    defaultChannel: el.defaultChannel ? el.defaultChannel.value : '0',
    manualChannel: el.manualChannel ? el.manualChannel.value : '0',
    manualId: el.manualId ? el.manualId.value : '',
    manualDlc: el.manualDlc ? el.manualDlc.value : '8',
    manualData: el.manualData ? el.manualData.value : '',
    dbcSearch: el.dbcSearch ? el.dbcSearch.value : ''
  };
}

function applyUiFields(ui) {
  if (!ui) return;
  if (el.defaultChannel && ui.defaultChannel != null) {
    el.defaultChannel.value = String(ui.defaultChannel);
  }
  if (el.manualChannel && ui.manualChannel != null) {
    el.manualChannel.value = String(ui.manualChannel);
  } else if (el.manualChannel && el.defaultChannel) {
    el.manualChannel.value = el.defaultChannel.value;
  }
  if (el.manualId && ui.manualId != null) el.manualId.value = String(ui.manualId);
  if (el.manualDlc && ui.manualDlc != null) el.manualDlc.value = String(ui.manualDlc);
  if (el.manualData && ui.manualData != null) el.manualData.value = String(ui.manualData);
  if (el.dbcSearch && ui.dbcSearch != null) el.dbcSearch.value = String(ui.dbcSearch);
  if (el.canChannelPill && el.defaultChannel) {
    el.canChannelPill.textContent = 'CH' + el.defaultChannel.value;
  }
}

function buildPayload(options) {
  options = options || {};
  var sources = (state.dbcSources || []).map(function (s) {
    return {
      id: s.id,
      name: s.name,
      size: s.size || 0,
      text: s.text || ''
    };
  }).filter(function (s) { return s.text; });

  var history = Array.isArray(state.history) ? state.history.slice(0, MAX_HISTORY) : [];
  if (options.historyLimit != null) {
    history = history.slice(0, options.historyLimit);
  }

  return {
    v: 1,
    ts: Date.now(),
    dbcSources: sources,
    selectedMessageKey: state.selectedMessageKey || '',
    signalValues: state.signalValues || {},
    checkedKeys: state.checkedKeys || {},
    expandedKeys: state.expandedKeys || {},
    transmitterFilter: transmitterFilterToPlain(),
    history: history,
    ui: collectUiFields()
  };
}

function writePayload(payload) {
  localStorage.setItem(LS_KEY, JSON.stringify(payload));
}

/** @description 立即写入 localStorage（空间不足时逐步丢弃历史/信号）。 */
export function saveCanSessionNow() {
  if (restoring) return;
  if (saveTimer) {
    clearTimeout(saveTimer);
    saveTimer = null;
  }
  try {
    writePayload(buildPayload());
    return;
  } catch (e1) {
    /* quota：先砍历史 */
  }
  try {
    writePayload(buildPayload({ historyLimit: 40 }));
    return;
  } catch (e2) {
    /* 再砍信号值，只保 DBC + 勾选 */
  }
  try {
    var slim = buildPayload({ historyLimit: 0 });
    slim.signalValues = {};
    slim.history = [];
    writePayload(slim);
  } catch (e3) {
    /* 放弃 */
  }
}

/** @description 防抖保存（编辑信号/勾选等高频操作）。 */
export function scheduleCanPersist() {
  if (restoring) return;
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(function () {
    saveTimer = null;
    saveCanSessionNow();
  }, SAVE_DEBOUNCE_MS);
}

/** @description 清空持久化（全部清除 DBC 时）。 */
export function clearCanSessionPersist() {
  if (saveTimer) {
    clearTimeout(saveTimer);
    saveTimer = null;
  }
  try {
    localStorage.removeItem(LS_KEY);
  } catch (_) {}
}

/**
 * @description 从 localStorage 恢复会话到 state + 表单。
 * @returns {{ restored: boolean, dbcCount: number, historyCount: number }}
 */
export function loadCanSession() {
  var empty = { restored: false, dbcCount: 0, historyCount: 0 };
  var raw = null;
  try {
    raw = localStorage.getItem(LS_KEY);
  } catch (_) {
    return empty;
  }
  if (!raw) return empty;
  var data = safeJsonParse(raw);
  if (!data || data.v !== 1) return empty;

  restoring = true;
  try {
    state.dbcSources = [];
    var list = Array.isArray(data.dbcSources) ? data.dbcSources : [];
    for (var i = 0; i < list.length; i++) {
      var src = list[i];
      if (!src || !src.text) continue;
      try {
        var dbc = parseDbc(src.text);
        if (!dbc.messages || !dbc.messages.length) continue;
        state.dbcSources.push({
          id: src.id || ('restored_' + i),
          name: src.name || ('saved_' + (i + 1) + '.dbc'),
          size: src.size || src.text.length,
          text: src.text,
          dbc: dbc
        });
      } catch (_) {
        /* 单文件损坏跳过 */
      }
    }
    rebuildMergedDbc();

    state.signalValues = (data.signalValues && typeof data.signalValues === 'object')
      ? data.signalValues : {};
    state.checkedKeys = (data.checkedKeys && typeof data.checkedKeys === 'object')
      ? data.checkedKeys : {};
    state.expandedKeys = (data.expandedKeys && typeof data.expandedKeys === 'object')
      ? data.expandedKeys : {};
    state.selectedMessageKey = data.selectedMessageKey || '';
    state.history = Array.isArray(data.history) ? data.history.slice(0, MAX_HISTORY) : [];

    state.transmitterFilter = Object.create(null);
    if (data.transmitterFilter && typeof data.transmitterFilter === 'object') {
      Object.keys(data.transmitterFilter).forEach(function (k) {
        state.transmitterFilter[k] = !!data.transmitterFilter[k];
      });
    }
    var anyTx = false;
    for (var tk in state.transmitterFilter) {
      if (state.transmitterFilter[tk]) { anyTx = true; break; }
    }
    state.transmitterFilterEmpty = !anyTx;

    /* 选中键若不存在则清空，交由调用方选第一条 */
    if (state.selectedMessageKey && state.dbc && state.dbc.messageMap) {
      if (!state.dbc.messageMap[state.selectedMessageKey]) {
        state.selectedMessageKey = '';
      }
    } else if (state.selectedMessageKey && (!state.dbc || !state.dbc.messages.length)) {
      state.selectedMessageKey = '';
    }

    applyUiFields(data.ui);

    return {
      restored: true,
      dbcCount: state.dbcSources.length,
      historyCount: state.history.length
    };
  } finally {
    restoring = false;
  }
}

/** @description 绑定 beforeunload / 可见性，降低丢失。 */
export function bindCanPersistLifecycle() {
  window.addEventListener('beforeunload', function () {
    saveCanSessionNow();
  });
  document.addEventListener('visibilitychange', function () {
    if (document.visibilityState === 'hidden') saveCanSessionNow();
  });
}
