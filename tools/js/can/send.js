/**
 * send.js - CAN 帧构建与发送（DBC 报文 / 原始帧 / 批量发送 / 发送历史）
 *
 * 包含：sendCanRequest、响应框、历史、批量/选中/手动发送、
 * 手动帧校验/重置/载入预览、CAN_SEND_ERR_DESC / GD32_ERR_DESC / buildSendResponseText。
 *
 * 依赖：dbc.js（state/el/getBaseUrl/padHex/bytesToHex/parseNumericId）、
 *       common/toast.js（showToast）、common/utils.js（escapeHtml）、
 *       messages.js（updateBatchUi）、signals.js（ensureMessageDefaults/getSignalValueMap/
 *       getSelectedMessage/updateEncodedPreview/encodeMessageData）。
 *
 * 注意：formatTime 保留本地实现（HH:MM:SS，不含毫秒），与 common/utils.js
 * 的 formatTime（含毫秒）不同，以保证发送历史时间戳输出格式一致。
 */
import { state, el, getBaseUrl, padHex, bytesToHex, parseNumericId } from './dbc.js?v=20260717x';
import { escapeHtml as sanitizeHtml } from '../common/utils.js?v=20260717x';
import { showToast } from '../common/toast.js?v=20260717x';
import { updateBatchUi } from './messages.js?v=20260717x';
import { ensureMessageDefaults, getSignalValueMap, getSelectedMessage, updateEncodedPreview, encodeMessageData } from './signals.js?v=20260717x';
import { scheduleCanPersist } from './persist.js?v=20260717x';

// ==================== 本地工具（保持与原 can.js 一致） ====================
function formatTime(date) {
  var d = date instanceof Date ? date : new Date();
  var hh = String(d.getHours()).padStart(2, '0');
  var mm = String(d.getMinutes()).padStart(2, '0');
  var ss = String(d.getSeconds()).padStart(2, '0');
  return hh + ':' + mm + ':' + ss;
}

// ==================== 发送错误码描述 ====================
export var CAN_SEND_ERR_DESC = {
  '-1': '参数错误（通道/ID/DLC 无效）',
  '-2': 'SPI 通信失败（ESP32 与 GD32 通信异常）',
  '-3': 'GD32 CAN 总线拒绝',
};

export var GD32_ERR_DESC = {
  0xE0: 'payload 过短',
  0xE1: '通道号非法',
  0xE2: 'BUS_OFF（总线离线，检查接线/终端电阻/对端上电）',
  0xE3: 'TEC 过高（错误计数 >96，对端 ACK 失败率高）',
  0xE4: '邮箱满（3 个硬件邮箱全 PENDING，对端不 ACK）',
  0xE5: '参数无效（通道未启用/DLC 越界）',
  0xEE: 'SPI 帧解析失败（GD32 收到的帧 CRC/SOF 校验失败，通常是 SPI 字节丢失或 CS 时序冲突）'
};

/** @description 构建发送响应的可读文本（成功显示 recv，失败拼接 CAN_SEND_ERR_DESC + GD32_ERR_DESC）。 */
export function buildSendResponseText(result) {
  if (result.status === 'ok') {
    return result.recv ? ('0x' + result.recv + ' (' + result.recv_len + ' 字节)') : '无响应数据';
  }
  var code = String(result.code);
  var desc = CAN_SEND_ERR_DESC[code] || '未知错误';
  /* 用 GD32 细分错误码补充具体原因 */
  if (result.gd32_err != null && result.gd32_err !== 0) {
    var gd32Desc = GD32_ERR_DESC[result.gd32_err];
    if (gd32Desc) {
      desc = desc + ' · ' + gd32Desc;
    } else {
      desc = desc + ' · gd32_err=0x' + result.gd32_err.toString(16).toUpperCase();
    }
  }
  return '发送失败: ' + desc + ' (code=' + result.code + ')';
}

// ==================== HTTP 请求 ====================
async function sendCanRequest(payload) {
  var resp = await fetch(getBaseUrl() + '/can/send', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
    signal: AbortSignal.timeout(5000)
  });
  var text = await resp.text();
  var json = {};
  try {
    json = text ? JSON.parse(text) : {};
  } catch (err) {
    throw new Error('响应不是合法 JSON: ' + text);
  }
  if (!resp.ok) {
    throw new Error('HTTP ' + resp.status + ': ' + (json.message || text || '请求失败'));
  }
  return json;
}

/**
 * @description 一次 HTTP 连续发送多帧（设备 /can/send_batch）。
 * @returns {Promise<object[]>} 与 frames 等长的 results
 */
