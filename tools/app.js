/**
 * app.js - ESP32 OTA 中继控制台主脚本（WiFi OTA 通道）
 *
 * 职责：
 * - 固件文件选择 / HEX→BIN 转换 / SHA-256 计算
 * - WiFi OTA 上传流程（ESP32 本机 + GD32 中继）
 * - 系统状态轮询（/status + /logs）
 * - CAN/SPI 调试工具
 * - UI 状态管理（进度条、阶段流程、Toast、日志）
 *
 * 依赖：index.html 中定义的 DOM 元素、style.css 样式
 */
// ==================== 全局状态 ====================
let esp32File = null;
let gd32File = null;
let esp32FileHandle = null;  // FileSystemFileHandle for hot reload
let gd32FileHandle = null;   // FileSystemFileHandle for hot reload
let uploadProgress = 0;
let isUploading = false;
let logFilter = 'all';
let logEntries = [];
let lastLogHash = null;
let autoScroll = true;
let logSocket = null;
let pageVisible = !document.hidden;   // 页面可见性：切到后台/最小化/切标签会变 false
let latestSystemStatus = null;
let isEsp32Rebooting = false;
let rebootStartTime = 0;
let pollingPausedForUpload = false;
let activeXhr = null;           // 当前上传 XHR，用于 abort
let uploadBtnOrigText = '';     // 上传按钮原始文字
let activeUploadChip = null;    // 当前上传目标芯片 ('esp32' | 'gd32' | null)

// ==================== 折叠面板 ====================
function toggleCollapse(headEl) {
  headEl.closest('.card').classList.toggle('collapsed');
}
// ==================== 刷写进度 ====================
var activePhase = null;  // 'esp32Ota' | 'gd32Ota' | null
var currentOtaTarget = null;  // 'esp32' | 'gd32' | null — 当前 OTA 目标，决定显示哪套阶段
// 切换 OTA 阶段面板显示
function switchStageFlow(target) {
  currentOtaTarget = target;
  var esp32El = document.getElementById('esp32Stages');
  var gd32El = document.getElementById('gd32Stages');
  if (target === 'esp32') {
    if (esp32El) esp32El.style.display = '';
    if (gd32El) gd32El.style.display = 'none';
  } else {
    if (esp32El) esp32El.style.display = 'none';
    if (gd32El) gd32El.style.display = '';
  }
  // 重置 ESP32 阶段节点
  ['es1','es2','es3','es4','es5','es6'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
  ['esd1','esd2','esd3','esd4','esd5','esd6'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.textContent = '';
  });
  // 重置 GD32 阶段节点
  ['s1','s2','s3','s4','s5','s6'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
  ['sd1','sd2','sd3','sd4','sd5','sd6'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.textContent = '';
  });
}
// ESP32 OTA 阶段状态: 0=待机, 1=上传中, 2=OTA写入, 3=SHA-256校验, 4=应用固件, 5=重启中, 6=启动完成
var esp32OtaPhase = 0;
var esp32PhaseLabels = ['', '上传中', '写入中', '校验中', '应用中', '重启中', '已完成'];
function updateEsp32StagesFromPhase(phase) {
  esp32OtaPhase = phase;
  var ids = ['es1', 'es2', 'es3', 'es4', 'es5', 'es6'];
  var detailIds = ['esd1', 'esd2', 'esd3', 'esd4', 'esd5', 'esd6'];
  var all = ids.map(function(id) { return document.getElementById(id); });
  var details = detailIds.map(function(id) { return document.getElementById(id); });
  all.forEach(function (s) { if (s) s.className = 'stage-node'; });
  details.forEach(function (d) { if (d) d.textContent = ''; });
  var statusBar = document.getElementById('stageStatusBar');
  var statusText = document.getElementById('stageStatusText');
  if (phase <= 0) {
    if (statusBar) statusBar.className = 'stage-status-bar';
    if (statusText) statusText.textContent = '待机';
    return;
  }
  for (var i = 0; i < phase - 1 && i < 6; i++) {
    if (all[i]) all[i].className = 'stage-node done';
  }
  if (phase >= 1 && phase <= 6) {
    if (all[phase - 1]) all[phase - 1].className = 'stage-node live';
    if (details[phase - 1]) details[phase - 1].textContent = esp32PhaseLabels[phase] || '';
  }
  if (statusBar && statusText) {
    if (phase >= 6) {
      statusBar.className = 'stage-status-bar ok';
      statusText.textContent = 'ESP32 刷写完成';
    } else {
      statusBar.className = 'stage-status-bar active';
      statusText.textContent = (esp32PhaseLabels[phase] || '进行中') + ' (' + phase + '/6)';
    }
  }
}
function updateEsp32StagesError() {
  var ids = ['es1', 'es2', 'es3', 'es4', 'es5', 'es6'];
  var all = ids.map(function(id) { return document.getElementById(id); });
  all.forEach(function (s) { if (s) s.className = 'stage-node'; });
  var statusBar = document.getElementById('stageStatusBar');
  var statusText = document.getElementById('stageStatusText');
  if (statusBar) statusBar.className = 'stage-status-bar err';
  if (statusText) statusText.textContent = '校验/应用失败';
}

/**
 * @description OTA 失败后的统一清理函数。
 * 所有上传失败、校验失败、中继触发失败等异常路径都应调用此函数，
 * 确保 UI 状态完整重置，不会残留"上传中"或中间态。
 * @param {string} reason 失败原因描述
 */
function cleanupAfterOtaFailure(reason) {
  // 1. 重置上传锁
  isUploading = false;

  // 2. 中止正在进行的 XHR（如有）
  if (activeXhr) {
    try { activeXhr.abort(); } catch (_) {}
    activeXhr = null;
  }

  // 3. 标记阶段指示器为错误态
  if (currentOtaTarget === 'esp32') {
    updateEsp32StagesError();
  } else if (currentOtaTarget === 'gd32') {
    updateStagesFromRelayState(255);
  }

  // 4. 状态栏标红
  var sb = document.getElementById('stageStatusBar');
  var st = document.getElementById('stageStatusText');
  if (sb) sb.className = 'stage-status-bar err';
  if (st) st.textContent = reason || '失败';

  // 5. 进度条显示失败信息
  setProgress(0, reason || '刷写失败');

  // 6. 重置活跃阶段
  setActivePhase(null);

  // 7. 清除 OTA 目标（下次操作时重新设置）
  currentOtaTarget = null;

  // 8. 隐藏校验面板
  var verifyInfo = document.getElementById('stageVerifyInfo');
  if (verifyInfo) verifyInfo.style.display = 'none';

  // 9. 恢复上传按钮（基于实际上传的芯片，而非当前 UI 选中的标签）
  var chip = activeUploadChip || window.flashTarget || 'esp32';
  var btnId = (chip === 'gd32') ? 'uploadGd32Btn' : 'uploadEsp32Btn';
  var btn = document.getElementById(btnId);
  if (btn && uploadBtnOrigText) {
    btn.disabled = false;
    btn.textContent = uploadBtnOrigText;
  }
  activeUploadChip = null;

  updateUploadButtonLabel();

  // 10. 恢复轮询
  resumePollingAfterUpload();

  // 11. 日志
  addLog('E', 'OTA', '刷写流程已中止: ' + (reason || '未知原因'));
}

function showVerifyActions(type) {
  // 已移除确认/重试/取消按钮：SHA-256 校验通过后自动重启，校验失败仅记录日志
  void type;
}

/** @description 设置当前活跃 OTA 阶段并更新 UI 步骤指示器。 */
function setActivePhase(phase) {
  if (phase === 'gd32Upload' || phase === 'gd32Relay') phase = 'gd32Ota';
  activePhase = phase;
  var stepMap = { esp32Ota: 'ppEsp32', gd32Ota: 'ppGd32Ota' };
  Object.keys(stepMap).forEach(function(p) {
    var el = document.getElementById(stepMap[p]);
    if (!el) return;
    el.classList.remove('active', 'done');
    if (p === phase) el.classList.add('active');
  });
  // Reset progress fill color
  var fill = document.getElementById('progressFill');
  if (fill) fill.classList.remove('done');
}
/** @description 标记指定阶段为完成，更新进度条颜色。 */
function markPhaseDone(phase) {
  if (phase === 'gd32Upload' || phase === 'gd32Relay') phase = 'gd32Ota';
  var stepMap = { esp32Ota: 'ppEsp32', gd32Ota: 'ppGd32Ota' };
  var el = document.getElementById(stepMap[phase]);
  if (el) { el.classList.remove('active'); el.classList.add('done'); }
  // All done? turn progress bar green
  if (phase === 'gd32Ota' || phase === 'esp32Ota') {
    var fill = document.getElementById('progressFill');
    if (fill) fill.classList.add('done');
  }
}
/** @description 更新进度条百分比和文本显示。 */
function setProgress(pct, text) {
  var fillEl = document.getElementById('progressFill');
  var txtEl = document.getElementById('progressTxt');
  var pctEl = document.getElementById('progressPct');
  if (fillEl) fillEl.style.width = pct + '%';
  if (txtEl) txtEl.textContent = text;
  if (pctEl) pctEl.textContent = pct + '%';
}
/** @description 重置进度条和阶段状态到初始值。 */
function resetProgress() {
  setProgress(0, '等待操作...');
  activePhase = null;
  esp32OtaPhase = 0;
  switchStageFlow(null);
  ['ppEsp32', 'ppGd32Ota'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.classList.remove('active', 'done');
  });
  var fill = document.getElementById('progressFill');
  if (fill) fill.classList.remove('done');
}
// ==================== 工具函数 ====================
/** @description 格式化字节大小为人类可读字符串（B/KB/MB）。 */
function formatBytes(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
}
/** @description 格式化 Date 对象为 HH:MM:SS.mmm 时间字符串。 */
function formatTime(date) {
  var h = String(date.getHours()).padStart(2, '0');
  var m = String(date.getMinutes()).padStart(2, '0');
  var s = String(date.getSeconds()).padStart(2, '0');
  var ms = String(date.getMilliseconds()).padStart(3, '0');
  return h + ':' + m + ':' + s + '.' + ms;
}
/** @description 格式化秒数为人类可读运行时间（d/h/m/s）。 */
function formatUptime(sec) {
  if (!Number.isFinite(sec) || sec < 0) return '--';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
  if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
  if (m > 0) return m + 'm ' + s + 's';
  return s + 's';
}
/** @description 转义 HTML 特殊字符，防止 XSS 注入。 */
function escapeHtml(text) {
  return String(text == null ? '' : text)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}
