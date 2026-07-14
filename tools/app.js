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
let rebootObservedDown = false;  // 是否已观察到设备实际下线（重启），用于避免 pre-reboot 窗口内误清除重启状态
let pollingPausedForUpload = false;
let uploadPollChip = null; // 当前暂停轮询的上传目标，用于区分 WiFi/日志策略
let activeXhr = null;           // 当前上传 XHR，用于 abort
let uploadBtnOrigText = '';     // 上传按钮原始文字
let activeUploadChip = null;    // 当前上传目标芯片 ('esp32' | 'gd32' | null)
let uploadWaitHeartbeatId = null; // 发完 body 后等待 HTTP 响应的心跳定时器
let uploadWaitStartMs = 0;

/** @description 停止「等待设备 HTTP 响应」心跳，避免残留定时器改进度条。 */
function stopUploadWaitHeartbeat() {
  if (uploadWaitHeartbeatId) {
    clearInterval(uploadWaitHeartbeatId);
    uploadWaitHeartbeatId = null;
  }
  uploadWaitStartMs = 0;
}

/**
 * @description 浏览器 body 已发完但尚未收到 HTTP 响应时启动心跳。
 *              上传期轮询已暂停，若无此反馈会像“卡在 100%”。
 */
function startUploadWaitHeartbeat() {
  stopUploadWaitHeartbeat();
  uploadWaitStartMs = Date.now();
  uploadWaitHeartbeatId = setInterval(function () {
    if (!activeXhr) {
      stopUploadWaitHeartbeat();
      return;
    }
    var sec = Math.round((Date.now() - uploadWaitStartMs) / 1000);
    setProgress(100, '数据已发完，设备处理中… 已等待 ' + sec + 's');
    // 每 10s 打一条日志，便于区分“仍在处理”与“真死锁”
    if (sec > 0 && sec % 10 === 0) {
      addLog('I', '上传', '仍在等待设备 HTTP 响应（已 ' + sec +
        's；设备可能仍在写 Flash / 算 SHA，或连接异常）');
    }
  }, 5000);
}

// ==================== 前端 OTA 会话状态机 ====================
// 统一管理一次 OTA 会话的完整生命周期，消除散布的布尔标志
// (isUploading / currentOtaTarget / activeUploadChip / isEsp32Rebooting)
// 在网络错误后被轮询错误覆盖、状态混乱的问题。
//
// 状态流转：
//   IDLE → UPLOADING → (ESP32: VERIFYING → REBOOTING) → DONE
//   IDLE → UPLOADING → (GD32:  RELAYING)              → DONE
//   任意活跃态 ──(failure/abort)──→ FAILED
//   FAILED/DONE ──(startUpload)──→ UPLOADING
//   FAILED/DONE ──(device idle detected by poll)──→ IDLE
var OtaSessionState = {
  IDLE: 'idle',          // 无会话
  UPLOADING: 'uploading', // 固件上传中（XHR 上传阶段）
  VERIFYING: 'verifying', // ESP32 SHA-256 校验 + 应用中
  RELAYING: 'relaying',   // GD32 中继转发中（triggerGd32Relay 发起后）
  REBOOTING: 'rebooting', // ESP32 重启等待中
  DONE: 'done',           // 会话成功完成
  FAILED: 'failed'        // 会话失败/中止
};
var otaSessionState = OtaSessionState.IDLE;
var otaSessionTarget = null;  // 'esp32' | 'gd32' | null — 本会话的目标芯片
var otaFailReason = '';       // FAILED 时的失败原因（用于 UI 显示）

/** @description 查询当前 OTA 会话是否处于活跃态（非 IDLE/DONE/FAILED）。 */
function otaSessionActive() {
  return otaSessionState === OtaSessionState.UPLOADING ||
         otaSessionState === OtaSessionState.VERIFYING ||
         otaSessionState === OtaSessionState.RELAYING ||
         otaSessionState === OtaSessionState.REBOOTING;
}

/** @description 查询当前 OTA 会话是否已终结（DONE 或 FAILED）。 */
function otaSessionTerminated() {
  return otaSessionState === OtaSessionState.DONE ||
         otaSessionState === OtaSessionState.FAILED;
}

/**
 * @description 转换 OTA 会话状态。
 *              统一在此处维护状态机一致性，同时同步旧标志位
 *              (isUploading / currentOtaTarget)，保证渐进式迁移期间两套标志不冲突。
 * @param {string} newState 目标状态。
 * @param {string} [reason] 失败原因（仅转入 FAILED 时使用）。
 */
function otaSessionTransition(newState, reason) {
  var prev = otaSessionState;
  otaSessionState = newState;
  if (newState === OtaSessionState.FAILED) {
    otaFailReason = reason || '未知原因';
  } else if (newState === OtaSessionState.UPLOADING) {
    otaFailReason = '';
  }
  // 同步旧标志（渐进式迁移）：UPLOADING 对应 isUploading=true
  if (newState === OtaSessionState.UPLOADING) isUploading = true;
  else if (newState === OtaSessionState.IDLE ||
           newState === OtaSessionState.DONE ||
           newState === OtaSessionState.FAILED) isUploading = false;
  // REBOOTING 复用 isEsp32Rebooting（pollStatus 重启检测依赖）
  if (newState === OtaSessionState.REBOOTING) isEsp32Rebooting = true;
  else if (newState !== OtaSessionState.VERIFYING) isEsp32Rebooting = false;
}

/**
 * @description 清掉本会话残留 UI/标志（失败、超时、中止、新会话开始前都要调）。
 *              不改变 otaSessionState（由调用方负责 FAILED/IDLE 转换）。
 * @param {object} [opts]
 * @param {boolean} [opts.keepErrorStages]  true=保留错误阶段样式；false=节点复位到待机
 * @param {string} [opts.statusText]    状态栏文案；null 表示「待机」
 * @param {boolean} [opts.errorStyle]   状态栏是否红色
 */
