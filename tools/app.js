// ==================== 全局状态 ====================
let esp32File = null;
let gd32File = null;
let uploadProgress = 0;
let isUploading = false;
let logFilter = 'all';
let logEntries = [];
let lastLogHash = null;
let autoScroll = true;
let logSocket = null;
let latestSystemStatus = null;
let isEsp32Rebooting = false;
let rebootStartTime = 0;

// ==================== 工具函数 ====================
function formatBytes(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
}

function formatTime(date) {
  var h = String(date.getHours()).padStart(2, '0');
  var m = String(date.getMinutes()).padStart(2, '0');
  var s = String(date.getSeconds()).padStart(2, '0');
  var ms = String(date.getMilliseconds()).padStart(3, '0');
  return h + ':' + m + ':' + s + '.' + ms;
}

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
  var dlc = parseInt(document.getElementById('canDlc').value) || 0;
  var dataHex = document.getElementById('canData').value.trim().replace(/\s/g, '');
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
  ['s1', 's2', 's3', 's4'].forEach(function (id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
}

function applyDisconnectedRuntimeState() {
  document.getElementById('pillIP').textContent = '--';
  document.getElementById('pillRSSI').textContent = '--';
  document.getElementById('pillRelayTxt').textContent = '离线';
  document.getElementById('pillRelay').className = 'pill offline';
  document.getElementById('mWifi').textContent = '未连接';
  document.getElementById('mRSSI').textContent = '--';
  document.getElementById('bOTAProgress').textContent = '--';
  document.getElementById('bOTAProgress').className = 'v err';
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
  cs.className = 'conn-indicator warn';
  cst.textContent = 'ESP32 正在重启...';

  var bc = document.getElementById('bConn');
  bc.textContent = '重启中';
  bc.className = 'v warn';

  document.getElementById('pillRelayTxt').textContent = '重启中';
  document.getElementById('pillRelay').className = 'pill processing';
}

function applyUploadingRuntimeState() {
  var pc = document.getElementById('pillConn');
  var pct = document.getElementById('pillConnTxt');
  pc.className = 'pill processing';
  pct.textContent = '上传中';

  var cs = document.getElementById('connStatus');
  var cst = document.getElementById('connStatusTxt');
  cs.className = 'conn-indicator warn';
  cst.textContent = '固件上传中，设备忙...';

  var bc = document.getElementById('bConn');
  bc.textContent = '上传中';
  bc.className = 'v warn';
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
  cs.className = connected ? 'conn-indicator ok' : 'conn-indicator err';
  cst.textContent = connected ? (text || '已连接') : '未连接';

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
    document.getElementById('mHeap').textContent = formatBytes(freeHeap);
  }
}

// ==================== OTA 阶段驱动 (真实状态) ====================
function updateStagesFromRelayState(rs) {
  var s1 = document.getElementById('s1');
  var s2 = document.getElementById('s2');
  var s3 = document.getElementById('s3');
  var s4 = document.getElementById('s4');
  var all = [s1, s2, s3, s4];

  // reset all
  all.forEach(function (s) { s.className = 'stage-node'; });

  if (rs === undefined || rs === null) return;

  if (rs === 255) {
    s1.className = 'stage-node err';
    s2.className = 'stage-node err';
    s3.className = 'stage-node err';
    s4.className = 'stage-node err';
    return;
  }

  var activeStage = 0;
  if (rs >= 1 && rs <= 6) activeStage = 1;
  else if (rs >= 7 && rs <= 8) activeStage = 2;
  else if (rs >= 9 && rs <= 15) activeStage = 3;
  else if (rs >= 16 && rs <= 17) activeStage = 4;

  for (var i = 0; i < activeStage - 1; i++) {
    all[i].className = 'stage-node done';
  }
  if (activeStage >= 1 && activeStage <= 4) {
    all[activeStage - 1].className = 'stage-node live';
  }
}