function formatPercent(value) {
  if (!Number.isFinite(value)) return '--';
  return value.toFixed(1).replace(/\.0$/, '') + '%';
}
function formatHttpErrorDetail(text) {
  var detail = (text || '').replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ').trim();
  if (detail.length > 180) detail = detail.slice(0, 177) + '...';
  return detail;
}
function isOtaBusyError(status, detail) {
  var text = (detail || '').toLowerCase();
  return status === 403 ||
    status === 409 ||
    status === 423 ||
    text.indexOf('busy') >= 0 ||
    text.indexOf('occupied') >= 0 ||
    text.indexOf('another ota') >= 0 ||
    text.indexOf('invalid state') >= 0 ||
    text.indexOf('占用') >= 0 ||
    text.indexOf('互斥') >= 0 ||
    text.indexOf('系统忙') >= 0;
}
/** @description 显示 Toast 提示消息，支持 info/success/warning/error 类型。 */
function showToast(message, type) {
  type = type || 'success';
  const toast = document.getElementById('toast');
  const ico = document.getElementById('toastIco');
  const msg = document.getElementById('toastMsg');
  toast.className = 'toast ' + (type === 'success' ? 'ok' : type === 'error' ? 'err' : 'warn') + ' show';
  ico.textContent = type === 'success' ? '\u2713' : type === 'error' ? '\u2715' : '\u26A0';
  msg.textContent = message;
  setTimeout(function () { toast.classList.remove('show'); }, 3000);
}
function getEspIp() {
  return document.getElementById('espIp').value.trim() || '192.168.31.127';
}
function getBaseUrl() {
  return 'http://' + getEspIp();
}
// ==================== 日志系统 (带去重 + 自动滚动) ====================
function makeLogHash(level, tag, message) {
  return level + '|' + tag + '|' + message;
}
/** @description 添加一条日志条目到环形缓冲区和日志面板。 */
function addLog(level, tag, message) {
  var scroll = document.getElementById('logScroll');
  var time = formatTime(new Date());
  var hash = makeLogHash(level, tag, message);
  if (hash === lastLogHash) return; // 去重
  lastLogHash = hash;
  var entry = { time: time, level: level, tag: tag, message: message };
  logEntries.push(entry);
  if (logFilter !== 'all' && level !== logFilter) return;
  var div = document.createElement('div');
  div.className = 'log-line';
  div.innerHTML =
    '<span class="lt">[' + time + ']</span>' +
    '<span class="lv ' + level + '">' + level + '</span>' +
    '<span class="lg">[' + tag + ']</span>' +
    '<span class="lm">' + message + '</span>';
  scroll.appendChild(div);
  if (autoScroll) scroll.scrollTop = scroll.scrollHeight;
  document.getElementById('logCount').textContent = logEntries.length;
}
function clearLogs() {
  document.getElementById('logScroll').innerHTML = '';
  logEntries = [];
  lastLogHash = null;
  document.getElementById('logCount').textContent = '0';
}
function setLogFilter(level, btn) {
  document.querySelectorAll('.log-filter').forEach(function (b) { b.classList.remove('active'); });
  btn.classList.add('active');
  logFilter = level;
  var scroll = document.getElementById('logScroll');
  scroll.innerHTML = '';
  logEntries.forEach(function (e) {
    if (level === 'all' || e.level === level) {
      var div = document.createElement('div');
      div.className = 'log-line';
      div.innerHTML =
        '<span class="lt">[' + e.time + ']</span>' +
        '<span class="lv ' + e.level + '">' + e.level + '</span>' +
        '<span class="lg">[' + e.tag + ']</span>' +
        '<span class="lm">' + e.message + '</span>';
      scroll.appendChild(div);
    }
  });
  if (autoScroll) scroll.scrollTop = scroll.scrollHeight;
}
function toggleAutoScroll() {
  autoScroll = !autoScroll;
  var btn = document.getElementById('autoScrollBtn');
  btn.textContent = autoScroll ? '\uD83D\uDCCC 自动滚动' : '\u23F8\uFE0F 暂停滚动';
}
function updateUploadButtonLabel() {
  var esp32Btn = document.getElementById('uploadEsp32Btn');
  var gd32Btn = document.getElementById('uploadGd32Btn');
  if (esp32Btn) esp32Btn.disabled = !esp32File || isUploading;
  if (gd32Btn) gd32Btn.disabled = !gd32File || isUploading;
}
function setHelper(id, text, type) {
  var el = document.getElementById(id);
  if (!el) return;
  el.className = 'helper-note ' + (type || '');
  el.innerHTML = text;
}
function updateCanHint() {
  var dlcEl = document.getElementById('canDlc');
  var dataEl = document.getElementById('canData');
  var hintEl = document.getElementById('canHint');
  if (!dlcEl || !dataEl || !hintEl) return;
  var dlc = parseInt(dlcEl.value) || 0;
  var dataHex = dataEl.value.trim().replace(/\s/g, '');
  if (!dataHex) {
    setHelper('canHint', '输入十六进制数据后会自动检查字节数是否与 DLC 一致。', '');
    return;
  }
  if (!/^[0-9a-fA-F]*$/.test(dataHex)) {
    setHelper('canHint', '数据包含非 HEX 字符，请只输入 <code>0-9 A-F</code>。', 'err');
    return;
  }
  if (dataHex.length % 2 !== 0) {
    setHelper('canHint', 'HEX 长度必须为偶数，当前还差半个字节。', 'err');
    return;
  }
  var bytes = dataHex.length / 2;
  if (dlc && bytes !== dlc) {
    setHelper('canHint', '当前数据为 <b>' + bytes + '</b> 字节，但 DLC = <b>' + dlc + '</b>。发送前建议对齐。', 'warn');
    return;
  }
  setHelper('canHint', 'HEX 数据合法，当前长度 <b>' + bytes + '</b> 字节，与 DLC 一致。', 'ok');
}
function resetStageFlow() {
  ['s1', 's2', 's3', 's4', 's5', 's6'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
  ['sd1', 'sd2', 'sd3', 'sd4', 'sd5', 'sd6'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.textContent = '';
  });
  var statusBar = document.getElementById('stageStatusBar');
  if (statusBar) statusBar.className = 'stage-status-bar';
  var statusText = document.getElementById('stageStatusText');
  if (statusText) statusText.textContent = '待机';
}
function applyDisconnectedRuntimeState() {
  document.getElementById('pillIP').textContent = '--';
  document.getElementById('pillRSSI').textContent = '--';
  if (isUploading || currentOtaTarget) return;
  resetProgress();
  resetStageFlow();
}
function applyRebootingRuntimeState() {
  var pc = document.getElementById('pillConn');
  var pct = document.getElementById('pillConnTxt');
  pc.className = 'pill processing';
  var elapsed = Math.round((Date.now() - rebootStartTime) / 1000);
  pct.textContent = '重启中 ' + elapsed + 's';
  var cs = document.getElementById('connStatus');
  var cst = document.getElementById('connStatusTxt');
  if (cs) cs.className = 'conn-indicator warn';
  if (cst) cst.textContent = 'ESP32 正在重启...';
  var bc = document.getElementById('bConn');
  if (bc) bc.textContent = '重启中';
  if (bc) bc.className = 'v warn';
}
function applyUploadingRuntimeState() {
  var pc = document.getElementById('pillConn');
  var pct = document.getElementById('pillConnTxt');
  pc.className = 'pill processing';
  pct.textContent = '上传中';
  var cs = document.getElementById('connStatus');
  var cst = document.getElementById('connStatusTxt');
  if (cs) cs.className = 'conn-indicator warn';
  if (cst) cst.textContent = '固件上传中，设备忙...';
  var bc = document.getElementById('bConn');
  if (bc) bc.textContent = '上传中';
  if (bc) bc.className = 'v warn';
}
// ==================== 连接管理 ====================
function updateConnectionUi(connected, text) {
  // header pill
  var pc = document.getElementById('pillConn');
  var pct = document.getElementById('pillConnTxt');
  pc.className = connected ? 'pill online' : 'pill offline';
  pct.textContent = text || (connected ? '已连接' : '未连接');
  // connection card indicator
  var cs = document.getElementById('connStatus');
  var cst = document.getElementById('connStatusTxt');
  if (cs) cs.className = connected ? 'conn-indicator ok' : 'conn-indicator err';
  if (cst) cst.textContent = connected ? (text || '已连接') : '未连接';
  // primary badge
  var bc = document.getElementById('bConn');
  bc.textContent = connected ? '在线' : '离线';
  bc.className = 'v ' + (connected ? 'ok' : 'err');
}
function updateHealthUi(healthCode, freeHeap) {
  var healthNames = ['正常', '警告', '危险', '关机'];
  var health = healthCode !== undefined ? (healthNames[healthCode] || '未知') : '未知';
  var connStatusTxt = document.getElementById('connStatusTxt');
  if (connStatusTxt && latestSystemStatus && latestSystemStatus.wifi) {
    connStatusTxt.textContent = (latestSystemStatus.wifi.connected ? '设备在线' : '设备离线') + ' · 系统' + health;
  }
  if (freeHeap !== undefined) {
    var mHeapEl = document.getElementById('mHeap');
    if (mHeapEl) mHeapEl.textContent = formatBytes(freeHeap);
  }
  var mh = document.getElementById('mHealth');
  if (mh) {
    mh.textContent = health;
    mh.className = 'v ' + (healthCode === 0 ? 'ok' : healthCode === 1 ? 'warn' : 'err');
  }
}
function updateHeapAndTaskStatusUi(data) {
  if (!data) return;
  var ota = data.ota || {};
  var wifi = data.wifi || {};
  var targetHost = getBaseUrl().replace(/^http:\/\//, '');
  var setText = function (id, value) {
    var el = document.getElementById(id);
    if (el) el.textContent = value;
  };
  // ---- 堆概况：已用 / 总计 + 占用条 ----
  function setFill(id, pct) {
    var el = document.getElementById(id);
    if (!el) return;
    el.style.width = Math.max(0, Math.min(100, pct)) + '%';
    el.className = 'heap-bar-fill' + (pct >= 85 ? ' danger' : pct >= 70 ? ' warn' : '');
  }
  function setBar(usageId, fillId, pctId, free, total) {
    if (free === undefined || total === undefined) {
      setText(usageId, '--'); setFill(fillId, 0); setText(pctId, '--');
      return;
    }
    var used = total - free;
    var pct = total > 0 ? (used * 100 / total) : 0;
    setText(usageId, '已用 ' + formatBytes(used) + ' / 总计 ' + formatBytes(total));
    setFill(fillId, pct);
    setText(pctId, pct.toFixed(1) + '%');
  }
  function setCpuFill(pct) {
    var el = document.getElementById('cpuUsageFill');
    if (!el) return;
    var safePct = Math.max(0, Math.min(100, Number(pct) || 0));
    el.style.width = safePct + '%';
    el.className = 'heap-bar-fill cpu-fill' + (safePct >= 85 ? ' danger' : safePct >= 70 ? ' warn' : '');
  }
  function updateCpuUsageUi(cpu) {
    cpu = cpu || {};
    var taskList = document.getElementById('cpuTaskList');
    if (!cpu.available) {
      setText('cpuUsageText', '未启用 FreeRTOS 统计');
      setText('cpuUsagePct', '--');
      setText('cpuIdlePct', '--');
      setText('cpuSampleMs', '--');
      setText('cpuTaskCount', '--');
      setCpuFill(0);
      if (taskList) taskList.innerHTML = '<div class="status-empty">固件未启用 CPU runtime stats</div>';
      return;
    }
    if (!cpu.valid) {
      setText('cpuUsageText', '等待下一次采样...');
      setText('cpuUsagePct', '--');
      setText('cpuIdlePct', '--');
      setText('cpuSampleMs', '--');
      setText('cpuTaskCount', '--');
      setCpuFill(0);
      if (taskList) taskList.innerHTML = '<div class="status-empty">首次刷新只建立基线，下一次刷新后显示 CPU 占用</div>';
      return;
    }
    var usagePct = Number(cpu.usage_pct) || 0;
    var idlePct = Number(cpu.idle_pct) || 0;
    var sampleMs = Number(cpu.sample_ms) || 0;
    var tasks = Array.isArray(cpu.tasks) ? cpu.tasks.slice() : [];
    tasks.sort(function (a, b) {
      return (Number(b.usage_pct) || 0) - (Number(a.usage_pct) || 0);
    });

    setText('cpuUsageText', 'CPU 已用 ' + formatPercent(usagePct));
    setText('cpuUsagePct', formatPercent(usagePct));
    setText('cpuIdlePct', formatPercent(idlePct));
    setText('cpuSampleMs', sampleMs > 0 ? sampleMs + ' ms' : '--');
    setText('cpuTaskCount', String(tasks.length));
    setCpuFill(usagePct);

    if (taskList) {
      var visibleTasks = tasks.slice(0, 8);
      if (!visibleTasks.length) {
        taskList.innerHTML = '<div class="status-empty">当前采样没有任务增量</div>';
      } else {
        taskList.innerHTML = visibleTasks.map(function (task) {
          var pct = Number(task.usage_pct) || 0;
          var delta = Number(task.runtime_delta) || 0;
          var fillClass = pct >= 85 ? 'err' : pct >= 70 ? 'warn' : '';
          return '' +
            '<div class="status-task cpu-task">' +
              '<div class="status-task-head">' +
                '<div class="status-task-name">' + escapeHtml(task.name || 'task') + '</div>' +
                '<div class="status-task-ratio">' + escapeHtml(formatPercent(pct)) + '</div>' +
              '</div>' +
              '<div class="status-task-bar"><div class="status-task-bar-fill ' + fillClass + '" style="width:' + Math.max(0, Math.min(100, pct)) + '%"></div></div>' +
              '<div class="status-task-meta">' +
                '<div><span>任务号</span><b>' + escapeHtml(task.task_number) + '</b></div>' +
                '<div><span>增量</span><b>' + escapeHtml(delta) + '</b></div>' +
                '<div><span>占比</span><b>' + escapeHtml(formatPercent(pct)) + '</b></div>' +
              '</div>' +
            '</div>';
        }).join('');
      }
    }
  }
  updateCpuUsageUi(data.cpu);
  setText('mInternalHeap', data.internal_free_heap !== undefined ? formatBytes(data.internal_free_heap) : '--');
  setText('mInternalMinHeap', data.internal_min_free_heap !== undefined ? formatBytes(data.internal_min_free_heap) : '--');
  setText('mInternalLargestBlock', data.internal_largest_free_block !== undefined ? formatBytes(data.internal_largest_free_block) : '--');
  setBar('memInternalUsage', 'memInternalFill', 'memInternalPct', data.internal_free_heap, data.internal_heap_total);

  setText('mLargestBlock', data.largest_free_block !== undefined ? formatBytes(data.largest_free_block) : '--');

  setText('sOtaState', (ota.sys_state || '--') + (Number.isFinite(ota.sys_state_code) ? ' (' + ota.sys_state_code + ')' : ''));
  setText('sRelayState', Number.isFinite(ota.relay_state) ? String(ota.relay_state) : '--');
  var storedText = '--';
  if (ota.total_size || ota.stored_size) {
    storedText = formatBytes(ota.stored_size || 0) + ' / ' + formatBytes(ota.total_size || 0);
  }
  setText('sOtaStored', storedText);
  setText('sStatusUpdated', formatTime(new Date()));

  var taskList = document.getElementById('taskStackList');
  if (taskList) {
    var taskStack = data.task_stack || {};
    var names = Object.keys(taskStack);
    if (!names.length) {
      taskList.innerHTML = '<div class="status-empty">当前状态未包含任务栈信息</div>';
    } else {
      taskList.innerHTML = names.map(function (name) {
        var item = taskStack[name] || {};
        var stackSize = Number(item.stack_size_bytes) || 0;
        var watermark = Number(item.high_watermark_bytes) || 0;
        var peakUsed = Number(item.peak_used_bytes) || 0;
        var usagePct = stackSize > 0 ? (peakUsed * 100 / stackSize) : 0;
        var fillClass = usagePct >= 85 ? 'err' : usagePct >= 70 ? 'warn' : '';
        return '' +
          '<div class="status-task">' +
            '<div class="status-task-head">' +
              '<div class="status-task-name">' + escapeHtml(name) + '</div>' +
              '<div class="status-task-ratio">' + escapeHtml(formatPercent(usagePct)) + '</div>' +
            '</div>' +
            '<div class="status-task-bar"><div class="status-task-bar-fill ' + fillClass + '" style="width:' + Math.max(0, Math.min(100, usagePct)) + '%"></div></div>' +
            '<div class="status-task-meta">' +
              '<div><span>栈大小</span><b>' + escapeHtml(formatBytes(stackSize)) + '</b></div>' +
              '<div><span>剩余水位</span><b>' + escapeHtml(formatBytes(watermark)) + '</b></div>' +
              '<div><span>峰值占用</span><b>' + escapeHtml(formatBytes(peakUsed)) + '</b></div>' +
            '</div>' +
          '</div>';
      }).join('');
    }
  }

  var statusJson = document.getElementById('statusJson');
  if (statusJson) {
    statusJson.textContent = JSON.stringify(data, null, 2);
  }
}
// ==================== OTA 阶段驱动 (真实状态) ====================
// Relay state comes from relay_state_t in main/relay/ota_relay.h:
//   0=IDLE, 1=HANDSHAKING, 2=SIZE_NEG, 3=TRANSFERRING,
//   4=FINISHING, 5=DONE, 255=ERROR.
var relayStateDetailMap = {
  0: '待机',
  1: '认证握手',
  2: '传输协商',
  3: '数据转发',
  4: '收尾验证',
  5: 'OTA完成',
  255: '错误'
};
function updateStagesFromRelayState(rs) {
  var ids = ['s1', 's2', 's3', 's4', 's5', 's6'];
  var detailIds = ['sd1', 'sd2', 'sd3', 'sd4', 'sd5', 'sd6'];
  var all = ids.map(function(id) { return document.getElementById(id); });
  var details = detailIds.map(function(id) { return document.getElementById(id); });
  // reset all
  all.forEach(function (s) { if (s) s.className = 'stage-node'; });
  details.forEach(function (d) { if (d) d.textContent = ''; });
  // status bar
  var statusBar = document.getElementById('stageStatusBar');
  var statusText = document.getElementById('stageStatusText');
  if (rs === undefined || rs === null) {
    if (statusBar) statusBar.className = 'stage-status-bar';
    if (statusText) statusText.textContent = '待机';
    return;
  }
  if (rs === 255) {
    all.forEach(function (s) { if (s) s.className = 'stage-node err'; });
    if (statusBar) statusBar.className = 'stage-status-bar err';
    if (statusText) statusText.textContent = '错误';
    return;
  }
  var activeStage = 0;
  if (rs === 1) activeStage = 1;
  else if (rs === 2) activeStage = 2;
  else if (rs === 3) activeStage = 3;
  else if (rs === 4) activeStage = 5;
  else if (rs === 5) activeStage = 6;
  for (var i = 0; i < activeStage - 1; i++) {
    if (all[i]) all[i].className = 'stage-node done';
  }
  if (rs === 5) {
    if (all[5]) all[5].className = 'stage-node done';
  } else if (activeStage >= 1 && activeStage <= 6) {
    if (all[activeStage - 1]) all[activeStage - 1].className = 'stage-node live';
  }
  // Show detail text for the active stage
  var detailText = relayStateDetailMap[rs] || '';
  if (activeStage >= 1 && activeStage <= 6 && details[activeStage - 1]) {
    details[activeStage - 1].textContent = detailText;
  }
  // Update status bar
  if (statusBar && statusText) {
    if (rs === 0) {
      statusBar.className = 'stage-status-bar';
      statusText.textContent = '待机';
    } else {
      statusBar.className = 'stage-status-bar active';
      statusText.textContent = (relayStateDetailMap[rs] || '未知') + ' (' + rs + ')';
    }
  }
}
function updateSystemStatusUi(data) {
  if (!data) return;
  latestSystemStatus = data;
  updateHeapAndTaskStatusUi(data);
  var wifi = data.wifi || {};
  var ota = data.ota || {};
  var healthCode = data.health;
  // Use global relayStateDetailMap defined in updateStagesFromRelayState section
  var connected = !!wifi.connected;
  var connText = connected ? (wifi.mode || 'STA') + ' 已连接' : (wifi.mode || 'STA') + ' 未连接';
  updateConnectionUi(connected, connText);
  updateHealthUi(healthCode, data.free_heap);
  if (data.version) {
    var pillVerEl = document.getElementById('pillVer');
    if (pillVerEl) {
      pillVerEl.textContent = 'v' + data.version;
      pillVerEl.dataset.tooltip = data.build_time ? ("编译: " + data.build_time) : '';
    }
    // GD32 版本
    var mGd32App = document.getElementById('mGd32App');
    var mGd32Boot = document.getElementById('mGd32Boot');
    if (mGd32App) {
      if (ota.gd32_app && ota.gd32_app !== 'v0.0.0.0') {
        mGd32App.textContent = ota.gd32_app;
        mGd32App.className = 'v ok';
      } else {
        mGd32App.textContent = ota.gd32_app_alive ? '未知' : '离线';
        mGd32App.className = 'v';
      }
    }
    if (mGd32Boot) {
      if (ota.gd32_boot && ota.gd32_boot !== 'v0.0.0.0') {
        mGd32Boot.textContent = ota.gd32_boot;
        mGd32Boot.className = 'v ok';
      } else {
        mGd32Boot.textContent = ota.gd32_app_alive ? '未知' : '离线';
        mGd32Boot.className = 'v';
      }
    }
  }
  // temperature
  var mTempEl = document.getElementById('mTemp');
  if (mTempEl) mTempEl.textContent =
    Number.isFinite(data.temperature) ? data.temperature.toFixed(1) + '\u00B0C' : '--';
  // uptime
  var uptimeEl = document.getElementById('pillUptime');
  if (uptimeEl) uptimeEl.textContent = '\u23F1 ' + formatUptime(data.uptime_sec);
  // wifi / signal
  var pillIPEl = document.getElementById('pillIP');
  if (pillIPEl) pillIPEl.textContent = wifi.ip || '--';
  var rssi = (wifi.rssi !== undefined && wifi.rssi !== -127) ? wifi.rssi + ' dBm' : '--';
  var pillRSSIEl = document.getElementById('pillRSSI');
  if (pillRSSIEl) pillRSSIEl.textContent = rssi;
  var mOtaCache = document.getElementById('mOtaCache');
  var otaStored = ota.stored_size || 0;
  if (mOtaCache) {
    if (otaStored > 0) {
      mOtaCache.textContent = formatBytes(otaStored) + ' / 216 KB';
      mOtaCache.className = 'v ok';
    } else {
      mOtaCache.textContent = '0 / 216 KB';
      mOtaCache.className = 'v';
    }
  }
  // relay state - skip GD32 relay UI when ESP32 OTA is active
  if (currentOtaTarget === 'esp32' && esp32OtaPhase > 0) {
    return;
  }
  var rs = ota.relay_state;
  var sysState = ota.sys_state || '';
  var sysStateCode = Number.isFinite(ota.sys_state_code) ? ota.sys_state_code : -1;
  var isGd32Storing = sysState === 'GD32_STORING' || sysStateCode === 1;
  var isGd32Relaying = sysState === 'GD32_RELAYING' || sysStateCode === 3;
  var isGd32Complete = sysState === 'GD32_COMPLETE' || sysStateCode === 4;
  var isGd32Error = sysState === 'ERROR' || sysStateCode === 8 || rs === 255;
  // OTA cache progress (GD32 upload to ESP32 flash)
  var total = ota.total_size || 0;
  var stored = ota.stored_size || 0;
  var storageProgress = total > 0 ? Math.min(100, Math.round(stored * 100 / total)) : 0;
  function setOtaProgressBadge() {}  // OTA 进度卡片已移除，保留空壳避免调用报错
  // GD32 relay forwarding progress
  var relaySeq = ota.relay_seq || 0;
  var relayTotal = ota.relay_total_packets || 0;
  var relayProgress = relayTotal > 0 ? Math.min(100, Math.round(relaySeq * 100 / relayTotal)) : 0;
  var gd32AppAlive = ota.gd32_app_alive;
  var displayRelayState = isGd32Complete ? 5 : (isGd32Relaying && rs === 0 ? 1 : rs);
  if (currentOtaTarget !== 'esp32') { updateStagesFromRelayState(displayRelayState); }

  if (isGd32Error && currentOtaTarget === 'gd32') {
    cleanupAfterOtaFailure('GD32 OTA 失败');
  } else if (isGd32Error) {
    // 非当前 OTA 会话的错误：仅标记阶段指示器，不触发完整清理
    updateStagesFromRelayState(255);
    var sbErr = document.getElementById('stageStatusBar');
    var stErr = document.getElementById('stageStatusText');
    if (sbErr) sbErr.className = 'stage-status-bar err';
    if (stErr) stErr.textContent = 'GD32 错误（待机）';
  } else if (isGd32Complete || rs === 5 || (gd32AppAlive && sysState === 'GD32_COMPLETE')) {
    markPhaseDone('gd32Ota');
    setOtaProgressBadge(100);
    setProgress(100, 'GD32 刷写完成');
    var sbDone = document.getElementById('stageStatusBar');
    var stDone = document.getElementById('stageStatusText');
    if (sbDone) sbDone.className = 'stage-status-bar ok';
    if (stDone) stDone.textContent = 'GD32 刷写完成';
  } else if (isGd32Storing || (total > 0 && stored < total && !isUploading)) {
    setActivePhase('gd32Ota');
    var storeOverall = Math.min(45, Math.round(storageProgress * 0.45));
    setOtaProgressBadge(storeOverall);
    if (!isUploading) {
      setProgress(storeOverall, '已缓存 ' + formatBytes(stored) + ' / ' + formatBytes(total));
    }
  } else if (rs === 1) {
    // HANDSHAKING: 握手 + 挑战认证
    setActivePhase('gd32Ota');
    setOtaProgressBadge(50);
    setProgress(50, '握手与认证中...');
  } else if (rs === 2) {
    // SIZE_NEG: 固件大小协商
    setActivePhase('gd32Ota');
    setOtaProgressBadge(55);
    setProgress(55, '协商固件大小...');
  } else if (rs === 3) {
    // TRANSFERRING: 数据包传输
    setActivePhase('gd32Ota');
    var relayOverall = 60 + Math.round(relayProgress * 0.30);
    setOtaProgressBadge(relayOverall);
    setProgress(relayOverall, '已转发 ' + relaySeq + ' / ' + relayTotal + ' 包');
  } else if (rs === 4) {
    // FINISHING: 结束OTA + APP存活验证
    setActivePhase('gd32Ota');
    setOtaProgressBadge(95);
    if (gd32AppAlive) {
      markPhaseDone('gd32Ota');
      setProgress(100, 'GD32 刷写完成');
      var sb = document.getElementById('stageStatusBar');
      var st = document.getElementById('stageStatusText');
      if (sb) sb.className = 'stage-status-bar ok';
      if (st) st.textContent = 'GD32 刷写完成';
    } else {
      setProgress(95, '收尾中，等待 APP 启动...');
    }
  } else if (total > 0 && stored >= total) {
    setActivePhase('gd32Ota');
    setOtaProgressBadge(45);
    setProgress(45, '上传完成，等待自动启动 GD32 OTA...');
  } else {
    setOtaProgressBadge(storageProgress);
  }
}
/** @description 测试与 ESP32 的连接（GET /status）。 */
async function testConnection() {
  // 如果在降速模式，先恢复正常轮询
  if (pollSlowMode) {
    addLog('I', '连接', '正在重新连接...');
    restorePolling();
  }
  // 立即发一次请求检测
  try {
    var resp = await fetch(getBaseUrl() + '/system/status', {
      signal: AbortSignal.timeout(3000)
    });
    if (resp.ok) {
      var data = await resp.json();
      latestSystemStatus = data;
      updateSystemStatusUi(data);
      var wifi = data.wifi || {};
      addLog('I', '连接', wifi.connected
        ? '已连接：' + (wifi.mode || 'STA') + ' ' + (wifi.ip || '')
        : '未连接：' + (wifi.mode || 'STA'));
      showToast(wifi.connected ? 'ESP32 当前在线' : 'ESP32 当前未连接',
        wifi.connected ? 'success' : 'warning');
    } else {
      addLog('E', '连接', 'HTTP ' + resp.status);
      showToast('设备响应异常: HTTP ' + resp.status, 'error');
    }
  } catch (e) {
    addLog('E', '连接', '连接失败: ' + e.message);
    // 连接失败时自动诊断
    var diag = await diagnoseNetwork();
    addLog('W', '诊断', diag);
    showToast('无法连接: ' + diag, 'error');
  }
}
async function refreshSystemStatusNow(event) {
  if (event) {
    event.preventDefault();
    event.stopPropagation();
  }
  if (pollingPausedForUpload) return;
  try {
    var resp = await fetch(getBaseUrl() + '/system/status', {
      signal: AbortSignal.timeout(3000)
    });
    if (!resp.ok) {
      addLog('E', '状态', '刷新失败: HTTP ' + resp.status);
      showToast('状态刷新失败: HTTP ' + resp.status, 'error');
      return;
    }
    var data = await resp.json();
    latestSystemStatus = data;
    updateSystemStatusUi(data);
    consecutivePollFailures = 0;
    if (pollSlowMode) restorePolling();
    addLog('I', '状态', '已手动刷新 /system/status');
  } catch (e) {
    addLog('E', '状态', '刷新失败: ' + e.message);
    showToast('状态刷新失败', 'error');
  }
}
// ==================== CAN 帧发送 ====================
async function sendCanFrame() {
  var chEl = document.getElementById('canChannel');
  var idEl = document.getElementById('canId');
  var dlcEl = document.getElementById('canDlc');
  var dataEl = document.getElementById('canData');
  if (!chEl || !idEl || !dlcEl || !dataEl) {
    showToast('请在独立 CAN 页面中发送帧', 'warning');
    return;
  }
  var ch = parseInt(chEl.value);
  var idStr = idEl.value.trim();
  var dlc = parseInt(dlcEl.value) || 8;
  var dataStr = dataEl.value.trim();
  var extId = 0;
  if (idStr.toLowerCase().indexOf('0x') === 0) extId = parseInt(idStr, 16);
  else { extId = parseInt(idStr, 10); if (isNaN(extId)) extId = parseInt(idStr, 16); }
  if (isNaN(extId)) { showToast('ID 格式错误', 'error'); return; }
  var dataHex = dataStr.replace(/\s/g, '');
  if (dataHex.length % 2 !== 0) { showToast('数据长度必须为偶数', 'error'); return; }
  addLog('I', 'CAN', '发送 CH' + ch + ' ID=0x' + extId.toString(16).toUpperCase() + ' DLC=' + dlc);
  try {
    var resp = await fetch(getBaseUrl() + '/can/send', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ch: ch, id: extId, dlc: dlc, data: dataHex }),
      signal: AbortSignal.timeout(5000)
    });
    var result = await resp.json();
    if (result.status === 'ok') {
      addLog('I', 'CAN', '响应: ' + (result.recv || '(无数据)') + ' (' + (result.recv_len || 0) + 'B)');
      var rd = document.getElementById('canResp');
      rd.style.display = 'block';
      document.getElementById('canRespText').textContent =
        result.recv ? '0x' + result.recv + ' (' + result.recv_len + ' 字节)' : '无响应数据';
      showToast('CH' + ch + ' 发送成功', 'success');
    } else {
      addLog('E', 'CAN', '发送失败: code=' + result.code);
      showToast('发送失败: code=' + result.code, 'error');
    }
  } catch (e) {
    addLog('E', 'CAN', '发送失败: ' + e.message);
    showToast('发送失败', 'error');
  }
}
function clearCanFields() {
  var idEl = document.getElementById('canId');
  var dlcEl = document.getElementById('canDlc');
  var dataEl = document.getElementById('canData');
  var respEl = document.getElementById('canResp');
  if (!idEl || !dlcEl || !dataEl) return;
  idEl.value = '0x18DA00F1';
  dlcEl.value = '8';
  dataEl.value = '0102030405060708';
  if (respEl) respEl.style.display = 'none';
  updateCanHint();
}
// ==================== SPI 发送 ====================
function updateSpiHint() {
  var typeHex = document.getElementById('spiType').value.trim();
  var dataHex = document.getElementById('spiData').value.trim().replace(/\s/g, '');
  var hint = document.getElementById('spiHint');
  if (!typeHex || !/^[0-9a-fA-F]{1,2}$/.test(typeHex)) {
    hint.className = 'helper-note err';
    hint.innerHTML = 'type 需填 1 字节 hex（如 01, B0）。';
    return;
  }
  if (dataHex && !/^[0-9a-fA-F]*$/.test(dataHex)) {
    hint.className = 'helper-note err';
    hint.innerHTML = '数据包含非 HEX 字符。';
    return;
  }
  if (dataHex && dataHex.length % 2 !== 0) {
    hint.className = 'helper-note err';
    hint.innerHTML = 'HEX 长度必须为偶数。';
    return;
  }
  var bytes = dataHex ? dataHex.length / 2 : 0;
  var totalOnWire = 5 + bytes;
  hint.className = 'helper-note ok';
  hint.innerHTML = 'type=<code>0x' + typeHex.toUpperCase() + '</code>' +
    (bytes > 0 ? '，payload: <b>' + bytes + '</b>B' : '，无 payload') +
    '，线上 <b>' + totalOnWire + '</b>B（SOF+TYPE+LEN+PAYLOAD+CRC）';
}
async function sendSpiFrame() {
  var typeHex = document.getElementById('spiType').value.trim();
  var dataStr = document.getElementById('spiData').value.trim();
  var timeout = parseInt(document.getElementById('spiTimeout').value) || 1000;
  if (!typeHex || !/^[0-9a-fA-F]{1,2}$/.test(typeHex)) {
    showToast('type 需填 1 字节 hex', 'error');
    return;
  }
  var dataHex = dataStr.replace(/\s/g, '');
  if (dataHex && !/^[0-9a-fA-F]*$/.test(dataHex)) {
    showToast('数据包含非 HEX 字符', 'error');
    return;
  }
  if (dataHex && dataHex.length % 2 !== 0) {
    showToast('HEX 数据长度必须为偶数', 'error');
    return;
  }
  var txBytes = dataHex ? dataHex.length / 2 : 0;
  addLog('I', 'SPI', 'type=0x' + typeHex.toUpperCase() +
    (txBytes > 0 ? ', payload=' + dataHex.toUpperCase() : ', 无负载'));
  var body = { type: typeHex, data: dataHex.toUpperCase(), timeout_ms: timeout };
  try {
    var resp = await fetch(getBaseUrl() + '/spi/send', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
      signal: AbortSignal.timeout(timeout + 2000)
    });
    var result = await resp.json();
    if (result.status === 'ok') {
      var recvHex = result.recv || '';
      var respTypeName = {'80': 'ACK', '81': 'WAIT', '82': 'ERROR'};
      var respTypeLabel = respTypeName[result.resp_type] || ('0x' + result.resp_type);
      addLog('I', 'SPI', '→ ' + respTypeLabel +
        (recvHex ? ' 0x' + recvHex : '') + ' (' + (result.resp_len || 0) + 'B)');
      var rd = document.getElementById('spiResp');
      rd.style.display = 'block';
      document.getElementById('spiRespText').textContent =
        'type=0x' + result.type + ' payload=' + result.payload_len + 'B → ' + respTypeLabel +
        (recvHex ? ' 0x' + recvHex : '') +
        ' (' + (result.resp_len || 0) + 'B)';
      showToast('SPI 发送成功', 'success');
    } else {
      addLog('E', 'SPI', '发送失败: code=' + result.code + ' ' + (result.message || ''));
      var rd = document.getElementById('spiResp');
      rd.style.display = 'block';
      document.getElementById('spiRespText').textContent =
        '错误: code=' + result.code + ' ' + (result.message || '');
      showToast('SPI 发送失败', 'error');
    }
  } catch (e) {
    addLog('E', 'SPI', '发送失败: ' + e.message);
    showToast('SPI 发送失败: ' + e.message, 'error');
  }
}
function clearSpiFields() {
  document.getElementById('spiType').value = '01';
  document.getElementById('spiData').value = '';
  document.getElementById('spiTimeout').value = '1000';
  document.getElementById('spiResp').style.display = 'none';
  updateSpiHint();
}
// ==================== 文件处理 ====================
function handleFileSelect(event, chip) {
  var file = event.target.files[0];
  if (file) processFile(file, chip);
}
// 通过 File System Access API 选择文件（支持热重载）
async function pickFileViaFSAPI(chip) {
  if (!window.showOpenFilePicker) {
    addLog('W', '文件', '当前浏览器不支持 File System Access API，请手动重新选择文件');
    return false;
  }
  try {
    var acceptMap = chip === 'esp32'
      ? { 'application/octet-stream': ['.bin'] }
      : { 'application/octet-stream': ['.bin', '.hex'] };
    var handles = await window.showOpenFilePicker({
      types: [{ description: '固件文件', accept: acceptMap }],
      multiple: false
    });
    if (handles && handles.length > 0) {
      var handle = handles[0];
      var file = await handle.getFile();
      if (chip === 'esp32') esp32FileHandle = handle;
      else gd32FileHandle = handle;
      processFile(file, chip);
      addLog('I', '文件', '已通过 FSAPI 选择文件（支持热重载）: ' + file.name);
      return true;
    }
  } catch (e) {
    if (e.name !== 'AbortError') {
      addLog('W', '文件', 'FSAPI 选择失败: ' + e.message);
    }
  }
  return false;
}
function setupDropZone(zoneId, chip) {
  var zone = document.getElementById(zoneId);
  if (!zone) return;
  zone.addEventListener('dragover', function (e) {
    e.preventDefault(); e.currentTarget.classList.add('dragover');
  });
  zone.addEventListener('dragleave', function (e) {
    e.currentTarget.classList.remove('dragover');
  });
  zone.addEventListener('drop', function (e) {
    e.preventDefault(); e.currentTarget.classList.remove('dragover');
    var file = e.dataTransfer.files[0];
    if (file) processFile(file, chip);
  });
}
setupDropZone('uploadZoneEsp32', 'esp32');
setupDropZone('uploadZoneGd32', 'gd32');
var canDataEl = document.getElementById('canData');
var canDlcEl = document.getElementById('canDlc');
if (canDataEl) canDataEl.addEventListener('input', updateCanHint);
if (canDlcEl) canDlcEl.addEventListener('input', updateCanHint);
var spiTypeEl = document.getElementById('spiType');
var spiDataEl = document.getElementById('spiData');
if (spiTypeEl) spiTypeEl.addEventListener('input', updateSpiHint);
if (spiDataEl) spiDataEl.addEventListener('input', updateSpiHint);
// ==================== Intel HEX → BIN 转换 ====================
/**
 * 将 Intel HEX 字符串解析为原始 BIN 字节数组。
 *
 * 支持记录类型: 00(数据), 01(EOF), 02(扩展段地址), 04(扩展线性地址)。
 * 地址间隙填充 0xFF（Flash 擦除态）。
 *
 * @param {string} hexText  Intel HEX 文件的完整文本内容。
 * @returns {{ data: Uint8Array, baseAddr: number, endAddr: number }}
 */
