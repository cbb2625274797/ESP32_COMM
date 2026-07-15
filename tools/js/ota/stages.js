/**
 * stages.js - OTA 阶段驱动 (真实状态)
 *
 * 从 app.js 的「OTA 阶段驱动 (真实状态)」段提取。
 * 将设备端 relay_state / sys_state 映射到 GD32 阶段节点 UI，
 * 并驱动 updateSystemStatusUi 综合 UI 更新。
 *
 * 共享状态（通过 window.* 引用）：
 *   window.latestSystemStatus / window.currentOtaTarget / window.esp32OtaPhase
 *   window.otaSessionState / window.otaSessionTarget / window.isUploading
 *   window.gd32CompleteAnnounced / window.gd32RelaySessionLive
 *   window.lastSysStateCode / window.lastRelayState
 *
 * 未拆分的函数（通过 window.* 引用）：
 *   window.updateHeapAndTaskStatusUi / window.updateConnectionUi / window.updateHealthUi
 *   window.updateStagesFromRelayState / window.setActivePhase / window.setProgress
 *   window.markGd32OtaUiComplete / window.cleanupAfterOtaFailure / window.otaSessionReset
 *   window.stopRelayProgressWatch / window.pollLogs / window.formatBytes / window.formatUptime
 */
import { formatBytes, formatUptime } from '../common/utils.js';
import { updateConnectionUi, updateHealthUi, updateHeapAndTaskStatusUi } from '../common/connection.js';
import { setActivePhase, setProgress, markPhaseDone } from './progress.js';
import {
  otaSessionActive, otaSessionReset, otaSessionTransition, OtaSessionState
} from './session.js';

// Relay state comes from relay_state_t in main/relay/ota_relay.h:
//   0=IDLE, 1=HANDSHAKING, 2=SIZE_NEG, 3=TRANSFERRING,
//   4=FINISHING, 5=DONE, 255=ERROR.
const relayStateDetailMap = {
  0: '待机',
  1: '认证握手',
  2: '传输协商',
  3: '数据转发',
  4: '收尾验证',
  5: 'OTA完成',
  255: '错误'
};

export function updateStagesFromRelayState(rs) {
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

export function updateSystemStatusUi(data) {
  if (!data) return;
  window.latestSystemStatus = data;
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
  if (window.currentOtaTarget === 'esp32' && window.esp32OtaPhase > 0) {
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

  if (window.otaSessionState === OtaSessionState.FAILED) {
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
    window.stopRelayProgressWatch();
    // 落到下方正常分支刷新 UI
  }

  var displayRelayState = isGd32Complete ? 5 : (isGd32Relaying && rs === 0 ? 1 : rs);
  if (window.currentOtaTarget !== 'esp32') { updateStagesFromRelayState(displayRelayState); }

  if (isGd32Error && (window.currentOtaTarget === 'gd32' || window.otaSessionTarget === 'gd32')) {
    var failDetail = 'GD32 OTA 失败';
    if (relayTotal > 0) {
      failDetail += '（已转发 ' + relaySeq + '/' + relayTotal +
        '，relay_state=' + rs + '，sys=' + (sysState || sysStateCode) + '）';
    } else {
      failDetail += '（relay_state=' + rs + '，sys=' + (sysState || sysStateCode) + '）';
    }
    window.cleanupAfterOtaFailure(failDetail);
    window.pollLogs();
  } else if (isGd32Error) {
    // 非当前 OTA 会话的错误：仅标记阶段指示器，不触发完整清理
    updateStagesFromRelayState(255);
    var sbErr = document.getElementById('stageStatusBar');
    var stErr = document.getElementById('stageStatusText');
    if (sbErr) sbErr.className = 'stage-status-bar err';
    if (stErr) stErr.textContent = 'GD32 错误（待机）';
  } else if (window.otaSessionState === OtaSessionState.UPLOADING) {
    /* 上传进行中：不要用旧缓存/旧完成态覆盖进度条 */
  } else if (window.otaSessionState === OtaSessionState.RELAYING) {
    /* 必须先看到本会话真实中继（HANDSHAKE~FINISHING），才允许 DONE；
     * 禁止一进 RELAYING 就吃掉上一次的 rs=5 / app_alive。 */
    if (isGd32Relaying || (rs >= 1 && rs <= 4)) {
      window.gd32RelaySessionLive = true;
    }
    if (window.gd32RelaySessionLive &&
        (isGd32Complete || rs === 5 ||
         (gd32AppAlive && relayTotal > 0 && relaySeq >= relayTotal && rs >= 3))) {
      if (!window.gd32CompleteAnnounced) {
        window.markGd32OtaUiComplete('GD32 刷写完成');
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
      if (!window.gd32CompleteAnnounced) {
        setProgress(95, gd32AppAlive ? '收尾中…' : '收尾中，等待 APP 启动...');
      }
    } else if (!window.gd32RelaySessionLive) {
      setActivePhase('gd32Ota');
      setProgress(48, '已触发中继，等待设备进入握手…');
    }
  } else if (isGd32Storing || (total > 0 && stored < total && !window.isUploading &&
             window.otaSessionState !== OtaSessionState.DONE &&
             window.otaSessionState !== OtaSessionState.IDLE)) {
    setActivePhase('gd32Ota');
    var storeOverall = Math.min(45, Math.round(storageProgress * 0.45));
    if (!window.isUploading) {
      setProgress(storeOverall, '已缓存 ' + formatBytes(stored) + ' / ' + formatBytes(total) + '');
    }
  }
}