function updateSystemStatusUi(data) {
  if (!data) return;
  latestSystemStatus = data;

  var wifi = data.wifi || {};
  var ota = data.ota || {};
  var healthCode = data.health;

  var relayStates = {
    0: '待机', 1: '等待握手', 2: '握手已发', 3: '挑战已发',
    4: '收到握手响应', 5: '会话密钥已发', 6: '认证完成',
    7: '大小已发', 8: '大小确认', 9: '接收中', 10: '转发中',
    11: '数据包已发', 12: 'ACK正常', 13: '等待重发',
    14: '接收完成', 15: '传输完成', 16: '结束已发', 17: 'OTA完成',
    255: '错误'
  };

  var connected = !!wifi.connected;
  var connText = connected ? (wifi.mode || 'STA') + ' 已连接' : (wifi.mode || 'STA') + ' 未连接';

  updateConnectionUi(connected, connText);
  updateHealthUi(healthCode, data.free_heap);

  // version
  if (data.version) {
    document.getElementById('pillVer').textContent = 'v' + data.version;
    document.getElementById('mVersion').textContent = 'v' + data.version;
  }

  // temperature
  document.getElementById('mTemp').textContent =
    Number.isFinite(data.temperature) ? data.temperature.toFixed(1) + '\u00B0C' : '--';

  // uptime
  var uptimeEl = document.getElementById('pillUptime');
  if (uptimeEl) uptimeEl.textContent = '\u23F1 ' + formatUptime(data.uptime_sec);

  // wifi / signal
  document.getElementById('mWifi').textContent = wifi.connected ? (wifi.ip || '已连接') : '未连接';
  document.getElementById('pillIP').textContent = wifi.ip || '--';
  var rssi = (wifi.rssi !== undefined && wifi.rssi !== -127) ? wifi.rssi + ' dBm' : '--';
  document.getElementById('mRSSI').textContent = rssi;
  document.getElementById('pillRSSI').textContent = rssi;

  // heap
  document.getElementById('mHeap').textContent =
    data.free_heap !== undefined ? formatBytes(data.free_heap) : '--';
  document.getElementById('mMinHeap').textContent =
    data.min_free_heap !== undefined ? formatBytes(data.min_free_heap) : '--';

  // ota cache
  var otaStored = ota.stored_size || 0;
  var mOtaCache = document.getElementById('mOtaCache');
  if (mOtaCache) {
    if (otaStored > 0) {
      mOtaCache.textContent = formatBytes(otaStored) + ' / 216 KB';
      mOtaCache.className = 'v ok';
    } else {
      mOtaCache.textContent = '0 / 216 KB';
      mOtaCache.className = 'v';
    }
  }

  // relay state
  var rs = ota.relay_state;
  var relayText = relayStates[rs] || '待机';
  document.getElementById('pillRelayTxt').textContent = relayText;
  var pr = document.getElementById('pillRelay');
  if (rs === 255) pr.className = 'pill offline';
  else if (rs > 0 && rs < 17) pr.className = 'pill processing';
  else if (rs === 17) pr.className = 'pill online';
  else pr.className = 'pill idle';

  // OTA progress
  var total = ota.total_size || 0;
  var stored = ota.stored_size || 0;
  var progress = total > 0 ? Math.min(100, Math.round(stored * 100 / total)) : 0;
  var bp = document.getElementById('bOTAProgress');
  bp.textContent = progress + '%';
  bp.className = 'v ' + (progress === 100 ? 'ok' : progress > 0 ? 'warn' : '');

  // stage flow from real state
  updateStagesFromRelayState(rs);
}

async function testConnection() {
  if (!latestSystemStatus) {
    addLog('W', '连接', '暂无同步数据，请等待 1 秒自动同步');
    showToast('暂无同步数据，请稍等 1 秒', 'warning');
    return;
  }
  var wifi = latestSystemStatus.wifi || {};
  var connected = !!wifi.connected;
  updateSystemStatusUi(latestSystemStatus);
  addLog('I', '连接', connected
    ? '当前已连接：' + (wifi.mode || 'STA') + ' ' + (wifi.ip || '')
    : '当前未连接：' + (wifi.mode || 'STA'));
  showToast(connected ? 'ESP32 当前在线' : 'ESP32 当前未连接',
    connected ? 'success' : 'warning');
}