function parseIntelHex(hexText) {
  var baseAddr = 0;     // 当前扩展地址
  var segments = [];     // { offset, data } 列表
  var minAddr = 0xFFFFFFFF;
  var maxAddr = 0;
  var lines = hexText.split(/\r?\n/);
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].trim();
    if (!line || line[0] !== ':') continue;
    // 解析行: :LLAAAATT[DD...]CC
    var byteCount = parseInt(line.substr(1, 2), 16);
    var addr16   = parseInt(line.substr(3, 4), 16);
    var recType  = parseInt(line.substr(7, 2), 16);
    // 校验和验证
    var sum = 0;
    for (var j = 1; j < line.length; j += 2) {
      sum += parseInt(line.substr(j, 2), 16);
    }
    if ((sum & 0xFF) !== 0) {
      throw new Error('HEX 行 ' + (i + 1) + ' 校验和错误: ' + line);
    }
    if (recType === 0x01) {
      // EOF
      break;
    } else if (recType === 0x04) {
      // 扩展线性地址 (upper 16 bits)
      baseAddr = parseInt(line.substr(9, 4), 16) << 16;
    } else if (recType === 0x02) {
      // 扩展段地址
      baseAddr = (parseInt(line.substr(9, 4), 16) << 4) & 0xFFFFF;
    } else if (recType === 0x00) {
      // 数据记录
      var absAddr = baseAddr + addr16;
      var data = new Uint8Array(byteCount);
      for (var k = 0; k < byteCount; k++) {
        data[k] = parseInt(line.substr(9 + k * 2, 2), 16);
      }
      segments.push({ offset: absAddr, data: data });
      if (absAddr < minAddr) minAddr = absAddr;
      var endAddr = absAddr + byteCount;
      if (endAddr > maxAddr) maxAddr = endAddr;
    }
    // 其他记录类型 (03, 05 等) 忽略
  }
  if (segments.length === 0) {
    throw new Error('HEX 文件中没有有效数据记录');
  }
  // 合并为连续 bin，间隙填 0xFF
  var totalSize = maxAddr - minAddr;
  var bin = new Uint8Array(totalSize);
  // 先填充 0xFF
  for (var m = 0; m < totalSize; m++) bin[m] = 0xFF;
  for (var s = 0; s < segments.length; s++) {
    var seg = segments[s];
    var off = seg.offset - minAddr;
    bin.set(seg.data, off);
  }
  return { data: bin, baseAddr: minAddr, endAddr: maxAddr };
}
/**
 * 检测文件内容是否为 Intel HEX 格式。
 * 看前几个非空行是否以 ':' 开头。
 */
