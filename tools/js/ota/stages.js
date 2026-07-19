/**
 * stages.js - OTA 阶段驱动 (真实状态)
 *
 * 将设备端 relay_state / sys_state 映射到 GD32 阶段节点 UI，
 * 并驱动 updateSystemStatusUi 综合 UI 更新。
 *
 * 结构：
 *   applyDashboardUi        — 堆/连接/版本/温湿度等常驻面板
 *   applyGd32OtaSessionUi   — GD32 会话进度/完成/失败（唯一 UI 应用路径）
 *   updateSystemStatusUi    — 入口：仪表盘 →（非 BLE/非 ESP32-OTA）会话
 *
 * 共享状态（通过 window.* 引用）：
 *   window.latestSystemStatus / window.currentOtaTarget / window.esp32OtaPhase
 *   window.otaSessionState / window.otaSessionTarget / window.isUploading
 *   window.gd32CompleteAnnounced / window.gd32RelaySessionLive
 *
 * 复用的函数（通过 window.* 引用）：
 *   window.setActivePhase / window.setProgress
 *   window.markGd32OtaUiComplete / window.cleanupAfterOtaFailure / window.otaSessionReset
 *   window.stopRelayProgressWatch / window.pollLogs / window.emitRelayProgress
 */
import { formatUptime } from '../common/utils.js';
import { updateConnectionUi, updateHealthUi, updateHeapAndTaskStatusUi } from '../common/connection.js';
import { setActivePhase, setProgress } from './progress.js';
import { otaSessionReset, OtaSessionState } from './session.js';

// Relay state comes from relay_state_t in main/relay/ota_relay.h:
//   0=IDLE, 1=HANDSHAKING, 2=SIZE_NEG, 3=TRANSFERRING,
//   4=FINISHING, 5=DONE, 255=ERROR.
// 与 index.html #gd32Stages s1..s6 一一对应（rs→节点序号）：
//   1→s1, 2→s2, 3→s3, 4→s4, 5→s1..s6 全 done。
const relayStateDetailMap = {
  0: '待机',
  1: '认证握手',
  2: '传输协商',
  3: '数据转发',
  4: '收尾验证',
  5: 'OTA完成',
  255: '错误'
};

/** 上次刷到阶段条的 displayRelayState，未变则跳过 DOM 重建 */
var lastDisplayedRelayState = undefined;

/**
 * 将 relay_state 映射到 GD32 阶段节点 (s1..s6)。
 * @param {number|null|undefined} rs
 */
export function updateStagesFromRelayState(rs) {
  var ids = ['s1', 's2', 's3', 's4', 's5', 's6'];
  var detailIds = ['sd1', 'sd2', 'sd3', 'sd4', 'sd5', 'sd6'];
  var all = ids.map(function(id) { return document.getElementById(id); });
  var details = detailIds.map(function(id) { return document.getElementById(id); });
  all.forEach(function (s) { if (s) s.className = 'stage-node'; });
  details.forEach(function (d) { if (d) d.textContent = ''; });
  var statusBar = document.getElementById('stageStatusBar');
  var statusText = document.getElementById('stageStatusText');
  if (rs === undefined || rs === null) {
    if (statusBar) statusBar.className = 'stage-status-bar';
    if (statusText) statusText.textContent = '待机';
    lastDisplayedRelayState = rs;
    return;
  }
  if (rs === 255) {
    all.forEach(function (s) { if (s) s.className = 'stage-node err'; });
    if (statusBar) statusBar.className = 'stage-status-bar err';
    if (statusText) statusText.textContent = '错误';
    lastDisplayedRelayState = 255;
    return;
  }
  /* rs → 节点：1..4 直接对应 s1..s4；rs=5 DONE → 点亮完成链（s1..s6 全 done） */
  var activeStage = 0;
  if (rs === 1) activeStage = 1;
  else if (rs === 2) activeStage = 2;
  else if (rs === 3) activeStage = 3;
  else if (rs === 4) activeStage = 4;
  else if (rs === 5) activeStage = 6;
  for (var i = 0; i < activeStage - 1; i++) {
    if (all[i]) all[i].className = 'stage-node done';
  }
  if (rs === 5) {
    /* DONE：s1..s5 已在上方标 done，s5/s6 一并完成 */
    if (all[4]) all[4].className = 'stage-node done';
    if (all[5]) all[5].className = 'stage-node done';
    if (details[5]) details[5].textContent = relayStateDetailMap[5];
  } else if (activeStage >= 1 && activeStage <= 6) {
    if (all[activeStage - 1]) all[activeStage - 1].className = 'stage-node live';
    if (details[activeStage - 1]) {
      details[activeStage - 1].textContent = relayStateDetailMap[rs] || '';
    }
  }
  if (statusBar && statusText) {
    if (rs === 0) {
      statusBar.className = 'stage-status-bar';
      statusText.textContent = '待机';
    } else {
      statusBar.className = 'stage-status-bar active';
      statusText.textContent = (relayStateDetailMap[rs] || '未知') + ' (' + rs + ')';
    }
  }
  lastDisplayedRelayState = rs;
}

