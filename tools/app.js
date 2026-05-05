// ==================== 全局状态 ====================
let currentFile = null;
let uploadProgress = 0;
let isUploading = false;
let logFilter = 'all';
let logEntries = [];
let logSocket = null;
let latestSystemStatus = null;

// ==================== 工具函数 ====================
function formatBytes(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
}

function formatTime(date) {
  return date.toTimeString().split(' ')[0];
}

function formatUptime(sec) {
  if (!Number.isFinite(sec) || sec < 0) return '--';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function showToast(message, type = 'success') {
  const toast = document.getElementById('toast');
  const icon = document.getElementById('toastIcon');
  const msg = document.getElementById('toastMessage');

  toast.className = `toast ${type} show`;
  icon.textContent = type === 'success' ? '✓' : type === 'error' ? '✕' : '⚠';
  msg.textContent = message;

  setTimeout(() => toast.classList.remove('show'), 3000);
}

function getEspIp() {
  return document.getElementById('espIp').value.trim() || '192.168.31.127';
}

function getBaseUrl() {
  return `http://${getEspIp()}`;
}

// ==================== 日志系统 ====================
function addLog(level, tag, message) {
  const logContent = document.getElementById('logContent');
  const time = formatTime(new Date());

  const entry = { time, level, tag, message };
  logEntries.push(entry);

  if (logFilter !== 'all' && level !== logFilter) {
    return;
  }

  const div = document.createElement('div');
  div.className = 'log-entry';
  div.innerHTML = `
    <span class="log-time">[${time}]</span>
    <span class="log-level ${level}">${level}</span>
    <span class="log-tag">[${tag}]</span>
    <span class="log-msg">${message}</span>
  `;

  logContent.appendChild(div);
  logContent.scrollTop = logContent.scrollHeight;
  document.getElementById('logCount').textContent = logEntries.length;
}

function clearLogs() {
  document.getElementById('logContent').innerHTML = '';
  logEntries = [];
  document.getElementById('logCount').textContent = '0';
}

function toggleFilter(level, btn) {
  document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  logFilter = level;

  const logContent = document.getElementById('logContent');
  logContent.innerHTML = '';
  logEntries.forEach(entry => {
    if (level === 'all' || entry.level === level) {
      const div = document.createElement('div');
      div.className = 'log-entry';
      div.innerHTML = `
        <span class="log-time">[${entry.time}]</span>
        <span class="log-level ${entry.level}">${entry.level}</span>
        <span class="log-tag">[${entry.tag}]</span>
        <span class="log-msg">${entry.message}</span>
      `;
      logContent.appendChild(div);
    }
  });
  logContent.scrollTop = logContent.scrollHeight;
}

// ==================== 连接管理 ====================
function updateConnectionUi(connected, text = null) {
  const badge = document.getElementById('connectionBadge');
  const label = document.getElementById('connectionText');
  badge.className = connected ? 'badge connected' : 'badge disconnected';
  label.textContent = text || (connected ? '已连接' : '未连接');
}

function updateHealthUi(healthCode, freeHeap) {
  const healthNames = ['正常', '警告', '危险', '关机'];
  const health = healthCode !== undefined ? (healthNames[healthCode] || '未知') : '未知';
  document.getElementById('healthText').textContent = `系统${health}`;
  document.getElementById('healthBadge').className = healthCode === 0 ? 'badge connected' : 'badge warning';
  if (freeHeap !== undefined) {
    document.getElementById('statHeap').textContent = formatBytes(freeHeap);
  }
}

function updateSystemStatusUi(data) {
  if (!data) return;

  latestSystemStatus = data;

  const wifi = data.wifi || {};
  const ota = data.ota || {};
  const healthCode = data.health;
  const relayStates = {
    0: '待机',
    1: '等待握手',
    2: '握手已发',
    3: '挑战已发',
    4: '收到握手响应',
    5: '会话密钥已发',
    6: '认证完成',
    7: '大小已发',
    8: '大小确认',
    9: '接收中',
    10: '转发中',
    11: '数据包已发',
    12: 'ACK正常',
    13: '等待重发',
    14: '接收完成',
    15: '传输完成',
    16: '结束已发',
    17: 'OTA完成',
    255: '错误'
  };

  updateConnectionUi(!!wifi.connected, wifi.connected ? `${wifi.mode || 'STA'} 已连接` : `${wifi.mode || 'STA'} 未连接`);
  updateHealthUi(healthCode, data.free_heap);

  // 显示固件版本
  if (data.version) {
    document.getElementById('fwVersion').textContent = `v${data.version}`;
  }

  document.getElementById('statTemperature').textContent = Number.isFinite(data.temperature) ? `${data.temperature.toFixed(1)}°C` : '--';
  const uptimeEl = document.getElementById('statUptime');
  if (uptimeEl) uptimeEl.textContent = formatUptime(data.uptime_sec);
  document.getElementById('statWifi').textContent = wifi.connected ? (wifi.ip || '已连接') : '未连接';
  document.getElementById('statSignal').textContent = (wifi.rssi !== undefined && wifi.rssi !== -127) ? `${wifi.rssi} dBm` : '--';
  document.getElementById('statHeap').textContent = data.free_heap !== undefined ? formatBytes(data.free_heap) : '--';
  document.getElementById('statMinHeap').textContent = data.min_free_heap !== undefined ? formatBytes(data.min_free_heap) : '--';

  const total = ota.total_size || 0;
  const stored = ota.stored_size || 0;
  const progress = total > 0 ? Math.min(100, Math.round(stored * 100 / total)) : 0;
  document.getElementById('statProgress').textContent = `${progress}%`;
  document.getElementById('statRelayState').textContent = relayStates[ota.relay_state] ?? '待机';
}

async function testConnection() {
  if (!latestSystemStatus) {
    addLog('W', '连接', '暂无同步数据，请等待 1 秒自动同步');
    showToast('暂无同步数据，请稍等 1 秒', 'warning');
    return;
  }

  const wifi = latestSystemStatus.wifi || {};
  const connected = !!wifi.connected;
  updateSystemStatusUi(latestSystemStatus);
  addLog('I', '连接', connected
    ? `当前已连接：${wifi.mode || 'STA'} ${wifi.ip || ''}`.trim()
    : `当前未连接：${wifi.mode || 'STA'}`);
  showToast(connected ? 'ESP32 当前在线' : 'ESP32 当前未连接', connected ? 'success' : 'warning');
}


// ==================== CAN 帧发送 ====================
async function sendCanFrame() {
  const ch = parseInt(document.getElementById('canChannel').value);
  const idStr = document.getElementById('canId').value.trim();
  const dlc = parseInt(document.getElementById('canDlc').value) || 8;
  const dataStr = document.getElementById('canData').value.trim();

  let extId = 0;
  if (idStr.startsWith('0x') || idStr.startsWith('0X')) {
    extId = parseInt(idStr, 16);
  } else {
    extId = parseInt(idStr, 10);
    if (isNaN(extId)) extId = parseInt(idStr, 16);
  }

  if (isNaN(extId)) {
    showToast('ID 格式错误', 'error');
    return;
  }

  let dataHex = dataStr.replace(/\s/g, '');
  if (dataHex.length % 2 !== 0) {
    showToast('数据长度必须为偶数', 'error');
    return;
  }

  addLog('I', 'CAN', `发送 CH${ch} ID=0x${extId.toString(16).toUpperCase()} DLC=${dlc}`);

  try {
    const response = await fetch(`${getBaseUrl()}/can/send`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        ch: ch,
        id: extId,
        dlc: dlc,
        data: dataHex
      }),
      signal: AbortSignal.timeout(5000)
    });

    const result = await response.json();

    if (result.status === 'ok') {
      addLog('I', 'CAN', `响应: ${result.recv || '(无数据)'} (${result.recv_len || 0}B)`);

      const respDiv = document.getElementById('canResponse');
      const respContent = document.getElementById('canRespContent');
      respDiv.style.display = 'block';
      respContent.textContent = result.recv
        ? `0x${result.recv} (${result.recv_len} 字节)`
        : '无响应数据';

      showToast(`CH${ch} 发送成功`, 'success');
    } else {
      addLog('E', 'CAN', `发送失败: code=${result.code}`);
      showToast(`发送失败: code=${result.code}`, 'error');
    }
  } catch (e) {
    addLog('E', 'CAN', `发送失败: ${e.message}`);
    showToast('发送失败', 'error');
  }
}

