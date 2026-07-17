// ==================== BLE OTA 连接管理 ====================
import {
  UUIDS,
  BLE_OTA_NOTIFY_ACK,
  BLE_OTA_NOTIFY_FINISH,
  BLE_OTA_NOTIFY_SHA256,
  BLE_OTA_ACK_WINDOW,
  BLE_OTA_FINISH_ESP32_DONE,
  BLE_OTA_FINISH_REBOOT_NOW,
  BLE_OTA_FINISH_GD32_RELAY_START,
  BLE_OTA_FINISH_GD32_RELAY_DONE,
  BLE_OTA_FINISH_ABORTED,
  BLE_OTA_FINISH_ERR_FINALIZE,
  BLE_OTA_FINISH_ERR_MUTEX,
  BLE_OTA_FINISH_ERR_GD32_SIZE,
  BLE_OTA_FINISH_ERR_SHA_MISMATCH,
  BLE_OTA_FINISH_ERR_GD32_RELAY,
  TARGETS,
} from './constants.js';
import {
  state,
  els,
  bleLog,
  toHex,
  setConnectionState,
  resetUiAfterDisconnect,
  currentPayloadSize,
  flowTarget,
  currentTarget,
  sleep,
} from './state.js';
import { setBleStage, setBleStageError, setBleStageText, setBleStageStatus, resetBleStages } from './stages.js';
import { setProgress, updateProgressFromAck } from './transfer.js';
import { showBleVerifyInfo } from './verify.js';
import {
  clearOtaResultWaiter,
  resolveOtaResult,
  rejectOtaResult,
  finishBelongsToWaiter,
  finishSessionId,
  isSessionlessFinish,
  hasCurrentWaiter,
} from './session.js';
import { formatBytes } from '../common/utils.js';
import { showToast } from '../common/toast.js';

/**
 * @description 中止当前 ACK 等待 Promise，并复位重连状态。
 * 用于所有"错误类 FINISH"和"用户中止"路径，避免 ackWaiter 悬挂。
 */
function abortAckWaiter(reason) {
  if (state.ackWaiter) {
    const waiter = state.ackWaiter;
    state.ackWaiter = null;
    clearTimeout(waiter.timer);
    waiter.reject(new Error(reason));
  }
  state.abortRequested = true;
  state.expectedReboot = false;
  state.reconnecting = false;
  state.reconnectAttempts = 0;
}

