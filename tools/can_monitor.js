(function () {
  'use strict';

  var state = { fbTimer: null };

  var el = {};

  var ids = [
    'monIp', 'monTestBtn', 'monConnPill', 'monConnTxt',
    'monConnIndicator', 'monConnIndicatorTxt', 'monHelper',
    'fbModePill', 'fbPollStatus',
    'fb0State', 'fb0Rx', 'fb0Tx', 'fb0Drop', 'fb0Err', 'fb0Tec', 'fb0Rec', 'fb0Bus', 'fb0Seq', 'fb0Delta',
    'fb0LastId', 'fb0Dlc', 'fb0Flags', 'fb0LastData',
    'fb1State', 'fb1Rx', 'fb1Tx', 'fb1Drop', 'fb1Err', 'fb1Tec', 'fb1Rec', 'fb1Bus', 'fb1Seq', 'fb1Delta',
    'fb1LastId', 'fb1Dlc', 'fb1Flags', 'fb1LastData',
    'fb2State', 'fb2Rx', 'fb2Tx', 'fb2Drop', 'fb2Err', 'fb2Tec', 'fb2Rec', 'fb2Bus', 'fb2Seq', 'fb2Delta',
    'fb2LastId', 'fb2Dlc', 'fb2Flags', 'fb2LastData',
    'btnModeAllPass', 'btnModeFilter', 'btnModeBridge',
    'idsnapChannel', 'idsnapStatus', 'idsnapBody',
    'toast', 'toastIco', 'toastMsg'
  ];

  function cacheElements() {
    ids.forEach(function (id) { el[id] = document.getElementById(id); });
  }

  function showToast(msg, type) {
    type = type || 'success';
    if (!el.toast) return;
    el.toast.className = 'toast ' + (type === 'success' ? 'ok' : type === 'error' ? 'err' : 'warn') + ' show';
    el.toastIco.textContent = type === 'success' ? '\u2713' : type === 'error' ? '\u2715' : '\u26A0';
    el.toastMsg.textContent = msg;
    clearTimeout(showToast._t);
    showToast._t = setTimeout(function () { el.toast.classList.remove('show'); }, 3000);
  }

  function sanitize(text) {
    return String(text == null ? '' : text)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function getBaseUrl() {
    var ip = (el.monIp && el.monIp.value ? el.monIp.value.trim() : '') || '192.168.31.127';
    return 'http://' + ip;
  }

  function setConnState(ok, text) {
    if (el.monConnPill) el.monConnPill.className = 'pill ' + (ok ? 'online' : 'offline');
    if (el.monConnTxt) el.monConnTxt.textContent = text;
    if (el.monConnIndicator) el.monConnIndicator.className = 'conn-indicator ' + (ok ? 'ok' : 'err');
    if (el.monConnIndicatorTxt) el.monConnIndicatorTxt.textContent = text;
  }

  // ── 1Hz 轮询 /can/feedback ─────────────────────────────────
  async function pollFeedback() {
    try {
      var resp = await fetch(getBaseUrl() + '/can/feedback', { signal: AbortSignal.timeout(3000) });
      if (!resp.ok) return;
      var data = await resp.json();
      var channels = data.channels || [];
      var selectedCh = el.idsnapChannel ? parseInt(el.idsnapChannel.value) : 0;

      for (var i = 0; i < 3; i++) {
        var ch = channels[i];
        if (!ch || !ch.valid) {
          renderChannelEmpty(i);
          continue;
        }
        renderChannel(i, ch);
      }

      // 渲染选中通道的 ID 快照表
      var sel = channels[selectedCh];
      if (sel && sel.valid && sel.entries) {
        renderIdTable(sel.entries, sel.count || sel.entries.length);
        if (el.idsnapStatus) {
          el.idsnapStatus.textContent = (sel.entries.length) + ' 个 ID';
          el.idsnapStatus.className = 'pill online';
        }
      } else {
        renderIdTableEmpty();
      }

      if (el.fbPollStatus) {
        el.fbPollStatus.textContent = '轮询: 正常';
        el.fbPollStatus.className = 'pill online';
      }
      setConnState(true, '已连接');
    } catch (e) {
      if (el.fbPollStatus) {
        el.fbPollStatus.textContent = '轮询: 离线';
        el.fbPollStatus.className = 'pill offline';
      }
      setConnState(false, '连接失败');
    }
  }

  function renderChannel(i, ch) {
    var p = 'fb' + i;
    if (el[p + 'State']) {
      var hasIds = ch.count > 0;
      el[p + 'State'].textContent = hasIds ? '活跃 (' + ch.count + ' ID)' : '无帧（总线空闲）';
      el[p + 'State'].className = 'fb-val ' + (hasIds ? 'ok' : 'info');
    }
    if (el[p + 'Rx'])   el[p + 'Rx'].textContent = ch.rx;
    if (el[p + 'Tx'])   el[p + 'Tx'].textContent = ch.tx;
    if (el[p + 'Drop']) { el[p + 'Drop'].textContent = ch.drop; el[p + 'Drop'].className = 'fb-val' + (ch.drop > 0 ? ' warn' : ''); }
    if (el[p + 'Err'])  { el[p + 'Err'].textContent = ch.err;  el[p + 'Err'].className = 'fb-val' + (ch.err > 0 ? ' err' : ''); }
    if (el[p + 'Tec']) { el[p + 'Tec'].textContent = (ch.tec !== undefined && ch.tec !== null) ? ch.tec : '--'; el[p + 'Tec'].className = 'fb-val' + (ch.tec > 0 ? ' warn' : ''); }
    if (el[p + 'Rec']) { el[p + 'Rec'].textContent = (ch.rec !== undefined && ch.rec !== null) ? ch.rec : '--'; el[p + 'Rec'].className = 'fb-val' + (ch.rec > 0 ? ' warn' : ''); }
    if (el[p + 'Bus']) {
      if (ch.bus_state === undefined || ch.bus_state === null) {
        el[p + 'Bus'].textContent = '未知';
        el[p + 'Bus'].className = 'fb-val';
      } else {
        var bs = ch.bus_state;
        var parts = [];
        if (bs & 1) parts.push('INIT');
        if (bs & 2) parts.push('SLEEP');
        if (bs & 4) parts.push('ERR_PASSIVE');
        if (bs & 8) parts.push('BUS_OFF');
        if (bs & 16) parts.push('ERR_WARN');
        el[p + 'Bus'].textContent = (bs === 0) ? '正常' : (parts.length ? parts.join(' ') : '正常');
        el[p + 'Bus'].className = 'fb-val' + ((bs & 8) ? ' err' : (bs & (4|16)) ? ' warn' : '');
      }
    }
    if (el[p + 'Seq'])  el[p + 'Seq'].textContent = ch.seq;
    if (el[p + 'Delta']) { el[p + 'Delta'].textContent = ch.count || 0; el[p + 'Delta'].className = 'fb-val'; }
    // 最新帧摘要
    var entries = ch.entries;
    if (entries && entries.length) {
      var last = entries[entries.length - 1];
      if (el[p + 'LastId'])   { el[p + 'LastId'].textContent = last.id;   el[p + 'LastId'].className = 'fb-val mono'; }
      if (el[p + 'Dlc'])      el[p + 'Dlc'].textContent = last.dlc;
      if (el[p + 'Flags'])    el[p + 'Flags'].textContent = (last.fd ? 'FD' : '-') + ' / ' + (last.brs ? 'BRS' : '-') + ' / ' + (last.esi ? 'ESI' : '-');
      if (el[p + 'LastData']) { el[p + 'LastData'].textContent = last.data || '--'; el[p + 'LastData'].className = 'fb-val mono fb-data-cell'; }
    } else {
      if (el[p + 'LastId'])   { el[p + 'LastId'].textContent = '--';   el[p + 'LastId'].className = 'fb-val mono'; }
      if (el[p + 'Dlc'])      el[p + 'Dlc'].textContent = '--';
      if (el[p + 'Flags'])    el[p + 'Flags'].textContent = '--';
      if (el[p + 'LastData']) { el[p + 'LastData'].textContent = '--'; el[p + 'LastData'].className = 'fb-val mono fb-data-cell'; }
    }
    if (i === 0 && el.fbModePill) {
      var modeText = ch.mode === 2 ? '桥接(CAN0→1/2)' : (ch.mode === 1 ? '过滤' : '全收');
      el.fbModePill.textContent = '模式: ' + modeText;
    }
  }

  function renderChannelEmpty(i) {
    var p = 'fb' + i;
    if (el[p + 'State']) { el[p + 'State'].textContent = 'SPI 离线'; el[p + 'State'].className = 'fb-val err'; }
    ['Rx','Tx','Drop','Err','Tec','Rec','Bus','Seq','Delta','Dlc','Flags'].forEach(function (f) {
      if (el[p + f]) { el[p + f].textContent = '--'; el[p + f].className = 'fb-val'; }
    });
    if (el[p + 'LastId'])   { el[p + 'LastId'].textContent = '--';   el[p + 'LastId'].className = 'fb-val mono'; }
    if (el[p + 'LastData']) { el[p + 'LastData'].textContent = '--'; el[p + 'LastData'].className = 'fb-val mono fb-data-cell'; }
  }
  function renderIdTable(entries, count) {
    var tbody = el.idsnapBody;
    if (!tbody) return;
    if (!entries || !entries.length) {
      tbody.innerHTML = '<tr><td colspan="7" class="status-empty">无数据（CAN 未收到帧）</td></tr>';
      return;
    }
    var html = '';
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      html += '<tr>';
      html += '<td>' + (i + 1) + '</td>';
      html += '<td class="mono">' + sanitize(e.id) + '</td>';
      html += '<td>' + e.dlc + '</td>';
      html += '<td class="' + (e.fd ? 'flag-on' : 'flag-off') + '">' + (e.fd ? 'Y' : '-') + '</td>';
      html += '<td class="' + (e.brs ? 'flag-on' : 'flag-off') + '">' + (e.brs ? 'Y' : '-') + '</td>';
      html += '<td class="' + (e.esi ? 'flag-on' : 'flag-off') + '">' + (e.esi ? 'Y' : '-') + '</td>';
      html += '<td class="idsnap-data mono">' + sanitize(e.data || '--') + '</td>';
      html += '</tr>';
    }
    tbody.innerHTML = html;
  }

  function renderIdTableEmpty() {
    var tbody = el.idsnapBody;
    if (tbody) tbody.innerHTML = '<tr><td colspan="7" class="status-empty">等待数据...</td></tr>';
    if (el.idsnapStatus) { el.idsnapStatus.textContent = '--'; el.idsnapStatus.className = 'pill'; }
  }

  // ── 连接测试 ─────────────────────────────────────────────
  async function testConnection() {
    try {
      var resp = await fetch(getBaseUrl() + '/can/feedback', { signal: AbortSignal.timeout(3000) });
      if (!resp.ok) { setConnState(false, 'HTTP ' + resp.status); return; }
      setConnState(true, '已连接');
      showToast('ESP32 已连接', 'success');
    } catch (e) {
      setConnState(false, '连接失败');
      showToast('连接失败: ' + e.message, 'error');
    }
  }

  // ── 模式切换 ─────────────────────────────────────────────
  async function setCanMode(mode) {
    try {
      var resp = await fetch(getBaseUrl() + '/can/mode', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ mode: mode }),
        signal: AbortSignal.timeout(3000)
      });
      var data = await resp.json();
      if (data.ok) {
        showToast('CAN 模式已切换: ' + (data.actual === 2 ? '桥接(CAN0→1/2)' : (data.actual === 1 ? '过滤' : '全收')), 'success');
      } else {
        showToast('模式切换失败', 'error');
      }
    } catch (e) {
      showToast('模式切换失败: ' + e.message, 'error');
    }
  }

  // ── 初始化 ───────────────────────────────────────────────
  function init() {
    cacheElements();
    setConnState(false, '未连接');
    if (el.monTestBtn) el.monTestBtn.addEventListener('click', testConnection);
    if (el.monIp) el.monIp.addEventListener('change', testConnection);
    if (el.btnModeAllPass) el.btnModeAllPass.addEventListener('click', function () { setCanMode(0); });
    if (el.btnModeFilter)  el.btnModeFilter.addEventListener('click', function () { setCanMode(1); });
    if (el.btnModeBridge)  el.btnModeBridge.addEventListener('click', function () { setCanMode(2); });
    if (el.idsnapChannel) el.idsnapChannel.addEventListener('change', pollFeedback);
    setTimeout(testConnection, 150);
    pollFeedback();
    state.fbTimer = setInterval(pollFeedback, 1000);
  }

  init();
})();