async function sendCanBatchRequest(channel, frames) {
  var resp = await fetch(getBaseUrl() + '/can/send_batch', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ch: channel, frames: frames }),
    signal: AbortSignal.timeout(Math.max(15000, frames.length * 500))
  });
  var text = await resp.text();
  var json = {};
  try {
    json = text ? JSON.parse(text) : {};
  } catch (err) {
    throw new Error('批量响应不是合法 JSON: ' + text);
  }
  if (!resp.ok) {
    throw new Error('HTTP ' + resp.status + ': ' + (json.message || text || '批量请求失败'));
  }
  if (!json.results || !Array.isArray(json.results)) {
    throw new Error('批量响应缺少 results');
  }
  /* 设备可能返回 skipped；补齐与 frames 等长的 results */
  var results = json.results.slice();
  var need = frames.length - results.length;
  if (need > 0) {
    var pad = {
      status: 'error',
      code: (json.abort_code != null) ? json.abort_code : -2,
      gd32_err: (json.abort_gd32_err != null) ? json.abort_gd32_err : 0
    };
    for (var p = 0; p < need; p++) results.push(pad);
  }
  return results;
}

// ==================== 响应框 ====================
function updateResponseBox(box, textEl, result, mode) {
  if (!box || !textEl) return;
  box.style.display = 'block';
  box.className = 'can-response' + (result.status === 'ok' ? ' ok' : ' err');
  textEl.textContent = buildSendResponseText(result);
  if (mode === 'manual') {
    el.manualHint.className = 'helper-note ' + (result.status === 'ok' ? 'ok' : 'err');
    el.manualHint.innerHTML = result.status === 'ok'
      ? '原始帧已发送到设备，若有响应则显示在下方。'
      : '设备返回错误码 <b>' + sanitizeHtml(result.code) + '</b>。';
  }
}

// ==================== 发送历史 ====================
/**
 * @param {object} entry
 * @param {{ silent?: boolean }} [opts] silent=true 时不立刻重绘/落盘（批量用）
 */
function addHistoryEntry(entry, opts) {
  state.history.unshift(entry);
  /* 批量发送会快速产生大量条目，上限放宽便于回看 */
  if (state.history.length > 200) state.history.length = 200;
  if (opts && opts.silent) return;
  renderHistory();
  scheduleCanPersist();
}

/** @description 渲染发送历史列表（单行紧凑：状态|时间|CH·ID·DLC|HEX|结果）。 */
export function renderHistory() {
  if (!el.historyList) return;
  if (!state.history.length) {
    el.historyList.innerHTML = '<div class="status-empty">尚未发送任何 CAN 帧。</div>';
    return;
  }
  el.historyList.innerHTML = state.history.map(function (item) {
    var meta = 'CH' + item.channel + ' · ' + item.idHex + ' · DLC ' + item.dlc;
    return '' +
      '<div class="history-item history-item-row' + (item.ok ? '' : ' is-err') + '">' +
      '<span class="history-item-tag ' + (item.ok ? 'ok' : 'err') + '">' + sanitizeHtml(item.mode) + '</span>' +
      '<span class="history-item-time">' + sanitizeHtml(item.time) + '</span>' +
      '<span class="history-item-main" title="' + sanitizeHtml(meta) + '">' + sanitizeHtml(meta) + '</span>' +
      '<span class="history-item-hex" title="' + sanitizeHtml(item.dataHex || '') + '">' + sanitizeHtml(item.dataHex || '--') + '</span>' +
      '<span class="history-item-foot" title="' + sanitizeHtml(item.resultText || '') + '">' + sanitizeHtml(item.resultText) + '</span>' +
      '</div>';
  }).join('');
}

// ==================== 批量发送 ====================
/** 单批最多帧数（与设备 /can/send_batch 上限一致） */
var BATCH_CHUNK = 128;

/**
 * @description 批量发送所有勾选的报文。
 * 优先 POST /can/send_batch；不支持时回退逐帧 /can/send。
 * 批量过程中不重绘历史，结束后统一刷新。
 */