/** @description 连接 BLE 设备（扫描→连接→发现服务→使能通知）。 */
export async function connectBle() {
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
    // ★ enableNotifications 内部已包含重试 + 主动断开逻辑；
    //   若返回时仍为 false，说明连接半死，UI 已被断开事件重置，不要继续推进到 Stage 3
    if (!state.notificationsEnabled) {
      bleLog('BLE', '连接就绪但 DATA_OUT 通知开启失败，等待用户重连', 'E');
      return;
    }
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

export function onDisconnected() {
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

/** @description 断开 BLE 连接并清理资源。 */
export async function disconnectBle() {
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

export async function readVersion() {
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

/** @description 处理 BLE Notify 事件（解析 ACK/ERROR/PROGRESS 等响应类型）。 */
export function handleNotify(event) {
  const view = event.target.value;
  const bytes = new Uint8Array(view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength));
  const hex = toHex(bytes);
  state.lastNotificationHex = hex;

  if (bytes.length >= 8 && bytes[0] === BLE_OTA_NOTIFY_ACK) {
    const ackCmd = bytes[1];
    const ackSeq = bytes[2] | (bytes[3] << 8);
    const ackBytes = (bytes[4]) | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
    // ★ 保护：陈旧 ACK 来自跨 run / 旧窗口时直接丢弃。
    //   仅以「本 run 上界」过滤（ackSeq 不能超过文件总包数 + 一个窗口余量），
    //   不再用 ackMinSeenSeq 作下界 —— 因为本 run 第一个窗口的 seq 本身就是 0..31，
    //   下界过严会把正常 ACK 也误判为越界（导致 ackBytes 永远 0）。
    if (state.uploading && state.activeTotalSize > 0) {
      const payloadSize = Math.max(1, currentPayloadSize());
      const maxExpectedSeq = Math.ceil(state.activeTotalSize / payloadSize) - 1;
      if (ackSeq > maxExpectedSeq + BLE_OTA_ACK_WINDOW) {
        bleLog('ACK', `忽略越界 ACK: seq=${ackSeq} (run 范围 0..${maxExpectedSeq})`, 'W');
        return;
      }
    } else if (!state.uploading) {
      // 非上传阶段：清空状态，避免污染下次 run
      state.lastAckCmd = null;
      state.lastAckSeq = -1;
      state.lastAckBytes = 0;
      return;
    }
    state.lastAckCmd = ackCmd;
    state.lastAckSeq = ackSeq;
    state.lastAckBytes = ackBytes >>> 0;

    // ★ 以设备端真实 ACK 字节数驱动 UI 进度
    if (state.uploading && state.activeTotalSize > 0 && state.activePacketTotal > 0) {
      updateProgressFromAck(state.activeTotalSize, state.activePacketTotal, state.lastAckBytes);
    }

    /* 传输期少打日志：整窗 ACK 频繁时 hex 刷屏拖慢 UI/主线程。
     * ★ 降为 D 级：成功路径默认不显示，需要时切过滤级查看。 */
    if (!state.uploading || (ackSeq % 16) === 0 || ackSeq < 2) {
      bleLog('ACK', `cmd=0x${ackCmd.toString(16).padStart(2, '0').toUpperCase()} seq=${ackSeq} bytes=${state.lastAckBytes} (${formatBytes(state.lastAckBytes)})`, 'D');
    }

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
      [BLE_OTA_FINISH_ABORTED]: '[FINISH] OTA 已中止（BLE 保持连接）',
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
      showBleVerifyInfo(state.fileSha256 || '-', state.deviceSha256 || '等待设备端上报...', { text: '缓存写入完成，正在通过 SPI 转发' });
      return;
    }

    if (stage === BLE_OTA_FINISH_GD32_RELAY_DONE) {
      if (!finishBelongsToWaiter('gd32', value)) return;
      if (typeof switchStageFlow === 'function') switchStageFlow('gd32');
      if (typeof updateStagesFromRelayState === 'function') updateStagesFromRelayState(5);
      if (typeof markPhaseDone === 'function') markPhaseDone('gd32Ota');
      setProgress(100, '[GD32] OTA 转发完成，APP 已启动');
      showBleVerifyInfo(state.fileSha256 || '-', state.deviceSha256 || '设备未上报', { text: 'GD32 OTA 完成', ok: true });
      setBleStageStatus('ok', 'GD32 刷写完成');
      state.lastUploadStatus = 'success';
      showToast('GD32 OTA 刷写完成', 'success');
      resolveOtaResult('success', { stage: stage, value: value });
      return;
    }

    // ── 0x20 ABORTED：ESP32 已中止 OTA（BLE 保持连接）──
    // 中止可由用户主动点"中止"按钮（abortTask 发 0xFB）触发，
    // 也可能是设备端因错误自动中止。两种情况都走同一清理路径。
    if (stage === BLE_OTA_FINISH_ABORTED) {
      state.lastUploadStatus = 'aborted';
      setProgress(0, 'OTA 已中止');
      setBleStageError('OTA 已中止');
      showToast('OTA 已中止（BLE 保持连接）', 'warning');
      abortAckWaiter('OTA 已中止');
      rejectOtaResult('OTA 已中止', { stage: stage, value: value });
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
      // ★ ESP32 收尾完成 = 成功路径：保持 status bar 绿色，不要切回 active 蓝
      setBleStageStatus('ok', 'ESP32 刷写完成，等待重启');
      if (typeof markPhaseDone === 'function') {
        markPhaseDone('esp32Ota');
      }
    }

    // OTA 结果 toast 提示（用户可见，不只是在日志里）
    if (stage === BLE_OTA_FINISH_ERR_MUTEX) {
      if (!isSessionlessFinish(value)) {
        if (hasCurrentWaiter('esp32') && !finishBelongsToWaiter('esp32', value)) return;
        if (hasCurrentWaiter('gd32') && !finishBelongsToWaiter('gd32', value)) return;
      }
      showToast('OTA 被其他通道占用，请稍后重试', 'error');
      state.abortRequested = true;
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
      abortAckWaiter(`设备端 FINISH 错误: stage=0x${stage.toString(16).padStart(2, '0').toUpperCase()} value=${value}`);
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
    bleLog('VERIFY', '设备端 SHA-256: ' + devSha, 'D');

    // 设备端上报 SHA-256 后，前端仅做展示与状态同步。
    var frontSha = state.fileSha256;
    var verifyStage = flowTarget().verifyStage;
    if (frontSha && frontSha.toUpperCase() === devSha) {
      bleLog('VERIFY', 'SHA-256 校验通过，前端与设备端一致', 'I');
      showBleVerifyInfo(frontSha, devSha, { text: '校验通过 — SHA-256 一致', ok: true });
      setBleStage(verifyStage, 'SHA-256 ✓', null);
      // ★ 校验通过 = 刷写成功：status bar 标绿 + 顶部胶囊推进
      setBleStageStatus('ok', '刷写成功 — SHA-256 校验通过');
      if (typeof markPhaseDone === 'function') {
        markPhaseDone(state.uploadTargetKey === 'gd32' ? 'gd32Ota' : 'esp32Ota');
      }
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


// ===== 自动重连 =====
export async function tryReconnect() {
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
  // 退避：约 1s / 1.6s / 2.6s / 4.1s / 6.6s / 10.5s…，上限 10s
  var backoff = Math.min(10000, 1000 * Math.pow(1.6, attempt - 1));
  if (backoff > 200) await sleep(backoff);
  // ★ 退避期间用户可能已手动断开（disconnectBle 会清 expectedReboot），需二次校验避免幽灵重连
  if (!state.expectedReboot || !state.device) {
    state.reconnecting = false;
    return;
  }
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

export async function enableNotifications() {
  // 修复偶发的 GATT CCC 竞态：startNotifications 与 NimBLE gap 完成握手存在窗口冲突。
  // 现象：Chrome 报 "GATT operation failed for unknown reason"，NimBLE 实际返回 EBUSY/ENOTCONN。
  // 处理：内部退避重试 + 最终失败时主动断开，让上层进入干净的重连路径，而不是带着半死连接去刷写。
  const MAX_RETRY = 3;
  for (let attempt = 1; attempt <= MAX_RETRY; attempt++) {
    try {
      await state.dataOutChar.startNotifications();
      state.dataOutChar.removeEventListener('characteristicvaluechanged', handleNotify);
      state.dataOutChar.addEventListener('characteristicvaluechanged', handleNotify);
      state.notificationsEnabled = true;
      setConnectionState(!!state.server);
      bleLog('BLE', 'DATA_OUT 通知已自动开启' + (attempt > 1 ? ` (重试 ${attempt}/${MAX_RETRY} 成功)` : ''));
      return;
    } catch (err) {
      bleLog('BLE', `自动开启 DATA_OUT 通知失败(第 ${attempt}/${MAX_RETRY} 次): ${err.message}`, 'E');
      if (attempt < MAX_RETRY) {
        // 200ms / 400ms 退避，给 NimBLE 主机任务时间把 connect/MTU 事件消化完
        await new Promise(r => setTimeout(r, 200 * attempt));
      }
    }
  }
  state.notificationsEnabled = false;
  setConnectionState(!!state.server);
  // ★ 最终失败：连接处于半死状态，主动断开让用户重连，避免带着残废连接走到刷写
  try {
    if (state.device && state.device.gatt && state.device.gatt.connected) {
      state.device.gatt.disconnect();
    }
  } catch (_) {}
  bleLog('BLE', 'DATA_OUT 通知开启彻底失败，已主动断开，请重连后重试', 'E');
}