function onTargetChipChange() {
  const chip = document.getElementById('targetChip').value;
  const relayBtn = document.getElementById('relayBtn');
  const esp32Actions = document.getElementById('esp32Actions');
  
  if (chip === 'esp32') {
    relayBtn.style.display = 'none';
    esp32Actions.style.display = 'block';
  } else {
    relayBtn.style.display = '';
    esp32Actions.style.display = 'none';
  }
  addLog('I', '目标', `切换到: ${chip === 'esp32' ? 'ESP32 自身升级' : 'GD32 Flash 中继'}`);
}

function clearCanFields() {
  document.getElementById('canId').value = '0x18DA00F1';
  document.getElementById('canDlc').value = '8';
  document.getElementById('canData').value = '0102030405060708';
  document.getElementById('canResponse').style.display = 'none';
}

// ==================== 文件处理 ====================
function handleFileSelect(event) {
  const file = event.target.files[0];
  if (file) processFile(file);
}

document.getElementById('uploadZone').addEventListener('dragover', (e) => {
  e.preventDefault();
  e.currentTarget.classList.add('dragover');
});

document.getElementById('uploadZone').addEventListener('dragleave', (e) => {
  e.currentTarget.classList.remove('dragover');
});

document.getElementById('uploadZone').addEventListener('drop', (e) => {
  e.preventDefault();
  e.currentTarget.classList.remove('dragover');
  const file = e.dataTransfer.files[0];
  if (file) processFile(file);
});