function clearOtaSessionResiduals(opts) {
  opts = opts || {};
  stopRelayProgressWatch();
  stopUploadWaitHeartbeat();

  gd32CompleteAnnounced = false;
  relayWatchLastSeq = -1;
  gd32RelaySessionLive = false;
  lastSysStateCode = -1;
  lastRelayState = 0;
  isEsp32Rebooting = false;
  rebootObservedDown = false;
  rebootStartTime = 0;
  isUploading = false;

  /* 先摘掉引用再 abort，避免 abort 事件回调再次进入 cleanup 递归 */
  var xh = activeXhr;
  activeXhr = null;
  if (xh) {
    try { xh.abort(); } catch (_) {}
  }

  // 校验面板
  var verifyInfo = document.getElementById('stageVerifyInfo');
  if (verifyInfo) verifyInfo.style.display = 'none';
  var sviFront = document.getElementById('sviFrontSha');
  var sviDevice = document.getElementById('sviDeviceSha');
  var sviResult = document.getElementById('sviResult');
  if (sviFront) sviFront.textContent = '-';
  if (sviDevice) sviDevice.textContent = '-';
  if (sviResult) {
    sviResult.textContent = '-';
    sviResult.className = 'svi-val';
  }

  // 阶段节点
  if (!opts.keepErrorStages) {
    ['es1','es2','es3','es4','es5','es6','s1','s2','s3','s4','s5','s6'].forEach(function(id) {
      var el = document.getElementById(id);
      if (el) el.className = 'stage-node';
    });
    ['esd1','esd2','esd3','esd4','esd5','esd6','sd1','sd2','sd3','sd4','sd5','sd6'].forEach(function(id) {
      var el = document.getElementById(id);
      if (el) el.textContent = '';
    });
    esp32OtaPhase = 0;
  }

  // 顶部步骤 pill
  ['ppEsp32', 'ppGd32Ota'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.classList.remove('active', 'done');
  });
  var fill = document.getElementById('progressFill');
  if (fill) fill.classList.remove('done');
  activePhase = null;

  // 状态栏
  var sb = document.getElementById('stageStatusBar');
  var st = document.getElementById('stageStatusText');
  if (sb) {
    sb.className = opts.errorStyle ? 'stage-status-bar err' : 'stage-status-bar';
  }
  if (st) {
    st.textContent = (opts.statusText !== undefined && opts.statusText !== null)
      ? opts.statusText
      : '待机';
  }

  // 设备侧缓存展示：失败后不要继续显示“已满缓存像成功”
  var mOtaCache = document.getElementById('mOtaCache');
  if (mOtaCache && opts.clearCacheDisplay) {
    mOtaCache.textContent = '0 / 216 KB';
    mOtaCache.className = 'v';
  }
  var sOtaStored = document.getElementById('sOtaStored');
  if (sOtaStored && opts.clearCacheDisplay) {
    sOtaStored.textContent = '--';
  }
}

/** @description 将会话从 FAILED/DONE 复位到 IDLE（用户发起新上传或设备空闲时调用）。 */
function otaSessionReset() {
  clearOtaSessionResiduals({
    keepErrorStages: false,
    statusText: '待机',
    errorStyle: false,
    clearCacheDisplay: false
  });
  if (otaSessionState !== OtaSessionState.IDLE) {
    otaSessionState = OtaSessionState.IDLE;
    otaSessionTarget = null;
    otaFailReason = '';
    currentOtaTarget = null;
    isUploading = false;
  }
  activeUploadChip = null;
}

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
 *
 * ★ 状态机改造：转入 FAILED 态，置位 otaFailReason。
 *   之后轮询（pollStatus/updateSystemStatusUi）不得覆盖此失败态，
 *   直到用户发起新上传（otaSessionReset）或检测到设备已回 IDLE。
 * @param {string} reason 失败原因描述
 */
