/**
 * can_monitor.js - CAN 接收监控（独立页 can_monitor.html 与 can.html 右侧抽屉共用）
 */
import { showToast } from './common/toast.js?v=20260717x';
import { escapeHtml } from './common/utils.js';

var state = { fbTimer: null, fbInFlight: false };

var el = {};

// ── Click-to-Copy (feedback 值单元格) ──────────────────────
function copyFbVal(text, target) {
  if (!text || text === '--') return;
  function onDone() {
    target.classList.add('copied');
    setTimeout(function () { target.classList.remove('copied'); }, 1200);
    showToast('已复制', 'success');
  }
  function fallback() {
    var ta = document.createElement('textarea');
    ta.value = text;
    ta.style.cssText = 'position:fixed;left:-9999px;opacity:0';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); onDone(); } catch (_) {}
    document.body.removeChild(ta);
  }
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(onDone).catch(fallback);
  } else {
    fallback();
  }
}

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
  'toast', 'toastIco', 'toastMsg',
  'toolIp' /* can.html 共用 IP */
];

function cacheElements() {
  ids.forEach(function (id) { el[id] = document.getElementById(id); });
}

function getBaseUrl() {
  var ip = '';
  if (el.monIp && el.monIp.value) {
    ip = el.monIp.value.trim();
  } else if (el.toolIp && el.toolIp.value) {
    ip = el.toolIp.value.trim();
  } else {
    try {
      var saved = localStorage.getItem('dcu_esp_ip');
      if (saved && saved.trim()) ip = saved.trim();
    } catch (_) {}
  }
  if (!ip) ip = '192.168.31.127';
  return 'http://' + ip;
}

function setConnState(ok, text) {
  /* 嵌入 can.html 时顶栏已有 canConnPill，不再用 monConn 重复占位 */
  if (el.monConnPill) el.monConnPill.className = 'pill ' + (ok ? 'online' : 'offline');
  if (el.monConnTxt) el.monConnTxt.textContent = text;
  if (el.monConnIndicator) el.monConnIndicator.className = 'conn-indicator ' + (ok ? 'ok' : 'err');
  if (el.monConnIndicatorTxt) el.monConnIndicatorTxt.textContent = text;
  /* 轮询状态即监控连通性（与顶栏 canConn 去重时仍更新 fbPollStatus） */
  if (!el.monConnPill && el.fbPollStatus && !ok) {
    el.fbPollStatus.textContent = '轮询: 离线';
    el.fbPollStatus.className = 'pill offline';
  }
}

