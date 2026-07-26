/**
 * signals.js - 信号编辑器渲染 + 信号编解码（raw ↔ physical）
 *
 * 包含：行内信号表格、信号值存取、编码写入、浮点/整数/Motorola/Intel 位操作。
 *
 * 依赖：dbc.js（state/el/padHex/bytesToHex/cssEscape/findSignal）、common/utils.js（escapeHtml）。
 */
import { state, el, padHex, bytesToHex, cssEscape, findSignal, findMessageByKey } from './dbc.js';
import { escapeHtml as sanitizeHtml } from '../common/utils.js';
import { scheduleCanPersist } from './persist.js';

// ==================== 信号值存取 ====================
/** @description 确保报文有 signalValues bag，并为每个信号填充默认值。返回 bag。 */
export function ensureMessageDefaults(message) {
  if (!message) return null;
  var bag = state.signalValues[message.key];
  if (!bag) {
    bag = {};
    state.signalValues[message.key] = bag;
  }
  for (var i = 0; i < message.signals.length; i++) {
    var sig = message.signals[i];
    if (bag[sig.name] == null || bag[sig.name] === '') {
      bag[sig.name] = getDefaultSignalValue(sig);
    }
  }
  return bag;
}

export function getSignalValueMap(message) {
  if (!message) return {};
  return state.signalValues[message.key] || ensureMessageDefaults(message);
}

function setSignalValue(message, signalName, value) {
  if (!message) return;
  var bag = state.signalValues[message.key];
  if (!bag) {
    bag = {};
    state.signalValues[message.key] = bag;
  }
  bag[signalName] = value;
}

function getDefaultSignalValue(signal) {
  /* 优先使用 DBC 中 BA_ "GenSigDefaultValue" 声明的物理值，否则回退为 0 */
  if (signal && signal.defaultValue != null && Number.isFinite(Number(signal.defaultValue))) {
    return String(signal.defaultValue);
  }
  return '0';
}

/** @description 获取当前选中的报文对象（无选中返回 null）。 */
export function getSelectedMessage() {
  if (!state.dbc || !state.selectedMessageKey) return null;
  return findMessageByKey(state.selectedMessageKey);
}

function getMultiplexerSignal(message) {
  if (!message) return null;
  for (var i = 0; i < message.signals.length; i++) {
    if (message.signals[i].multiplexRole === 'mux') return message.signals[i];
  }
  return null;
}

/** @description 根据当前 multiplexer 值返回激活信号列表。 */
export function getActiveSignals(message) {
  if (!message) return [];
  var muxSignal = getMultiplexerSignal(message);
  if (!muxSignal) return message.signals.slice();
  var bag = getSignalValueMap(message);
  var currentValue = Number(bag[muxSignal.name]);
  return message.signals.filter(function (signal) {
    if (!signal.multiplexRole || signal.multiplexRole === 'mux') return true;
    return signal.multiplexRole === 'value' && Number.isFinite(currentValue) && signal.multiplexValue === currentValue;
  });
}

// ==================== 信号行渲染 ====================
/** @description 在报文展开行内渲染信号表格（inline grid）。 */
export function renderInlineSignalGrid(message, host) {
  if (!host) return;
  ensureMessageDefaults(message);
  var activeSignals = getActiveSignals(message);
  if (!activeSignals.length) {
    host.innerHTML = '<div class="status-empty">当前报文没有信号定义。</div>';
    return;
  }
  var rows = activeSignals.map(function (signal, index) {
    return renderSignalRow(message, signal, index);
  }).join('');
  host.innerHTML =
    '<div class="signal-table signal-table-inline">' +
    '<div class="signal-table-head">' +
    '<span class="signal-cell signal-cell-index">#</span>' +
    '<span class="signal-cell signal-cell-name">信号名称</span>' +
    '<span class="signal-cell signal-cell-value">输入值</span>' +
    '<span class="signal-cell signal-cell-range">范围 / 枚举</span>' +
    '<span class="signal-cell signal-cell-meta">位定义</span>' +
    '<span class="signal-cell signal-cell-comment">注释</span>' +
    '</div>' +
    '<div class="signal-table-body">' + rows + '</div>' +
    '</div>';
  bindSignalInputs(host, message);
}