function isIntelHexText(text) {
  var lines = text.split(/\r?\n/);
  for (var i = 0; i < Math.min(lines.length, 5); i++) {
    var line = lines[i].trim();
    if (!line) continue;
    return line[0] === ':';
  }
  return false;
}
/**
 * 公共 HEX→BIN 转换：如果文件是 .hex 格式，自动解析为 bin。
 * BLE / WiFi 共用此函数，避免转换逻辑重复。
 * @param {File} file 原始文件
 * @returns {Promise<{file: File, converted: boolean, baseAddr: number|null}>}
 */
async function convertHexFileToBin(file) {
  if (!file || !file.name.toLowerCase().endsWith('.hex')) {
    return { file: file, converted: false, baseAddr: null };
  }
  var text = await file.text();
  if (!isIntelHexText(text)) {
    return { file: file, converted: false, baseAddr: null };
  }
  var result = parseIntelHex(text);
  var binName = file.name.replace(/\.hex$/i, '.bin');
  var binBlob = new Blob([result.data], { type: 'application/octet-stream' });
  var binFile = new File([binBlob], binName, { type: 'application/octet-stream', lastModified: file.lastModified });
  return { file: binFile, converted: true, baseAddr: result.baseAddr, endAddr: result.endAddr };
}
// ==================== 文件处理 ====================
/** @description 处理用户选择的固件文件（HEX→BIN 转换、SHA-256 计算、文件校验）。 */
function processFile(file, chip) {
  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  var isHexByName = file.name.toLowerCase().endsWith('.hex');
  if (chip === 'gd32' && isHexByName) {
    // GD32 固件: .hex 文件自动转换为 .bin，然后替换文件对象
    convertHexFileToBin(file).then(function(result) {
      if (!result.converted) {
        // 文件名 .hex 但内容不像，直接用原始文件
        addLog('W', '文件', file.name + ' 扩展名为 .hex 但内容不像 HEX 格式，按原始文件处理');
        gd32File = file;
        document.getElementById('fileInfo' + suffix).classList.add('show');
        document.getElementById('fileName' + suffix).textContent = file.name;
        document.getElementById('fileType' + suffix).textContent = 'Binary(?)';
        document.getElementById('fileSize' + suffix).textContent = formatBytes(file.size);
        updateUploadButtonLabel();
        return;
      }
      gd32File = result.file;
      var baseHex = '0x' + result.baseAddr.toString(16).toUpperCase().padStart(8, '0');
      addLog('I', '文件', 'HEX → BIN 转换完成: ' + file.name + ' → ' + result.file.name);
      addLog('I', '文件', '基地址: ' + baseHex + ', 数据大小: ' + formatBytes(result.file.size));
      showToast('HEX 已自动转换为 BIN', 'success');
      document.getElementById('fileInfo' + suffix).classList.add('show');
      document.getElementById('fileName' + suffix).textContent = result.file.name + ' (由 ' + file.name + ' 转换)';
      document.getElementById('fileType' + suffix).textContent = 'Binary (HEX转换)';
      document.getElementById('fileSize' + suffix).textContent = formatBytes(result.file.size);
      updateUploadButtonLabel();
    }).catch(function(err) {
      addLog('E', '文件', 'HEX 解析失败: ' + err.message);
      showToast('HEX 文件解析失败: ' + err.message, 'error');
      gd32File = null;
      document.getElementById('fileInfo' + suffix).classList.remove('show');
      updateUploadButtonLabel();
    });
    return; // 异步，回调里设置 gd32File
  }
  // .bin 文件或 ESP32 文件: 直接使用
  if (chip === 'esp32') esp32File = file;
  else gd32File = file;
  document.getElementById('fileInfo' + suffix).classList.add('show');
  document.getElementById('fileName' + suffix).textContent = file.name;
  document.getElementById('fileType' + suffix).textContent =
    isHexByName ? 'Intel HEX' : 'Binary';
  document.getElementById('fileSize' + suffix).textContent = formatBytes(file.size);
  updateUploadButtonLabel();
  addLog('I', '文件', chip.toUpperCase() + ' 已选择: ' + file.name + ' (' + formatBytes(file.size) + ')');
  showToast(chip.toUpperCase() + ' 文件已选择', 'success');
  // ESP32 文件: 计算并显示 SHA-256
  if (chip === 'esp32') {
    var shaElem = document.getElementById('fileSha256' + suffix);
    if (shaElem) {
      shaElem.textContent = '计算中...';
      file.arrayBuffer().then(function(buf) {
        return crypto.subtle.digest('SHA-256', buf);
      }).then(function(hashBuffer) {
        var hashArray = Array.from(new Uint8Array(hashBuffer));
        var hashHex = hashArray.map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
        shaElem.textContent = hashHex.toUpperCase();
        // 存储到全局变量供上传时使用
        window._esp32FileSha256 = hashHex.toUpperCase();
        addLog('I', '文件', 'ESP32 固件 SHA-256: ' + hashHex.toUpperCase());
      }).catch(function(err) {
        shaElem.textContent = '计算失败: ' + err.message;
      });
    }
  }
}
/** @description 清除已选固件文件并重置对应 UI 状态。 */
function clearFile(chip) {
  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  if (chip === 'esp32') { esp32File = null; esp32FileHandle = null; window._esp32FileSha256 = null; }
  else { gd32File = null; gd32FileHandle = null; }
  document.getElementById('fileInput' + suffix).value = '';
  document.getElementById('fileInfo' + suffix).classList.remove('show');
  var shaElem = document.getElementById('fileSha256' + suffix);
  if (shaElem) shaElem.textContent = '-';
  updateUploadButtonLabel();
  addLog('I', '文件', chip.toUpperCase() + ' 文件已清除');
}
// ==================== 上传固件 ====================
// ★ 跨通道互斥检查：WiFi 与 BLE 共用同一个 ESP32 OTA 状态机与全局互斥，
//   但前端只用一个 `isUploading` bool 锁同一通道。这里在每次 startUpload
//   入口主动查询 /system/status，若发现另一通道已经进入 OTA 活跃态
//   （ESP32 三个状态或 GD32 存储/中继），直接拒绝并提示用户。
/** @description 启动 ESP32 本机 OTA 上传流程（互斥获取→分块上传→SHA验证→重启）。 */
async function startUpload(chip) {
  var file = chip === 'esp32' ? esp32File : gd32File;
  if (!file) return;
  if (isUploading) {
    var busyChip = (activeUploadChip || window.flashTarget || chip || 'ota').toUpperCase();
    addLog('W', '上传', '已有 ' + busyChip + ' OTA 正在进行，拒绝新的 ' + chip.toUpperCase() + ' 上传');
    showToast('OTA 通道正在占用，请等待当前刷写结束', 'error');
    return;
  }
  pausePollingForUpload();
  // ★ 必须先抢锁再 await：守卫同步设置，await 期间重复点击才能被拦下。
  //   旧实现把 isUploading 设在 await handle.getFile() 之后 → 用户在 BLE 期间
  //   狂点 WiFi 上传按钮时，多个 startUpload 都能通过守卫，最终并发 xhr.send，
  //   设备端互斥被绕过、403 路径走漏。
  isUploading = true;
  activeUploadChip = chip;
  // 跨通道互斥前置检查（含 BLE 取消后的瞬态重试）
  var _probeReady = false;
  for (var _pi = 0; _pi < 4; _pi++) {
    if (_pi > 0) {
      var _pd = [1500, 3000, 3000][_pi - 1];
      addLog('I', '上传', '等待设备清理 OTA 状态 (' + _pd + 'ms)...');
      await new Promise(function(r) { setTimeout(r, _pd); });
    }
    try {
      var probe = await fetch(getBaseUrl() + '/system/status', {
        signal: AbortSignal.timeout(2000)
      });
      if (probe.ok) {
        var st = await probe.json();
        var ota = st.ota || {};
        var ssc = Number.isFinite(ota.sys_state_code) ? ota.sys_state_code : -1;
        var otaChannel = Number.isFinite(ota.channel) ? ota.channel : -1;
        var otaChannelName = ota.channel_name || (otaChannel === 1 ? 'BLE' : 'WIFI');
        // ESP32 RECEIVING(5) 可由同一 WiFi 通道的新上传接管；后台/休眠可能让 /ota/update 没发出，
        // 若这里提前拦截，后端 beginEsp32OtaWithRetry 的同通道清理分支永远没有机会执行。
        // ERROR(8) 也放行给后端清理底层 OTA 句柄；FINALIZING(6)/REBOOTING(7) 是不可逆忙态。
        if (ssc === 5 && otaChannel === 1 && _pi < 3) continue;
        var esp32Busy = (ssc === 6 || ssc === 7 || (ssc === 5 && otaChannel !== 0));
        // GD32 STORING(1) 可由同一 WiFi 通道的新上传接管；RELAYING(3) 不可中断。
        var gd32Busy = (ssc === 3 || (ssc === 1 && otaChannel !== 0));
        if ((esp32Busy || gd32Busy)) {
          addLog('E', '上传', '设备已有 OTA 进行中 (sys_state=' + (ota.sys_state || ssc) + ', channel=' + otaChannelName + ')，拒绝本次 ' + chip.toUpperCase() + ' 上传');
          showToast('设备正在 OTA 中，请等待结束后再试', 'error');
          isUploading = false;
          resumePollingAfterUpload();
          return;
        }
      }
      _probeReady = true;
      break;
    } catch (e) {
      // 探测失败：可能是设备瞬时忙或网络问题。保守继续走，由后端 403 兜底。
      addLog('W', '上传', '跨通道互斥状态探测失败: ' + e.message + '（继续尝试，由设备端兜底）');
      _probeReady = true;
      break;
    }
  }
  // 每次上传前尝试重新从本地读取文件
  var handle = chip === 'esp32' ? esp32FileHandle : gd32FileHandle;
  if (handle) {
    // 有 FileSystemFileHandle，调用 getFile() 获取最新内容
    try {
      var freshFile = await handle.getFile();
      if (chip === 'esp32') {
        esp32File = freshFile;
        file = freshFile;
      } else {
        if (freshFile.name.toLowerCase().endsWith('.hex')) {
          processFile(freshFile, 'gd32');
          await new Promise(function(resolve) { setTimeout(resolve, 200); });
          file = gd32File;
        } else {
          gd32File = freshFile;
          file = freshFile;
        }
      }
      addLog('I', '文件', '已重新读取本地文件: ' + file.name + ' (' + formatBytes(file.size) + ')');
    } catch (e) {
      addLog('W', '文件', '重新读取文件失败: ' + e.message + '，将使用缓存版本');
    }
  } else {
    // 没有 handle，尝试直接读取当前 File 对象
    try {
      await file.arrayBuffer();
    } catch (e) {
      // File 引用已失效，提示用户重新选择
      addLog('E', '文件', '本地文件已变更或失效，请重新选择文件');
      showToast('文件已变更，请重新选择', 'error');
      clearFile(chip);
      isUploading = false;
      resumePollingAfterUpload();
      return;
    }
  }
  var uploadBtn = document.getElementById(chip === 'esp32' ? 'uploadEsp32Btn' : 'uploadGd32Btn');
  uploadBtnOrigText = uploadBtn.textContent;
  uploadBtn.disabled = true;
  uploadBtn.textContent = '上传中...';
  switchStageFlow(chip);
  setActivePhase(chip === 'esp32' ? 'esp32Ota' : 'gd32Ota');
  if (chip === 'esp32') updateEsp32StagesFromPhase(1);
  // Reset verification panel
  var verifyInfo = document.getElementById('stageVerifyInfo');
  if (verifyInfo) verifyInfo.style.display = 'none';
  var sviFront = document.getElementById('sviFrontSha');
  var sviDevice = document.getElementById('sviDeviceSha');
  var sviResult = document.getElementById('sviResult');
  if (sviFront) sviFront.textContent = '-';
  if (sviDevice) sviDevice.textContent = '-';
  if (sviResult) { sviResult.textContent = '-'; sviResult.className = 'svi-val'; }
  addLog('I', '上传', '开始上传: ' + file.name + ' \u2192 ' + chip.toUpperCase());
  showToast('开始上传固件到 ' + chip.toUpperCase(), 'success');
  try {
    var arrayBuffer = await file.arrayBuffer();
    // 刷写时重新计算 SHA-256（ESP32 和 GD32 都算）
    if (chip === 'esp32') {
      try {
        var hashBuffer = await crypto.subtle.digest('SHA-256', arrayBuffer);
        var hashArray = Array.from(new Uint8Array(hashBuffer));
        var hashHex = hashArray.map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
        window._esp32FileSha256 = hashHex.toUpperCase();
        var shaElem = document.getElementById('fileSha256Esp32');
        if (shaElem) shaElem.textContent = window._esp32FileSha256;
        addLog('I', '文件', '刷写前重新计算 SHA-256: ' + window._esp32FileSha256);
      } catch (e) {
        addLog('W', '文件', 'SHA-256 重新计算失败: ' + e.message);
      }
    } else if (chip === 'gd32') {
      try {
        var hashBuffer = await crypto.subtle.digest('SHA-256', arrayBuffer);
        var hashArray = Array.from(new Uint8Array(hashBuffer));
        var hashHex = hashArray.map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
        window._gd32FileSha256 = hashHex.toUpperCase();
        addLog('I', '文件', 'GD32 SHA-256: ' + window._gd32FileSha256);
      } catch (e) {
        addLog('W', '文件', 'GD32 SHA-256 重新计算失败: ' + e.message);
        window._gd32FileSha256 = null;
      }
    }
    var fileSize = file.size;
    var body, url = getBaseUrl() + '/ota/upload';
    if (chip === 'esp32') {
      var headerObj = { type: 'esp32', total_size: fileSize };
      var payload = arrayBuffer;
      addLog('I', '上传', '直传: ' + formatBytes(fileSize));
      var headerJson = JSON.stringify(headerObj);
      var encoder = new TextEncoder();
      var headerBytes = encoder.encode(headerJson);
      var combined = new Uint8Array(headerBytes.length + payload.byteLength);
      combined.set(headerBytes, 0);
      combined.set(new Uint8Array(payload), headerBytes.length);
      body = combined.buffer;
    } else {
      var headerObj = { total_size: fileSize };
      var payload = arrayBuffer;
      addLog('I', '上传', '直传: ' + formatBytes(fileSize));
      var gd32Header = JSON.stringify(headerObj);
      var encoder = new TextEncoder();
      var headerBytes = encoder.encode(gd32Header);
      var combined = new Uint8Array(headerBytes.length + payload.byteLength);
      combined.set(headerBytes, 0);
      combined.set(new Uint8Array(payload), headerBytes.length);
      body = combined.buffer;
    }
    activeXhr = new XMLHttpRequest();
    var xhr = activeXhr;
    xhr.upload.addEventListener('progress', function (e) {
      if (e.lengthComputable) {
        var pct = Math.round((e.loaded / e.total) * 100);
        uploadProgress = pct;
        if (chip === 'gd32') {
          var overallPct = Math.min(45, Math.round(pct * 0.45));
          setProgress(overallPct, '已上传 ' + formatBytes(e.loaded) + ' / ' + formatBytes(e.total));
        } else {
          setProgress(pct, '已上传 ' + formatBytes(e.loaded) + ' / ' + formatBytes(e.total));
        }
      }
    });
    xhr.addEventListener('load', function () {
      isUploading = false;
      activeXhr = null;
      resumePollingAfterUpload();
      uploadBtn.disabled = false;
      uploadBtn.textContent = uploadBtnOrigText;
      updateUploadButtonLabel();
      if (xhr.status === 200) {
        addLog('I', '上传', '固件上传完成！');
        showToast(chip === 'gd32' ? '上传完成，GD32 OTA 已自动开始' : '固件上传成功', 'success');
        setProgress(100, '上传完成');
        if (chip === 'esp32') {
          markPhaseDone('esp32Ota');
          updateEsp32StagesFromPhase(2);  // OTA 写入阶段
          addLog('I', 'OTA', '固件上传完成，准备 SHA-256 校验...');
          verifyEsp32Firmware();
        } else {
          switchStageFlow('gd32');
          setActivePhase('gd32Ota');
          setProgress(45, '上传完成，校验并触发 GD32 OTA...');
          triggerGd32Relay();
        }
      } else if (isOtaBusyError(xhr.status, formatHttpErrorDetail(xhr.responseText))) {
        var busyDetail = formatHttpErrorDetail(xhr.responseText);
        addLog('E', '上传', chip.toUpperCase() + ' OTA 被其他通道占用（BLE/WiFi 互斥）' + (busyDetail ? ': ' + busyDetail : ''));
        showToast('OTA 通道被占用，请稍后重试', 'error');
        cleanupAfterOtaFailure('通道被占用');
      } else {
        var detail = formatHttpErrorDetail(xhr.responseText);
        var message = '上传失败: HTTP ' + xhr.status + (detail ? ' - ' + detail : '');
        addLog('E', '上传', message);
        showToast(detail ? ('上传失败: ' + detail) : '上传失败', 'error');
        cleanupAfterOtaFailure('HTTP ' + xhr.status);
      }
    });
    xhr.addEventListener('error', function () {
      if (activeXhr) {
        cleanupAfterOtaFailure('网络错误');
        showToast('上传过程发生网络错误', 'error');
      }
    });
    xhr.addEventListener('abort', function () {
      if (activeXhr) {
        cleanupAfterOtaFailure('已中止');
        showToast('上传已中止', 'error');
      }
    });
    xhr.open('POST', url);
    xhr.send(body);
  } catch (e) {
    addLog('E', '上传', '上传失败: ' + e.message);
    showToast('上传失败: ' + e.message, 'error');
    cleanupAfterOtaFailure('上传异常');
  }
}
// ==================== ESP32 OTA 完成升级 ====================
// 一次调用完成：校验 SHA-256 → 设置启动分区 → 自动重启（无需确认按钮）
/** @description 验证 ESP32 固件 SHA-256 是否与设备端计算值一致。 */
async function verifyEsp32Firmware() {
  updateEsp32StagesFromPhase(3);  // 校验中
  var sha256 = window._esp32FileSha256;
  if (sha256) {
    addLog('I', 'OTA', '前端文件 SHA-256: ' + sha256);
    var verifyInfo = document.getElementById('stageVerifyInfo');
    if (verifyInfo) verifyInfo.style.display = 'block';
    var frontSha = document.getElementById('sviFrontSha');
    if (frontSha) frontSha.textContent = sha256;
  }

  showToast('ESP32 正在校验固件并应用...', 'success');
  try {
    // verify_only=false: 服务器端会先校验 SHA-256，匹配后自动设置启动分区并重启
    // 单次请求即完成全部流程，无需前端再发送"应用"请求
    var requestBody = {};
    if (sha256) requestBody.sha256 = sha256;
    requestBody.verify_only = false;

    var resp = await fetch(getBaseUrl() + '/ota/update', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody),
      signal: AbortSignal.timeout(120000)
    });

    var data = await resp.json();

    if (data.status === 'ok') {
      addLog('I', 'OTA', '设备端 SHA-256: ' + (data.sha256 || '-'));
      if (data.sha256) {
        var deviceSha = document.getElementById('sviDeviceSha');
        if (deviceSha) deviceSha.textContent = data.sha256;
      }
      var resultEl = document.getElementById('sviResult');
      if (resultEl) {
        if (sha256 && data.sha256 && sha256.toLowerCase() === data.sha256.toLowerCase()) {
          resultEl.textContent = '验证通过';
          resultEl.className = 'svi-val ok';
        } else {
          resultEl.textContent = '已应用（未比对 SHA）';
          resultEl.className = 'svi-val ok';
        }
      }
      // 服务器已设置启动分区并自动重启，前端直接进入重启等待状态
      addLog('I', 'OTA', 'SHA-256 校验通过，设备自动重启中...');
      showToast('校验通过，设备重启中...', 'success');
      showVerifyActions('none');
      updateEsp32StagesFromPhase(4);  // 应用固件
      isEsp32Rebooting = true;
      updateEsp32StagesFromPhase(5);  // 重启中
      rebootStartTime = Date.now();
    } else {
      var detail = data.message || (data.device ? '不匹配' : '未知错误');
      addLog('E', 'OTA', '校验失败: ' + detail);
      showToast('校验失败: ' + detail, 'error');
      var resultEl = document.getElementById('sviResult');
      if (resultEl) { resultEl.textContent = '✗ ' + detail; resultEl.className = 'svi-val err'; }
      showVerifyActions('retry');
      cleanupAfterOtaFailure('校验失败');
    }
  } catch (e) {
    addLog('W', 'OTA', '应用请求出现异常（设备可能已重启）: ' + e.message);
    // 设备在收到请求后即会重启，网络中断属于正常现象，直接进入重启等待
    showVerifyActions('none');
    isEsp32Rebooting = true;
    updateEsp32StagesFromPhase(5);
    rebootStartTime = Date.now();
  }
}

