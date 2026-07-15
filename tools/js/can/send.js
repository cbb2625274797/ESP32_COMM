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
import { state, el, getBaseUrl, padHex, bytesToHex, parseNumericId } from './dbc.js';
import { escapeHtml as sanitizeHtml } from '../common/utils.js';
import { showToast } from '../common/toast.js';
import { updateBatchUi } from './messages.js';
import { ensureMessageDefaults, getSignalValueMap, getSelectedMessage, updateEncodedPreview, encodeMessageData } from './signals.js';

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
function addHistoryEntry(entry) {
  state.history.unshift(entry);
  if (state.history.length > 20) state.history.length = 20;
  renderHistory();
}

/** @description 渲染发送历史列表。 */
export function renderHistory() {
  if (!el.historyList) return;
  if (!state.history.length) {
    el.historyList.innerHTML = '<div class="status-empty">尚未发送任何 CAN 帧。</div>';
    return;
  }
  el.historyList.innerHTML = state.history.map(function (item) {
    return '' +
      '<div class="history-item">' +
      '<div class="history-item-top">' +
      '<span class="history-item-tag ' + (item.ok ? 'ok' : 'err') + '">' + sanitizeHtml(item.mode) + '</span>' +
      '<span class="history-item-time">' + sanitizeHtml(item.time) + '</span>' +
      '</div>' +
      '<div class="history-item-main">' +
      '<b>CH' + sanitizeHtml(item.channel) + '</b> · ' +
      sanitizeHtml(item.idHex) + ' · DLC ' + sanitizeHtml(item.dlc) +
      '</div>' +
      '<div class="history-item-hex">' + sanitizeHtml(item.dataHex || '--') + '</div>' +
      '<div class="history-item-foot">' + sanitizeHtml(item.resultText) + '</div>' +
      '</div>';
  }).join('');
}

// ==================== 批量发送 ====================
/** @description 批量发送所有勾选的报文（30ms 帧间间隔防 GD32 SPI RXORERR）。 */
export async function sendCheckedMessages() {
  if (!state.dbc) { showToast('请先导入 DBC', 'warning'); return; }
  var keys = Object.keys(state.checkedKeys);
  if (!keys.length) { showToast('请先勾选要发送的报文', 'warning'); return; }
  if (state.batchInFlight) return;
  state.batchInFlight = true;
  updateBatchUi();
  var channel = parseInt(el.defaultChannel.value, 10) || 0;
  var ok = 0, fail = 0;
  /* BATCH 间隔：GD32 SPI 从机用 RBNE 中断逐字节接收，无 RX DMA，
   * 连续高频 CS 事务易触发 RXORERR -> 帧字节丢失 -> CRC 失败 -> 0xEE。
   * 30ms 间隔给 GD32 足够时间完成 onCsRelease 清理 + EXTI 重新待命。 */
  var BATCH_INTERVAL_MS = 30;
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
        resultText: '编码失败: ' + err.message
      });
      continue;
    }
    var dataHex = bytesToHex(dataBytes);
    try {
      var result = await sendCanRequest({
        ch: channel,
        id: msg.id,
        dlc: msg.dlc,
        data: dataHex
      });
      if (result.status === 'ok') ok++; else fail++;
      addHistoryEntry({
        time: formatTime(new Date()),
        mode: 'BATCH',
        ok: result.status === 'ok',
        channel: channel,
        idHex: idHex,
        dlc: msg.dlc,
        dataHex: dataHex,
        resultText: msg.name + ' · ' + buildSendResponseText(result)
      });
    } catch (err) {
      fail++;
      addHistoryEntry({
        time: formatTime(new Date()),
        mode: 'BATCH',
        ok: false,
        channel: channel,
        idHex: idHex,
        dlc: msg.dlc,
        dataHex: dataHex,
        resultText: msg.name + ' · ' + err.message
      });
    }
    /* 帧间间隔，避免 GD32 SPI RXORERR（最后一帧不需要等） */
    if (i < keys.length - 1) {
      await new Promise(function (r) { setTimeout(r, BATCH_INTERVAL_MS); });
    }
  }
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