function renderSignalRow(message, signal, index) {
  var bag = getSignalValueMap(message);
  var enumKeys = Object.keys(signal.enumMap || {});
  var enumCount = enumKeys.length;
  var bitRange = signal.startBit + '|' + signal.length + ' @' + (signal.byteOrder === 'little' ? 'Intel' : 'Motorola');
  var rangeText = '';
  if (Number.isFinite(signal.min) || Number.isFinite(signal.max)) {
    rangeText = (Number.isFinite(signal.min) ? signal.min : '-') + ' ~ ' + (Number.isFinite(signal.max) ? signal.max : '-');
  } else {
    rangeText = '未声明';
  }
  var typeText = (signal.valueType || 'int').toUpperCase() + ' / ' + (signal.isSigned ? 'signed' : 'unsigned');
  var factorText = 'factor ' + signal.factor + ' / offset ' + signal.offset + (signal.unit ? (' / ' + signal.unit) : '');
  var comment = signal.comment || '-';
  if (comment.length > 48) comment = comment.slice(0, 48) + '...';
  var inputHtml = '';
  if (enumCount) {
    inputHtml = '<select class="input signal-input signal-input-compact" data-msg="' + sanitizeHtml(message.key) + '" data-signal="' + sanitizeHtml(signal.name) + '">' +
      enumKeys.map(function (key) {
        var label = signal.enumMap[key];
        var selected = String(bag[signal.name]) === String(key) ? ' selected' : '';
        return '<option value="' + sanitizeHtml(key) + '"' + selected + '>' +
          sanitizeHtml(key + ' - ' + label) + '</option>';
      }).join('') + '</select>';
  } else {
    inputHtml = '<input class="input signal-input signal-input-compact" type="number" step="any" data-msg="' + sanitizeHtml(message.key) + '" data-signal="' + sanitizeHtml(signal.name) +
      '" value="' + sanitizeHtml(bag[signal.name]) + '">';
  }
  return '' +
    '<div class="signal-row">' +
    '<span class="signal-cell signal-cell-index">' + (index + 1) + '</span>' +
    '<span class="signal-cell signal-cell-name">' +
    '<b>' + sanitizeHtml(signal.name) + '</b>' +
    '<small>' + sanitizeHtml((signal.receivers || []).join(', ') || '-') + '</small>' +
    '</span>' +
    '<span class="signal-cell signal-cell-value">' + inputHtml + '</span>' +
    '<span class="signal-cell signal-cell-range">' +
    '<b>' + sanitizeHtml(rangeText) + '</b>' +
    (enumCount ? '<small>' + sanitizeHtml(enumKeys.slice(0, 3).map(function (key) { return key + ':' + signal.enumMap[key]; }).join(' / ')) + (enumCount > 3 ? ' ...' : '') + '</small>' : '<small>' + sanitizeHtml(factorText) + '</small>') +
    '</span>' +
    '<span class="signal-cell signal-cell-meta">' +
    '<b>' + sanitizeHtml(bitRange) + '</b>' +
    '<small>' + sanitizeHtml(typeText) + '</small>' +
    '</span>' +
    '<span class="signal-cell signal-cell-comment" title="' + sanitizeHtml(signal.comment || '') + '">' + sanitizeHtml(comment) + '</span>' +
    '</div>';
}