async function retryEsp32Upload() {
  addLog('I', 'OTA', '重新上传固件...');
  showVerifyActions('none');
  var verifyInfo = document.getElementById('stageVerifyInfo');
  if (verifyInfo) verifyInfo.style.display = 'none';
  var sviFront = document.getElementById('sviFrontSha');
  var sviDevice = document.getElementById('sviDeviceSha');
  var sviResult = document.getElementById('sviResult');
  if (sviFront) sviFront.textContent = '-';
  if (sviDevice) sviDevice.textContent = '-';
  if (sviResult) { sviResult.textContent = '-'; sviResult.className = 'svi-val'; }
  startUpload('esp32');
}

async function cancelEsp32Ota() {
  addLog('I', 'OTA', '用户取消 OTA 刷写');
  showToast('已取消 OTA', 'error');
  showVerifyActions('none');
  var xhr = activeXhr;
  activeXhr = null;
  if (xhr) {
    try { xhr.abort(); } catch (_) {}
  }
  cleanupAfterOtaFailure('用户取消');
}

async function applyEsp32Firmware() {
  addLog('I', 'OTA', '确认应用固件并重启 ESP32...');
  showToast('正在应用固件并重启...', 'success');
  try {
    var resp = await fetch(getBaseUrl() + '/ota/update', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({}),
      signal: AbortSignal.timeout(120000)
    });

    if (resp.ok) {
      var data = await resp.json();
      addLog('I', 'OTA', '升级完成: ' + (data.message || ''));
      updateEsp32StagesFromPhase(4);  // 应用固件
      isEsp32Rebooting = true;
      updateEsp32StagesFromPhase(5);  // 重启中
      rebootStartTime = Date.now();
    } else {
      var detail = '应用失败: HTTP ' + resp.status;
      try { var d = await resp.json(); detail += ' - ' + (d.message || ''); } catch (_) {}
      addLog('E', 'OTA', detail);
      showToast(detail, 'error');
    }
  } catch (e) {
    addLog('W', 'OTA', '应用请求出现异常（设备可能已重启）: ' + e.message);
    isEsp32Rebooting = true;
    updateEsp32StagesFromPhase(5);
    rebootStartTime = Date.now();
  }
}


