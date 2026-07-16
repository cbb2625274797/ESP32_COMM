/**
 * dbc.js - DBC 解析器 + 共享状态/元素缓存/基础工具
 *
 * 包含：共享 state/el、基础工具、DBC 解析、cssEscape、readDbcText。
 *
 * 本模块是 can 子系统的基础层，不依赖其他 can 子模块；state/el 在此导出，
 * 供 messages/signals/send/feedback 共享（ES Module 的 live binding 保证引用一致）。
 */

// ==================== 共享状态 ====================
export var state = {
  /** @type {{ id: string, name: string, size: number, dbc: object }[]} */
  dbcSources: [],
  /** 合并视图 { messages, nodeNames, messageMap }；由 rebuildMergedDbc() 维护 */
  dbc: null,
  filteredMessages: [],
  selectedMessageKey: '',
  signalValues: {},
  checkedKeys: {},
  expandedKeys: {},
  transmitterFilter: Object.create(null),
  transmitterFilterEmpty: true,
  generatedData: new Uint8Array(0),
  history: [],
  latestStatus: null,
  batchInFlight: false
};

// ==================== 共享元素缓存 ====================
// 由入口文件（js/can.js）的 cacheElements() 填充，各子模块通过 import 引用同一对象。
export var el = {};

// ==================== 基础工具 ====================
var LS_ESP_IP = 'dcu_esp_ip';
var DEFAULT_ESP_IP = '192.168.31.127';

/** @description 设备 IP：优先 localStorage（与 OTA 页共用），否则默认。 */
export function getDeviceIp() {
  try {
    var saved = localStorage.getItem(LS_ESP_IP);
    if (saved && saved.trim()) return saved.trim();
  } catch (_) {}
  if (el.toolIp && el.toolIp.value && el.toolIp.value.trim()) {
    return el.toolIp.value.trim();
  }
  return DEFAULT_ESP_IP;
}

/** @description 记住设备 IP（status 回报或其它页写入）。 */
export function setDeviceIp(ip) {
  if (!ip || !String(ip).trim()) return;
  var v = String(ip).trim();
  try { localStorage.setItem(LS_ESP_IP, v); } catch (_) {}
  if (el.toolIp) el.toolIp.value = v;
}

/** @description 拼接设备基础 URL（http://<ip>）。 */
export function getBaseUrl() {
  return 'http://' + getDeviceIp();
}

/** @description 解析手动输入的 CAN ID（支持 0x/hex/十进制）。 */
export function parseNumericId(input) {
  var raw = String(input == null ? '' : input).trim();
  if (!raw) return NaN;
  if (/^0x/i.test(raw)) return parseInt(raw, 16);
  if (/^[0-9]+$/.test(raw)) return parseInt(raw, 10);
  if (/^[0-9a-fA-F]+h$/i.test(raw)) return parseInt(raw.slice(0, -1), 16);
  if (/^[0-9a-fA-F]+$/.test(raw)) return parseInt(raw, 16);
  return NaN;
}

/** @description 将数值格式化为指定宽度的十六进制大写字符串。 */
export function padHex(value, width) {
  return value.toString(16).toUpperCase().padStart(width, '0');
}

/** @description 将字节数组转为连续的十六进制字符串。 */
export function bytesToHex(bytes) {
  if (!bytes || !bytes.length) return '';
  var parts = [];
  for (var i = 0; i < bytes.length; i++) {
    parts.push(padHex(bytes[i], 2));
  }
  return parts.join('');
}

/** @description 转义 CSS 选择器中的特殊字符。 */
export function cssEscape(value) {
  if (window.CSS && CSS.escape) return CSS.escape(value);
  return String(value).replace(/[^a-zA-Z0-9_-]/g, function (ch) { return '\\\\' + ch; });
}

/** @description 去除回车并 trim，用于 DBC 搜索等输入归一化。 */
export function normalizeWhitespace(text) {
  return String(text || '').replace(/\r/g, '').trim();
}

// ==================== DBC 文件读取 ====================
/** @description 读取 DBC 文件文本，优先 UTF-8，失败回退 GB18030，自动去 BOM。 */
export async function readDbcText(file) {
  var buf = await file.arrayBuffer();
  var bytes = new Uint8Array(buf);
  var stripBom = function (s) {
    return (s && s.charCodeAt(0) === 0xFEFF) ? s.slice(1) : s;
  };
  try {
    var strictUtf8 = new TextDecoder('utf-8', { fatal: true });
    return stripBom(strictUtf8.decode(bytes));
  } catch (e) {
    try {
      var gb = new TextDecoder('gb18030');
      return stripBom(gb.decode(bytes));
    } catch (e2) {
      return stripBom(new TextDecoder('utf-8').decode(bytes));
    }
  }
}