function handleSignalInputEvent(evt) {
  var input = evt.currentTarget;
  var key = input.getAttribute('data-msg');
  var name = input.getAttribute('data-signal');
  if (!key || !name || !state.dbc) return;
  var message = findMessageByKey(key);
  if (!message) return;
  setSignalValue(message, name, input.value);
  var signal = findSignal(message, name);
  if (signal && signal.multiplexRole === 'mux') {
    /* MUX 值变更后需重建信号表格（激活信号集变化）。
     * 限定在 el.messageList 作用域内查找，避免多列表/多 DBC 场景匹配到错误节点
     *（与 messages.js 中 toggleMessageExpand 的作用域策略一致）。 */
    var root = el.messageList || document;
    var inlineGrid = root.querySelector('.message-row-expand[data-key="' + cssEscape(key) + '"] .signal-table-host');
    if (inlineGrid) renderInlineSignalGrid(message, inlineGrid);
  }
  if (key === state.selectedMessageKey) updateEncodedPreview();
  scheduleCanPersist();
}

function bindSignalInputs(container, message) {
  if (!container) return;
  var inputs = container.querySelectorAll('.signal-input');
  for (var i = 0; i < inputs.length; i++) {
    inputs[i].addEventListener('input', handleSignalInputEvent);
    inputs[i].addEventListener('change', handleSignalInputEvent);
  }
  if (message && message.signals.length && message.key === state.selectedMessageKey) {
    updateEncodedPreview();
  }
}

// ==================== 编码预览 ====================
/** @description 根据当前选中报文 + 信号值更新编码预览（hex / byte strip / hint）。 */
export function updateEncodedPreview() {
  var message = getSelectedMessage();
  if (!message) {
    state.generatedData = new Uint8Array(0);
    if (el.generatedHex) el.generatedHex.textContent = '--';
    if (el.generatedBytes) el.generatedBytes.innerHTML = '';
    return;
  }
  try {
    state.generatedData = encodeMessageData(message, getSignalValueMap(message));
    var hex = bytesToHex(state.generatedData);
    el.generatedHex.textContent = hex || '--';
    el.generatedBytes.innerHTML = renderByteStrip(state.generatedData);
    var hint = 'DLC <b>' + message.dlc + '</b>，当前编码 <b>' + state.generatedData.length +
      '</b> 字节，发送通道 <b>CH' + (el.defaultChannel ? el.defaultChannel.value : '0') + '</b>。';
    if (el.encodeHint) {
      el.encodeHint.className = 'helper-note ok';
      el.encodeHint.innerHTML = hint;
    }
  } catch (err) {
    state.generatedData = new Uint8Array(0);
    el.generatedHex.textContent = '--';
    el.generatedBytes.innerHTML = '';
    if (el.encodeHint) {
      el.encodeHint.className = 'helper-note err';
      el.encodeHint.innerHTML = '编码失败：' + sanitizeHtml(err.message);
    }
  }
}

function renderByteStrip(bytes) {
  if (!bytes || !bytes.length) return '<div class="status-empty">无数据</div>';
  var html = [];
  for (var i = 0; i < bytes.length; i++) {
    html.push('<span class="byte-pill"><b>B' + i + '</b><span>' + padHex(bytes[i], 2) + '</span></span>');
  }
  return html.join('');
}

// ==================== 信号编解码（raw ↔ physical） ====================
/** @description 将报文 + 信号值编码为 Uint8Array。 */
export function encodeMessageData(message, signalValues) {
  var dlc = message && message.dlc != null ? Number(message.dlc) : 0;
  if (!Number.isFinite(dlc) || dlc < 0) dlc = 0;
  if (dlc > 64) dlc = 64;
  /* DLC=0：无数据区（如 CANFD_BOOT），不写信号，直接空帧 */
  if (dlc === 0) {
    return new Uint8Array(0);
  }
  var bytes = new Uint8Array(dlc);
  var activeSignals = getActiveSignals(message);
  for (var i = 0; i < activeSignals.length; i++) {
    var signal = activeSignals[i];
    /* 信号完全落在 DLC 外则跳过，避免把空帧/定义异常拖成批量失败 */
    if (!signalFitsInDlc(signal, dlc)) {
      continue;
    }
    var parsed = parseSignalInput(signal, signalValues[signal.name]);
    if (signal.valueType === 'float' || signal.valueType === 'double') {
      writeFloatSignal(bytes, signal, parsed.numeric);
    } else {
      writeIntegerSignal(bytes, signal, parsed);
    }
  }
  return bytes;
}