async function restartEsp32() {
  if (!confirm('确定要重启 ESP32 吗？')) return;
  addLog('I', '系统', '=== 请求重启 ESP32 ===');
  showToast('ESP32 正在重启...', 'success');
  try {
    var resp = await fetch(getBaseUrl() + '/restart', {
      method: 'POST', signal: AbortSignal.timeout(5000)
    });
    if (resp.ok) {
      var data = await resp.json();
      addLog('I', '系统', '重启已触发: ' + data.message);
      showToast('ESP32 重启中，页面将自动恢复...', 'success');
      isEsp32Rebooting = true;
      rebootStartTime = Date.now();
    }
  } catch (e) {
    addLog('E', '系统', '重启请求失败: ' + e.message);
    showToast('重启失败', 'error');
  }
}
// ==================== GD32 OTA 触发（带 SHA-256 校验，对齐 BLE 0xF7）====================
/** @description 触发 GD32 中继 OTA 流程（通过 HTTP 接口）。 */
async function triggerGd32Relay() {
  var sha256 = window._gd32FileSha256;
  if (!sha256) {
    addLog('E', 'OTA', '缺少 GD32 文件 SHA-256，请先上传文件');
    showToast('SHA-256 缺失', 'error');
    cleanupAfterOtaFailure('SHA-256 缺失');
    return;
  }
  addLog('I', 'OTA', 'GD32 SHA-256: ' + sha256 + ' → 触发中继...');
  showToast('正在触发 GD32 OTA', 'success');
  try {
    var resp = await fetch(getBaseUrl() + '/ota/relay', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sha256: sha256 }),
      signal: AbortSignal.timeout(30000)  // 中继可能耗时较长
    });
    var text = await resp.text();
    if (resp.ok) {
      try {
        var data = JSON.parse(text);
        if (data.sha256) addLog('I', 'OTA', '设备端 SHA-256: ' + data.sha256);
      } catch (_) {}
      addLog('I', 'OTA', 'GD32 OTA 已触发，等待设备响应...');
      showToast('GD32 OTA 已触发', 'success');
    } else {
      // 尝试解析错误响应中的 device SHA
      try {
        var err = JSON.parse(text);
        if (err.device) {
          addLog('E', 'OTA', 'GD32 SHA-256 不匹配: 设备=' + err.device + ' 前端=' + sha256);
          showToast('SHA-256 不匹配', 'error');
          cleanupAfterOtaFailure('SHA-256 不匹配');
          return;
        }
      } catch (_) {}
      addLog('E', 'OTA', 'GD32 触发失败: HTTP ' + resp.status + ' ' + text);
      showToast('GD32 触发失败', 'error');
      cleanupAfterOtaFailure('触发失败 HTTP ' + resp.status);
    }
  } catch (e) {
    addLog('E', 'OTA', 'GD32 触发失败: ' + e.message);
    showToast('GD32 触发失败', 'error');
    cleanupAfterOtaFailure('触发异常');
  }
}

