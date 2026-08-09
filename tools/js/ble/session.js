// ==================== BLE OTA 结果等待与中止 ====================
import { BLE_OTA_CTRL_ABORT_OTA } from './constants.js';
import { state, els, bleLog } from './state.js';
import { connectBle, disconnectBle } from './gatt.js';
import { autoFlash } from './transfer.js';
import { syncSelectedFileFromInput } from './file.js';

export function clearOtaResultWaiter() {
  if (state.otaResultWaiter) {
    clearTimeout(state.otaResultWaiter.timer);
    state.otaResultWaiter = null;
  }
}

export function resolveOtaResult(status, detail) {
  if (!state.otaResultWaiter) return;
  var waiter = state.otaResultWaiter;
  state.otaResultWaiter = null;
  clearTimeout(waiter.timer);
  waiter.resolve({ status: status, detail: detail || {} });
}

export function rejectOtaResult(message, detail) {
  if (!state.otaResultWaiter) return;
  var waiter = state.otaResultWaiter;
  state.otaResultWaiter = null;
  clearTimeout(waiter.timer);
  var err = new Error(message || 'OTA 收尾失败');
  err.detail = detail || {};
  waiter.reject(err);
}

export function nextOtaSessionId() {
  state.otaRunId = (state.otaRunId + 1) & 0xFFFF;
  if (state.otaRunId === 0) state.otaRunId = 1;
  return state.otaRunId;
}

export function finishBelongsToWaiter(targetKey, value) {
  if (!state.otaResultWaiter || state.otaResultWaiter.targetKey !== targetKey) return false;
  return (value & 0xFFFF) === state.otaResultWaiter.sessionId;
}

export function finishSessionId(value) {
  return value & 0xFFFF;
}

export function isSessionlessFinish(value) {
  return finishSessionId(value) === 0;
}

export function hasCurrentWaiter(targetKey) {
  return !!(state.otaResultWaiter && state.otaResultWaiter.targetKey === targetKey);
}

export function waitForOtaResult(targetKey, sessionId, timeoutMs) {
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

/**
 * @description 中止当前 BLE OTA 任务并清理状态。
 *   改进：发 0xFB 控制命令让 ESP32 主动清理 OTA 会话，BLE 保持连接。
 *   旧做法是直接断开 BLE 触发 GAP disconnect 清理，导致用户需重连。
 *   若 0xFB 发送失败（如 CTRL 特征不可用），回退到设置标志等待发包循环自然退出。
 */
export async function abortTask() {
  state.abortRequested = true;  // 立即停止发送循环
  bleLog('OTA', '已请求中止，发送 0xFB 让设备端清理（BLE 保持连接）');

  if (state.ctrlChar && state.server && state.device.gatt.connected) {
    try {
      const session = state.sessionId || 0;
      const abortCmd = new Uint8Array([BLE_OTA_CTRL_ABORT_OTA, session & 0xFF, (session >> 8) & 0xFF]);
      await state.ctrlChar.writeValue(abortCmd);
      bleLog('OTA', '0xFB 已发送，等待设备端回复 ABORTED (stage=0x20)');
    } catch (e) {
      bleLog('OTA', '0xFB 发送失败: ' + e.message + '，回退到本地中止', 'E');
      // CTRL 写入失败时回退：本地立即标记失败，不再等设备端 ABORTED 回复
      state.lastUploadStatus = 'failed';
      if (state.ackWaiter) {
        const waiter = state.ackWaiter;
        state.ackWaiter = null;
        clearTimeout(waiter.timer);
        waiter.reject(new Error('中止失败: ' + e.message));
      }
      rejectOtaResult('中止失败: ' + e.message, { stage: 0xFB, value: 0 });
    }
  } else {
    // CTRL 特征未就绪（连接已断或未完成握手），仅本地清理
    bleLog('OTA', 'CTRL 特征不可用，仅本地中止', 'W');
    state.lastUploadStatus = 'failed';
    rejectOtaResult('CTRL 特征不可用', { stage: 0xFB, value: 0 });
  }
}

export function bindEvents() {
  if (els.bleConnectToggle) {
    // launch 风格按钮：用 click 而非 change 触发——navigator.bluetooth.requestDevice
    // 必须在 user gesture 内调用，change 事件不被识别为 user gesture，会报
    // "Must be handling a user gesture to show a permission request."
    // preventDefault 阻止 label 默认勾选，checked 由 setConnectionState 按实际结果驱动。
    els.bleConnectToggle.addEventListener('click', (e) => {
      e.preventDefault();
      if (state._bleConnecting) return;          // 连接进行中，忽略重复点击
      var btn = document.querySelector('.ble-launch-btn');
      if (state.server) {
        els.bleConnectToggle.checked = false;
        if (btn) btn.setAttribute('data-ble-state', 'idle');
        disconnectBle();                          // → resetUiAfterDisconnect 置 idle
      } else {
        state._bleConnecting = true;
        els.bleConnectToggle.checked = true;     // 即时视觉反馈
        if (btn) btn.setAttribute('data-ble-state', 'connecting');
        // 连接结束后按实际结果收口：成功→connected / 失败→idle
        // 不依赖 state.js 的 setConnectionState（ES 模块子导入可能被浏览器缓存为旧版）
        connectBle().finally(() => {
          state._bleConnecting = false;
          var b = document.querySelector('.ble-launch-btn');
          if (b) b.setAttribute('data-ble-state', state.server ? 'connected' : 'idle');
        });
      }
    });

    // 兜底：自动断开（onDisconnected）、重连等路径绕过 click handler，
    // 用轮询同步 data-ble-state，确保按钮状态始终与 state.server 一致。
    setInterval(function() {
      if (state._bleConnecting) return;            // 连接中不干扰
      var btn = document.querySelector('.ble-launch-btn');
      if (!btn || !btn.offsetParent) return;        // 按钮不可见（WiFi 模式）时跳过
      var expected = state.server ? 'connected' : 'idle';
      if (btn.getAttribute('data-ble-state') !== expected) {
        btn.setAttribute('data-ble-state', expected);
      }
    }, 800);
  }
  els.autoFlashBtn.addEventListener('click', autoFlash);
  els.abortBtn.addEventListener('click', abortTask);
  // 上传区事件由 app.js 的 setupSharedUploadZone 统一处理
  // 文件输入变化时同步 BLE 文件状态
  els.firmwareFile.addEventListener('change', syncSelectedFileFromInput);
  els.firmwareFile.addEventListener('input', syncSelectedFileFromInput);
  window.addEventListener('focus', () => setTimeout(syncSelectedFileFromInput, 0));
}
