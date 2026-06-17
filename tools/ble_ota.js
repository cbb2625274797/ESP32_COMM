// ==================== BLE OTA 控制台脚本 ====================
// 与 WiFi OTA 共享界面元素：进度条、阶段流程、日志面板、连接状态、文件选择。
// 仅传输层不同 (GATT vs HTTP)。通过 connectionMode 变量判断当前模式。
(() => {
  const UUIDS = {
    service: 0xFFFF,
    ctrl: 0x0001,
    dataIn: 0x0002,
    dataOut: 0x0003,
    version: 0x0004,
  };

  const UI_BUILD = '2026-06-15-ble-merged-v2';
  const BLE_OTA_NOTIFY_ACK = 0xA1;
  const BLE_OTA_NOTIFY_FINISH = 0xB1;
  const BLE_OTA_NOTIFY_SHA256 = 0xC1;
  const BLE_OTA_ACK_WINDOW = 32;
  const BLE_OTA_FINISH_ESP32_DONE = 0x02;
  const BLE_OTA_FINISH_REBOOT_NOW = 0x07;
  const BLE_OTA_FINISH_GD32_RELAY_START = 0x10;
  const BLE_OTA_FINISH_GD32_RELAY_DONE = 0x11;
  const BLE_OTA_FINISH_ERR_FINALIZE = 0x81;
  const BLE_OTA_FINISH_ERR_MUTEX = 0x83;
  const BLE_OTA_FINISH_ERR_GD32_SIZE = 0x84;
  const BLE_OTA_FINISH_ERR_SHA_MISMATCH = 0x85;
  const BLE_OTA_FINISH_ERR_GD32_RELAY = 0x86;

  const TARGETS = {
    gd32: {
      label: 'GD32',
      packetCmd: 0x00,
      ctrlHex: 'F7',
      recommendedPayload: 480,
      finishText: '自动触发中继',
      hint: '当前目标：<b>GD32</b>。页面会使用 <b>cmd=0x00</b> 发送数据，写入完成后自动触发 SPI OTA 中继。',
      verifyStage: 6,
      finalizeStage: 5,
      stage5Label: '完成触发',
      stage6Label: '校验比对'
    },
    esp32: {
      label: 'ESP32',
      packetCmd: 0x01,
      ctrlHex: 'F8',
      recommendedPayload: 480,
      finishText: '自动完成 OTA',
      hint: '当前目标：ESP32。页面会使用 <b>cmd=0x01</b> 发送数据，写入完成后发 <b>0xF8</b> + 32 字节 SHA-256 触发设备端收尾：设备端读 OTA 分区算 SHA 并比对，一致则 end+set_boot+reboot；不一致上报 0x85，不重启。',
      verifyStage: 5,
      finalizeStage: 6,
      stage5Label: '校验比对',
      stage6Label: '完成触发'
    }
  };

  const state = {
    device: null,
    server: null,
    service: null,
    ctrlChar: null,
    dataInChar: null,
    dataOutChar: null,
    versionChar: null,
    notificationsEnabled: false,
    selectedFile: null,
    selectedFileHandle: null,  // FileSystemFileHandle for hot reload
    abortRequested: false,
    uploading: false,
    autoFlashing: false,
    lastUploadStatus: 'idle',
    lastNotificationHex: '',
    lastAckCmd: null,
    lastAckSeq: -1,
    lastAckBytes: 0,
    ackWaiter: null,
    otaResultWaiter: null,
    otaRunId: 0,
    lastEsp32DoneSession: null,
    fileSha256: null,
    deviceSha256: null,
    expectedReboot: false,        // 发送 0xFA 后置 true，重连后清
    reconnecting: false,          // 自动重连中
    reconnectAttempts: 0,         // 重连尝试次数
    uploadTargetKey: null,
  };

  const $ = (id) => document.getElementById(id);
  const els = {
    connectBtn: $('connectBtn'),
    disconnectBtn: $('disconnectBtn'),
    autoFlashBtn: $('autoFlashBtn'),
    abortBtn: $('abortBtn'),
    uploadZone: $('uploadZone'),
    firmwareFile: $('firmwareFile'),
    payloadSize: $('payloadSize'),
    ctrlHex: $('ctrlHex'),
    connStatus: $('connStatus'),          // 共享连接指示器
    connStatusTxt: $('connStatusTxt'),    // 共享连接指示器文本
    connState: $('connState'),
    connBadge: $('connBadge'),
    deviceName: $('deviceName'),
    deviceId: $('deviceId'),
    gattState: $('gattState'),
    versionText: $('versionText'),
    fileInfo: $('fileInfo'),
    clearFileBtn: $('clearFileBtn'),
    progressFill: $('progressFill'),      // 共享进度条
    progressTxt: $('progressTxt'),        // 共享进度文本
    progressPct: $('progressPct'),        // 共享进度百分比
    fileName: $('fileName'),
    fileType: $('fileType'),
    fileSize: $('fileSize'),
    packetCount: $('packetCount'),
    sentCount: $('sentCount'),
    logScroll: $('logScroll'),            // 共享日志面板
  };

  function currentTarget() {
    return TARGETS[window.flashTarget] || TARGETS.esp32;
  }

  function flowTarget() {
    return TARGETS[state.uploadTargetKey] || currentTarget();
  }

  const MAX_LOG_LINES = 1000;

  function bleLog(tag, message, level) {
    level = level || 'I';
    if (typeof addLog === 'function') {
      addLog(level, 'BLE-' + tag, message);
    }
  }

  // ====== BLE OTA Stage Flow Management ======
  var bleStageIds = ['bs1','bs2','bs3','bs4','bs5','bs6'];
  var bleStageDetailIds = ['bsd1','bsd2','bsd3','bsd4','bsd5','bsd6'];
  var bleStageLabelIds = ['bsl5', 'bsl6'];

  function updateBleStageLabels(cfg) {
    var stage5 = document.getElementById(bleStageLabelIds[0]);
    var stage6 = document.getElementById(bleStageLabelIds[1]);
    if (stage5) stage5.textContent = cfg.stage5Label || '完成触发';
    if (stage6) stage6.textContent = cfg.stage6Label || '校验比对';
  }

  function resetBleStages() {
    bleStageIds.forEach(function(id) {
      var el = document.getElementById(id);
      if (el) el.className = 'stage-node';
    });
    bleStageDetailIds.forEach(function(id) {
      var el = document.getElementById(id);
      if (el) el.textContent = '';
    });
    var bar = document.getElementById('stageStatusBar');
    if (bar) { bar.className = 'stage-status-bar'; bar.innerHTML = '<span class="ssb-dot"></span><span class="ssb-text" id="stageStatusText">待机</span>'; }
  }

  function setBleStage(stageNum, detail, barState) {
    // stageNum: 1-6, detail: sub-step text
    for (var i = 0; i < bleStageIds.length; i++) {
      var el = document.getElementById(bleStageIds[i]);
      if (!el) continue;
      if (i + 1 < stageNum) el.className = 'stage-node done';
      else if (i + 1 === stageNum) el.className = 'stage-node live';
      else el.className = 'stage-node';
    }
    // Set detail text
    if (stageNum >= 1 && stageNum <= 6) {
      var detailEl = document.getElementById(bleStageDetailIds[stageNum - 1]);
      if (detailEl && detail !== undefined) detailEl.textContent = detail;
    }
    // Update status bar
    var bar = document.getElementById('stageStatusBar');
    if (bar) {
      bar.className = 'stage-status-bar' + (barState ? ' ' + barState : (stageNum > 0 ? ' active' : ''));
    }
  }

  function setBleStageText(text) {
    var txt = document.getElementById('stageStatusText');
    if (txt) txt.textContent = text;
  }

  function setBleStageError(detail) {
    // Mark current active stage as error
    for (var i = 0; i < bleStageIds.length; i++) {
      var el = document.getElementById(bleStageIds[i]);
      if (el && el.className === 'stage-node live') {
        el.className = 'stage-node err';
        var detailEl = document.getElementById(bleStageDetailIds[i]);
        if (detailEl && detail) detailEl.textContent = detail;
      }
    }
    var bar = document.getElementById('stageStatusBar');
    if (bar) bar.className = 'stage-status-bar err';
    setBleStageText('错误: ' + (detail || ''));
  }

  function clearOtaResultWaiter() {
    if (state.otaResultWaiter) {
      clearTimeout(state.otaResultWaiter.timer);
      state.otaResultWaiter = null;
    }
  }

  function resolveOtaResult(status, detail) {
    if (!state.otaResultWaiter) return;
    var waiter = state.otaResultWaiter;
    state.otaResultWaiter = null;
    clearTimeout(waiter.timer);
    waiter.resolve({ status: status, detail: detail || {} });
  }

  function rejectOtaResult(message, detail) {
    if (!state.otaResultWaiter) return;
    var waiter = state.otaResultWaiter;
    state.otaResultWaiter = null;
    clearTimeout(waiter.timer);
    var err = new Error(message || 'OTA 收尾失败');
    err.detail = detail || {};
    waiter.reject(err);
  }

  function nextOtaSessionId() {
    state.otaRunId = (state.otaRunId + 1) & 0xFFFF;
    if (state.otaRunId === 0) state.otaRunId = 1;
    return state.otaRunId;
  }

  function finishBelongsToWaiter(targetKey, value) {
    if (!state.otaResultWaiter || state.otaResultWaiter.targetKey !== targetKey) return false;
    return (value & 0xFFFF) === state.otaResultWaiter.sessionId;
  }

  function finishSessionId(value) {
    return value & 0xFFFF;
  }

  function hasCurrentWaiter(targetKey) {
    return !!(state.otaResultWaiter && state.otaResultWaiter.targetKey === targetKey);
  }

  function waitForOtaResult(targetKey, sessionId, timeoutMs) {
    clearOtaResultWaiter();
    var runId = state.otaRunId;
    return new Promise(function(resolve, reject) {
      var timer = setTimeout(function() {
        if (state.otaResultWaiter && state.otaResultWaiter.runId === runId) {
          state.otaResultWaiter = null;
        }
        reject(new Error(targetKey === 'gd32' ? '等待 GD32 OTA 转发结果超时' : '等待 ESP32 OTA 收尾结果超时'));
      }, timeoutMs);
      state.otaResultWaiter = {
        targetKey: targetKey,
        sessionId: sessionId,
        runId: runId,
        timer: timer,
        resolve: resolve,
        reject: reject,
      };
    });
  }

  function showBleVerifyInfo(frontSha, devSha, result) {
    var panel = document.getElementById('stageVerifyInfo');
    if (panel) { panel.className = 'stage-verify-info'; panel.style.display = 'block'; }
    if (frontSha !== undefined) {
      var el = document.getElementById('sviFrontSha');
      if (el) el.textContent = frontSha || '-';
    }
    if (devSha !== undefined) {
      var el = document.getElementById('sviDeviceSha');
      if (el) el.textContent = devSha || '-';
    }
    if (result !== undefined) {
      var el = document.getElementById('sviResult');
      if (el) {
        el.textContent = result.text || '-';
        el.className = 'svi-val' + (result.ok ? ' ok' : result.err ? ' err' : '');
      }
    }
  }

  function hideBleVerifyInfo() {
    var panel = document.getElementById('stageVerifyInfo');
    if (panel) { panel.className = 'stage-verify-info'; panel.style.display = 'none'; }
  }

  // formatBytes 使用 app.js 中的全局版本

  function toHex(uint8) {
    return Array.from(uint8).map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
  }

  function hexToBytes(hex) {
    const clean = hex.replace(/[^0-9a-fA-F]/g, '');
    if (clean.length === 0 || clean.length % 2 !== 0) throw new Error('HEX 长度必须为偶数');
    const out = new Uint8Array(clean.length / 2);
    for (let i = 0; i < clean.length; i += 2) out[i / 2] = parseInt(clean.slice(i, i + 2), 16);
    return out;
  }

  function setProgress(percent, text) {
    const p = Math.max(0, Math.min(100, percent));
    els.progressFill.style.width = `${p}%`;
    els.progressPct.textContent = `${p.toFixed(0)}%`;
    els.progressTxt.textContent = text;
    // Toggle done class for green finish effect
    if (p >= 100) {
      els.progressFill.classList.add('done');
    } else {
      els.progressFill.classList.remove('done');
    }
  }

  function setReadyHint(html) {
    const el = $('readyHint');
    if (el) el.innerHTML = html;
  }

  function setConnectionState(connected) {
    const idle = connected && state.selectedFile && !state.uploading;
    // 仅在 BLE 模式下更新共享连接指示器，避免覆盖 WiFi 轮询的状态
    if (typeof connectionMode !== 'undefined' && connectionMode === 'ble') {
      if (els.connStatus) els.connStatus.className = connected ? 'conn-indicator ok' : 'conn-indicator err';
      if (els.connStatusTxt) els.connStatusTxt.textContent = connected ? 'BLE 已连接' : 'BLE 未连接';
    }
    // BLE 专用连接徽章 — 仅在 BLE 模式下更新，避免覆盖 WiFi 状态
    if (typeof connectionMode !== 'undefined' && connectionMode === 'ble') {
      if (els.connState) els.connState.textContent = connected ? '已连接' : '未连接';
      if (els.connState && els.connState.parentElement) {
        els.connState.parentElement.className = connected ? 'pill online' : 'pill offline';
      }
      els.connBadge.textContent = connected ? 'online' : 'offline';
      els.connBadge.className = `v ${connected ? 'ok' : 'bad'}`;
      els.gattState.textContent = connected ? 'GATT 已就绪' : '未就绪';
    }
    if (connected) {
      els.connectBtn.disabled = true;
    } else {
      els.connectBtn.disabled = false;
    }
    els.disconnectBtn.disabled = !connected;
    els.payloadSize.disabled = state.uploading;
    els.ctrlHex.disabled = state.uploading;
    els.autoFlashBtn.disabled = !idle;
    els.abortBtn.disabled = !state.uploading;

    if (!connected) {
      setReadyHint('当前未连接设备。建议顺序：连接设备 → 选择固件 → 开始刷写。');
    } else if (!state.notificationsEnabled) {
      setReadyHint('设备已连接，正在自动准备刷写通道。若长时间无响应，请断开后重连。');
    } else if (!state.selectedFile) {
      setReadyHint('设备已连接。下一步请选择固件文件，并确认目标芯片与 Payload。');
    } else {
      setReadyHint('设备与文件都已就绪，可以直接开始刷写或执行一键刷写。');
    }
  }

  function resetUiAfterDisconnect() {
    state.server = null;
    state.service = null;
    state.ctrlChar = null;
    state.dataInChar = null;
    state.dataOutChar = null;
    state.versionChar = null;
    state.notificationsEnabled = false;
    state.uploading = false;
    state.autoFlashing = false;
    state.lastUploadStatus = 'idle';
    state.deviceSha256 = null;
    if (typeof connectionMode === 'undefined' || connectionMode === 'ble') {
      els.deviceName.textContent = '-';
      els.deviceId.textContent = '-';
      if (els.versionText) els.versionText.textContent = '未读取';
      var pv = document.getElementById('pillVer');
      if (pv) pv.textContent = 'v--';
    }
    resetBleStages();
    hideBleVerifyInfo();
    setConnectionState(false);
  }

  function refreshTargetUi() {
    const cfg = currentTarget();
    updateBleStageLabels(cfg);
    els.ctrlHex.value = cfg.ctrlHex;
    els.payloadSize.value = String(cfg.recommendedPayload);
    if (state.selectedFile) updateFileInfo(state.selectedFile);
    setConnectionState(!!state.server);
    // 同步目标胶囊标签
    var espTab = document.getElementById('targetEsp32Tab');
    var gdTab = document.getElementById('targetGd32Tab');
    var isEsp = cfg.label === 'ESP32';
    if (espTab) espTab.classList.toggle('active', isEsp);
    if (gdTab) gdTab.classList.toggle('active', !isEsp);
    bleLog('CFG', `目标切换为 ${cfg.label}: packetCmd=0x${cfg.packetCmd.toString(16).padStart(2, '0').toUpperCase()}, payload=${cfg.recommendedPayload}`);
  }

  async function connectBle() {
    if (!navigator.bluetooth) {
      bleLog('BLE', '当前浏览器不支持 Web Bluetooth', 'E');
      alert('当前浏览器不支持 Web Bluetooth，请使用 Chrome / Edge。');
      return;
    }

    try {
      resetBleStages();
      setBleStage(1, '扫描中...', 'active');
      setBleStageText('正在打开设备选择器...');
      bleLog('BLE', '打开设备选择器...');
      const device = await navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: 'ESP32-OTA' }],
        optionalServices: [UUIDS.service],
      });

      state.device = device;
      state.device.addEventListener('gattserverdisconnected', onDisconnected, { once: true });
      if (typeof connectionMode === 'undefined' || connectionMode === 'ble') {
        els.deviceName.textContent = device.name || '未知设备';
        els.deviceId.textContent = device.id || '-';
      }

      setBleStage(1, '已选择', null);
      setBleStage(2, '连接GATT...', 'active');
      setBleStageText('正在连接 GATT 服务...');
      bleLog('BLE', `已选择设备: ${device.name || 'unknown'}`);
      const server = await device.gatt.connect();
      state.server = server;

      setBleStage(2, '获取服务...', null);
      setBleStageText('获取 GATT 服务和特征...');
      state.service = await server.getPrimaryService(UUIDS.service);
      state.ctrlChar = await state.service.getCharacteristic(UUIDS.ctrl);
      state.dataInChar = await state.service.getCharacteristic(UUIDS.dataIn);
      state.dataOutChar = await state.service.getCharacteristic(UUIDS.dataOut);
      state.versionChar = await state.service.getCharacteristic(UUIDS.version);

      setBleStage(2, '已就绪', null);
      setConnectionState(true);
      bleLog('BLE', 'GATT 服务与特征获取完成');
      await enableNotifications();
      // Mark stages 1-2 as done
      setBleStage(3, '', null);
      setBleStageText('BLE 连接就绪，请选择固件文件');
    } catch (err) {
      bleLog('BLE', `连接失败: ${err.message}`, 'E');
      if (err.name !== 'NotFoundError') {
        setBleStageError(err.message);
      } else {
        resetBleStages();
      }
      resetUiAfterDisconnect();
    }
  }

  function onDisconnected() {
    bleLog('BLE', '设备已断开', 'W');
    var wasExpected = state.expectedReboot;
    resetUiAfterDisconnect();
    if (wasExpected && state.device) {
      // 是主动重启引起的断开，启动自动重连
      state.reconnecting = true;
      state.reconnectAttempts = 0;
      tryReconnect();
    }
  }

  async function disconnectBle() {
    // 用户主动断开：取消任何进行中的重连 / expectedReboot
    state.expectedReboot = false;
    state.reconnecting = false;
    try {
      if (state.dataOutChar && state.notificationsEnabled) {
        await state.dataOutChar.stopNotifications();
      }
    } catch (_) {}
    if (state.device && state.device.gatt && state.device.gatt.connected) {
      state.device.gatt.disconnect();
    }
    resetUiAfterDisconnect();
  }

  async function readVersion() {
    try {
      const value = await state.versionChar.readValue();
      const text = new TextDecoder().decode(value.buffer).replace(/\0/g, '').trim() || '(空)';
      if (typeof connectionMode === 'undefined' || connectionMode === 'ble') {
        if (els.versionText) els.versionText.textContent = text;
        // 更新头部版本显示：仅显示版本号，编译时间放在 title 中悬停显示
        var pv = document.getElementById('pillVer');
        if (pv) {
          var spaceIdx = text.indexOf(' ');
          if (spaceIdx > 0) {
            pv.textContent = 'v' + text.substring(0, spaceIdx);
            pv.dataset.tooltip = '编译: ' + text.substring(spaceIdx + 1);
          } else {
            pv.textContent = 'v' + text;
            delete pv.dataset.tooltip;
          }
        }
      }
      bleLog('BLE', `版本: ${text}`);
    } catch (err) {
      bleLog('BLE', `读取版本失败: ${err.message}`, 'E');
    }
  }

  function handleNotify(event) {
    const view = event.target.value;
    const bytes = new Uint8Array(view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength));
    const hex = toHex(bytes);
    state.lastNotificationHex = hex;

    if (bytes.length >= 8 && bytes[0] === BLE_OTA_NOTIFY_ACK) {
      const ackCmd = bytes[1];
      const ackSeq = bytes[2] | (bytes[3] << 8);
      const ackBytes = (bytes[4]) | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
      state.lastAckCmd = ackCmd;
      state.lastAckSeq = ackSeq;
      state.lastAckBytes = ackBytes >>> 0;

      const ackText = `[ACK] cmd=0x${ackCmd.toString(16).padStart(2, '0').toUpperCase()} seq=${ackSeq} bytes=${state.lastAckBytes} (${formatBytes(state.lastAckBytes)}) raw=${hex}`;
      bleLog('ACK', ackText);

      // ★ 放宽匹配条件：只要 cmd 匹配 + generation 匹配 + 字节数足够，即接受 ACK
      // 不再要求 ackSeq >= expectedSeq，因为 ESP32 可能用较低的 seq 发送 ACK
      // 但字节数已覆盖期望位置（例如 ACK seq=255 bytes=138240 覆盖 seq=287 的数据）
      if (state.ackWaiter && state.ackWaiter.cmd === ackCmd
          && state.ackWaiter.generation === state.ackGeneration) {
        const waiter = state.ackWaiter;
        const expectedBytes = waiter.expectedTotalSize !== undefined
          ? Math.min((waiter.expectedSeq + 1) * waiter.payloadSize, waiter.expectedTotalSize)
          : 0;

        // ★ ACK 字节数不足：立即 resolve，让重试循环立即续传，不等超时
        if (expectedBytes > 0 && state.lastAckBytes < expectedBytes) {
          bleLog('ACK', `ACK 字节数不足: ${state.lastAckBytes}/${expectedBytes}，立即进入重试`, 'W');
          state.ackWaiter = null;
          clearTimeout(waiter.timer);
          waiter.resolve({ seq: ackSeq, bytes: state.lastAckBytes });
          return;
        }

        // 字节数足够，resolve
        state.ackWaiter = null;
        clearTimeout(waiter.timer);
        waiter.resolve({ seq: ackSeq, bytes: state.lastAckBytes });
      }
      return;
    }


    if (bytes.length >= 6 && bytes[0] === BLE_OTA_NOTIFY_FINISH) {
      const stage = bytes[1];
      const value = (bytes[2]) | (bytes[3] << 8) | (bytes[4] << 16) | (bytes[5] << 24);
      const finishMap = {
        [BLE_OTA_FINISH_ESP32_DONE]: '[FINISH] ESP32 OTA 收尾完成',
        [BLE_OTA_FINISH_REBOOT_NOW]: '[FINISH] ESP32 即将重启',
        [BLE_OTA_FINISH_GD32_RELAY_START]: '[FINISH] GD32 OTA 转发已开始',
        [BLE_OTA_FINISH_GD32_RELAY_DONE]: '[FINISH] GD32 OTA 转发完成',
        [BLE_OTA_FINISH_ERR_FINALIZE]: `[FINISH] ESP32 OTA 收尾失败: err=${value}`,
        [BLE_OTA_FINISH_ERR_MUTEX]: '[FINISH] OTA 被其他通道占用，互斥拒绝',
        [BLE_OTA_FINISH_ERR_GD32_SIZE]: `[FINISH] GD32 固件超出分区大小: ${formatBytes(value)} (上限 216KB)`,
        [BLE_OTA_FINISH_ERR_SHA_MISMATCH]: '[FINISH] OTA SHA-256 校验失败',
        [BLE_OTA_FINISH_ERR_GD32_RELAY]: `[FINISH] GD32 OTA 转发失败: result=${value >> 16}`,
      };
      const finishText = finishMap[stage] || `[FINISH] 未知阶段 stage=0x${stage.toString(16).padStart(2, '0').toUpperCase()} value=${value}`;
      bleLog('FINISH', finishText);
      const finalizeStage = TARGETS.esp32.finalizeStage;
      const resultCode = (value >>> 16);

      if (stage === BLE_OTA_FINISH_GD32_RELAY_START) {
        if (!finishBelongsToWaiter('gd32', value)) return;
        if (typeof switchStageFlow === 'function') switchStageFlow('gd32');
        if (typeof setActivePhase === 'function') setActivePhase('gd32Ota');
        if (typeof updateStagesFromRelayState === 'function') updateStagesFromRelayState(1);
        setProgress(50, '[GD32] OTA 转发已开始，正在认证握手...');
        showBleVerifyInfo(state.fileSha256 || '-', '等待 GD32 APP 启动确认...', { text: '缓存写入完成，正在通过 SPI 转发' });
        return;
      }

      if (stage === BLE_OTA_FINISH_GD32_RELAY_DONE) {
        if (!finishBelongsToWaiter('gd32', value)) return;
        if (typeof switchStageFlow === 'function') switchStageFlow('gd32');
        if (typeof updateStagesFromRelayState === 'function') updateStagesFromRelayState(5);
        if (typeof markPhaseDone === 'function') markPhaseDone('gd32Ota');
        setProgress(100, '[GD32] OTA 转发完成，APP 已启动');
        showBleVerifyInfo(state.fileSha256 || '-', 'GD32 APP 已确认启动', { text: 'GD32 OTA 完成', ok: true });
        var gd32Bar = document.getElementById('stageStatusBar');
        var gd32Text = document.getElementById('stageStatusText');
        if (gd32Bar) gd32Bar.className = 'stage-status-bar ok';
        if (gd32Text) gd32Text.textContent = 'GD32 刷写完成';
        state.lastUploadStatus = 'success';
        showToast('GD32 OTA 刷写完成', 'success');
        resolveOtaResult('success', { stage: stage, value: value });
        return;
      }

      if (stage === BLE_OTA_FINISH_ERR_GD32_RELAY) {
        if (!finishBelongsToWaiter('gd32', value)) return;
        if (typeof switchStageFlow === 'function') switchStageFlow('gd32');
        if (typeof updateStagesFromRelayState === 'function') updateStagesFromRelayState(255);
        setProgress(0, '[GD32] OTA 转发失败');
        state.lastUploadStatus = 'failed';
        showToast('GD32 OTA 转发失败', 'error');
        showBleVerifyInfo(state.fileSha256 || '-', 'GD32 未完成转发', { text: 'GD32 OTA 转发失败: ' + resultCode, err: true });
        rejectOtaResult('GD32 OTA 转发失败: ' + resultCode, { stage: stage, value: value });
        return;
      }

      // Update BLE stage flow based on FINISH notifications
      if (stage === BLE_OTA_FINISH_ESP32_DONE) {
        if (!finishBelongsToWaiter('esp32', value)) return;
        state.lastEsp32DoneSession = finishSessionId(value);
        setBleStage(finalizeStage, '收尾完成', null);
        setBleStageText('OTA 收尾完成，等待重启');
        state.lastUploadStatus = 'success';
        setProgress(100, '[ESP32] OTA 收尾完成，等待重启');
        resolveOtaResult('success', { stage: stage, value: value });
      } else if (stage === BLE_OTA_FINISH_REBOOT_NOW) {
        if (state.lastEsp32DoneSession !== null && state.lastEsp32DoneSession !== finishSessionId(value)) return;
        state.expectedReboot = true;
        setBleStage(finalizeStage, '设备重启', null);
        setBleStageText('ESP32 即将重启');
        var bar = document.getElementById('stageStatusBar');
        if (bar) bar.className = 'stage-status-bar ok';
      }

      // OTA 结果 toast 提示（用户可见，不只是在日志里）
      if (stage === BLE_OTA_FINISH_ERR_MUTEX) {
        if (hasCurrentWaiter('esp32') && !finishBelongsToWaiter('esp32', value)) return;
        if (hasCurrentWaiter('gd32') && !finishBelongsToWaiter('gd32', value)) return;
        showToast('OTA 被其他通道占用，请稍后重试', 'error');
        setProgress(0, 'OTA 被其他通道占用');
        state.lastUploadStatus = 'failed';
        setBleStageError('OTA 被其他通道占用');
        rejectOtaResult('OTA 被其他通道占用', { stage: stage, value: value });
      } else if (stage === BLE_OTA_FINISH_ERR_GD32_SIZE) {
        if (hasCurrentWaiter('gd32') && !finishBelongsToWaiter('gd32', value)) return;
        showToast(`GD32 固件超出分区大小 (${formatBytes(value)}，上限 216KB)`, 'error');
        setProgress(0, 'GD32 固件太大，无法刷写');
        state.lastUploadStatus = 'failed';
        setBleStageError('固件超出分区大小');
        rejectOtaResult('GD32 固件超出分区大小', { stage: stage, value: value });
      } else if (stage === BLE_OTA_FINISH_ERR_FINALIZE) {
        if (!finishBelongsToWaiter('esp32', value)) return;
        showToast('ESP32 OTA 刷写失败', 'error');
        setProgress(0, '刷写失败');
        state.lastUploadStatus = 'failed';
        setBleStageError('OTA 刷写失败');
        showBleVerifyInfo(state.fileSha256 || '-', state.deviceSha256 || '设备未上报', { text: '刷写失败', err: true });
        rejectOtaResult('ESP32 OTA 刷写失败', { stage: stage, value: value });
      } else if (stage === BLE_OTA_FINISH_ERR_SHA_MISMATCH) {
        const isGd32Waiter = hasCurrentWaiter('gd32');
        const targetKey = isGd32Waiter ? 'gd32' : 'esp32';
        if (!finishBelongsToWaiter(targetKey, value)) return;
        const msg = isGd32Waiter
          ? 'GD32 缓存 SHA-256 校验失败，未进入 SPI 转发'
          : 'ESP32 OTA 校验失败，未触发重启';
        showToast(msg, 'error');
        setProgress(0, '设备端 SHA-256 校验失败');
        state.lastUploadStatus = 'failed';
        setBleStage(flowTarget().verifyStage, 'SHA-256 ✗', null);
        setBleStageError('设备端 SHA-256 校验失败');
        showBleVerifyInfo(state.fileSha256 || '-', state.deviceSha256 || '设备未上报', { text: msg, err: true });
        rejectOtaResult(msg, { stage: stage, value: value });
      } else if (stage === BLE_OTA_FINISH_REBOOT_NOW) {
        if (state.lastEsp32DoneSession !== null && state.lastEsp32DoneSession !== finishSessionId(value)) return;
        showToast('ESP32 OTA 刷写完成，等待重启', 'success');
        // If device SHA-256 hasn't arrived yet, show what we have
        var devSha = state.deviceSha256 || '设备未上报';
        if (state.deviceSha256 && state.fileSha256 && state.deviceSha256.toUpperCase() === state.fileSha256.toUpperCase()) {
          showBleVerifyInfo(state.fileSha256 || '-', devSha, { text: '校验通过 — SHA-256 一致，设备重启中', ok: true });
        } else if (state.deviceSha256) {
          showBleVerifyInfo(state.fileSha256 || '-', devSha, { text: '校验失败 — SHA-256 不一致', err: true });
        } else {
          showBleVerifyInfo(state.fileSha256 || '-', devSha, { text: '刷写成功，设备未上报 SHA-256（重启中）' });
        }
      }
      // 错误类 FINISH 事件立即中断 waitForAck，并中止发送循环
      if (stage >= 0x80) {
        state.abortRequested = true;  // ★ 阻止 fire-and-forget 继续发包
        state.expectedReboot = false; // ★ 刷写失败，取消任何重连退避
        state.reconnecting = false;
        state.reconnectAttempts = 0;
        if (state.ackWaiter) {
          const waiter = state.ackWaiter;
          state.ackWaiter = null;
          clearTimeout(waiter.timer);
          waiter.reject(new Error(`设备端 FINISH 错误: stage=0x${stage.toString(16).padStart(2, '0').toUpperCase()} value=${value}`));
        }
      }
      return;
    }

    // SHA-256 verification from device (after transfer complete)
    if (bytes[0] === BLE_OTA_NOTIFY_SHA256 && bytes.length >= 33) {
      if (bytes.length >= 35) {
        var shaSessionId = bytes[33] | (bytes[34] << 8);
        if (!state.otaResultWaiter ||
            state.otaResultWaiter.sessionId !== shaSessionId) {
          bleLog('VERIFY', '忽略非本轮 SHA-256 通知: session=' + shaSessionId, 'W');
          return;
        }
      }
      var devSha = Array.from(bytes.slice(1, 33)).map(function(b) {
        return b.toString(16).padStart(2, '0');
      }).join('').toUpperCase();
      state.deviceSha256 = devSha;
      bleLog('VERIFY', '设备端 SHA-256: ' + devSha);

      // 设备端上报 SHA-256 后，前端仅做展示与状态同步。
      var frontSha = state.fileSha256;
      var verifyStage = flowTarget().verifyStage;
      if (frontSha && frontSha.toUpperCase() === devSha) {
        bleLog('VERIFY', 'SHA-256 校验通过，前端与设备端一致', 'I');
        showBleVerifyInfo(frontSha, devSha, { text: '校验通过 — SHA-256 一致', ok: true });
        setBleStage(verifyStage, 'SHA-256 ✓', null);
        setBleStageText('校验通过 — 固件完整性已确认');
      } else if (frontSha) {
        bleLog('VERIFY', 'SHA-256 校验失败！前端: ' + frontSha + ' 设备: ' + devSha, 'E');
        showBleVerifyInfo(frontSha, devSha, { text: '校验失败 — SHA-256 不一致', err: true });
        setBleStage(verifyStage, 'SHA-256 ✗', null);
        setBleStageError('SHA-256 不一致');
      } else {
        showBleVerifyInfo(undefined, devSha, { text: '前端 SHA-256 不可用，无法比对' });
        setBleStage(verifyStage, '无法比对', null);
      }
      return;
    }


    bleLog('NOTIFY', hex);
  }


  // ===== 重启设备 + 自动重连 =====
  async function restartEsp32() {
    if (!state.ctrlChar || !state.device) return;
    if (!confirm('确定要重启 ESP32 吗？\n\n重启后页面会自动重连。')) return;
    if (state.expectedReboot) return;
    state.expectedReboot = true;
    state.reconnectAttempts = 0;
    if (typeof connectionMode !== 'undefined' && connectionMode === 'ble') {
      if (els.connState) els.connState.textContent = '重启中';
      if (els.connState && els.connState.parentElement) els.connState.parentElement.className = 'pill processing';
    }
    setConnectionState(!!state.server);
    bleLog('SYS', '=== 触发 ESP32 重启 (0xFA) ===');
    showToast('ESP32 重启中...', 'success');
    try {
      await state.ctrlChar.writeValue(new Uint8Array([0xFA]));
      bleLog('SYS', '0xFA 已发，等待设备端推送 FINISH_REBOOT_NOW 后断开');
    } catch (e) {
      bleLog('SYS', '0xFA 发送失败: ' + e.message, 'E');
      showToast('0xFA 发送失败', 'error');
      state.expectedReboot = false;
      setConnectionState(!!state.server);
    }
  }

  async function tryReconnect() {
    if (!state.device || !state.expectedReboot) {
      state.reconnecting = false;
      return;
    }
    if (!state.device.gatt) {
      // 设备引用已丧失，重连不了
      state.reconnecting = false;
      state.expectedReboot = false;
      showToast('设备引用丧失，请手动重连', 'error');
      return;
    }
    state.reconnecting = true;
    state.reconnectAttempts += 1;
    var attempt = state.reconnectAttempts;
    // 退避：1s, 2s, 4s, 8s, 上限 10s
    var backoff = Math.min(10000, 1000 * Math.pow(1.6, attempt - 1));
    if (backoff > 200) await sleep(backoff);
    if (typeof connectionMode !== 'undefined' && connectionMode === 'ble') {
      if (els.connState) els.connState.textContent = '重连中(' + attempt + ')';
      if (els.connState && els.connState.parentElement) els.connState.parentElement.className = 'pill processing';
    }
    setConnectionState(!!state.server);
    bleLog('BLE', '自动重连第 ' + attempt + ' 次' + (backoff > 0 ? '（等待 ' + Math.round(backoff) + 'ms）' : '') + '...', 'W');
    try {
      var server = await state.device.gatt.connect();
      state.server = server;
      state.service = await server.getPrimaryService(UUIDS.service);
      state.ctrlChar = await state.service.getCharacteristic(UUIDS.ctrl);
      state.dataInChar = await state.service.getCharacteristic(UUIDS.dataIn);
      state.dataOutChar = await state.service.getCharacteristic(UUIDS.dataOut);
      state.versionChar = await state.service.getCharacteristic(UUIDS.version);
      state.notificationsEnabled = false;
      await enableNotifications();
      state.expectedReboot = false;
      state.reconnecting = false;
      state.reconnectAttempts = 0;
      // 重连后重注 onDisconnected（{once: true}，旧的已被上次断开时消耗）
      state.device.addEventListener('gattserverdisconnected', onDisconnected, { once: true });
      bleLog('BLE', '✅ 自动重连成功', 'I');
      showToast('BLE 已重连', 'success');
      setConnectionState(true);
      // 重连后默认刷新一下版本
      try { await readVersion(); } catch (_) {}
    } catch (e) {
      bleLog('BLE', '重连第 ' + attempt + ' 次失败: ' + e.message, 'W');
      if (state.expectedReboot && attempt < 12) {
        tryReconnect();
      } else {
        state.reconnecting = false;
        state.expectedReboot = false;
        showToast('重连失败，请手动连接', 'error');
        bleLog('BLE', '自动重连放弃，请点击「连接设备」重连', 'E');
        setConnectionState(!!state.server);
      }
    }
  }

  function waitForAck(cmd, expectedSeq, timeoutMs = 8000, opts = {}) {
    const gen = state.ackGeneration;
    // ★ 提前返回：只要 cmd 匹配 + generation 匹配 + 字节数足够，即可 resolve
    // 不再要求 lastAckSeq >= expectedSeq（与 handleNotify 保持一致）
    if (state.lastAckCmd === cmd && gen === state.ackGeneration) {
      const expectedBytes = opts.expectedTotalSize !== undefined
        ? Math.min((expectedSeq + 1) * (opts.payloadSize || 480), opts.expectedTotalSize)
        : 0;
      if (expectedBytes === 0 || state.lastAckBytes >= expectedBytes) {
        return Promise.resolve({ seq: state.lastAckSeq, bytes: state.lastAckBytes });
      }
      // 字节数不够，不提前 resolve，等待后续 ACK
    }
    if (state.ackWaiter) {
      clearTimeout(state.ackWaiter.timer);
      state.ackWaiter.reject(new Error('ACK waiter 被新的窗口覆盖'));
      state.ackWaiter = null;
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        if (state.ackWaiter && state.ackWaiter.expectedSeq === expectedSeq && state.ackWaiter.cmd === cmd) {
          state.ackWaiter = null;
        }
        reject(new Error(`等待 ACK 超时: cmd=0x${cmd.toString(16).padStart(2, '0').toUpperCase()} seq=${expectedSeq}`));
      }, timeoutMs);
      state.ackWaiter = {
        cmd, expectedSeq, resolve, reject, timer, generation: gen,
        expectedTotalSize: opts.expectedTotalSize,
        payloadSize: opts.payloadSize,
      };
    });
  }

  async function enableNotifications() {
    try {
      await state.dataOutChar.startNotifications();
      state.dataOutChar.removeEventListener('characteristicvaluechanged', handleNotify);
      state.dataOutChar.addEventListener('characteristicvaluechanged', handleNotify);
      state.notificationsEnabled = true;
      setConnectionState(!!state.server);
      bleLog('BLE', 'DATA_OUT 通知已自动开启');
    } catch (err) {
      state.notificationsEnabled = false;
      setConnectionState(!!state.server);
      bleLog('BLE', `自动开启 DATA_OUT 通知失败: ${err.message}`, 'E');
    }
  }

  function updateFileInfo(file) {
    els.fileName.textContent = file ? file.name : '-';
    els.fileType.textContent = file ? (file.name.toLowerCase().includes('.hex') ? 'Intel HEX' : 'Binary') : '-';
    els.fileSize.textContent = file ? formatBytes(file.size) : '0 B';
    const payloadSize = Number(els.payloadSize.value) || 480;
    const packets = file ? Math.ceil(file.size / payloadSize) : 0;
    els.packetCount.textContent = String(packets);
    els.sentCount.textContent = '0';

    state.selectedFile = file || null;
    state.lastUploadStatus = 'idle';

    if (els.fileInfo) {
      if (file) els.fileInfo.classList.add('show');
      else els.fileInfo.classList.remove('show');
    }

    // ★ HEX → BIN 自动转换：复用 app.js 公共函数 convertHexFileToBin
    if (file && file.name.toLowerCase().endsWith('.hex') && typeof convertHexFileToBin === 'function') {
      return convertHexFileToBin(file).then(function(result) {
        if (!result.converted) {
          bleLog('FILE', file.name + ' 扩展名为 .hex 但内容不像 HEX 格式，按原始文件处理', 'W');
          finalizeFileInfo(file);
          computeFileSha256(file);
          return;
        }
        state.selectedFile = result.file;
        var baseHex = '0x' + result.baseAddr.toString(16).toUpperCase().padStart(8, '0');
        bleLog('FILE', 'HEX → BIN 转换完成: ' + file.name + ' → ' + result.file.name + ' (基地址 ' + baseHex + ', ' + formatBytes(result.file.size) + ')');
        showToast('HEX 已自动转换为 BIN', 'success');
        // 更新 UI 显示转换后的信息
        els.fileName.textContent = result.file.name + ' (由 ' + file.name + ' 转换)';
        els.fileType.textContent = 'Binary (HEX转换)';
        els.fileSize.textContent = formatBytes(result.file.size);
        var newPackets = Math.ceil(result.file.size / payloadSize);
        els.packetCount.textContent = String(newPackets);
        finalizeFileInfo(result.file);
        computeFileSha256(result.file);
      }).catch(function(err) {
        bleLog('FILE', 'HEX 解析失败: ' + err.message, 'E');
        showToast('HEX 文件解析失败: ' + err.message, 'error');
        state.selectedFile = null;
        state.fileSha256 = null;
        if (els.fileInfo) els.fileInfo.classList.remove('show');
        setConnectionState(!!state.server);
      });
    }

    // 非 HEX 文件直接完成
    finalizeFileInfo(file);
    computeFileSha256(file);
    return Promise.resolve();
  }

  function finalizeFileInfo(file) {
    setConnectionState(!!state.server);
    if (file) {
      setProgress(0, `已选择 ${file.name}`);
      bleLog('FILE', `已选择: ${file.name} (${formatBytes(file.size)}), 目标=${currentTarget().label}`);
      if (state.server) {
        setBleStage(3, '计算哈希...', 'active');
        setBleStageText('正在计算文件 SHA-256...');
      }
    } else {
      setProgress(0, '等待选择文件');
      if (state.server) {
        resetBleStages();
        setBleStage(3, '', null);
        setBleStageText('BLE 连接就绪，请选择固件文件');
      }
    }
  }

  function computeFileSha256(file) {
    var shaElem = document.getElementById('fileSha256');
    if (file && shaElem) {
      shaElem.textContent = '计算中...';
      file.arrayBuffer().then(function(buf) {
        return crypto.subtle.digest('SHA-256', buf);
      }).then(function(hashBuffer) {
        var hashArray = Array.from(new Uint8Array(hashBuffer));
        var hashHex = hashArray.map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
        shaElem.textContent = hashHex.toUpperCase();
        state.fileSha256 = hashHex.toUpperCase();
        bleLog('FILE', 'SHA-256: ' + hashHex.toUpperCase());
        // If connected, advance to stage 3 done
        if (state.server) {
          setBleStage(3, 'SHA-256 已计算', null);
          setBleStage(4, '', null);
          setBleStageText('文件就绪，可以开始刷写');
        }
      }).catch(function(err) {
        shaElem.textContent = '计算失败: ' + err.message;
        state.fileSha256 = null;
      });
    } else if (shaElem) {
      shaElem.textContent = '-';
      state.fileSha256 = null;
    }
  }

  async function syncSelectedFileFromInput() {
    // FSAPI 选择的文件以 selectedFileHandle 为准，不从 input 元素同步，避免切标签页回来误清空
    if (state.selectedFileHandle) return;
    const file = (els.firmwareFile && els.firmwareFile.files && els.firmwareFile.files[0]) || null;
    await updateFileInfo(file);
  }

  // 通过 File System Access API 选择文件（支持热重载）
  async function pickFileViaFSAPI() {
    if (!window.showOpenFilePicker) {
      bleLog('FILE', '当前浏览器不支持 File System Access API', 'W');
      return false;
    }
    try {
      const handles = await window.showOpenFilePicker({
        types: [{ description: '固件文件', accept: { 'application/octet-stream': ['.bin', '.hex'] } }],
        multiple: false
      });
      if (handles && handles.length > 0) {
        const handle = handles[0];
        const file = await handle.getFile();
        state.selectedFileHandle = handle;
        await updateFileInfo(file);
        bleLog('FILE', '已通过 FSAPI 选择文件（支持热重载）: ' + file.name);
        return true;
      }
    } catch (e) {
      if (e.name !== 'AbortError') {
        bleLog('FILE', 'FSAPI 选择失败: ' + e.message, 'W');
      }
    }
    return false;
  }

  function clearSelectedFile() {
    state.selectedFile = null;
    state.selectedFileHandle = null;
    state.lastUploadStatus = 'idle';
    state.fileSha256 = null;
    state.deviceSha256 = null;
    if (els.firmwareFile) els.firmwareFile.value = '';
    updateFileInfo(null);
    hideBleVerifyInfo();
    // Reset stages back to connection-ready state
    if (state.server) {
      resetBleStages();
      setBleStage(3, '', null);
      setBleStageText('BLE 连接就绪，请选择固件文件');
    }
    bleLog('FILE', '文件已清除');
  }

  function buildPacket(seq, totalSize, payload, cmd) {
    const packet = new Uint8Array(7 + payload.length);
    packet[0] = cmd;
    packet[1] = seq & 0xFF;
    packet[2] = (seq >> 8) & 0xFF;
    packet[3] = totalSize & 0xFF;
    packet[4] = (totalSize >> 8) & 0xFF;
    packet[5] = (totalSize >> 16) & 0xFF;
    packet[6] = (totalSize >> 24) & 0xFF;
    packet.set(payload, 7);
    return packet;
  }

  async function sleep(ms) {
    return new Promise(r => setTimeout(r, ms));
  }


  async function uploadFirmware() {
    // 每次刷写前尝试重新从本地读取文件
    if (state.selectedFileHandle) {
      try {
        const freshFile = await state.selectedFileHandle.getFile();
        await updateFileInfo(freshFile);
        var actualFile = state.selectedFile || freshFile;
        bleLog('FILE', '已重新读取本地文件: ' + actualFile.name + ' (' + formatBytes(actualFile.size) + ')');
      } catch (e) {
        bleLog('FILE', '重新读取文件失败: ' + e.message + '，将使用缓存版本', 'W');
      }
    } else {
      await syncSelectedFileFromInput();
    }
    if (!state.selectedFile || !state.dataInChar) return false;

    const cfg = currentTarget();
    state.uploadTargetKey = window.flashTarget || 'esp32';
    const payloadSize = Math.max(32, Math.min(498, Number(els.payloadSize.value) || 480));
    const file = state.selectedFile;

    state.abortRequested = false;
    state.uploading = true;
    state.lastUploadStatus = 'uploading';
    const otaSessionId = nextOtaSessionId();
    clearOtaResultWaiter();
    switchStageFlow('ble');
    state.lastAckCmd = null;
    state.lastAckSeq = -1;
    state.lastAckBytes = 0;
    state.ackGeneration = 0;
    state.deviceSha256 = null;
    state.lastEsp32DoneSession = null;
    if (state.ackWaiter) {
      clearTimeout(state.ackWaiter.timer);
      state.ackWaiter = null;
    }
    setConnectionState(true);

    // Stage 4: Data writing
    setBleStage(4, '准备中...', 'active');
    setBleStageText('正在准备数据写入...');
    hideBleVerifyInfo();

    try {
      if (!state.notificationsEnabled) {
        await enableNotifications();
      }
      if (!state.notificationsEnabled) {
        throw new Error('DATA_OUT 通知未开启，无法使用 ACK 模式');
      }

      bleLog('OTA', `开始准备固件: target=${cfg.label}, file=${file.name}, payload=${payloadSize}, cmd=0x${cfg.packetCmd.toString(16).padStart(2, '0').toUpperCase()}, ackWindow=${BLE_OTA_ACK_WINDOW}`);
      setProgress(0, `[${cfg.label}] 正在读取固件文件...`);
      setBleStage(4, '读取固件...', null);
      const fileBytes = new Uint8Array(await file.arrayBuffer());
      const totalSize = fileBytes.length;
      const packetTotal = Math.ceil(totalSize / payloadSize);
      bleLog('OTA', `固件已读取: totalSize=${totalSize}, packetTotal=${packetTotal}, file.size=${file.size}`);
      if (totalSize !== file.size) {
        bleLog('OTA', `⚠️ file.size(${file.size}) ≠ arrayBuffer(${totalSize})，以 arrayBuffer 为准，请确认页面已 Ctrl+F5 刷新`, 'W');
      }

      // SHA-256 已在文件选择时（updateFileInfo）计算并存入 state.fileSha256。
      // 此处不再重复打日志（避免与 [FILE] SHA-256 重复），仅保证 fileSha256 变量有值。
      var fileSha256;
      if (state.fileSha256) {
        fileSha256 = state.fileSha256.toLowerCase();
      } else {
        // 极端兜底：state.fileSha256 未初始化（理论上 updateFileInfo 总会先设置）
        bleLog('OTA', '正在计算文件 SHA-256 哈希...');
        const hashBuffer = await crypto.subtle.digest('SHA-256', fileBytes.buffer);
        const hashArray = Array.from(new Uint8Array(hashBuffer));
        fileSha256 = hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
        state.fileSha256 = fileSha256.toUpperCase();
      }
      // Update SHA display in file info
      var shaElem = document.getElementById('fileSha256');
      if (shaElem) shaElem.textContent = fileSha256.toUpperCase();

      setBleStage(4, '写入中...', null);
      setBleStageText(`正在写入 ${packetTotal} 包数据...`);
      bleLog('OTA', `开始写入 DATA_IN: target=${cfg.label}, file=${file.name}, total=${totalSize}, packets=${packetTotal}, payload=${payloadSize}, windowAck=${BLE_OTA_ACK_WINDOW}`);

      const MAX_WINDOW_RETRIES = 3;
      let bleWriteTs = 0;  // 上一次 BLE 写入时间戳，用于节流
      const BLE_WRITE_INTERVAL = 2;  // 最小写入间隔 (ms)，避免 BLE 栈过载
      for (let windowStart = 0; windowStart < packetTotal; windowStart += BLE_OTA_ACK_WINDOW) {
        if (state.abortRequested) throw new Error('用户中止');

        const endSeq = Math.min(windowStart + BLE_OTA_ACK_WINDOW, packetTotal) - 1;

        // 流水线发送：不逐包 await，减少 BLE 栈 per-packet 开销
        for (let seq = windowStart; seq <= endSeq; seq++) {
          const start = seq * payloadSize;
          const end = Math.min(start + payloadSize, totalSize);
          const payload = fileBytes.slice(start, end);
          const packet = buildPacket(seq, totalSize, payload, cfg.packetCmd);

          // 节流：避免过快地向 BLE 栈提交写入
          const now = performance.now();
          const gap = now - bleWriteTs;
          if (gap < BLE_WRITE_INTERVAL) {
            await sleep(BLE_WRITE_INTERVAL - gap);
          }

          // ★ seq=0 必须用 writeValueWithResponse（带响应）：
          // seq=0 触发设备端 esp_ota_begin()（flash 擦除 + OTA 会话创建），
          // 若用 writeValueWithoutResponse 在 BLE 链路层丢失，
          // 设备永远不会启动 OTA 会话，后续所有包都被丢弃。
          if (seq === 0) {
            try {
              await state.dataInChar.writeValueWithResponse(packet);
              bleLog('OTA', 'seq=0 已用 writeValueWithResponse 确认送达', 'I');
            } catch (e) {
              bleLog('OTA', `seq=0 写入失败: ${e.message}`, 'E');
              throw new Error('seq=0 写入失败，OTA 会话无法启动: ' + e.message);
            }
            // seq=0 写入后等待设备完成 esp_ota_begin()（flash 擦除较耗时）
            await sleep(200);
          } else {
            state.dataInChar.writeValueWithoutResponse(packet);  // fire-and-forget
          }
          bleWriteTs = performance.now();

          const percent = ((seq + 1) / packetTotal) * 100;
          els.sentCount.textContent = String(seq + 1);
          setProgress(percent, `[${cfg.label}] 已写 ${seq + 1}/${packetTotal} 包`);
          setBleStage(4, `${seq + 1}/${packetTotal} 包`, null);
          setBleStageText(`数据写入 ${Math.round(percent)}%`);
        }

        state.ackGeneration++;
        try {
          await waitForAck(cfg.packetCmd, endSeq, 3000, { expectedTotalSize: totalSize, payloadSize });
        } catch (e) {
          bleLog('OTA', `ACK 等待失败: ${e.message}，进入重试`, 'W');
        }

        let expectedBytes = Math.min((endSeq + 1) * payloadSize, totalSize);
        let retryCount = 0;
        let prevAckBytes = state.lastAckBytes;  // ★ 进度跟踪：检测重试是否有效
        // ★ 仅以字节数判断是否完成，不依赖 ackOk（generation 不匹配时 ackOk 不可靠）
        while (state.lastAckBytes < expectedBytes && retryCount < MAX_WINDOW_RETRIES) {
          // 如果 ESP32 已确认所有数据（ACK 延迟到达），直接跳出
          if (state.lastAckBytes >= expectedBytes) break;

          // ★ 进度检测：如果重试后字节数没有增长，说明续传无效
          if (retryCount > 0 && state.lastAckBytes <= prevAckBytes) {
            bleLog('OTA', `重试无进展: ackBytes=${state.lastAckBytes} 未增长 (前次=${prevAckBytes})，放弃`, 'E');
            break;
          }
          prevAckBytes = state.lastAckBytes;

          // 从 ESP32 实际确认的位置续传，而非重发整个窗口
          const resumeSeq = Math.max(windowStart, Math.ceil(state.lastAckBytes / payloadSize));
          const missing = expectedBytes - state.lastAckBytes;
          const missingPackets = Math.ceil(missing / payloadSize);
          retryCount++;
          bleLog('OTA', `丢包检测: seq=${endSeq} 丢失${missing}字节(${missingPackets}包)，从 seq=${resumeSeq} 续发 (非全窗口重发)，第${retryCount}次`, 'W');
          setBleStage(4, `续发 #${retryCount} (seq=${resumeSeq})`, null);
          setBleStageText(`丢包续发中 (${retryCount}/${MAX_WINDOW_RETRIES})...`);

          // ★ 等待设备端 reorder buffer flush + ACK 回传。
          //   设备端可能正在 flush 缺口包（getReceived 跳跃增长），
          //   留足时间让最新的 ACK 到达，避免重发已 flush 的包。
          await sleep(150);

          for (let seq = resumeSeq; seq <= endSeq; seq++) {
            const start = seq * payloadSize;
            const end = Math.min(start + payloadSize, totalSize);
            const payload = fileBytes.slice(start, end);
            const packet = buildPacket(seq, totalSize, payload, cfg.packetCmd);
            // 流水线发送 + 节流
            const now = performance.now();
            const gap = now - bleWriteTs;
            if (gap < BLE_WRITE_INTERVAL) await sleep(BLE_WRITE_INTERVAL - gap);
            // ★ 重试路径中 seq=0 也必须用 writeValueWithResponse
            if (seq === 0) {
              try {
                await state.dataInChar.writeValueWithResponse(packet);
                bleLog('OTA', '重试 seq=0 已用 writeValueWithResponse 确认', 'I');
              } catch (e) {
                bleLog('OTA', `重试 seq=0 写入失败: ${e.message}`, 'E');
              }
              await sleep(200);
            } else {
              state.dataInChar.writeValueWithoutResponse(packet);
            }
            bleWriteTs = performance.now();
          }

          state.ackGeneration++;
          try {
            await waitForAck(cfg.packetCmd, endSeq, 3000, { expectedTotalSize: totalSize, payloadSize });
          } catch (e) {
            bleLog('OTA', `重试 ACK 等待失败: ${e.message}`, 'W');
          }
          expectedBytes = Math.min((endSeq + 1) * payloadSize, totalSize);
        }

        if (state.lastAckBytes < expectedBytes) {
          const missing = expectedBytes - state.lastAckBytes;
          throw new Error(`BLE 丢包检测: seq=${endSeq} 期望${expectedBytes}字节 实际${state.lastAckBytes}字节 丢失${missing}字节(${(missing / payloadSize).toFixed(1)}包) 已重试${MAX_WINDOW_RETRIES}次`);
        }
      }

      // Stage 4 done
      setBleStage(4, '完成', null);
      setProgress(cfg.label === 'GD32' ? 45 : 90, `[${cfg.label}] 固件数据已全部写入 DATA_IN`);

      // Stage 5: 发送 0xF8 + 32B 文件 SHA-256，由设备端执行收尾并完成比对
      if (cfg.label === 'ESP32') {
        state.deviceSha256 = null;
        const frontSha = fileSha256.toUpperCase();
        const frontShaBytes = new Uint8Array(32);
        for (let i = 0; i < 32; i++) {
          frontShaBytes[i] = parseInt(frontSha.slice(i * 2, i * 2 + 2), 16);
        }
        const trigger = new Uint8Array(35);
        trigger[0] = 0xF8;
        trigger.set(frontShaBytes, 1);
        trigger[33] = otaSessionId & 0xFF;
        trigger[34] = (otaSessionId >> 8) & 0xFF;
        const resultPromise = waitForOtaResult('esp32', otaSessionId, 45000);
        showBleVerifyInfo(frontSha, '等待设备端比对...', { text: '已发送文件 SHA-256，等待设备端校验结果' });
        setBleStage(5, '发送校验...', 'active');
        setBleStageText('发送 0xF8 + SHA-256，等待设备端校验...');
        try {
          await state.ctrlChar.writeValue(trigger);
          bleLog('OTA', '0xF8 + 32B SHA-256 已发送，等待设备端校验');
        } catch (e) {
          clearOtaResultWaiter();
          state.lastUploadStatus = 'failed';
          setProgress(0, `0xF8 发送失败: ${e.message}`);
          bleLog('OTA', `0xF8 发送失败: ${e.message}`, 'E');
          setBleStageError('0xF8 发送失败');
          return false;
        }
        setBleStage(5, '等待设备端校验', null);
        setBleStage(6, '设备端收尾中', 'active');
        setBleStageText('0xF8 已发，等待设备端完成 OTA 收尾...');
        try {
          await resultPromise;
        } catch (e) {
          state.lastUploadStatus = 'failed';
          setProgress(0, e.message);
          setBleStageError(e.message);
          clearOtaResultWaiter();
          return false;
        }
        return true;
      }

      // GD32: send 0xF7 + 32B SHA-256 + session, then wait for SPI relay result.
      const frontSha = fileSha256.toUpperCase();
      const frontShaBytes = new Uint8Array(32);
      for (let i = 0; i < 32; i++) {
        frontShaBytes[i] = parseInt(frontSha.slice(i * 2, i * 2 + 2), 16);
      }
      state.deviceSha256 = null;
      setBleStage(5, '发送触发...', 'active');
      setBleStageText('发送 0xF7 + SHA-256，等待设备端缓存校验...');
      showBleVerifyInfo(frontSha, '等待设备端比对...', { text: '已发送文件 SHA-256，等待 GD32 缓存校验' });
      await sleep(30);
      const cmdHex = els.ctrlHex.value || cfg.ctrlHex;
      const cmdBase = hexToBytes(cmdHex);
      const cmd = new Uint8Array(cmdBase.length + 32 + 2);
      cmd.set(cmdBase, 0);
      cmd.set(frontShaBytes, cmdBase.length);
      cmd[cmdBase.length + 32] = otaSessionId & 0xFF;
      cmd[cmdBase.length + 33] = (otaSessionId >> 8) & 0xFF;
      const resultPromise = waitForOtaResult('gd32', otaSessionId, 120000);
      try {
        await state.ctrlChar.writeValue(cmd);
        setBleStage(5, '已触发', null);
        setBleStage(6, '等待设备端校验', 'active');
        bleLog('OTA', `0xF7 + 32B SHA-256 已发送，等待设备端缓存校验与转发`);
      } catch (_) {
        clearOtaResultWaiter();
        state.lastUploadStatus = 'failed';
        setProgress(0, `触发命令发送失败: ${toHex(cmd)}`);
        bleLog('OTA', `触发命令发送失败: ${toHex(cmd)}`, 'E');
        setBleStageError('触发命令发送失败');
        return false;
      }

      setBleStageText('GD32 缓存校验已发起，等待 SPI 转发流程...');
      try {
        await resultPromise;
      } catch (e) {
        state.lastUploadStatus = 'failed';
        setProgress(0, e.message);
        setBleStageError(e.message);
        clearOtaResultWaiter();
        return false;
      }
      bleLog('OTA', `✅ GD32 OTA 刷写完成！`);
      bleLog('OTA', `📊 文件 SHA-256: ${fileSha256.toUpperCase()}`);
      return true;
    } catch (err) {
      state.lastUploadStatus = 'failed';
      state.expectedReboot = false;   // ★ 刷写失败，取消重连退避
      state.reconnecting = false;
      state.reconnectAttempts = 0;
      bleLog('OTA', `写入失败: ${err.message}`, 'E');
      setProgress(0, `写入失败: ${err.message}`);
      setBleStageError(err.message);
      return false;
    } finally {
      if (state.ackWaiter) {
        clearTimeout(state.ackWaiter.timer);
        state.ackWaiter = null;
      }
      if (state.lastUploadStatus === 'failed') {
        clearOtaResultWaiter();
      }
      state.uploading = false;
      state.uploadTargetKey = null;
      setConnectionState(!!state.server);
    }
  }

  async function autoFlash() {
    // 每次刷写前尝试重新从本地读取文件（支持热重载：用户重新编译后无需重新选择）
    if (state.selectedFileHandle) {
      try {
        const freshFile = await state.selectedFileHandle.getFile();
        // 仅在文件变化时才刷新元数据并重算 SHA-256
        // （name/size/lastModified 任一不同即视为变化）
        // 避免对同一文件重复触发 updateFileInfo → 产生重复的 [FILE] SHA-256 日志
        const cached = state.selectedFile;
        // HEX 文件转换后 cached.name 是 .bin 而 freshFile.name 是 .hex，
        // 因此 HEX 文件用 lastModified 判断是否变化，避免每次都重新转换
        const isHex = freshFile.name.toLowerCase().endsWith('.hex');
        const changed = !cached ||
                        (isHex ? false : (cached.name !== freshFile.name || cached.size !== freshFile.size)) ||
                        cached.lastModified !== freshFile.lastModified;
        if (changed) {
          await updateFileInfo(freshFile);
          var actualFile = state.selectedFile || freshFile;
          bleLog('FILE', '已重新读取本地文件（已变更）: ' + actualFile.name + ' (' + formatBytes(actualFile.size) + ')');
        }
      } catch (e) {
        bleLog('FILE', '重新读取文件失败: ' + e.message + '，将使用缓存版本', 'W');
      }
    } else {
      await syncSelectedFileFromInput();
    }
    if (!state.selectedFile || !state.server) return;

    const cfg = currentTarget();
    state.autoFlashing = true;
    try {
      bleLog('AUTO', `开始一键刷写 -> ${cfg.label}`);
      const ok = await uploadFirmware();
      if (!ok || state.abortRequested) {
        bleLog('AUTO', '一键刷写中止', 'W');
        return;
      }
      if (cfg.label === 'ESP32') {
        setProgress(100, '[ESP32] OTA 完成，等待设备重启');
        bleLog('AUTO', 'ESP32 OTA 完成，等待设备重启');
      } else {
        setProgress(100, '[GD32] OTA 转发完成，APP 已启动');
        bleLog('AUTO', 'GD32 OTA 完成');
      }
    } catch (err) {
      bleLog('AUTO', `一键刷写异常: ${err && err.message ? err.message : err}`, 'E');
      setProgress(0, `一键刷写异常: ${err && err.message ? err.message : err}`);
    } finally {
      state.autoFlashing = false;
      setConnectionState(!!state.server);
    }
  }

  function abortTask() {
    state.abortRequested = true;
    bleLog('OTA', '已请求中止，等待当前分包返回');
  }

  // showToast 使用 app.js 中的全局版本

  function bindEvents() {
    els.connectBtn.addEventListener('click', connectBle);
    els.disconnectBtn.addEventListener('click', disconnectBle);
    els.autoFlashBtn.addEventListener('click', autoFlash);
    els.abortBtn.addEventListener('click', abortTask);
    // 上传区事件由 app.js 的 setupSharedUploadZone 统一处理
    // 文件输入变化时同步 BLE 文件状态
    els.firmwareFile.addEventListener('change', syncSelectedFileFromInput);
    els.firmwareFile.addEventListener('input', syncSelectedFileFromInput);
    window.addEventListener('focus', () => setTimeout(syncSelectedFileFromInput, 0));
    els.payloadSize.addEventListener('change', () => updateFileInfo(state.selectedFile));
  }

  // 暴露到 window，方便外部模式切换/集成时调用（默认 API：init / disconnect）
  window.bleOta = {
    init: function() {
      bindEvents();
      refreshTargetUi();
      setConnectionState(false);
      resetBleStages();
      bleLog('SYS', `Web Bluetooth 刷写控制台已就绪 (${UI_BUILD})`);
    },
    disconnect: disconnectBle,
    showToast: showToast,
    notifyFileChange: async function(file, handle) {
      if (file) {
        if (handle) state.selectedFileHandle = handle;
        await updateFileInfo(file);
      }
    },
    clearFile: clearSelectedFile,
    refreshButtons: function() {
      setConnectionState(!!state.server);
    },
    refreshTargetUi: refreshTargetUi,
  };

  // 脚本在 body 末尾同步加载，DOM 元素已就绪，直接初始化
  window.bleOta.init();
})();