export async function sendCheckedMessages() {
  if (!state.dbc) { showToast('请先导入 DBC', 'warning'); return; }
  var keys = Object.keys(state.checkedKeys);
  if (!keys.length) { showToast('请先勾选要发送的报文', 'warning'); return; }
  if (state.batchInFlight) return;
  state.batchInFlight = true;
  updateBatchUi();
  var channel = parseInt(el.defaultChannel.value, 10) || 0;
  var ok = 0, fail = 0;
  var silent = { silent: true };

  /* 先全部编码，避免发到一半才因编码失败打断节奏 */
  var prepared = [];
  for (var i = 0; i < keys.length; i++) {
    var key = keys[i];
    var msg = null;
    for (var m = 0; m < state.dbc.messages.length; m++) {
      if (state.dbc.messages[m].key === key) { msg = state.dbc.messages[m]; break; }
    }
    if (!msg) continue;
    ensureMessageDefaults(msg);
    var idHex = '0x' + padHex(msg.id, msg.isExtended ? 8 : 3);
    var dataBytes;
    try {
      dataBytes = encodeMessageData(msg, getSignalValueMap(msg));
    } catch (err) {
      fail++;
      addHistoryEntry({
        time: formatTime(new Date()),
        mode: 'BATCH',
        ok: false,
        channel: channel,
        idHex: idHex,
        dlc: msg.dlc,
        dataHex: '',
        resultText: msg.name + ' · 编码失败: ' + err.message
      }, silent);
      continue;
    }
    var dataHex = bytesToHex(dataBytes);
    var dlc = (typeof msg.dlc === 'number') ? msg.dlc : dataBytes.length;
    prepared.push({
      msg: msg,
      idHex: idHex,
      dlc: dlc,
      dataHex: dataHex,
      frame: { id: msg.id, dlc: dlc, data: dataHex }
    });
  }

  /* 分块批量接口；不支持时整段回退为无间隔单帧 */
  var useBatchApi = true;
  for (var off = 0; off < prepared.length; ) {
    var chunk = prepared.slice(off, off + BATCH_CHUNK);
    off += chunk.length;
    var frames = chunk.map(function (p) { return p.frame; });
    var results = null;

    if (useBatchApi) {
      try {
        results = await sendCanBatchRequest(channel, frames);
        if (results.length !== chunk.length) {
          throw new Error('results 长度不匹配');
        }
      } catch (err) {
        useBatchApi = false;
        results = null;
        /* 本 chunk 改单帧紧接 */
      }
    }

    if (results) {
      for (var r = 0; r < chunk.length; r++) {
        var item = chunk[r];
        var result = results[r] || { status: 'error', code: -3 };
        if (result.status === 'ok') ok++; else fail++;
        addHistoryEntry({
          time: formatTime(new Date()),
          mode: 'BATCH',
          ok: result.status === 'ok',
          channel: channel,
          idHex: item.idHex,
          dlc: item.dlc,
          dataHex: item.dataHex || '',
          resultText: item.msg.name + ' · ' + buildSendResponseText(result)
        }, silent);
      }
      continue;
    }

    /* 回退：逐帧 HTTP，帧间无 sleep、无重试 */
    for (var s = 0; s < chunk.length; s++) {
      var one = chunk[s];
      try {
        var oneResult = await sendCanRequest({
          ch: channel,
          id: one.frame.id,
          dlc: one.frame.dlc,
          data: one.frame.data
        });
        if (oneResult.status === 'ok') ok++; else fail++;
        addHistoryEntry({
          time: formatTime(new Date()),
          mode: 'BATCH',
          ok: oneResult.status === 'ok',
          channel: channel,
          idHex: one.idHex,
          dlc: one.dlc,
          dataHex: one.dataHex || '',
          resultText: one.msg.name + ' · ' + buildSendResponseText(oneResult)
        }, silent);
      } catch (err) {
        fail++;
        addHistoryEntry({
          time: formatTime(new Date()),
          mode: 'BATCH',
          ok: false,
          channel: channel,
          idHex: one.idHex,
          dlc: one.dlc,
          dataHex: one.dataHex || '',
          resultText: one.msg.name + ' · ' + err.message
        }, silent);
      }
    }
  }

  renderHistory();
  scheduleCanPersist();
  state.batchInFlight = false;
  updateBatchUi();
  var tone = fail === 0 ? 'success' : (ok === 0 ? 'error' : 'warning');
  showToast('批量发送完成：成功 ' + ok + '，失败 ' + fail, tone);
}

// ==================== 选中报文发送 ====================
/** @description 发送当前选中报文（DBC 编码后 POST /can/send）。 */
export async function sendSelectedMessage() {
  var msg = getSelectedMessage();
  if (!msg) {
    showToast('请先选择一条报文', 'warning');
    return;
  }
  updateEncodedPreview();
  if (!state.generatedData.length && msg.dlc > 0) {
    showToast('当前报文还没有可发送的数据', 'error');
    return;
  }
  var channel = parseInt(el.defaultChannel.value, 10) || 0;
  var payload = {
    ch: channel,
    id: msg.id,
    dlc: msg.dlc,
    data: bytesToHex(state.generatedData)
  };
  try {
    var result = await sendCanRequest(payload);
    updateResponseBox(el.dbcResp, el.dbcRespText, result, 'dbc');
    addHistoryEntry({
      time: formatTime(new Date()),
      mode: 'DBC',
      ok: result.status === 'ok',
      channel: channel,
      idHex: '0x' + padHex(msg.id, msg.isExtended ? 8 : 3),
      dlc: msg.dlc,
      dataHex: bytesToHex(state.generatedData),
      resultText: buildSendResponseText(result)
    });
    showToast(result.status === 'ok' ? 'DBC 报文发送成功' : '发送失败', result.status === 'ok' ? 'success' : 'error');
  } catch (err) {
    if (el.dbcResp) {
      el.dbcResp.style.display = 'block';
      el.dbcResp.className = 'can-response err';
      el.dbcRespText.textContent = err.message;
    }
    addHistoryEntry({
      time: formatTime(new Date()),
      mode: 'DBC',
      ok: false,
      channel: channel,
      idHex: '0x' + padHex(msg.id, msg.isExtended ? 8 : 3),
      dlc: msg.dlc,
      dataHex: bytesToHex(state.generatedData),
      resultText: err.message
    });
    showToast('发送失败: ' + err.message, 'error');
  }
}