// ── 1Hz 轮询 /can/feedback ─────────────────────────────────
async function pollFeedback() {
  // 防重入：上一次 fetch 未完成则跳过本次 tick，避免并发请求叠加
  if (state.fbInFlight) return;
  state.fbInFlight = true;
  var ac = new AbortController();
  var timer = setTimeout(function () { ac.abort(); }, 3000);
  try {
    var resp = await fetch(getBaseUrl() + '/can/feedback', { signal: ac.signal });
    if (!resp.ok) return;
    var data = await resp.json();
    var channels = data.channels || [];
    var selectedCh = el.idsnapChannel ? parseInt(el.idsnapChannel.value, 10) : 0;
    if (!Number.isFinite(selectedCh)) selectedCh = 0;

    for (var i = 0; i < 3; i++) {
      var ch = channels[i];
      var card = document.getElementById('fbCh' + i);
      /* enabled 来自 GD32 HELLO/PING 身份；未知时默认按开启展示 */
      var enabled = !ch || ch.enabled === undefined || ch.enabled === null || !!ch.enabled;
      if (card) {
        card.classList.toggle('ch-disabled', !enabled);
        card.title = enabled ? '' : 'GD32 CONFIG_CAN_CH' + i + '_ENABLE=false（HELLO/PING 身份）';
      }
      if (!enabled) {
        renderChannelDisabled(i);
        continue;
      }
      if (!ch || !ch.valid) {
        renderChannelEmpty(i);
        continue;
      }
      renderChannel(i, ch);
    }

    var sel = channels[selectedCh];
    if (sel && sel.valid && sel.entries) {
      renderIdTable(sel.entries);
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
    if (e && e.name === 'AbortError') return;
    if (el.fbPollStatus) {
      el.fbPollStatus.textContent = '轮询: 离线';
      el.fbPollStatus.className = 'pill offline';
    }
    setConnState(false, '连接失败');
  } finally {
    clearTimeout(timer);
    state.fbInFlight = false;
  }
}

function renderChannel(i, ch) {
  var p = 'fb' + i;
  if (el[p + 'State']) {
    var hasIds = ch.count > 0;
    el[p + 'State'].textContent = hasIds ? '活跃 (' + ch.count + ' ID)' : '无帧（总线空闲）';
    el[p + 'State'].className = 'fb-val ' + (hasIds ? 'ok' : 'info');
  }
  if (el[p + 'Rx']) el[p + 'Rx'].textContent = ch.rx;
  if (el[p + 'Tx']) el[p + 'Tx'].textContent = ch.tx;
  if (el[p + 'Drop']) { el[p + 'Drop'].textContent = ch.drop; el[p + 'Drop'].className = 'fb-val' + (ch.drop > 0 ? ' warn' : ''); }
  if (el[p + 'Err']) { el[p + 'Err'].textContent = ch.err; el[p + 'Err'].className = 'fb-val' + (ch.err > 0 ? ' err' : ''); }
  if (el[p + 'Tec']) {
    el[p + 'Tec'].textContent = (ch.tec !== undefined && ch.tec !== null) ? ch.tec : '--';
    el[p + 'Tec'].className = 'fb-val' + (ch.tec > 0 ? ' warn' : '');
  }
  if (el[p + 'Rec']) {
    el[p + 'Rec'].textContent = (ch.rec !== undefined && ch.rec !== null) ? ch.rec : '--';
    el[p + 'Rec'].className = 'fb-val' + (ch.rec > 0 ? ' warn' : '');
  }
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
      el[p + 'Bus'].className = 'fb-val' + ((bs & 8) ? ' err' : (bs & (4 | 16)) ? ' warn' : '');
    }
  }
  if (el[p + 'Seq']) el[p + 'Seq'].textContent = ch.seq;
  if (el[p + 'Delta']) { el[p + 'Delta'].textContent = ch.count || 0; el[p + 'Delta'].className = 'fb-val'; }
  var entries = ch.entries;
  if (entries && entries.length) {
    var last = entries[entries.length - 1];
    if (el[p + 'LastId']) { el[p + 'LastId'].textContent = last.id; el[p + 'LastId'].className = 'fb-val mono'; }
    if (el[p + 'Dlc']) el[p + 'Dlc'].textContent = last.dlc;
    if (el[p + 'Flags']) el[p + 'Flags'].textContent = (last.fd ? 'FD' : '-') + ' / ' + (last.brs ? 'BRS' : '-') + ' / ' + (last.esi ? 'ESI' : '-');
    if (el[p + 'LastData']) { el[p + 'LastData'].textContent = last.data || '--'; el[p + 'LastData'].className = 'fb-val mono fb-data-cell'; }
  } else {
    if (el[p + 'LastId']) { el[p + 'LastId'].textContent = '--'; el[p + 'LastId'].className = 'fb-val mono'; }
    if (el[p + 'Dlc']) el[p + 'Dlc'].textContent = '--';
    if (el[p + 'Flags']) el[p + 'Flags'].textContent = '--';
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
  ['Rx', 'Tx', 'Drop', 'Err', 'Tec', 'Rec', 'Bus', 'Seq', 'Delta', 'Dlc', 'Flags'].forEach(function (f) {
    if (el[p + f]) { el[p + f].textContent = '--'; el[p + f].className = 'fb-val'; }
  });
  if (el[p + 'LastId']) { el[p + 'LastId'].textContent = '--'; el[p + 'LastId'].className = 'fb-val mono'; }
  if (el[p + 'LastData']) { el[p + 'LastData'].textContent = '--'; el[p + 'LastData'].className = 'fb-val mono fb-data-cell'; }
}

/** GD32 编译关闭该通道（APP PING feat can_chN=false） */
function renderChannelDisabled(i) {
  var p = 'fb' + i;
  if (el[p + 'State']) {
    el[p + 'State'].textContent = '固件未启用';
    el[p + 'State'].className = 'fb-val warn';
  }
  ['Rx', 'Tx', 'Drop', 'Err', 'Tec', 'Rec', 'Bus', 'Seq', 'Delta', 'Dlc', 'Flags'].forEach(function (f) {
    if (el[p + f]) { el[p + f].textContent = '--'; el[p + f].className = 'fb-val'; }
  });
  if (el[p + 'LastId']) { el[p + 'LastId'].textContent = '--'; el[p + 'LastId'].className = 'fb-val mono'; }
  if (el[p + 'LastData']) { el[p + 'LastData'].textContent = '--'; el[p + 'LastData'].className = 'fb-val mono fb-data-cell'; }
}