function processFile(file) {
  currentFile = file;
  const fileInfo = document.getElementById('fileInfo');
  const uploadBtn = document.getElementById('uploadBtn');

  document.getElementById('fileName').textContent = file.name;
  document.getElementById('fileType').textContent = file.name.endsWith('.hex') ? 'Intel HEX' : 'Binary';
  document.getElementById('fileSize').textContent = formatBytes(file.size);

  const chunks = Math.ceil(file.size / 8192);
  document.getElementById('fileChunks').textContent = chunks + ' 包';

  fileInfo.classList.add('visible');
  uploadBtn.disabled = false;

  addLog('I', '文件', `已选择: ${file.name} (${formatBytes(file.size)})`);
  showToast('文件已选择', 'success');
}

function clearFile() {
  currentFile = null;
  document.getElementById('fileInput').value = '';
  document.getElementById('fileInfo').classList.remove('visible');
  document.getElementById('uploadBtn').disabled = true;
  document.getElementById('progressSection').classList.remove('visible');
  addLog('I', '文件', '文件已清除');
}

// ==================== 上传固件 ====================
async function startUpload() {
  if (!currentFile || isUploading) return;

  const chip = document.getElementById('targetChip').value;
  isUploading = true;
  const uploadBtn = document.getElementById('uploadBtn');
  uploadBtn.disabled = true;
  uploadBtn.textContent = '上传中...';

  const progressSection = document.getElementById('progressSection');
  progressSection.classList.add('visible');

  addLog('I', '上传', `开始上传: ${currentFile.name} → ${chip.toUpperCase()}`);
  showToast(`开始上传固件到 ${chip.toUpperCase()}`, 'success');

  try {
    const arrayBuffer = await currentFile.arrayBuffer();
    const fileSize = currentFile.size;

    let body;
    let url = `${getBaseUrl()}/ota/upload`;

    if (chip === 'esp32') {
      const headerJson = JSON.stringify({
        type: 'esp32',
        total_size: fileSize
      });
      const encoder = new TextEncoder();
      const headerBytes = encoder.encode(headerJson);
      const fileBytes = new Uint8Array(arrayBuffer);
      
      const combined = new Uint8Array(headerBytes.length + fileSize);
      combined.set(headerBytes, 0);
      combined.set(fileBytes, headerBytes.length);
      
      body = combined.buffer;
    } else {
      body = arrayBuffer;
    }

    const xhr = new XMLHttpRequest();

    xhr.upload.addEventListener('progress', (e) => {
      if (e.lengthComputable) {
        const percent = Math.round((e.loaded / e.total) * 100);
        uploadProgress = percent;
        document.getElementById('progressBar').style.width = percent + '%';
        document.getElementById('progressText').textContent = 
          `已上传 ${formatBytes(e.loaded)} / ${formatBytes(e.total)}`;
        document.getElementById('progressPercent').textContent = percent + '%';
        document.getElementById('statProgress').textContent = percent + '%';
      }
    });

    xhr.addEventListener('load', () => {
      isUploading = false;
      uploadBtn.disabled = false;
      uploadBtn.textContent = '⬆️ 上传固件';

      if (xhr.status === 200) {
        addLog('I', '上传', '固件上传完成！');
        showToast('固件上传成功', 'success');

        if (chip === 'esp32') {
          document.getElementById('esp32Actions').style.display = 'block';
          // 安全间隔：3 秒后自动调用刷写，留时间确认状态
          let countdown = 3;
          addLog('I', 'OTA', `${countdown} 秒后自动调用 POST /ota/update 完成升级并重启...`);
          const timer = setInterval(() => {
            countdown--;
            if (countdown > 0) {
              addLog('I', 'OTA', `${countdown} 秒后开始刷写...`);
            } else {
              clearInterval(timer);
              addLog('I', 'OTA', '开始刷写！');
              triggerEsp32Update();
            }
          }, 1000);
        }
      } else {
        addLog('E', '上传', `上传失败: HTTP ${xhr.status}`);
        showToast('上传失败', 'error');
      }
    });

    xhr.addEventListener('error', () => {
      isUploading = false;
      uploadBtn.disabled = false;
      uploadBtn.textContent = '⬆️ 上传固件';
      addLog('E', '上传', '上传过程发生错误');
      showToast('上传出错', 'error');
    });

    xhr.open('POST', url);
    xhr.send(body);

  } catch (e) {
    addLog('E', '上传', `上传失败: ${e.message}`);
    showToast('上传失败', 'error');
    isUploading = false;
    uploadBtn.disabled = false;
    uploadBtn.textContent = '⬆️ 上传固件';
  }
}