// ==================== CAN 帧发送 ====================
async function sendCanFrame() {
  var ch = parseInt(document.getElementById('canChannel').value);
  var idStr = document.getElementById('canId').value.trim();
  var dlc = parseInt(document.getElementById('canDlc').value) || 8;
  var dataStr = document.getElementById('canData').value.trim();

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
  document.getElementById('canId').value = '0x18DA00F1';
  document.getElementById('canDlc').value = '8';
  document.getElementById('canData').value = '0102030405060708';
  document.getElementById('canResp').style.display = 'none';
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

document.getElementById('canData').addEventListener('input', updateCanHint);
document.getElementById('canDlc').addEventListener('input', updateCanHint);

document.getElementById('spiType').addEventListener('input', updateSpiHint);
document.getElementById('spiData').addEventListener('input', updateSpiHint);

function processFile(file, chip) {
  if (chip === 'esp32') esp32File = file;
  else gd32File = file;
  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  document.getElementById('fileInfo' + suffix).classList.add('show');
  document.getElementById('fileName' + suffix).textContent = file.name;
  document.getElementById('fileType' + suffix).textContent =
    file.name.toLowerCase().indexOf('.hex') >= 0 ? 'Intel HEX' : 'Binary';
  document.getElementById('fileSize' + suffix).textContent = formatBytes(file.size);
  updateUploadButtonLabel();
  addLog('I', '文件', chip.toUpperCase() + ' 已选择: ' + file.name + ' (' + formatBytes(file.size) + ')');
  showToast(chip.toUpperCase() + ' 文件已选择', 'success');
}

function clearFile(chip) {
  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  if (chip === 'esp32') esp32File = null;
  else gd32File = null;
  document.getElementById('fileInput' + suffix).value = '';
  document.getElementById('fileInfo' + suffix).classList.remove('show');
  updateUploadButtonLabel();
  addLog('I', '文件', chip.toUpperCase() + ' 文件已清除');
}

// ==================== 上传固件 ====================
async function startUpload(chip) {
  var file = chip === 'esp32' ? esp32File : gd32File;
  if (!file || isUploading) return;
  isUploading = true;
  var uploadBtn = document.getElementById(chip === 'esp32' ? 'uploadEsp32Btn' : 'uploadGd32Btn');
  var origText = uploadBtn.textContent;
  uploadBtn.disabled = true;
  uploadBtn.textContent = '上传中...';
  document.getElementById('progressWrap').classList.add('show');
  addLog('I', '上传', '开始上传: ' + file.name + ' \u2192 ' + chip.toUpperCase());
  showToast('开始上传固件到 ' + chip.toUpperCase(), 'success');

  try {
    var arrayBuffer = await file.arrayBuffer();
    var fileSize = file.size;
    var body, url = getBaseUrl() + '/ota/upload';

    if (chip === 'esp32') {
      var headerJson = JSON.stringify({ type: 'esp32', total_size: fileSize });
      var encoder = new TextEncoder();
      var headerBytes = encoder.encode(headerJson);
      var fileBytes = new Uint8Array(arrayBuffer);
      var combined = new Uint8Array(headerBytes.length + fileSize);
      combined.set(headerBytes, 0);
      combined.set(fileBytes, headerBytes.length);
      body = combined.buffer;
    } else {
      body = arrayBuffer;
    }

    var xhr = new XMLHttpRequest();
    xhr.upload.addEventListener('progress', function (e) {
      if (e.lengthComputable) {
        var pct = Math.round((e.loaded / e.total) * 100);
        uploadProgress = pct;
        document.getElementById('progressFill').style.width = pct + '%';
        document.getElementById('progressTxt').textContent =
          '已上传 ' + formatBytes(e.loaded) + ' / ' + formatBytes(e.total);
        document.getElementById('progressPct').textContent = pct + '%';
        document.getElementById('bOTAProgress').textContent = pct + '%';
      }
    });

    xhr.addEventListener('load', function () {
      isUploading = false;
      uploadBtn.disabled = false;
      uploadBtn.textContent = origText;
      updateUploadButtonLabel();
      if (xhr.status === 200) {
        addLog('I', '上传', '固件上传完成！');
        showToast('固件上传成功', 'success');
        if (chip === 'esp32') {
          var countdown = 3;
          addLog('I', 'OTA', countdown + ' 秒后自动调用 POST /ota/update 完成升级并重启...');
          var timer = setInterval(function () {
            countdown--;
            if (countdown > 0) addLog('I', 'OTA', countdown + ' 秒后开始刷写...');
            else {
              clearInterval(timer);
              addLog('I', 'OTA', '开始刷写！');
              triggerEsp32Update();
            }
          }, 1000);
        }
      } else {
        addLog('E', '上传', '上传失败: HTTP ' + xhr.status);
        showToast('上传失败', 'error');
      }
    });

    xhr.addEventListener('error', function () {
      isUploading = false;
      uploadBtn.disabled = false;
      uploadBtn.textContent = origText;
      updateUploadButtonLabel();
      addLog('E', '上传', '上传过程发生错误');
      showToast('上传出错', 'error');
    });

    xhr.open('POST', url);
    xhr.send(body);
  } catch (e) {
    addLog('E', '上传', '上传失败: ' + e.message);
    showToast('上传失败', 'error');
    isUploading = false;
    uploadBtn.disabled = false;
    uploadBtn.textContent = origText;
    updateUploadButtonLabel();
  }
}

// ==================== ESP32 OTA 完成升级 ====================
async function triggerEsp32Update() {
  addLog('I', 'OTA', '=== ESP32 OTA 完成，开始重启 ===');
  showToast('ESP32 正在重启...', 'success');
  try {
    var resp = await fetch(getBaseUrl() + '/ota/update', {
      method: 'POST', signal: AbortSignal.timeout(5000)
    });
    if (resp.ok) {
      var data = await resp.json();
      addLog('I', 'OTA', '升级完成: ' + data.message);
      showToast('ESP32 重启中...', 'success');
      isEsp32Rebooting = true;
      rebootStartTime = Date.now();
    }
  } catch (e) {
    addLog('E', 'OTA', '升级失败: ' + e.message);
    showToast('升级失败', 'error');
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

// ==================== 触发 OTA 中继 (真实状态驱动) ====================
async function triggerRelay() {
  addLog('I', 'OTA', '触发 GD32 OTA 中继...');
  showToast('正在触发 OTA 中继', 'success');

  try {
    var resp = await fetch(getBaseUrl() + '/ota/relay', {
      method: 'POST', signal: AbortSignal.timeout(5000)
    });
    if (resp.ok) {
      addLog('I', 'OTA', 'OTA 中继已触发，等待 GD32 响应...');
      // 不再使用假的 setTimeout 推进阶段。
      // pollStatus 的 updateSystemStatusUi 会从 ota.relay_state
      // 自动更新 stage flow 和 header 状态。
      showToast('中继已触发，等待设备响应', 'success');
    } else {
      addLog('E', 'OTA', '触发失败: HTTP ' + resp.status);
      showToast('触发失败', 'error');
    }
  } catch (e) {
    addLog('E', 'OTA', '触发失败: ' + e.message);
    showToast('触发失败', 'error');
  }
}

// ==================== 轮询状态 ====================
async function pollStatus() {
  try {
    var resp = await fetch(getBaseUrl() + '/system/status', {
      signal: AbortSignal.timeout(3000)
    });
    if (resp.ok) {
      var data = await resp.json();
      if (isEsp32Rebooting) {
        isEsp32Rebooting = false;
        addLog('I', '系统', 'ESP32 已恢复在线');
        showToast('ESP32 已恢复在线', 'success');
      }
      updateSystemStatusUi(data);
    }
  } catch (e) {
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
    } else {
      updateConnectionUi(false, '未连接');
      applyDisconnectedRuntimeState();
    }
  }
}
setInterval(pollStatus, 1000);

// ==================== ESP32 实时日志拉取 (增量) ====================
var lastLogTimestamp = 0;
async function pollLogs() {
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
setInterval(pollLogs, 1000);

setTimeout(function () {
  updateUploadButtonLabel();
  updateCanHint();
  addLog('I', '系统', '控制台初始化完成，等待固件文件...');
  updateSpiHint();
}, 500);