// ==================== DBC 解析 ====================
/** @description 解析 DBC 文本，返回 { messages, nodeNames, messageMap }。 */
export function parseDbc(text) {
  var lines = String(text || '').replace(/\r/g, '').split('\n');
  var messages = [];
  var messageMap = {};
  var comments = { messages: {}, signals: {} };
  var valueTables = {};
  var sigDefaults = {};
  var nodeNames = [];
  var current = null;

  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].trim();
    if (!line) continue;

    var nodeMatch = line.match(/^BU_\s*:\s*(.*)$/);
    if (nodeMatch) {
      nodeNames = nodeMatch[1].trim() ? nodeMatch[1].trim().split(/\s+/) : [];
      continue;
    }

    var messageMatch = line.match(/^BO_\s+(\d+)\s+([A-Za-z0-9_]+)\s*:\s*(\d+)\s+(.+)$/);
    if (messageMatch) {
      current = {
        idRaw: parseInt(messageMatch[1], 10),
        name: messageMatch[2],
        dlc: parseInt(messageMatch[3], 10),
        transmitter: messageMatch[4].trim(),
        signals: [],
        comment: ''
      };
      current.id = current.idRaw & 0x1FFFFFFF;
      current.isExtended = current.idRaw > 0x7FF;
      current.key = String(current.idRaw) + ':' + current.name;
      messages.push(current);
      messageMap[current.idRaw] = current;
      continue;
    }

    var signalMatch = line.match(/^SG_\s+([A-Za-z0-9_]+)(?:\s+((?:M|m-?\d+)))?\s*:\s*(\d+)\|(\d+)@([01])([+-])\s+\(([^\),]+),([^\)]+)\)\s+\[([^\|]*)\|([^\]]*)\]\s+"([^"]*)"\s+(.+)$/);
    if (signalMatch && current) {
      var muxInfo = parseMultiplexToken(signalMatch[2]);
      current.signals.push({
        name: signalMatch[1],
        startBit: parseInt(signalMatch[3], 10),
        length: parseInt(signalMatch[4], 10),
        byteOrder: signalMatch[5] === '1' ? 'little' : 'big',
        isSigned: signalMatch[6] === '-',
        factor: parseFloat(signalMatch[7]),
        offset: parseFloat(signalMatch[8]),
        min: signalMatch[9].trim() === '' ? null : parseFloat(signalMatch[9]),
        max: signalMatch[10].trim() === '' ? null : parseFloat(signalMatch[10]),
        unit: signalMatch[11],
        receivers: signalMatch[12].split(/\s*,\s*/),
        comment: '',
        valueType: 'int',
        enumMap: {},
        defaultValue: null,
        multiplexRole: muxInfo.role,
        multiplexValue: muxInfo.value
      });
      continue;
    }

    var messageCommentMatch = line.match(/^CM_\s+BO_\s+(\d+)\s+"([\s\S]*)";$/);
    if (messageCommentMatch) {
      comments.messages[messageCommentMatch[1]] = messageCommentMatch[2];
      continue;
    }

    var signalCommentMatch = line.match(/^CM_\s+SG_\s+(\d+)\s+([A-Za-z0-9_]+)\s+"([\s\S]*)";$/);
    if (signalCommentMatch) {
      comments.signals[signalCommentMatch[1] + ':' + signalCommentMatch[2]] = signalCommentMatch[3];
      continue;
    }

    var valueMatch = line.match(/^VAL_\s+(\d+)\s+([A-Za-z0-9_]+)\s+(.+);$/);
    if (valueMatch) {
      var table = {};
      var pairRe = /(-?\d+)\s+"([^"]*)"/g;
      var m;
      while ((m = pairRe.exec(valueMatch[3])) !== null) {
        table[m[1]] = m[2];
      }
      valueTables[valueMatch[1] + ':' + valueMatch[2]] = table;
      continue;
    }

    var valTypeMatch = line.match(/^SIG_VALTYPE_\s+(\d+)\s+([A-Za-z0-9_]+)\s*:\s*(\d+)\s*;$/);
    if (valTypeMatch) {
      var msg = messageMap[valTypeMatch[1]];
      if (!msg) continue;
      var signal = findSignal(msg, valTypeMatch[2]);
      if (signal) {
        signal.valueType = valTypeMatch[3] === '1' ? 'float' : valTypeMatch[3] === '2' ? 'double' : 'int';
      }
      continue;
    }

    /* BA_ "GenSigDefaultValue" SG_ <msgId> <signalName> <value>;
     * 注意该值是物理值（已应用 factor/offset），与 GenSigStartValue（raw）不同 */
    var sigDefaultMatch = line.match(/^BA_\s+"GenSigDefaultValue"\s+SG_\s+(\d+)\s+([A-Za-z0-9_]+)\s+(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*;$/);
    if (sigDefaultMatch) {
      sigDefaults[sigDefaultMatch[1] + ':' + sigDefaultMatch[2]] = sigDefaultMatch[3];
      continue;
    }
  }

  messages.forEach(function (msg) {
    msg.comment = comments.messages[String(msg.idRaw)] || '';
    msg.signals.forEach(function (signal) {
      signal.comment = comments.signals[String(msg.idRaw) + ':' + signal.name] || '';
      signal.enumMap = valueTables[String(msg.idRaw) + ':' + signal.name] || {};
      var dv = sigDefaults[String(msg.idRaw) + ':' + signal.name];
      signal.defaultValue = (dv != null && Number.isFinite(Number(dv))) ? dv : null;
    });
  });

  return {
    messages: messages,
    nodeNames: nodeNames,
    messageMap: messageMap
  };
}