function setDisplayedRelayState(rs) {
  if (lastDisplayedRelayState === rs) return;
  updateStagesFromRelayState(rs);
}

/**
 * 常驻仪表盘：堆、连接、健康、版本、温度、IP/RSSI。
 * @param {object} data  /system/status JSON
 * @param {object} ota   data.ota
 * @param {object} wifi  data.wifi
 */
function applyDashboardUi(data, ota, wifi) {
  updateHeapAndTaskStatusUi(data);
  var connected = !!wifi.connected;
  var connText = connected ? (wifi.mode || 'STA') + ' 已连接' : (wifi.mode || 'STA') + ' 未连接';
  updateConnectionUi(connected, connText);
  updateHealthUi(data.health, data.free_heap);

  if (data.version) {
    var pillVerEl = document.getElementById('pillVer');
    if (pillVerEl) {
      pillVerEl.textContent = 'v' + data.version;
      pillVerEl.dataset.tooltip = data.build_time ? ('编译: ' + data.build_time) : '';
    }
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
        /* BOOT 版本与 APP alive 无直接关系，无版本时统一「未知」 */
        mGd32Boot.textContent = '未知';
        mGd32Boot.className = 'v';
      }
    }
  }

  var mTempEl = document.getElementById('mTemp');
  if (mTempEl) {
    /* 与固件 app_config：TEMP_WARNING=80 / TEMP_CRITICAL=100 对齐 */
    if (Number.isFinite(data.temperature)) {
      var tC = data.temperature;
      mTempEl.textContent = tC.toFixed(1) + '\u00B0C';
      if (tC >= 100) {
        mTempEl.className = 'v err';
      } else if (tC >= 80) {
        mTempEl.className = 'v warn';
      } else {
        mTempEl.className = 'v ok';
      }
      mTempEl.title = 'ESP32 片内温度传感器（警告≥80°C，危险≥100°C）';
    } else {
      mTempEl.textContent = '--';
      mTempEl.className = 'v';
      mTempEl.title = '温度无效或未采样';
    }
  }
  /* GD32 ADC：enabled 来自 HELLO_ACK 功能位（GD32 编译开关），ESP32 无本地开关 */
  var ga = data.gd32_adc || null;
  var feat = data.gd32_features || null;
  var mGd32TempEl = document.getElementById('mGd32Temp');
  var mVccEl = document.getElementById('mVcc');
  var mVddaEl = document.getElementById('mVdda');

  function setMetricUi(el, text, cls, title) {
    if (!el) return;
    el.textContent = text;
    el.className = cls || 'v';
    if (title) el.title = title;
  }

  if (mGd32TempEl || mVccEl || mVddaEl) {
    if (ga && ga.enabled === false) {
      var offTitle = 'GD32 CONFIG_SPI_ADC_FEEDBACK_ENABLE=0（HELLO/PING 身份）';
      setMetricUi(mGd32TempEl, '关闭', 'v', offTitle);
      setMetricUi(mVccEl, '关闭', 'v', offTitle);
      setMetricUi(mVddaEl, '关闭', 'v', offTitle);
    } else if (ga && Number.isFinite(ga.temperature) && ga.fresh) {
      setMetricUi(mGd32TempEl, ga.temperature.toFixed(1) + '\u00B0C',
        ga.valid ? 'v ok' : 'v warn',
        '片内温度 · seq=' + (ga.seq || 0) + ' age=' + (ga.age_ms || 0) + 'ms'
          + (ga.valid ? '' : ' (valid=0)'));
      /* VCC：status 已带 vcc；旧固件仅 vin_adc 时本地 *11（47k/4.7k） */
      var vcc = Number.isFinite(ga.vcc) ? ga.vcc
        : (Number.isFinite(ga.vin_adc) ? ga.vin_adc * 11.0 : NaN);
      if (Number.isFinite(vcc) && vcc > 0.5) {
        setMetricUi(mVccEl, vcc.toFixed(1) + 'V', ga.valid ? 'v ok' : 'v warn',
          '输入母线 VCC（PA6 分压反推）· vin_adc='
          + (Number.isFinite(ga.vin_adc) ? ga.vin_adc.toFixed(3) + 'V' : '--'));
      } else {
        setMetricUi(mVccEl, '--', 'v', '无 VIN 采样');
      }
      if (Number.isFinite(ga.vdda) && ga.vdda > 0.5) {
        setMetricUi(mVddaEl, ga.vdda.toFixed(2) + 'V', ga.valid ? 'v ok' : 'v warn',
          'MCU 模拟供电 VDDA（片内 VREFINT 反推）');
      } else {
        setMetricUi(mVddaEl, '--', 'v', '无 VDDA 采样');
      }
    } else if (ga && Number.isFinite(ga.temperature)) {
      setMetricUi(mGd32TempEl, ga.temperature.toFixed(1) + '\u00B0C (旧)', 'v warn', '超过 5s 未更新');
      var vccOld = Number.isFinite(ga.vcc) ? ga.vcc
        : (Number.isFinite(ga.vin_adc) ? ga.vin_adc * 11.0 : NaN);
      setMetricUi(mVccEl, Number.isFinite(vccOld) ? (vccOld.toFixed(1) + 'V (旧)') : '--',
        'v warn', '超过 5s 未更新');
      setMetricUi(mVddaEl,
        Number.isFinite(ga.vdda) ? (ga.vdda.toFixed(2) + 'V (旧)') : '--',
        'v warn', '超过 5s 未更新');
    } else if (ga && ga.enabled === true) {
      var waitTitle = 'GD32 已开 0xC4，等待首帧';
      setMetricUi(mGd32TempEl, '等待…', 'v', waitTitle);
      setMetricUi(mVccEl, '等待…', 'v', waitTitle);
      setMetricUi(mVddaEl, '等待…', 'v', waitTitle);
    } else {
      var idleTitle = feat && feat.known === false
        ? '尚未 HELLO 身份或旧 GD32（无 feat_map）'
        : '等待 HELLO / 0xC4';
      setMetricUi(mGd32TempEl, '--', 'v', idleTitle);
      setMetricUi(mVccEl, '--', 'v', idleTitle);
      setMetricUi(mVddaEl, '--', 'v', idleTitle);
    }
  }
  var uptimeEl = document.getElementById('pillUptime');
  if (uptimeEl) uptimeEl.textContent = '\u23F1 ' + formatUptime(data.uptime_sec);
  var pillIPEl = document.getElementById('pillIP');
  if (pillIPEl) pillIPEl.textContent = wifi.ip || '--';
  var rssi = (wifi.rssi !== undefined && wifi.rssi !== -127) ? wifi.rssi + ' dBm' : '--';
  var pillRSSIEl = document.getElementById('pillRSSI');
  if (pillRSSIEl) pillRSSIEl.textContent = rssi;
}