// ==================== 手动触发 GD32 OTA (调试/旧页面兜底) ====================
/** @description 触发 GD32 OTA 中继（封装函数，调用实际 API）。 */
async function triggerRelay() {
  // 转发到统一入口：自动使用最近一次上传的 GD32 文件 SHA
  await triggerGd32Relay();
}
// ==================== 轮询状态 ====================
var consecutivePollFailures = 0;
var statusTimerId = null;
var logsTimerId = null;
var pollSlowMode = false;   // 降速模式（10s 一次）
var lastDiagTime = 0;       // 上次诊断时间
var diagResult = '';        // 最近一次诊断结果
// 从设备 IP 推导网关 IP（取前三段 + .1）
function getGatewayIp() {
  var parts = getEspIp().split('.');
  if (parts.length === 4) return parts[0] + '.' + parts[1] + '.' + parts[2] + '.1';
  return '192.168.31.1';
}
// 网络诊断：区分浏览器离线 / 网关不通 / 仅设备不通
async function diagnoseNetwork() {
  lastDiagTime = Date.now();
  var result = [];
  // 1) 浏览器是否在线
  if (navigator.onLine === false) {
    result.push('浏览器无网络连接');
    diagResult = result.join('；');
    return diagResult;
  }
  // 2) 尝试触达网关（超时 2s）
  var gwIp = getGatewayIp();
  var gwOk = false;
  try {
    await fetch('http://' + gwIp + '/', { mode: 'no-cors', signal: AbortSignal.timeout(2000) });
    gwOk = true; // no-cors 模式下不抛异常即视为可达
  } catch (_) {}
  if (!gwOk) {
    // 再试一次 favicon（有些路由器不响应 /）
    try {
      await fetch('http://' + gwIp + '/favicon.ico', { mode: 'no-cors', signal: AbortSignal.timeout(1500) });
      gwOk = true;
    } catch (_) {}
  }
  if (!gwOk) {
    result.push('网关 ' + gwIp + ' 不可达（本地网络不通）');
  } else {
    result.push('网关 ' + gwIp + ' 可达');
    // 3) 网关通但设备不通 → 设备自身问题
    result.push('设备 ' + getEspIp() + ' 不响应（可能断电、重启中或 WiFi 断开）');
  }
  diagResult = result.join('；');
  return diagResult;
}
/** @description 启动定时轮询（状态 + 日志）。 */
function startPolling() {
  consecutivePollFailures = 0;
  pollSlowMode = false;
  if (statusTimerId) clearInterval(statusTimerId);
  if (logsTimerId) clearInterval(logsTimerId);
  statusTimerId = setInterval(pollStatus, 1000);
  logsTimerId = setInterval(pollLogs, 1000);
}
function slowDownPolling() {
  pollSlowMode = true;
  if (statusTimerId) clearInterval(statusTimerId);
  if (logsTimerId) clearInterval(logsTimerId);
  statusTimerId = setInterval(pollStatus, 10000);
  logsTimerId = setInterval(pollLogs, 10000);
}
function restorePolling() {
  if (pollSlowMode) {
    pollSlowMode = false;
    consecutivePollFailures = 0;
    if (statusTimerId) clearInterval(statusTimerId);
    if (logsTimerId) clearInterval(logsTimerId);
    statusTimerId = setInterval(pollStatus, 1000);
    logsTimerId = setInterval(pollLogs, 1000);
  }
}
function pausePollingForUpload() {
  if (pollingPausedForUpload) return;
  pollingPausedForUpload = true;
  consecutivePollFailures = 0;
  if (statusTimerId) clearInterval(statusTimerId);
  if (logsTimerId) clearInterval(logsTimerId);
  statusTimerId = null;
  logsTimerId = null;
}
function resumePollingAfterUpload() {
  if (!pollingPausedForUpload) return;
  pollingPausedForUpload = false;
  consecutivePollFailures = 0;
  if (pollSlowMode) {
    statusTimerId = setInterval(pollStatus, 10000);
    logsTimerId = setInterval(pollLogs, 10000);
  } else {
    statusTimerId = setInterval(pollStatus, 1000);
    logsTimerId = setInterval(pollLogs, 1000);
  }
  pollStatus();
  pollLogs();
}
/** @description 轮询 ESP32 系统状态（GET /status）并更新 UI。 */
async function pollStatus() {
  if (pollingPausedForUpload) return;
  try {
    var resp = await fetch(getBaseUrl() + '/system/status', {
      signal: AbortSignal.timeout(3000)
    });
    if (resp.ok) {
      var data = await resp.json();
      // 恢复：如果之前在降速模式，切回 1 秒轮询
      if (consecutivePollFailures > 0 || pollSlowMode) {
        var wasSlow = pollSlowMode;
        consecutivePollFailures = 0;
        if (wasSlow) {
          restorePolling();
          addLog('I', '系统', '设备已恢复在线，轮询恢复正常');
          showToast('设备已恢复在线', 'success');
        }
      }
      if (isEsp32Rebooting) {
        isEsp32Rebooting = false;
        updateEsp32StagesFromPhase(6);  // 启动完成
        addLog('I', '系统', 'ESP32 已恢复在线');
        showToast('ESP32 已恢复在线', 'success');
      }
      try { updateSystemStatusUi(data); } catch (e2) { console.warn('updateSystemStatusUi 异常:', e2); }
    }
  } catch (e) {
    // 后台期间 setInterval 会被 Chrome/Edge 节流到最低 1 分钟/次，
    // 因此后台累积的失败计数不应递增，避免切回前台瞬间误触发 5-fail 诊断。
    if (pageVisible) consecutivePollFailures++;
    // OTA/传输/重启中屏蔽网关无响应的检测功能（设备忙或正在重启，无法响应轮询是正常的）
    if (pageVisible && consecutivePollFailures >= 5 && !pollSlowMode && !isUploading && !isEsp32Rebooting && !currentOtaTarget) {
      addLog('W', '诊断', '设备连续 5 次无响应，正在诊断网络...');
      var diag = await diagnoseNetwork();
      addLog('W', '诊断', diag);
      updateConnectionUi(false, '未连接');
      applyDisconnectedRuntimeState();
      var cst = document.getElementById('connStatusTxt');
      if (cst) cst.textContent = '诊断中: ' + diag;
      // 降速到 10 秒继续监测，等待设备恢复
      slowDownPolling();
      addLog('I', '系统', '已切换为慢速轮询（10s），持续监测设备恢复...');
    }
    if (isUploading) {
      applyUploadingRuntimeState();
    } else if (isEsp32Rebooting) {
      var elapsed = Math.round((Date.now() - rebootStartTime) / 1000);
      if (elapsed > 60) {
        isEsp32Rebooting = false;
        addLog('E', '系统', 'ESP32 重启超时，设备未恢复');
        showToast('ESP32 重启超时，请检查设备', 'error');
        updateConnectionUi(false, '未连接');
        applyDisconnectedRuntimeState();
      } else {
        applyRebootingRuntimeState();
      }
    } else if (!pollSlowMode) {
      updateConnectionUi(false, '未连接');
      applyDisconnectedRuntimeState();
    }
  }
}
startPolling();
// ==================== Page Visibility 补偿 ====================
// Chrome/Edge 在标签隐藏/窗口最小化时会把 setInterval 节流到最低 1 分钟/次。
// 切回前台时立即拉一次状态 + 日志，并清零后台期间累积的失败计数，
// 避免在"刚切回前台那一轮"被误判为 5-fail 离线。
document.addEventListener('visibilitychange', function () {
  pageVisible = !document.hidden;
  if (!pageVisible) return;
  consecutivePollFailures = 0;
  pollStatus();
  pollLogs();
  // 之前若在慢速模式（10s），切回前台先用 1s 观察一会儿
  if (pollSlowMode) restorePolling();
});
// ==================== ESP32 实时日志拉取 (增量) ====================
var lastLogTimestamp = 0;
/** @description 轮询 ESP32 日志（GET /logs）并追加到日志面板。 */
async function pollLogs() {
  if (pollingPausedForUpload) return;
  try {
    var resp = await fetch(getBaseUrl() + '/logs', {
      signal: AbortSignal.timeout(2000)
    });
    if (!resp.ok) return;
    var logs = await resp.json();
    if (!Array.isArray(logs) || logs.length === 0) return;
    var levelMap = { E: 'E', W: 'W', I: 'I', D: 'D' };
    logs.forEach(function (line) {
      var m = line.match(/^\[([EWID])\]\[([^\]]+)\]\s*(.*)$/);
      if (m) {
        addLog(levelMap[m[1]] || 'I', m[2], m[3].trimEnd());
      } else {
        addLog('I', 'ESP32', line.trimEnd());
      }
    });
  } catch (e) { /* 静默 */ }
}
setTimeout(function () {
  updateUploadButtonLabel();
  updateCanHint();
  addLog('I', '系统', '控制台初始化完成，等待固件文件...');
  updateSpiHint();
  // IP 输入变化时自动恢复轮询
  var ipInput = document.getElementById('espIp');
  if (ipInput) {
    ipInput.addEventListener('change', function () {
      if (pollSlowMode) {
        addLog('I', '系统', 'IP 已修改，重新启动轮询...');
        restorePolling();
      }
    });
  }
}, 500);