/** @description 粗判信号位域是否完全落在 dlc 字节内（越界整段则跳过）。 */
function signalFitsInDlc(signal, dlc) {
  if (!signal || dlc <= 0) return false;
  var maxBit = dlc * 8 - 1;
  if (signal.byteOrder === 'little') {
    var end = signal.startBit + signal.length - 1;
    return signal.startBit >= 0 && signal.startBit <= maxBit && end <= maxBit;
  }
  /* float：要求整段字节落在 DLC 内（startBit 必须字节对齐，由 writeFloatSignal 另校验） */
  if (signal.valueType === 'float' || signal.valueType === 'double') {
    var startByte = signal.startBit / 8;
    var byteLen = signal.length / 8;
    return startByte >= 0 && Number.isInteger(startByte) &&
           startByte + byteLen <= dlc;
  }
  /* Motorola：用 buildMotorolaBitPositions 计算实际占用位序，检查最大位是否落在 DLC 内。
   * 此前仅判 startByte，长跨字节信号末位可能越界 → 会被 setBitByIndex 抛错而非跳过，
   * 与 little 分支"越界整段跳过"语义不一致；这里统一为"越界即跳过"。 */
  if (signal.length <= 0 || signal.length > 64) return false;
  var positions = buildMotorolaBitPositions(signal.startBit, signal.length);
  for (var j = 0; j < positions.length; j++) {
    if (positions[j] < 0 || positions[j] > maxBit) return false;
  }
  return true;
}

function parseSignalInput(signal, inputValue) {
  var value = inputValue;
  if (value === '' || value == null) value = getDefaultSignalValue(signal);
  var num = Number(value);
  if (!Number.isFinite(num)) throw new Error(signal.name + ' 不是合法数字');
  /* enum 信号：取值是离散枚举 key（数字字符串），DBC 的 min/max 是物理连续值范围，
   * 对 enum 不适用 —— 合法 enum 值可能落在 [min,max] 之外（例如 fault 枚举）。
   * 因此 enum 信号只做"值必须 ∈ enumMap keys"的校验，跳过 min/max。 */
  var enumKeys = Object.keys(signal.enumMap || {});
  if (enumKeys.length) {
    if (enumKeys.indexOf(String(value)) < 0) {
      throw new Error(signal.name + ' 不是合法枚举值：' + value);
    }
    return { numeric: num, text: String(value).trim() };
  }
  if (Number.isFinite(signal.min) && num < signal.min) {
    throw new Error(signal.name + ' 低于最小值 ' + signal.min);
  }
  if (Number.isFinite(signal.max) && num > signal.max) {
    throw new Error(signal.name + ' 高于最大值 ' + signal.max);
  }
  return {
    numeric: num,
    text: String(value).trim()
  };
}

function isIntegerLike(value) {
  return /^-?\d+$/.test(String(value == null ? '' : value).trim());
}

function physicalToRawBigInt(signal, parsedValue) {
  if ((signal.factor === 1 || signal.factor === 1.0) &&
      (signal.offset === 0 || signal.offset === 0.0) &&
      isIntegerLike(parsedValue.text)) {
    return BigInt(parsedValue.text);
  }
  /* factor=0 是 DBC 异常情况（规范上 factor 不应为 0），此处为防御性兜底：
   *   - 有 offset：物理值直接减 offset 作为 raw（把 offset 视为"物理=raw 的偏移基线"）
   *   - 无 offset：物理值即 raw
   * 实际 DBC 不应走到此分支，但避免除零导致 NaN。 */
  var rawNumber = signal.factor === 0
    ? Math.round(signal.offset ? (parsedValue.numeric - signal.offset) : parsedValue.numeric)
    : Math.round((parsedValue.numeric - signal.offset) / signal.factor);
  if (!Number.isFinite(rawNumber)) {
    throw new Error(signal.name + ' 原始值计算失败');
  }
  return BigInt(rawNumber);
}

function bigPow2(bits) {
  return 1n << BigInt(bits);
}