/**
 * GD32 OTA 会话 UI：阶段节点 + 进度条 + 完成/失败。
 * 门控逻辑保持与历史实现一致，勿随意放宽完成条件。
 * @param {object} ota data.ota
 */
function applyGd32OtaSessionUi(ota) {
  var rs = ota.relay_state;
  var sysState = ota.sys_state || '';
  var sysStateCode = Number.isFinite(ota.sys_state_code) ? ota.sys_state_code : -1;
  var isGd32Relaying = sysState === 'GD32_RELAYING' || sysStateCode === 3;
  var isGd32Complete = sysState === 'GD32_COMPLETE' || sysStateCode === 4;
  var isGd32Error = sysState === 'ERROR' || sysStateCode === 8 || rs === 255;

  /* 进度以 relay_seq/relay_total 为准；不再用 stored_size 做「OTA 缓存」展示 */
  var relaySeq = ota.relay_seq || 0;
  var relayTotal = ota.relay_total_packets || 0;
  var relayProgress = relayTotal > 0 ? Math.min(100, Math.round(relaySeq * 100 / relayTotal)) : 0;
  var gd32AppAlive = ota.gd32_app_alive;

  /* ★ FAILED 守卫：禁止后端状态覆盖失败 UI；设备回 IDLE 才自动复位 */
  if (window.otaSessionState === OtaSessionState.FAILED) {
    var deviceIdle = (sysStateCode === 0 || sysStateCode === 2 || sysStateCode === 4) &&
                     (rs === 0 || rs === 5);
    if (!deviceIdle) {
      if ((rs === 3 || isGd32Relaying) && relayTotal > 0) {
        setActivePhase('gd32Ota');
        var failRelayOverall = 60 + Math.round(relayProgress * 0.30);
        setProgress(failRelayOverall,
          '已转发 ' + relaySeq + ' / ' + relayTotal + ' 包（设备仍在跑）');
        setDisplayedRelayState(3);
      } else {
        setDisplayedRelayState(255);
      }
      return;
    }
    otaSessionReset();
    window.stopRelayProgressWatch();
    /* 落到下方正常分支刷新 UI */
  }

  var displayRelayState = isGd32Complete ? 5 : (isGd32Relaying && rs === 0 ? 1 : rs);
  if (window.currentOtaTarget !== 'esp32') {
    setDisplayedRelayState(displayRelayState);
  }

  var gd32Session =
    window.otaSessionState === OtaSessionState.RELAYING ||
    window.currentOtaTarget === 'gd32' ||
    window.otaSessionTarget === 'gd32';

  if (isGd32Error && gd32Session) {
    var failDetail = 'GD32 OTA 失败';
    if (relayTotal > 0) {
      failDetail += '（已转发 ' + relaySeq + '/' + relayTotal +
        '，relay_state=' + rs + '，sys=' + (sysState || sysStateCode) + '）';
    } else {
      failDetail += '（relay_state=' + rs + '，sys=' + (sysState || sysStateCode) + '）';
    }
    window.cleanupAfterOtaFailure(failDetail);
    window.pollLogs();
    return;
  }

  if (isGd32Error) {
    setDisplayedRelayState(255);
    var sbErr = document.getElementById('stageStatusBar');
    var stErr = document.getElementById('stageStatusText');
    if (sbErr) sbErr.className = 'stage-status-bar err';
    if (stErr) stErr.textContent = 'GD32 错误（待机）';
    return;
  }

  if (window.otaSessionState === OtaSessionState.UPLOADING) {
    /* 上传进行中：不要用旧缓存/旧完成态覆盖进度条 */
    return;
  }

  if (window.otaSessionState === OtaSessionState.RELAYING) {
    /* 必须先看到本会话真实中继，才允许 DONE */
    if (isGd32Relaying || (rs >= 1 && rs <= 4)) {
      window.gd32RelaySessionLive = true;
    }

    var deviceIdleAfterRelay =
      (sysStateCode === 0 || sysStateCode === 2) &&
      (rs === 0 || rs === 5) &&
      window.gd32RelaySessionLive &&
      !window.gd32CompleteAnnounced;
    if (deviceIdleAfterRelay) {
      window.cleanupAfterOtaFailure(
        'GD32 中继已结束但未成功（设备已回 IDLE' +
        (relayTotal > 0 ? '，已转发 ' + relaySeq + '/' + relayTotal : '') + '）'
      );
      window.pollLogs();
      return;
    }

    /*
     * 包进度/完成：统一走 emitRelayProgress + markGd32OtaUiComplete。
     * FINISHING 时 force100 后只改文案，禁止 setProgress(95) 回跳。
     */
    if (window.gd32RelaySessionLive && relayTotal > 0 &&
        (relaySeq >= relayTotal || rs >= 4 || isGd32Complete)) {
      if (typeof window.emitRelayProgress === 'function') {
        window.emitRelayProgress(relayTotal, relayTotal, { force100: true });
      }
    } else if (window.gd32RelaySessionLive && relayTotal > 0 && relaySeq > 0 &&
               typeof window.emitRelayProgress === 'function') {
      window.emitRelayProgress(relaySeq, relayTotal);
    }

    if (window.gd32RelaySessionLive &&
        (isGd32Complete || rs === 5 ||
         (gd32AppAlive && relayTotal > 0 && relaySeq >= relayTotal && rs >= 3))) {
      if (!window.gd32CompleteAnnounced) {
        window.markGd32OtaUiComplete('GD32 刷写完成');
      }
      return;
    }

    if (rs === 1) {
      setActivePhase('gd32Ota');
      setProgress(50, '握手与认证中...');
    } else if (rs === 4) {
      /* 收尾：保持 100%，只更新文案（避免 100→95 回跳） */
      setActivePhase('gd32Ota');
      if (!window.gd32CompleteAnnounced) {
        setProgress(100, gd32AppAlive ? '收尾中…' : '收尾中，等待 APP 启动...');
      }
    } else if (rs === 3 && relaySeq <= 0) {
      setActivePhase('gd32Ota');
      setProgress(60, '数据包传输中...');
    } else if (rs === 2 || (isGd32Relaying && relaySeq === 0)) {
      setActivePhase('gd32Ota');
      setProgress(55, relayTotal > 0
        ? ('大小协商中（共 ' + relayTotal + ' 包）')
        : '固件大小协商中');
    } else if (!window.gd32RelaySessionLive) {
      setActivePhase('gd32Ota');
      setProgress(48, '已触发中继，等待设备进入握手…');
    }
    return;
  }

  /* 不再用 stored_size 做「OTA 缓存」展示；上传走 XHR，中继走 relay_seq。 */
}

/**
 * /system/status → UI 总入口。
 * @param {object} data
 */
export function updateSystemStatusUi(data) {
  if (!data) return;
  window.latestSystemStatus = data;
  var wifi = data.wifi || {};
  var ota = data.ota || {};

  applyDashboardUi(data, ota, wifi);

  /* BLE：仪表盘可更新，进度/阶段由 ble 路径独占 */
  if (window.connectionMode === 'ble') {
    return;
  }
  /* ESP32 OTA 进行中：勿用 GD32 relay 覆盖阶段条 */
  if (window.currentOtaTarget === 'esp32' && window.esp32OtaPhase > 0) {
    return;
  }

  applyGd32OtaSessionUi(ota);
}