function cleanupAfterOtaFailure(reason) {
  var chip = activeUploadChip || otaSessionTarget || currentOtaTarget || window.flashTarget || 'esp32';

  // 0. 状态机：转入 FAILED（先于标志重置）
  otaSessionTransition(OtaSessionState.FAILED, reason);

  // 1. 清掉本会话全部残留标志 / 定时器 / 校验面板 / 旧 progress 语义
  clearOtaSessionResiduals({
    keepErrorStages: true,
    statusText: reason || '失败',
    errorStyle: true,
    clearCacheDisplay: true
  });

  // 2. 阶段指示器标错
  if (chip === 'esp32') {
    updateEsp32StagesError();
  } else {
    updateStagesFromRelayState(255);
  }

  // 3. 进度条失败文案（clear 已把 fill 的 done 去掉）
  setProgress(0, reason || '刷写失败');
  setActivePhase(null);
  currentOtaTarget = null;

  // 4. 恢复上传按钮
  var btnId = (chip === 'gd32') ? 'uploadGd32Btn' : 'uploadEsp32Btn';
  var btn = document.getElementById(btnId);
  if (btn) {
    btn.disabled = false;
    if (uploadBtnOrigText) btn.textContent = uploadBtnOrigText;
  }
  activeUploadChip = null;
  otaSessionTarget = null;
  uploadBtnOrigText = '';

  updateUploadButtonLabel();

  // 5. 恢复轮询（之后 status 不得覆盖 FAILED UI，直至用户新上传或设备 idle 复位）
  resumePollingAfterUpload();

  addLog('E', 'OTA', '刷写流程已中止，已清理残留状态: ' + (reason || '未知原因'));
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
var _toastHideTimer = null;
var _toastLastKey = '';
var _toastLastAt = 0;
function showToast(message, type) {
  type = type || 'success';
  // 相同文案 5s 内不重复弹，避免轮询把 ✓ 刷成「疯狂跳」
  var key = type + '|' + message;
  var now = Date.now();
  if (key === _toastLastKey && (now - _toastLastAt) < 5000) {
    return;
  }
  _toastLastKey = key;
  _toastLastAt = now;

  const toast = document.getElementById('toast');
  const ico = document.getElementById('toastIco');
  const msg = document.getElementById('toastMsg');
  toast.className = 'toast ' + (type === 'success' ? 'ok' : type === 'error' ? 'err' : 'warn') + ' show';
  ico.textContent = type === 'success' ? '\u2713' : type === 'error' ? '\u2715' : '\u26A0';
  msg.textContent = message;
  if (_toastHideTimer) clearTimeout(_toastHideTimer);
  _toastHideTimer = setTimeout(function () {
    toast.classList.remove('show');
    _toastHideTimer = null;
  }, 3000);
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
  // ★ 状态机守卫：会话活跃时不重置进度/阶段，避免覆盖进行中的 OTA 进度
  if (otaSessionActive()) return;
  // FAILED 态：保留失败进度显示，只清连接胶囊（让用户看到失败原因）
  if (otaSessionState === OtaSessionState.FAILED) return;
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
  // ★ 状态机守卫：会话已失败/完成时，绝不重新应用"上传中"运行时态，
  //   防止网络抖动后的轮询错误分支把 UI 拉回"上传中"。
  if (otaSessionState === OtaSessionState.FAILED ||
      otaSessionState === OtaSessionState.DONE) {
    return;
  }
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
  // 缓存最近一次成功的 sys_state_code / relay_state，供 pollStatus 失败时判断设备是否在忙
  if (Number.isFinite(ota.sys_state_code)) lastSysStateCode = ota.sys_state_code;
  if (Number.isFinite(ota.relay_state)) lastRelayState = ota.relay_state;
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
  // 共享进度条按当前连接模式归属：BLE 模式下由 ble_ota.js 的设备 ACK
  // 驱动包数、阶段和完成状态，HTTP 状态轮询不能覆盖这套实时进度。
  if (window.connectionMode === 'ble') {
    return;
  }
  // relay state - skip GD32 relay UI when ESP32 OTA is active
  if (currentOtaTarget === 'esp32' && esp32OtaPhase > 0) {
    return;
  }
  // ★ 状态机守卫：本会话已 FAILED 时，禁止后端状态覆盖失败 UI。
  //   后端可能仍在跑 GD32 中继（网络断开不会停止设备端流程），
  //   若不拦截，会把进度条/阶段从"网络错误"错误地刷回"数据转发"。
  //   只有设备真正回 IDLE 才允许自动复位状态机（见下方 else 分支）。
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
  // GD32 relay forwarding progress — 以 /system/status 的 relay_seq 为准，不依赖日志
  var relaySeq = ota.relay_seq || 0;
  var relayTotal = ota.relay_total_packets || 0;
  var relayProgress = relayTotal > 0 ? Math.min(100, Math.round(relaySeq * 100 / relayTotal)) : 0;
  var gd32AppAlive = ota.gd32_app_alive;

  if (otaSessionState === OtaSessionState.FAILED) {
    var _deviceIdle = (sysStateCode === 0 || sysStateCode === 2 || sysStateCode === 4) &&
                      (rs === 0 || rs === 5);
    if (!_deviceIdle) {
      // 会话已失败，但设备可能仍在中继：继续用 relay_seq 刷进度，避免“假卡死”
      if ((rs === 3 || isGd32Relaying) && relayTotal > 0) {
        setActivePhase('gd32Ota');
        var failRelayOverall = 60 + Math.round(relayProgress * 0.30);
        setProgress(failRelayOverall,
          '已转发 ' + relaySeq + ' / ' + relayTotal + ' 包（设备仍在跑）');
        updateStagesFromRelayState(3);
      } else {
        updateStagesFromRelayState(255);
      }
      return;
    }
    // 设备已回 IDLE —— 自动复位状态机，恢复正常 UI
    otaSessionReset();
    stopRelayProgressWatch();
    // 落到下方正常分支刷新 UI
  }

  var displayRelayState = isGd32Complete ? 5 : (isGd32Relaying && rs === 0 ? 1 : rs);
  if (currentOtaTarget !== 'esp32') { updateStagesFromRelayState(displayRelayState); }

  if (isGd32Error && (currentOtaTarget === 'gd32' || otaSessionTarget === 'gd32')) {
    var failDetail = 'GD32 OTA 失败';
    if (relayTotal > 0) {
      failDetail += '（已转发 ' + relaySeq + '/' + relayTotal +
        '，relay_state=' + rs + '，sys=' + (sysState || sysStateCode) + '）';
    } else {
      failDetail += '（relay_state=' + rs + '，sys=' + (sysState || sysStateCode) + '）';
    }
    cleanupAfterOtaFailure(failDetail);
    pollLogs();
  } else if (isGd32Error) {
    // 非当前 OTA 会话的错误：仅标记阶段指示器，不触发完整清理
    updateStagesFromRelayState(255);
    var sbErr = document.getElementById('stageStatusBar');
    var stErr = document.getElementById('stageStatusText');
    if (sbErr) sbErr.className = 'stage-status-bar err';
    if (stErr) stErr.textContent = 'GD32 错误（待机）';
  } else if (otaSessionState === OtaSessionState.UPLOADING) {
    /* 上传进行中：不要用旧缓存/旧完成态覆盖进度条 */
  } else if (otaSessionState === OtaSessionState.RELAYING) {
    /* 必须先看到本会话真实中继（HANDSHAKE~FINISHING），才允许 DONE；
     * 禁止一进 RELAYING 就吃掉上一次的 rs=5 / app_alive。 */
    if (isGd32Relaying || (rs >= 1 && rs <= 4)) {
      gd32RelaySessionLive = true;
    }
    if (gd32RelaySessionLive &&
        (isGd32Complete || rs === 5 ||
         (gd32AppAlive && relayTotal > 0 && relaySeq >= relayTotal && rs >= 3))) {
      if (!gd32CompleteAnnounced) {
        markGd32OtaUiComplete('GD32 刷写完成');
      }
    } else if (rs === 1) {
      // HANDSHAKING
      setActivePhase('gd32Ota');
      setProgress(50, '握手与认证中...');
    } else if (rs === 3 || (relayTotal > 0 && relaySeq > 0 && rs !== 5)) {
      // TRANSFERRING
      setActivePhase('gd32Ota');
      var relayOverall = 60 + Math.round(relayProgress * 0.30);
      var relayLabel = relayTotal > 0
        ? ('已转发 ' + relaySeq + ' / ' + relayTotal + ' 包 (' + relayProgress + '%)')
        : '数据包传输中...';
      setProgress(relayOverall, relayLabel);
    } else if (rs === 2 || (isGd32Relaying && relaySeq === 0)) {
      // SIZE_NEG
      setActivePhase('gd32Ota');
      setProgress(55, relayTotal > 0
        ? ('大小协商中（共 ' + relayTotal + ' 包）')
        : '固件大小协商中');
    } else if (rs === 4) {
      setActivePhase('gd32Ota');
      if (!gd32CompleteAnnounced) {
        setProgress(95, gd32AppAlive ? '收尾中…' : '收尾中，等待 APP 启动...');
      }
    } else if (!gd32RelaySessionLive) {
      setActivePhase('gd32Ota');
      setProgress(48, '已触发中继，等待设备进入握手…');
    }
  } else if (isGd32Storing || (total > 0 && stored < total && !isUploading &&
             otaSessionState !== OtaSessionState.DONE &&
             otaSessionState !== OtaSessionState.IDLE)) {
    setActivePhase('gd32Ota');
    var storeOverall = Math.min(45, Math.round(storageProgress * 0.45));
    if (!isUploading) {
      setProgress(storeOverall, '已缓存 ' + formatBytes(stored) + ' / ' + formatBytes(total) + '');
    }
  }
}
/** @description 测试与 ESP32 的连接（GET /status）。 */
async function testConnection() {
  // 立即发一次请求检测
  // 手动检测使用独立的 AbortController，避免打断递归调度的 pollStatus
  var testAbort = new AbortController();
  var testTimer = setTimeout(function() { try { testAbort.abort(); } catch(_e) {} }, 3000);
  try {
    var resp = await fetch(getBaseUrl() + '/system/status', {
      signal: testAbort.signal
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
  } finally {
    clearTimeout(testTimer);
  }
}
async function refreshSystemStatusNow(event) {
  if (event) {
    event.preventDefault();
    event.stopPropagation();
  }
  // 手动刷新使用独立的 AbortController，避免与递归调度的 pollStatus 互相干扰
  var manualAbort = new AbortController();
  var manualTimer = setTimeout(function() { try { manualAbort.abort(); } catch(_e) {} }, 3000);
  try {
    var resp = await fetch(getBaseUrl() + '/system/status', {
      signal: manualAbort.signal
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
    addLog('I', '状态', '已手动刷新 /system/status');
  } catch (e) {
    addLog('E', '状态', '刷新失败: ' + e.message);
    showToast('状态刷新失败', 'error');
  } finally {
    clearTimeout(manualTimer);
  }
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
  // ★ 状态机守卫：用统一的会话活跃判断替代裸 isUploading。
  //   FAILED/DONE 会话需要先复位才能发起新上传（下方 otaSessionReset）。
  if (otaSessionActive()) {
    var busyChip = (otaSessionTarget || activeUploadChip || window.flashTarget || chip || 'ota').toUpperCase();
    addLog('W', '上传', '已有 ' + busyChip + ' OTA 会话进行中 (' + otaSessionState + ')，拒绝新的 ' + chip.toUpperCase() + ' 上传');
    showToast('OTA 通道正在占用，请等待当前刷写结束', 'error');
    return;
  }
  // 上一次会话已终结（DONE/FAILED）：发起新会话前先复位状态机
  if (otaSessionTerminated()) otaSessionReset();
  // 记录本会话目标
  otaSessionTarget = chip;

  pausePollingForUpload();
  // ★ 必须先抢锁再 await：守卫同步设置，await 期间重复点击才能被拦下。
  //   旧实现把 isUploading 设在 await handle.getFile() 之后 → 用户在 BLE 期间
  //   狂点 WiFi 上传按钮时，多个 startUpload 都能通过守卫，最终并发 xhr.send，
  //   设备端互斥被绕过、403 路径走漏。
  // ★ 状态机：进入 UPLOADING 态（内部同步 isUploading = true）
  otaSessionTransition(OtaSessionState.UPLOADING);
  activeUploadChip = chip;
  /* 新上传：清掉上次完成标志，避免立刻被 status 残留判成“又完成了” */
  gd32CompleteAnnounced = false;
  relayWatchLastSeq = -1;
  gd32RelaySessionLive = false;
  if (chip === 'esp32') {
    updateEsp32StagesFromPhase(1);
  }
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
          // ★ 状态机：预检失败也需完整清理（转入 FAILED）
          cleanupAfterOtaFailure('设备 OTA 占用中');
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
      // ★ 状态机：文件失效属于会话失败
      cleanupAfterOtaFailure('本地文件失效');
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
      addLog('I', '上传', '直传: ' + formatBytes(fileSize) +
        '（设备边收边写 Flash；进度 100% 后仍需等 HTTP 响应）');
      setProgress(1, '连接设备并开始写入…');
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
        } else if (pct >= 100) {
          setProgress(100, '数据已发完，等待设备写 Flash 并响应…');
        } else {
          setProgress(pct, '已上传 ' + formatBytes(e.loaded) + ' / ' + formatBytes(e.total));
        }
      }
    });
    xhr.upload.addEventListener('load', function () {
      // 请求体发送完毕 ≠ 服务端处理完毕
      setProgress(100, '数据已发完，等待设备写 Flash 并响应…');
      addLog('I', '上传', '浏览器已发完全部数据，等待 ESP32 HTTP 响应…');
      startUploadWaitHeartbeat();
    });
    // 1MB+ 固件：服务端写 Flash 可能较久；禁止无限挂起无反馈
    xhr.timeout = 300000;
    xhr.addEventListener('timeout', function () {
      stopUploadWaitHeartbeat();
      if (activeXhr) {
        cleanupAfterOtaFailure('上传超时（设备未在 5 分钟内响应）');
        showToast('上传超时：设备可能忙或连接被踢，请重试', 'error');
      }
    });
    xhr.addEventListener('load', function () {
      stopUploadWaitHeartbeat();
      isUploading = false;
      activeXhr = null;
      resumePollingAfterUpload();
      uploadBtn.disabled = false;
      uploadBtn.textContent = uploadBtnOrigText;
      updateUploadButtonLabel();
      if (xhr.status === 200) {
        /* 校验 body：避免空响应/网关误 200 被当成上传成功 */
        var bodyTxt = (xhr.responseText || '').trim();
        var bodyOk = false;
        try {
          var bj = bodyTxt ? JSON.parse(bodyTxt) : null;
          bodyOk = !!(bj && (bj.status === 'ok' || bj.target === chip));
        } catch (_pe) {
          bodyOk = bodyTxt.indexOf('"status"') >= 0 && bodyTxt.indexOf('ok') >= 0;
        }
        if (!bodyOk) {
          addLog('E', '上传', 'HTTP 200 但响应异常: ' + (bodyTxt.slice(0, 120) || '(空)'));
          showToast('上传响应异常，请重试', 'error');
          cleanupAfterOtaFailure('响应异常');
          return;
        }
        // 成功路径只打一条「上传完成」日志，避免 [上传]+[OTA] 语义重复
        if (chip === 'esp32') {
          addLog('I', 'OTA', '上传完成，开始 SHA-256 校验…');
          showToast('固件上传成功', 'success');
          setProgress(100, '上传完成');
          markPhaseDone('esp32Ota');
          updateEsp32StagesFromPhase(2);  // OTA 写入阶段
          // ★ 状态机：ESP32 上传完成 → 进入 VERIFYING（verifyEsp32Firmware 内部继续推进）
          otaSessionTransition(OtaSessionState.VERIFYING);
          verifyEsp32Firmware();
        } else {
          addLog('I', 'OTA', '上传完成，校验 SHA 并触发 GD32 中继…');
          showToast('上传完成，GD32 OTA 已自动开始', 'success');
          switchStageFlow('gd32');
          setActivePhase('gd32Ota');
          setProgress(45, '上传完成，校验并触发 GD32 OTA…');
          // ★ 状态机：GD32 上传完成 → 进入 RELAYING（triggerGd32Relay 内部驱动中继）
          otaSessionTransition(OtaSessionState.RELAYING);
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
      stopUploadWaitHeartbeat();
      if (activeXhr) {
        cleanupAfterOtaFailure('网络错误');
        showToast('上传过程发生网络错误', 'error');
      }
    });
    xhr.addEventListener('abort', function () {
      stopUploadWaitHeartbeat();
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
    // 带 sha256 时设备端校验、set_boot 并自动重启；无需二次“应用”请求
    var requestBody = {};
    if (sha256) requestBody.sha256 = sha256;

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
      updateEsp32StagesFromPhase(4);  // 应用固件
      rebootObservedDown = false;    // 重置：尚未观察到设备实际下线
      updateEsp32StagesFromPhase(5);  // 重启中
      rebootStartTime = Date.now();
      // ★ 状态机：校验通过 → 进入 REBOOTING（内部同步 isEsp32Rebooting = true）
      otaSessionTransition(OtaSessionState.REBOOTING);
    } else {
      var detail = data.message || (data.device ? '不匹配' : '未知错误');
      addLog('E', 'OTA', '校验失败: ' + detail);
      showToast('校验失败: ' + detail, 'error');
      var resultEl = document.getElementById('sviResult');
      if (resultEl) { resultEl.textContent = '✗ ' + detail; resultEl.className = 'svi-val err'; }
      cleanupAfterOtaFailure('校验失败');
    }
  } catch (e) {
    // fetch 异常仅代表请求未完成，不能推断设备已重启：
    // 设备可能仍在 finalize（SHA 计算耗时）、可能已崩溃、可能网络抖动。
    // 如实报告失败，由 pollStatus 探测设备真实状态。
    addLog('E', 'OTA', '应用请求失败: ' + e.message + '（设备状态未知，等待轮询探测）');
    showToast('应用请求失败: ' + e.message, 'error');
    var resultEl = document.getElementById('sviResult');
    if (resultEl) { resultEl.textContent = '✗ 请求失败: ' + e.message; resultEl.className = 'svi-val err'; }
    cleanupAfterOtaFailure('应用请求失败');
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
      rebootObservedDown = false;    // 重置：尚未观察到设备实际下线
      rebootStartTime = Date.now();
    }
  } catch (e) {
    addLog('E', '系统', '重启请求失败: ' + e.message);
    showToast('重启失败', 'error');
  }
}
// ==================== GD32 OTA 触发（带 SHA-256 校验，对齐 BLE 0xF7）====================
/**
 * GD32 中继进度观察：以 /system/status 为唯一真相源。
 * 日志环只有 64 条，中继/擦除期间 INFO 会被挤掉，时间戳还是浏览器本地时间——
 * 绝不能用「最后一条日志停在 SEND_SIZE」判断卡死。
 */
var relayWatchTimerId = null;
var relayWatchLastUiLogAt = 0;
var relayWatchInFlight = false;
var gd32CompleteAnnounced = false;
var relayWatchLastSeq = -1;
/** 本会话是否已观察到真实中继态（1~4），避免旧 relay_state=5 残留导致一点击就“完成” */
var gd32RelaySessionLive = false;

function markGd32OtaUiComplete(msg) {
  // 已公告过：禁止再改 className / setProgress / toast，否则每 400ms 重触发动效会「疯狂跳 ✓」
  if (gd32CompleteAnnounced) {
    return;
  }
  /* 仅本会话 RELAYING/FAILED 可宣布完成；IDLE/UPLOADING 时 status 里的旧 relay 残留一律忽略 */
  if (otaSessionState !== OtaSessionState.RELAYING &&
      otaSessionState !== OtaSessionState.FAILED) {
    return;
  }
  gd32CompleteAnnounced = true;
  stopRelayProgressWatch();

  markPhaseDone('gd32Ota');
  currentOtaTarget = null;
  setProgress(100, msg || 'GD32 刷写完成');
  var sbDone = document.getElementById('stageStatusBar');
  var stDone = document.getElementById('stageStatusText');
  if (sbDone) sbDone.className = 'stage-status-bar ok';
  if (stDone) stDone.textContent = msg || 'GD32 刷写完成';
  if (otaSessionState === OtaSessionState.RELAYING ||
      otaSessionState === OtaSessionState.FAILED) {
    otaSessionTransition(OtaSessionState.DONE);
  }
  addLog('I', 'OTA', msg || 'GD32 刷写完成');
  showToast(msg || 'GD32 刷写完成', 'success');
}

function stopRelayProgressWatch() {
  if (relayWatchTimerId) {
    clearInterval(relayWatchTimerId);
    relayWatchTimerId = null;
  }
  relayWatchInFlight = false;
  if (statusPollIntervalMs < 1000) {
    statusPollIntervalMs = 1000;
  }
}

function startRelayProgressWatch() {
  stopRelayProgressWatch();
  if (pollingPausedForUpload) {
    resumePollingAfterUpload();
  }
  statusPollIntervalMs = 400;
  scheduleStatusPoll(0);
  pollLogs();
  addLog('I', 'OTA', '跟踪中继进度：看进度条与 relay_seq（/system/status），勿只看日志');
  relayWatchTimerId = setInterval(function () {
    var active = otaSessionState === OtaSessionState.RELAYING ||
                 (otaSessionState === OtaSessionState.FAILED &&
                  (lastSysStateCode === 3 || lastRelayState >= 1 && lastRelayState <= 4));
    if (!active) {
      if (otaSessionState === OtaSessionState.DONE) {
        stopRelayProgressWatch();
      }
      return;
    }
    if (relayWatchInFlight) return;
    relayWatchInFlight = true;
    fetch(getBaseUrl() + '/system/status', { signal: AbortSignal.timeout(3000) })
      .then(function (resp) { return resp.ok ? resp.json() : null; })
      .then(function (data) {
        if (!data) return;
        try { updateSystemStatusUi(data); } catch (e) { /* ignore */ }
        var o = data.ota || {};
        var code = Number(o.sys_state_code);
        var rstate = Number(o.relay_state);
        var alive = !!o.gd32_app_alive;
        var rseq = Number(o.relay_seq) || 0;
        var rtot = Number(o.relay_total_packets) || 0;
        if (rstate >= 1 && rstate <= 4) {
          gd32RelaySessionLive = true;
        }
        if (gd32RelaySessionLive &&
            (code === 4 || rstate === 5 ||
             (alive && rtot > 0 && rseq >= rtot && rstate >= 3))) {
          markGd32OtaUiComplete('GD32 刷写完成');
          pollLogs();
          return;
        }
        if (code === 8 || rstate === 255) {
          stopRelayProgressWatch();
          pollLogs();
          return;
        }
        // 前端进度同步：包数变化时更新进度条（updateSystemStatusUi 已做）并写一条前端日志
        // 后端不再打「转发进度」文字，只靠这里 + 进度条
        if (rtot > 0 && rseq > 0 && rseq !== relayWatchLastSeq) {
          relayWatchLastSeq = rseq;
          var pct = Math.round(rseq * 100 / rtot);
          setProgress(60 + Math.round(pct * 0.30),
            '已转发 ' + rseq + ' / ' + rtot + ' 包 (' + pct + '%)');
          addLog('I', 'OTA', '转发 ' + rseq + '/' + rtot + ' (' + pct + '%)');
        }
      })
      .catch(function () { /* 下轮继续 */ })
      .then(function () { relayWatchInFlight = false; });
  }, 400);
}

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
  setProgress(48, '校验 SHA 并触发 GD32 中继…');
  try {
    // 仅触发中继（设备端异步跑），不阻塞到刷写结束；SHA 读分区可能数秒
    var resp = await fetch(getBaseUrl() + '/ota/relay', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sha256: sha256 }),
      signal: AbortSignal.timeout(120000)
    });
    var text = await resp.text();
    if (resp.ok) {
      try {
        var data = JSON.parse(text);
        if (data.sha256) addLog('I', 'OTA', '设备端 SHA-256: ' + data.sha256);
      } catch (_) {}
      addLog('I', 'OTA', 'GD32 OTA 已触发；进度看状态栏包数（勿只看日志时间戳）');
      showToast('GD32 OTA 已触发', 'success');
      setProgress(50, '中继已启动，同步进度…');
      gd32CompleteAnnounced = false;
      relayWatchLastSeq = -1;
      startRelayProgressWatch();
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
    // 请求超时/断连 ≠ 设备未启动中继：先探测 status 再决定是否标失败
    try {
      var probe = await fetch(getBaseUrl() + '/system/status', {
        signal: AbortSignal.timeout(3000)
      });
      if (probe.ok) {
        var st = await probe.json();
        var o = st.ota || {};
        var code = Number(o.sys_state_code);
        var rstate = Number(o.relay_state);
        if (code === 3 || (rstate >= 1 && rstate <= 4)) {
          addLog('W', 'OTA', '触发请求中断(' + e.message + ')，但设备已在中继，继续跟踪进度');
          showToast('设备已在刷写，同步进度中', 'success');
          if (otaSessionState !== OtaSessionState.RELAYING) {
            otaSessionTransition(OtaSessionState.RELAYING);
          }
          try { updateSystemStatusUi(st); } catch (_) {}
          startRelayProgressWatch();
          return;
        }
      }
    } catch (_) { /* probe 失败再走失败清理 */ }
    addLog('E', 'OTA', 'GD32 触发失败: ' + e.message);
    showToast('GD32 触发失败', 'error');
    cleanupAfterOtaFailure('触发异常');
  }
}