function maxUnsignedValue(bits) {
  return bigPow2(bits) - 1n;
}

function minSignedValue(bits) {
  return -(1n << BigInt(bits - 1));
}

function maxSignedValue(bits) {
  return (1n << BigInt(bits - 1)) - 1n;
}

function writeIntegerSignal(bytes, signal, parsedValue) {
  var raw = physicalToRawBigInt(signal, parsedValue);
    if (signal.length <= 0 || signal.length > 64) {
    throw new Error(signal.name + ' 位宽超出当前页面支持范围');
  }
  if (signal.isSigned) {
    var min = minSignedValue(signal.length);
    var max = maxSignedValue(signal.length);
    if (raw < min || raw > max) {
      throw new Error(signal.name + ' 原始值超出带符号范围');
    }
  } else {
    var maxUnsigned = maxUnsignedValue(signal.length);
    if (raw < 0 || raw > maxUnsigned) {
      throw new Error(signal.name + ' 原始值超出无符号范围');
    }
  }
  var unsignedRaw = raw < 0 ? bigPow2(signal.length) + raw : raw;

  if (signal.byteOrder === 'little') {
    for (var i = 0; i < signal.length; i++) {
      var bitVal = Number((unsignedRaw >> BigInt(i)) & 1n);
      var bitIndex = signal.startBit + i;
      setBitByIndex(bytes, bitIndex, bitVal);
    }
    return;
  }

  var positions = buildMotorolaBitPositions(signal.startBit, signal.length);
  for (var j = 0; j < positions.length; j++) {
    var shift = signal.length - 1 - j;
    var motorolaBit = Number((unsignedRaw >> BigInt(shift)) & 1n);
    setBitByIndex(bytes, positions[j], motorolaBit);
  }
}

function writeFloatSignal(bytes, signal, physicalValue) {
  if (signal.length !== 32 && signal.length !== 64) {
    throw new Error(signal.name + ' 浮点信号长度需为 32 或 64 位');
  }
  if (signal.byteOrder !== 'little') {
    throw new Error(signal.name + ' 浮点信号当前仅支持 Intel 小端');
  }
  if (signal.startBit % 8 !== 0) {
    throw new Error(signal.name + ' 浮点信号起始位必须按字节对齐');
  }
  var startByte = signal.startBit / 8;
  var byteLen = signal.length / 8;
  /* 越界防御：signalFitsInDlc 已在 encodeMessageData 入口过滤，这里二次断言，
   * 避免任何直接调用方（如未来新增的 decode 路径）漏检导致写越界。 */
  if (startByte + byteLen > bytes.length) {
    throw new Error(signal.name + ' 浮点信号写入越界：startByte=' + startByte +
      ' + length=' + byteLen + ' > 缓冲区 ' + bytes.length + ' 字节');
  }
  var buffer = new ArrayBuffer(byteLen);
  var view = new DataView(buffer);
  if (signal.length === 32) view.setFloat32(0, physicalValue, true);
  else view.setFloat64(0, physicalValue, true);
  var rawBytes = new Uint8Array(buffer);
  for (var i = 0; i < rawBytes.length; i++) {
    bytes[startByte + i] = rawBytes[i];
  }
}

function buildMotorolaBitPositions(startBit, length) {
  var positions = [];
  var bit = startBit;
  for (var i = 0; i < length; i++) {
    positions.push(bit);
    if (bit % 8 === 0) bit += 15;
    else bit -= 1;
  }
  return positions;
}

function setBitByIndex(bytes, bitIndex, bitValue) {
  var byteIndex = Math.floor(bitIndex / 8);
  var bitInByte = bitIndex % 8;
  if (byteIndex < 0 || byteIndex >= bytes.length) {
    throw new Error('信号写入越界，bit=' + bitIndex +
      '（缓冲区 ' + bytes.length + ' 字节）');
  }
  if (bitValue) bytes[byteIndex] |= (1 << bitInByte);
  else bytes[byteIndex] &= ~(1 << bitInByte);
}