/** @description 在报文的 signals 数组中按名称查找信号。 */
export function findSignal(message, signalName) {
  for (var i = 0; i < message.signals.length; i++) {
    if (message.signals[i].name === signalName) return message.signals[i];
  }
  return null;
}

/** @description 解析 SG_ 行的 multiplex token（M / mN / 空）。 */
export function parseMultiplexToken(token) {
  if (!token) return { role: null, value: null };
  if (token === 'M') return { role: 'mux', value: null };
  var match = token.match(/^m(-?\d+)$/);
  if (match) {
    return { role: 'value', value: parseInt(match[1], 10) };
  }
  return { role: null, value: null };
}

/**
 * 将多个 DBC 源合并为统一 messages 列表。
 * 每条报文 key = sourceId:idRaw:name，避免不同文件同 ID/同名冲突。
 */
export function rebuildMergedDbc() {
  if (!state.dbcSources.length) {
    state.dbc = null;
    return null;
  }
  var messages = [];
  var nodeSet = Object.create(null);
  var nodeNames = [];
  var messageMap = {};
  for (var s = 0; s < state.dbcSources.length; s++) {
    var src = state.dbcSources[s];
    var dbc = src.dbc;
    if (!dbc || !dbc.messages) continue;
    for (var n = 0; n < (dbc.nodeNames || []).length; n++) {
      var nn = dbc.nodeNames[n];
      if (nn && !nodeSet[nn]) {
        nodeSet[nn] = 1;
        nodeNames.push(nn);
      }
    }
    for (var i = 0; i < dbc.messages.length; i++) {
      var msg = dbc.messages[i];
      var merged = {
        idRaw: msg.idRaw,
        name: msg.name,
        dlc: msg.dlc,
        transmitter: msg.transmitter,
        signals: msg.signals,
        comment: msg.comment,
        id: msg.id,
        isExtended: msg.isExtended,
        sourceId: src.id,
        sourceName: src.name,
        key: src.id + ':' + msg.idRaw + ':' + msg.name
      };
      messages.push(merged);
      messageMap[merged.key] = merged;
    }
  }
  state.dbc = { messages: messages, nodeNames: nodeNames, messageMap: messageMap };
  return state.dbc;
}

/** @description 生成唯一 source id（同名文件可多次添加时追加 #2） */
export function makeDbcSourceId(fileName) {
  var base = String(fileName || 'dbc').replace(/[^\w.\-]+/g, '_');
  var id = base;
  var n = 2;
  while (state.dbcSources.some(function (s) { return s.id === id; })) {
    id = base + '#' + n;
    n++;
  }
  return id;
}

/** @description 更新顶部 DBC / 报文数量摘要 pill。 */
export function buildSummary() {
  if (!state.dbc || !state.dbcSources.length) {
    if (el.dbcSummaryPill) el.dbcSummaryPill.textContent = '未导入 DBC';
    if (el.messageSummaryPill) el.messageSummaryPill.textContent = '0 报文';
    return;
  }
  var nFiles = state.dbcSources.length;
  if (el.dbcSummaryPill) {
    el.dbcSummaryPill.textContent =
      nFiles + ' 文件 · ' + (state.dbc.nodeNames.length || 0) + ' 节点';
  }
  if (el.messageSummaryPill) {
    el.messageSummaryPill.textContent = state.dbc.messages.length + ' 报文';
  }
}