// ==================== ESP32 OTA 完成升级 ====================
async function triggerEsp32Update() {
  addLog('I', 'OTA', '=== ESP32 OTA 完成，开始重启 ===');
  showToast('ESP32 正在重启...', 'success');

  try {
    const response = await fetch(`${getBaseUrl()}/ota/update`, {
      method: 'POST',
      signal: AbortSignal.timeout(5000)
    });

    if (response.ok) {
      const data = await response.json();
      addLog('I', 'OTA', `升级完成: ${data.message}`);
      showToast('ESP32 重启中...', 'success');
    }
  } catch (e) {
    addLog('E', 'OTA', `升级失败: ${e.message}`);
    showToast('升级失败', 'error');
  }
}

async function restartEsp32() {
  if (!confirm('确定要重启 ESP32 吗？')) return;

  addLog('I', '系统', '=== 请求重启 ESP32 ===');
  showToast('ESP32 正在重启...', 'success');

  try {
    const response = await fetch(`${getBaseUrl()}/restart`, {
      method: 'POST',
      signal: AbortSignal.timeout(5000)
    });

    if (response.ok) {
      const data = await response.json();
      addLog('I', '系统', `重启已触发: ${data.message}`);
      showToast('ESP32 重启中，页面将自动恢复...', 'success');
    }
  } catch (e) {
    addLog('E', '系统', `重启请求失败: ${e.message}`);
    showToast('重启失败', 'error');
  }
}

// ==================== 触发 OTA 中继 ====================
async function triggerRelay() {
  addLog('I', 'OTA', '触发 GD32 OTA 中继...');
  showToast('正在触发 OTA 中继', 'success');

  document.getElementById('stage1').className = 'stage active';
  document.getElementById('statRelayState').textContent = '进行中';

  try {
    const response = await fetch(`${getBaseUrl()}/ota/relay`, {
      method: 'POST',
      signal: AbortSignal.timeout(5000)
    });

    if (response.ok) {
      addLog('I', 'OTA', 'OTA 中继已触发，正在等待 GD32 响应...');

      setTimeout(() => {
        document.getElementById('stage1').className = 'stage completed';
        document.getElementById('stage2').className = 'stage active';
      }, 2000);

      setTimeout(() => {
        document.getElementById('stage2').className = 'stage completed';
        document.getElementById('stage3').className = 'stage active';
      }, 4000);

      setTimeout(() => {
        document.getElementById('stage3').className = 'stage completed';
        document.getElementById('stage4').className = 'stage active';
      }, 6000);

      setTimeout(() => {
        document.getElementById('stage4').className = 'stage completed';
        document.getElementById('statRelayState').textContent = '完成';
        addLog('I', 'OTA', '=== OTA 中继完成 ===');
        showToast('OTA 流程完成！', 'success');
      }, 8000);

    } else {
      addLog('E', 'OTA', `触发失败: HTTP ${response.status}`);
      showToast('触发失败', 'error');
    }
  } catch (e) {
    addLog('E', 'OTA', `触发失败: ${e.message}`);
    showToast('触发失败', 'error');
    document.getElementById('statRelayState').textContent = '失败';
  }
}

// ==================== 轮询状态 ====================
async function pollStatus() {
  try {
    const response = await fetch(`${getBaseUrl()}/system/status`, {
      signal: AbortSignal.timeout(3000)
    });
    if (response.ok) {
      const data = await response.json();
      updateSystemStatusUi(data);
    }
  } catch (e) {
    updateConnectionUi(false, '未连接');
  }
}

setInterval(pollStatus, 1000);

// ==================== ESP32 实时日志拉取 ====================
async function pollLogs() {
  try {
    const response = await fetch(`${getBaseUrl()}/logs`, {
      signal: AbortSignal.timeout(2000)
    });
    if (!response.ok) return;
    const logs = await response.json();
    if (!Array.isArray(logs) || logs.length === 0) return;

    // 解析 [I][WIFI] xxx 格式
    const levelMap = { 'E': 'E', 'W': 'W', 'I': 'I', 'D': 'D' };
    logs.forEach(line => {
      // 格式: [I][WIFI] message 或 [E][OTA] message
      const m = line.match(/^\[([EWID])\]\[([^\]]+)\]\s*(.*)$/);
      if (m) {
        addLog(levelMap[m[1]] || 'I', m[2], m[3].trimEnd());
      } else {
        addLog('I', 'ESP32', line.trimEnd());
      }
    });
  } catch (e) {
    // 静默失败，不打扰用户
  }
}

setInterval(pollLogs, 1000);

setTimeout(() => addLog('I', '系统', '上位机初始化完成，等待固件文件...'), 500);
