(function () {
  'use strict';

  var state = {
    dbcFile: null,
    dbcText: '',
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

  var el = {};

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

  function showToast(message, type) {
    type = type || 'success';
    if (!el.toast) return;
    el.toast.className = 'toast ' + (type === 'success' ? 'ok' : type === 'error' ? 'err' : 'warn') + ' show';
    el.toastIco.textContent = type === 'success' ? '\u2713' : type === 'error' ? '\u2715' : '\u26A0';
    el.toastMsg.textContent = message;
    clearTimeout(showToast._timer);
    showToast._timer = setTimeout(function () {
      el.toast.classList.remove('show');
    }, 3000);
  }

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes < 0) return '--';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(bytes < 10 * 1024 ? 1 : 0) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
  }

  function formatTime(date) {
    var d = date instanceof Date ? date : new Date();
    var hh = String(d.getHours()).padStart(2, '0');
    var mm = String(d.getMinutes()).padStart(2, '0');
    var ss = String(d.getSeconds()).padStart(2, '0');
    return hh + ':' + mm + ':' + ss;
  }

  function sanitizeHtml(text) {
    return String(text == null ? '' : text)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function getBaseUrl() {
    var ip = (el.toolIp && el.toolIp.value ? el.toolIp.value.trim() : '') || '192.168.31.127';
    return 'http://' + ip;
  }

  function parseNumericId(input) {
    var raw = String(input == null ? '' : input).trim();
    if (!raw) return NaN;
    if (/^0x/i.test(raw)) return parseInt(raw, 16);
    if (/^[0-9]+$/.test(raw)) return parseInt(raw, 10);
    if (/^[0-9a-fA-F]+h$/i.test(raw)) return parseInt(raw.slice(0, -1), 16);
    if (/^[0-9a-fA-F]+$/.test(raw)) return parseInt(raw, 16);
    return NaN;
  }

  function padHex(value, width) {
    return value.toString(16).toUpperCase().padStart(width, '0');
  }

  function bytesToHex(bytes) {
    if (!bytes || !bytes.length) return '';
    var parts = [];
    for (var i = 0; i < bytes.length; i++) {
      parts.push(padHex(bytes[i], 2));
    }
    return parts.join('');
  }

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

  function formatUptime(sec) {
    if (!Number.isFinite(sec) || sec < 0) return '--';
    var s = Math.floor(sec);
    var days = Math.floor(s / 86400);
    s %= 86400;
    var hours = Math.floor(s / 3600);
    s %= 3600;
    var mins = Math.floor(s / 60);
    s %= 60;
    if (days > 0) return days + 'd ' + hours + 'h';
    if (hours > 0) return hours + 'h ' + mins + 'm';
    if (mins > 0) return mins + 'm ' + s + 's';
    return s + 's';
  }

  function normalizeWhitespace(text) {
    return String(text || '').replace(/\r/g, '').trim();
  }

  function parseDbc(text) {
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

  function findSignal(message, signalName) {
    for (var i = 0; i < message.signals.length; i++) {
      if (message.signals[i].name === signalName) return message.signals[i];
    }
    return null;
  }

  function parseMultiplexToken(token) {
    if (!token) return { role: null, value: null };
    if (token === 'M') return { role: 'mux', value: null };
    var match = token.match(/^m(-?\d+)$/);
    if (match) {
      return { role: 'value', value: parseInt(match[1], 10) };
    }
    return { role: null, value: null };
  }

  function buildSummary() {
    if (!state.dbc) {
      if (el.dbcSummaryPill) el.dbcSummaryPill.textContent = '未导入 DBC';
      if (el.messageSummaryPill) el.messageSummaryPill.textContent = '0 报文';
      return;
    }
    if (el.dbcSummaryPill) {
      el.dbcSummaryPill.textContent = (state.dbc.nodeNames.length || 0) + ' 节点';
    }
    if (el.messageSummaryPill) {
      el.messageSummaryPill.textContent = state.dbc.messages.length + ' 报文';
    }
  }

  function updateDbcFileInfo(file, dbc) {
    if (!el.dbcFileInfo) return;
    if (!file || !dbc) {
      el.dbcFileInfo.classList.remove('show');
      el.dbcFileName.textContent = '-';
      el.dbcMessageCount.textContent = '0';
      el.dbcSignalCount.textContent = '0';
      el.dbcNodeCount.textContent = '0';
      el.dbcFileSize.textContent = '0 B';
      return;
    }
    var signalCount = dbc.messages.reduce(function (sum, msg) { return sum + msg.signals.length; }, 0);
    el.dbcFileInfo.classList.add('show');
    el.dbcFileName.textContent = file.name || '-';
    el.dbcMessageCount.textContent = String(dbc.messages.length);
    el.dbcSignalCount.textContent = String(signalCount);
    el.dbcNodeCount.textContent = String(dbc.nodeNames.length);
    el.dbcFileSize.textContent = formatBytes(file.size || 0);
  }

  function filterMessages() {
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

  function renderNodeFilterList() {
    if (!el.nodeFilterList) return;
    var txs = collectTransmitters();
    var kw = (el.nodeFilterSearch && el.nodeFilterSearch.value || '').trim().toLowerCase();
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
        '<input type="checkbox" data-node="' + sanitizeHtml(name) + '"' + checked + '>' +
        '<span class="can-node-filter-name">' + sanitizeHtml(name) + '</span>' +
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
      });
    }
  }

  function recomputeTransmitterFilterEmpty() {
    var any = false;
    for (var k in state.transmitterFilter) {
      if (state.transmitterFilter[k]) { any = true; break; }
    }
    state.transmitterFilterEmpty = !any;
  }

  function updateNodeFilterLabel() {
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

  function setNodeFilterPanelOpen(open) {
    if (!el.nodeFilterPanel || !el.nodeFilterBtn) return;
    if (open) {
      renderNodeFilterList();
      el.nodeFilterPanel.hidden = false;
      el.nodeFilterBtn.setAttribute('aria-expanded', 'true');
    } else {
      el.nodeFilterPanel.hidden = true;
      el.nodeFilterBtn.setAttribute('aria-expanded', 'false');
    }
  }

  function bindNodeFilterEvents() {
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
    });
    if (el.nodeFilterNoneBtn) el.nodeFilterNoneBtn.addEventListener('click', function () {
      state.transmitterFilter = Object.create(null);
      state.transmitterFilterEmpty = true;
      renderNodeFilterList();
      filterMessages();
    });
  }

  function renderMessageList() {
    if (!el.messageList) return;
    if (!state.dbc) {
      el.messageList.innerHTML = '<div class="status-empty">等待导入 DBC...</div>';
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
        '<div class="message-row' + active + '" data-key="' + sanitizeHtml(msg.key) + '" role="button" tabindex="0">' +
        '<span class="message-col message-col-check"><input type="checkbox" class="message-check" data-key="' + sanitizeHtml(msg.key) + '"' + checked + '></span>' +
        '<span class="message-col message-col-toggle"><button type="button" class="message-toggle' + (expanded ? ' expanded' : '') + '" data-key="' + sanitizeHtml(msg.key) + '" aria-label="展开/收起信号">' + caret + '</button></span>' +
        '<span class="message-col message-col-index">' + (index + 1) + '</span>' +
        '<span class="message-col message-col-name" title="' + sanitizeHtml(msg.name) + '">' + sanitizeHtml(msg.name) + '</span>' +
        '<span class="message-col message-col-id">' + sanitizeHtml(idHex) + '</span>' +
        '<span class="message-col message-col-channel">CH' + sanitizeHtml(channelTxt) + '</span>' +
        '<span class="message-col message-col-type">' + sanitizeHtml(msg.isExtended ? '扩展' : '标准') + '</span>' +
        '<span class="message-col message-col-dlc">' + sanitizeHtml(msg.dlc) + '</span>' +
        '<span class="message-col message-col-signal">' + sanitizeHtml(msg.signals.length) + '</span>' +
        '<span class="message-col message-col-node" title="' + sanitizeHtml(msg.transmitter || 'Vector__XXX') + '">' + sanitizeHtml(msg.transmitter || 'Vector__XXX') + '</span>' +
        '<span class="message-col message-col-comment" title="' + sanitizeHtml(msg.comment || '') + '">' + sanitizeHtml(comment || '-') + '</span>' +
        '</div>';
      var expandHtml = '<div class="message-row-expand" data-key="' + sanitizeHtml(msg.key) + '"' + (expanded ? '' : ' hidden') + '>' +
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
    var msg = null;
    for (var i = 0; i < state.dbc.messages.length; i++) {
      if (state.dbc.messages[i].key === key) { msg = state.dbc.messages[i]; break; }
    }
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
  }

  function renderInlineSignalGrid(message, host) {
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

  function updateBatchUi() {
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
  function selectMessage(key) {
    if (!state.dbc) return;
    var msg = null;
    for (var i = 0; i < state.dbc.messages.length; i++) {
      if (state.dbc.messages[i].key === key) {
        msg = state.dbc.messages[i];
        break;
      }
    }
    if (!msg) return;
    state.selectedMessageKey = key;
    ensureMessageDefaults(msg);
    renderMessageList();
    renderSelectedMessage();
    updateEncodedPreview();
  }

  function ensureMessageDefaults(message) {
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

  function getSignalValueMap(message) {
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

  function getSelectedMessage() {
    if (!state.dbc || !state.selectedMessageKey) return null;
    for (var i = 0; i < state.dbc.messages.length; i++) {
      if (state.dbc.messages[i].key === state.selectedMessageKey) return state.dbc.messages[i];
    }
    return null;
  }

  function getMultiplexerSignal(message) {
    if (!message) return null;
    for (var i = 0; i < message.signals.length; i++) {
      if (message.signals[i].multiplexRole === 'mux') return message.signals[i];
    }
    return null;
  }

  function getActiveSignals(message) {
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

  /**
   * @description 旧版“顶部独立信号编辑面板”的兼容 shim。
   *              该面板已移除，信号编辑现在通过报文行展开完成；
   *              仍被 selectMessage、handleDbcFile、clearDbc、handleSignalInputEvent 调用，
   *              保留空函数以避免修改所有调用方。
   */
  function renderSelectedMessage() {}

  function renderSignalRow(message, signal, index) {
    var bag = getSignalValueMap(message);
    var enumKeys = Object.keys(signal.enumMap || {});
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
    if (enumKeys.length) {
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
      (enumKeys.length ? '<small>' + sanitizeHtml(enumKeys.slice(0, 3).map(function (key) { return key + ':' + signal.enumMap[key]; }).join(' / ')) + (enumKeys.length > 3 ? ' ...' : '') + '</small>' : '<small>' + sanitizeHtml(factorText) + '</small>') +
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
    var message = null;
    for (var i = 0; i < state.dbc.messages.length; i++) {
      if (state.dbc.messages[i].key === key) { message = state.dbc.messages[i]; break; }
    }
    if (!message) return;
    setSignalValue(message, name, input.value);
    var signal = findSignal(message, name);
    if (signal && signal.multiplexRole === 'mux') {
      if (key === state.selectedMessageKey) renderSelectedMessage();
      var inlineGrid = document.querySelector('.message-row-expand[data-key="' + cssEscape(key) + '"] .signal-table-host');
      if (inlineGrid) renderInlineSignalGrid(message, inlineGrid);
    }
    if (key === state.selectedMessageKey) updateEncodedPreview();
  }

  function cssEscape(value) {
    if (window.CSS && CSS.escape) return CSS.escape(value);
    return String(value).replace(/[^a-zA-Z0-9_-]/g, function (ch) { return '\\\\' + ch; });
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

  function updateEncodedPreview() {
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

  function encodeMessageData(message, signalValues) {
    var bytes = new Uint8Array(message.dlc);
    var activeSignals = getActiveSignals(message);
    for (var i = 0; i < activeSignals.length; i++) {
      var signal = activeSignals[i];
      var parsed = parseSignalInput(signal, signalValues[signal.name]);
      if (signal.valueType === 'float' || signal.valueType === 'double') {
        writeFloatSignal(bytes, signal, parsed.numeric);
      } else {
        writeIntegerSignal(bytes, signal, parsed);
      }
    }
    return bytes;
  }

  function parseSignalInput(signal, inputValue) {
    var value = inputValue;
    if (value === '' || value == null) value = getDefaultSignalValue(signal);
    var num = Number(value);
    if (!Number.isFinite(num)) throw new Error(signal.name + ' 不是合法数字');
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
    var buffer = new ArrayBuffer(signal.length / 8);
    var view = new DataView(buffer);
    if (signal.length === 32) view.setFloat32(0, physicalValue, true);
    else view.setFloat64(0, physicalValue, true);
    var rawBytes = new Uint8Array(buffer);
    for (var i = 0; i < rawBytes.length; i++) {
      bytes[(signal.startBit / 8) + i] = rawBytes[i];
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
      throw new Error('信号写入越界，bit=' + bitIndex);
    }
    if (bitValue) bytes[byteIndex] |= (1 << bitInByte);
    else bytes[byteIndex] &= ~(1 << bitInByte);
  }

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

  function addHistoryEntry(entry) {
    state.history.unshift(entry);
    if (state.history.length > 20) state.history.length = 20;
    renderHistory();
  }

  function renderHistory() {
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

  async function sendCheckedMessages() {
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
  async function sendSelectedMessage() {
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

  function refreshManualHint() {
    try {
      var dataHex = validateManualData();
      el.manualHint.className = 'helper-note ok';
      el.manualHint.innerHTML = '原始 HEX 合法，当前长度 <b>' + (dataHex.length / 2) + '</b> 字节。';
    } catch (err) {
      el.manualHint.className = 'helper-note err';
      el.manualHint.innerHTML = sanitizeHtml(err.message);
    }
  }

  async function sendManualFrame() {
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

  function resetManualFields() {
    el.manualChannel.value = el.defaultChannel.value;
    el.manualId.value = '0x18DA00F1';
    el.manualDlc.value = '8';
    el.manualData.value = '0102030405060708';
    if (el.manualResp) el.manualResp.style.display = 'none';
    refreshManualHint();
  }

  function loadPreviewToManual() {
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

  async function readDbcText(file) {
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

  function handleDbcError(err) {
    if (el.dbcHint) {
      el.dbcHint.className = 'helper-note err';
      el.dbcHint.innerHTML = 'DBC 解析失败：' + sanitizeHtml(err.message);
    }
    showToast('DBC 解析失败: ' + err.message, 'error');
  }



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

  var CAN_SEND_ERR_DESC = {
    '-1': '参数错误（通道/ID/DLC 无效）',
    '-2': 'SPI 通信失败（ESP32 与 GD32 通信异常）',
    '-3': 'GD32 CAN 总线拒绝',
  };

  var GD32_ERR_DESC = {
    0xE0: 'payload 过短',
    0xE1: '通道号非法',
    0xE2: 'BUS_OFF（总线离线，检查接线/终端电阻/对端上电）',
    0xE3: 'TEC 过高（错误计数 >96，对端 ACK 失败率高）',
    0xE4: '邮箱满（3 个硬件邮箱全 PENDING，对端不 ACK）',
    0xE5: '参数无效（通道未启用/DLC 越界）',
    0xEE: 'SPI 帧解析失败（GD32 收到的帧 CRC/SOF 校验失败，通常是 SPI 字节丢失或 CS 时序冲突）'
  };

  function buildSendResponseText(result) {
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

  init();
})();