function renderIdTable(entries) {
  var tbody = el.idsnapBody;
  if (!tbody) return;
  if (!entries || !entries.length) {
    tbody.innerHTML = '<tr><td colspan="7" class="status-empty">无数据（CAN 未收到帧）</td></tr>';
    return;
  }
  var frag = document.createDocumentFragment();
  for (var i = 0; i < entries.length; i++) {
    var e = entries[i];
    var tr = document.createElement('tr');
    tr.innerHTML =
      '<td>' + (i + 1) + '</td>' +
      '<td class="mono">' + escapeHtml(e.id) + '</td>' +
      '<td>' + escapeHtml(e.dlc) + '</td>' +
      '<td class="' + (e.fd ? 'flag-on' : 'flag-off') + '">' + (e.fd ? 'Y' : '-') + '</td>' +
      '<td class="' + (e.brs ? 'flag-on' : 'flag-off') + '">' + (e.brs ? 'Y' : '-') + '</td>' +
      '<td class="' + (e.esi ? 'flag-on' : 'flag-off') + '">' + (e.esi ? 'Y' : '-') + '</td>' +
      '<td class="idsnap-data mono">' + escapeHtml(e.data || '--') + '</td>';
    frag.appendChild(tr);
  }
  tbody.innerHTML = '';
  tbody.appendChild(frag);
}

function renderIdTableEmpty() {
  var tbody = el.idsnapBody;
  if (tbody) tbody.innerHTML = '<tr><td colspan="7" class="status-empty">等待数据...</td></tr>';
  if (el.idsnapStatus) { el.idsnapStatus.textContent = '--'; el.idsnapStatus.className = 'pill'; }
}

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

/**
 * 初始化监控逻辑。可在 can_monitor.html（独立）或 can.html（抽屉）中调用。
 * 幂等：重复调用不会叠加多个 interval。
 */
export function initCanMonitor() {
  cacheElements();
  /* 无监控 DOM 时跳过（纯 can 发送页历史形态） */
  if (!el.fb0State && !el.idsnapBody) return;

  if (state.fbTimer) {
    clearInterval(state.fbTimer);
    state.fbTimer = null;
  }

  setConnState(false, '未连接');
  if (!state.bound) {
    state.bound = true;
    if (el.monTestBtn) el.monTestBtn.addEventListener('click', testConnection);
    if (el.monIp) el.monIp.addEventListener('change', testConnection);
    /* 嵌入 can.html 时 IP 与发送台共用 */
    if (el.toolIp && !el.monIp) {
      el.toolIp.addEventListener('change', function () {
        pollFeedback();
      });
    }
    if (el.btnModeAllPass) el.btnModeAllPass.addEventListener('click', function () { setCanMode(0); });
    if (el.btnModeFilter) el.btnModeFilter.addEventListener('click', function () { setCanMode(1); });
    if (el.btnModeBridge) el.btnModeBridge.addEventListener('click', function () { setCanMode(2); });
    if (el.idsnapChannel) el.idsnapChannel.addEventListener('change', pollFeedback);

    /* feedback 值 click-to-copy：在 grid 容器上事件代理 */
    document.querySelectorAll('.feedback-grid').forEach(function (grid) {
      grid.addEventListener('click', function (e) {
        var target = e.target.closest('.fb-val');
        if (!target) return;
        copyFbVal(target.textContent.trim(), target);
      });
    });

    /* 通道卡：默认折叠详情，点标题展开/收起 */
    document.querySelectorAll('.feedback-channel .feedback-ch-head').forEach(function (head) {
      var toggle = function (e) {
        e.preventDefault();
        var card = head.closest('.feedback-channel');
        if (card) card.classList.toggle('collapsed');
      };
      head.addEventListener('click', toggle);
      head.addEventListener('keydown', function (e) {
        if (e.key === 'Enter' || e.key === ' ') toggle(e);
      });
    });
  }
  setTimeout(testConnection, 150);
  pollFeedback();
  state.fbTimer = setInterval(pollFeedback, 1000);
}

/* 独立页 can_monitor.html：有 monIp 时自启；嵌入 can.html 由 can.js 调用 initCanMonitor() */
if (document.getElementById('monIp')) {
  initCanMonitor();
}