// ==================== 轮询状态 ====================
var consecutivePollFailures = 0;
var statusTimerId = null;
var logsTimerId = null;
var lastDiagTime = 0;       // 上次诊断时间
var diagResult = '';        // 最近一次诊断结果
var lastSysStateCode = -1;  // 上次成功的 sys_state_code，用于失败时判断设备是否在忙
var lastRelayState = 0;     // 上次成功的 relay_state
var statusAbortCtrl = null;  // 当前 /system/status 请求的 AbortController（3s 超时用）
var statusPollIntervalMs = 1000;  // 状态轮询目标间隔
var statusPollInFlight = false;   // 是否正在请求 /system/status，防止重叠
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
/** @description 调度下一次 /system/status 轮询（递归 setTimeout）。
 *  上一次请求完成（或 3s 超时）后才调度下一次，避免 setInterval 把慢响应
 *  直接 abort 而导致堆/任务栈/CPU 等卡片刷新率不稳。
 */
function scheduleStatusPoll(delayMs) {
  if (statusTimerId) clearTimeout(statusTimerId);
  // 中继期加快：进度条依赖 status.relay_seq，1s 间隔体感像卡死
  var interval = statusPollIntervalMs;
  if (otaSessionState === OtaSessionState.RELAYING) {
    interval = Math.min(interval, 400);
  }
  statusTimerId = setTimeout(function() {
    statusTimerId = null;
    pollStatus();
  }, delayMs !== undefined ? delayMs : interval);
}