// ==================== 原始帧发送 ====================
function validateManualData() {
  var dataHex = (el.manualData.value || '').trim().replace(/\s+/g, '');
  var dlc = parseInt(el.manualDlc.value, 10);
  if (!Number.isFinite(dlc) || dlc < 0 || dlc > 64) {
    throw new Error('DLC 需在 0~64 之间');
  }
  if (dataHex && !/^[0-9a-fA-F]+$/.test(dataHex)) {
    throw new Error('HEX 数据包含非法字符');
  }
  if (dataHex.length % 2 !== 0) {
    throw new Error('HEX 数据长度必须为偶数');
  }
  var byteLen = dataHex.length / 2;
  if (byteLen !== dlc) {
    throw new Error('当前 HEX 为 ' + byteLen + ' 字节，但 DLC=' + dlc);
  }
  return dataHex.toUpperCase();
}

/** @description 刷新手动帧输入的校验提示。 */
export function refreshManualHint() {
  try {
    var dataHex = validateManualData();
    el.manualHint.className = 'helper-note ok';
    el.manualHint.innerHTML = '原始 HEX 合法，当前长度 <b>' + (dataHex.length / 2) + '</b> 字节。';
  } catch (err) {
    el.manualHint.className = 'helper-note err';
    el.manualHint.innerHTML = sanitizeHtml(err.message);
  }
}

/** @description 发送手动输入的原始 CAN 帧。 */
export async function sendManualFrame() {
  var id = parseNumericId(el.manualId.value);
  if (!Number.isFinite(id)) {
    showToast('CAN ID 格式错误', 'error');
    return;
  }
  var channel = parseInt(el.manualChannel.value, 10) || 0;
  var dlc = parseInt(el.manualDlc.value, 10) || 0;
  var dataHex = '';
  try {
    dataHex = validateManualData();
  } catch (err) {
    showToast(err.message, 'error');
    refreshManualHint();
    return;
  }

  try {
    var result = await sendCanRequest({
      ch: channel,
      id: id,
      dlc: dlc,
      data: dataHex
    });
    updateResponseBox(el.manualResp, el.manualRespText, result, 'manual');
    addHistoryEntry({
      time: formatTime(new Date()),
      mode: 'RAW',
      ok: result.status === 'ok',
      channel: channel,
      idHex: '0x' + padHex(id, id > 0x7FF ? 8 : 3),
      dlc: dlc,
      dataHex: dataHex,
      resultText: buildSendResponseText(result)
    });
    showToast(result.status === 'ok' ? '原始帧发送成功' : '发送失败', result.status === 'ok' ? 'success' : 'error');
  } catch (err) {
    if (el.manualResp) {
      el.manualResp.style.display = 'block';
      el.manualResp.className = 'can-response err';
      el.manualRespText.textContent = err.message;
    }
    addHistoryEntry({
      time: formatTime(new Date()),
      mode: 'RAW',
      ok: false,
      channel: channel,
      idHex: '0x' + padHex(id, id > 0x7FF ? 8 : 3),
      dlc: dlc,
      dataHex: dataHex,
      resultText: err.message
    });
    showToast('发送失败: ' + err.message, 'error');
  }
}

/** @description 重置手动帧输入区为默认值。 */
export function resetManualFields() {
  el.manualChannel.value = el.defaultChannel.value;
  el.manualId.value = '0x18DA00F1';
  el.manualDlc.value = '8';
  el.manualData.value = '0102030405060708';
  if (el.manualResp) el.manualResp.style.display = 'none';
  refreshManualHint();
}

/** @description 将当前选中报文的编码预览载入到手动帧输入区。 */
export function loadPreviewToManual() {
  var msg = getSelectedMessage();
  if (!msg || !state.generatedData.length) {
    showToast('当前没有可载入的预览帧', 'warning');
    return;
  }
  el.manualChannel.value = el.defaultChannel.value;
  el.manualId.value = '0x' + padHex(msg.id, msg.isExtended ? 8 : 3);
  el.manualDlc.value = String(msg.dlc);
  el.manualData.value = bytesToHex(state.generatedData);
  refreshManualHint();
  showToast('已载入到原始发送区', 'success');
}