/** @description 启动定时轮询（状态 + 日志）。 */

function startPolling() {
  consecutivePollFailures = 0;
  statusPollIntervalMs = 1000;
  if (statusTimerId) clearTimeout(statusTimerId);
  if (logsTimerId) clearInterval(logsTimerId);
  scheduleStatusPoll(0);   // 立即开始第一次，完成后按 interval 递归调度
  logsTimerId = setInterval(pollLogs, 1000);
}
function pausePollingForUpload() {
  if (pollingPausedForUpload) return;
  pollingPausedForUpload = true;
  uploadPollChip = activeUploadChip || window.flashTarget || null;
  consecutivePollFailures = 0;
  // ★ 上传大固件期间必须停掉 status/logs 轮询。
  //   httpd max_open_sockets=4 + lru_purge：轮询会再占 socket，LRU 可能踢掉
  //   正在写 Flash 的 /ota/upload 连接 → 浏览器 upload 进度已 100%（发完），
  //   但永远等不到 HTTP 响应，卡在「已上传 xxx / xxx」。
  if (statusTimerId) {
    clearTimeout(statusTimerId);
    statusTimerId = null;
  }
  if (logsTimerId) {
    clearInterval(logsTimerId);
    logsTimerId = null;
  }
  if (statusAbortCtrl) {
    try { statusAbortCtrl.abort(); } catch (e) { /* ignore */ }
    statusAbortCtrl = null;
    statusPollInFlight = false;
  }
}
function resumePollingAfterUpload() {
  if (!pollingPausedForUpload) return;
  pollingPausedForUpload = false;
  uploadPollChip = null;
  consecutivePollFailures = 0;
  statusPollIntervalMs = 1000;
  if (logsTimerId) clearInterval(logsTimerId);
  logsTimerId = setInterval(pollLogs, statusPollIntervalMs);
  // 上传结束立即刷新一次状态；若当前已在请求中，则让它完成后按新间隔继续
  if (statusPollInFlight) {
    if (statusTimerId) clearTimeout(statusTimerId);
  } else {
    scheduleStatusPoll(0);
  }
  pollLogs();
}
/** @description 轮询 ESP32 系统状态（GET /status）并更新 UI。 */
async function pollStatus() {
  // 递归 setTimeout 调度，正常情况下不会重叠；做个保险避免并发请求
  if (statusPollInFlight) return;
  statusPollInFlight = true;
  statusAbortCtrl = new AbortController();
  var pollTimer = setTimeout(function() { try { statusAbortCtrl.abort(); } catch(_e) {} }, 3000);
  try {
    var resp = await fetch(getBaseUrl() + '/system/status', {
      signal: statusAbortCtrl.signal
    });
    if (resp.ok) {
      var data = await resp.json();
      // 恢复：之前有失败计数，说明设备曾离线，现在恢复在线
      if (consecutivePollFailures > 0) {
        consecutivePollFailures = 0;
        addLog('I', '系统', '设备已恢复在线，轮询恢复正常');
        showToast('设备已恢复在线', 'success');
        // 设备离线后恢复在线，清理残留的 OTA 会话状态
        if (otaSessionState !== OtaSessionState.IDLE) {
          otaSessionReset();
          addLog('I', '系统', '已清理 OTA 会话状态');
        }
      }
      if (isEsp32Rebooting) {
        var rbElapsed = Math.round((Date.now() - rebootStartTime) / 1000);
        if (!rebootObservedDown && rbElapsed < 10) {
          // pre-reboot 窗口：后端发响应后约 1s 才 esp_restart，期间设备仍响应轮询
          // 保持"重启中"状态，不清除标志，不推进阶段
          // ★ 真正的"重启完成"由 pollLogs 收到启动 banner（MAIN: ESP32-C3 OTA Relay v1.0 启动）触发
        } else {
          // 设备已重启并恢复在线（rebootObservedDown=true）
          // 或超 10s 安全阈值仍未观察到下线（重启过快未捕获或未重启）——接受恢复
          isEsp32Rebooting = false;
          rebootObservedDown = false;
          updateEsp32StagesFromPhase(6);  // 启动完成
          addLog('I', '系统', 'ESP32 已恢复在线');
          showToast('ESP32 已恢复在线', 'success');
          // ★ 状态机：ESP32 重启完成 → DONE
          if (otaSessionState === OtaSessionState.REBOOTING) otaSessionTransition(OtaSessionState.DONE);
        }
      }
      try { updateSystemStatusUi(data); } catch (e2) { console.warn('updateSystemStatusUi 异常:', e2); }
      // pre-reboot 窗口内 updateSystemStatusUi 会把连接胶囊刷成"已连接"，需覆盖回"重启中"
      if (isEsp32Rebooting) {
        applyRebootingRuntimeState();
      }
    }
    clearTimeout(pollTimer);
  } catch (e) {
    clearTimeout(pollTimer);
    // 自身 AbortController 触发的取消不算失败，静默返回
    if (e && e.name === "AbortError") return;
    // 后台期间 setInterval 会被 Chrome/Edge 节流到最低 1 分钟/次，
    // 因此后台累积的失败计数不应递增，避免切回前台瞬间误触发 5-fail 诊断。
    if (pageVisible) consecutivePollFailures++;
    // 判断设备是否处于"已知忙/重启"状态（依据最近一次成功轮询的 sys_state_code）：
    //   1=GD32_STORING, 3=GD32_RELAYING, 5=ESP32_RECEIVING,
    //   6=ESP32_FINALIZING, 7=ESP32_REBOOTING
    // 也包含 relay_state 非 IDLE（1-4 表示握手/协商/传输/收尾）
    var lastDeviceBusy =
      lastSysStateCode === 1 || lastSysStateCode === 3 ||
      lastSysStateCode === 5 || lastSysStateCode === 6 ||
      lastSysStateCode === 7 ||
      (lastRelayState >= 1 && lastRelayState <= 4);
    // OTA/传输/重启中屏蔽网关无响应的检测功能（设备忙或正在重启，无法响应轮询是正常的）
    // 设备已知忙时：保持 1s 轮询，不触发降速诊断，避免错过设备恢复的即时反馈
    // ★ 状态机：用统一的会话活跃/失败判断替代散布的标志。
    //   FAILED 态不触发诊断降速（让用户尽快看到设备恢复），但也不覆盖失败 UI。
    var sessionActive = otaSessionActive();
    var sessionFailed = otaSessionState === OtaSessionState.FAILED;
    if (pageVisible && consecutivePollFailures >= 5 && !sessionActive && !isEsp32Rebooting && !currentOtaTarget && !lastDeviceBusy && !sessionFailed) {
      addLog('W', '诊断', '设备连续 5 次无响应，正在诊断网络...');
      var diag = await diagnoseNetwork();
      addLog('W', '诊断', diag);
      updateConnectionUi(false, '未连接');
      applyDisconnectedRuntimeState();
      var cst = document.getElementById('connStatusTxt');
      if (cst) cst.textContent = '诊断中: ' + diag;
    }
    // ★ 状态机优先：会话已 FAILED → 保持失败 UI，仅标连接胶囊为"设备忙/未连接"
    if (sessionFailed) {
      if (lastDeviceBusy) {
        // 设备端仍在跑（中继没停），标"设备忙"但不覆盖进度/阶段
        var pcF = document.getElementById('pillConn');
        var pctF = document.getElementById('pillConnTxt');
        if (pcF) pcF.className = 'pill processing';
        if (pctF) pctF.textContent = '设备忙';
        var csF = document.getElementById('connStatus');
        var cstF = document.getElementById('connStatusTxt');
        if (csF) csF.className = 'conn-indicator warn';
        if (cstF) cstF.textContent = '设备忙（OTA 后台运行中），等待回 IDLE';
        updateConnectionUi(true, '设备忙');
      } else {
        // 设备已回 IDLE：连接胶囊显示未连接，但失败进度保留
        updateConnectionUi(false, '未连接');
      }
    } else if (otaSessionState === OtaSessionState.UPLOADING) {
      applyUploadingRuntimeState();
    } else if (otaSessionState === OtaSessionState.REBOOTING || isEsp32Rebooting) {
      rebootObservedDown = true;  // 确认设备已下线（正在重启）
      var elapsed = Math.round((Date.now() - rebootStartTime) / 1000);
      if (elapsed > 60) {
        otaSessionTransition(OtaSessionState.FAILED, 'ESP32 重启超时');
        isEsp32Rebooting = false;
        rebootObservedDown = false;
        addLog('E', '系统', 'ESP32 重启超时，设备未恢复');
        showToast('ESP32 重启超时，请检查设备', 'error');
        updateConnectionUi(false, '未连接');
        applyDisconnectedRuntimeState();
      } else {
        applyRebootingRuntimeState();
      }
    } else if (sessionActive || lastDeviceBusy) {
      // 设备在 OTA/中继/重启期间不响应是正常的：保持 1s 轮询，UI 标记为"设备忙"
      // 不调用 applyRebootingRuntimeState（避免显示"重启中 Xs"误导）
      var pc = document.getElementById('pillConn');
      var pct = document.getElementById('pillConnTxt');
      if (pc) pc.className = 'pill processing';
      if (pct) pct.textContent = '设备忙';
      var cs = document.getElementById('connStatus');
      var cst = document.getElementById('connStatusTxt');
      if (cs) cs.className = 'conn-indicator warn';
      if (cst) cst.textContent = '设备忙（OTA/重启中），保持 1s 轮询';
      updateConnectionUi(true, '设备忙');
    } else {
      updateConnectionUi(false, '未连接');
      applyDisconnectedRuntimeState();
    }
  } finally {
    statusPollInFlight = false;
    scheduleStatusPoll();
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
  // 切回前台立即刷新状态；若当前已在请求中，取消等待中的下一次让它尽快重排
  if (statusPollInFlight) {
    if (statusTimerId) clearTimeout(statusTimerId);
  } else {
    if (statusTimerId) clearTimeout(statusTimerId);
    scheduleStatusPoll(0);
  }
  pollLogs();
});
// ==================== ESP32 实时日志拉取 (增量) ====================
var lastLogTimestamp = 0;
/** @description 轮询 ESP32 日志（GET /logs）并追加到日志面板。 */
async function pollLogs() {
  // ★ ESP32 上传期间也允许日志轮询
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
        // ★ 复用设备启动 banner 作为 ESP32 重启完成的权威信号。
        //   esp_restart() 是硬件复位，不保证 TCP FIN/RST；靠轮询超时/网络错误判断
        //   "已下线"不可靠。设备启动后会推 [I][MAIN] ESP32-C3 OTA Relay v1.0 启动，
        //   收到这行即说明设备已重启回来，直接推进到阶段 6（启动完成）。
        if (isEsp32Rebooting && m[2] === 'MAIN' && /OTA Relay v1\.0 启动/.test(m[3])) {
          isEsp32Rebooting = false;
          rebootObservedDown = false;
          updateEsp32StagesFromPhase(6);  // 启动完成
          addLog('I', '系统', 'ESP32 已恢复在线（启动 banner 收到）');
          showToast('ESP32 已恢复在线', 'success');
          if (otaSessionState === OtaSessionState.REBOOTING) otaSessionTransition(OtaSessionState.DONE);
        }
      } else {
        addLog('I', 'ESP32', line.trimEnd());
      }
    });
  } catch (e) { /* 静默 */ }
}
setTimeout(function () {
  updateUploadButtonLabel();
  addLog('I', '系统', '控制台初始化完成，等待固件文件...');
  updateSpiHint();
  // IP 输入变化时清零失败计数并立即刷新状态
  var ipInput = document.getElementById('espIp');
  if (ipInput) {
    ipInput.addEventListener('change', function () {
      consecutivePollFailures = 0;
      addLog('I', '系统', 'IP 已修改，重新启动轮询...');
      if (!statusPollInFlight) {
        if (statusTimerId) clearTimeout(statusTimerId);
        scheduleStatusPoll(0);
      }
    });
  }
}, 500);
