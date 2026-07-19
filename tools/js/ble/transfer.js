// ==================== BLE OTA 数据写入与 ACK ====================
import { BLE_OTA_ACK_WINDOW, UI_BUILD } from './constants.js';
import { state, els, bleLog, currentTarget, currentPayloadSize, flowTarget, setConnectionState, sleep, toHex, hexToBytes } from './state.js';
import { setBleStage, setBleStageText, setBleStageError } from './stages.js';
import { showBleVerifyInfo, hideBleVerifyInfo } from './verify.js';
import { clearOtaResultWaiter, waitForOtaResult, nextOtaSessionId } from './session.js';
import { enableNotifications } from './gatt.js';
import { syncSelectedFileFromInput, updateFileInfo } from './file.js';
import { computeFileSha } from '../ota/file-handle.js';
import { formatBytes } from '../common/utils.js';
import { showToast } from '../common/toast.js';

export function setProgress(percent, text) {
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

// ★ 以设备端 ACK 字节数为唯一进度数据源
// 仅当处于上传阶段（state.uploading）且未中止时才刷 UI，避免陈旧 ACK / abort 后覆盖
export function updateProgressFromAck(totalSize, packetTotal, ackBytes) {
  if (!state.uploading) return;
  if (state.abortRequested) return;
  if (!Number.isFinite(totalSize) || totalSize <= 0) return;
  const safeBytes = Math.max(0, Math.min(totalSize, ackBytes >>> 0));
  const percent = (safeBytes / totalSize) * 100;
  const payloadSize = Math.max(1, currentPayloadSize());
  const ackedPackets = Math.min(packetTotal, Math.ceil(safeBytes / payloadSize));
  const cfg = currentTarget();
  setProgress(percent, `[${cfg.label}] 已确认 ${ackedPackets}/${packetTotal} 包`);
  els.sentCount.textContent = String(ackedPackets);
  // 阶段 4 detail 文本：只在写入阶段刷新，避免与 stage 5/6 冲突
  var detailEl = document.getElementById('bsd4');
  if (detailEl) detailEl.textContent = `${ackedPackets}/${packetTotal} 包`;
  setBleStageText(`数据写入 ${Math.round(percent)}%`);
}

/** @description 构建 BLE OTA 数据包（seq + totalSize + payload + cmd + CRC）。 */
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

/** @description 将 64 字符十六进制 SHA-256 字符串转为 32 字节数组。 */
function sha256HexToBytes(shaHex) {
  const hex = shaHex.toUpperCase();
  const out = new Uint8Array(32);
  for (let i = 0; i < 32; i++) {
    out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

export function waitForAck(cmd, expectedSeq, timeoutMs = 8000, opts = {}) {
  // ★ 中止快速退出：若 0x20 ABORTED 已先于本调用到达（ackWaiter 当时为 null），
  //   这里立即 reject，避免在已中止的会话上盲等超时。
  if (state.abortRequested) {
    return Promise.reject(new Error('OTA 已中止'));
  }
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

/** @description 分块上传固件到 ESP32（通过 BLE GATT 写入）。 */
export async function uploadFirmware() {
  // 热重载：有 handle 则重新读盘（本地编译会改同路径固件）；否则用已选/input
  if (state.selectedFileHandle) {
    try {
      const freshFile = await state.selectedFileHandle.getFile();
      await updateFileInfo(freshFile);
    } catch (e) {
      bleLog('FILE', '重新读取文件失败: ' + e.message + '，将使用缓存版本', 'W');
    }
  } else if (!state.selectedFile) {
    await syncSelectedFileFromInput();
  }
  if (!state.selectedFile || !state.dataInChar) return false;

  const cfg = currentTarget();
  state.uploadTargetKey = window.flashTarget || 'esp32';
  const payloadSize = Math.max(32, Math.min(498, currentPayloadSize()));
  const file = state.selectedFile;

  state.abortRequested = false;
  state.uploading = true;
  state.lastUploadStatus = 'uploading';
  const otaSessionId = nextOtaSessionId();
  clearOtaResultWaiter();
  if (typeof setActivePhase === 'function') {
    setActivePhase(cfg.label === 'GD32' ? 'gd32Ota' : 'esp32Ota');
  }
  switchStageFlow('ble');
  state.lastAckCmd = null;
  state.lastAckSeq = -1;
  state.lastAckBytes = 0;
  state.ackGeneration = 0;
  state.deviceSha256 = null;
  state.lastEsp32DoneSession = null;
  state.activeTotalSize = 0;
  state.activePacketTotal = 0;
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

    bleLog('OTA', `开始准备固件: target=${cfg.label}, file=${file.name}, payload=${payloadSize}, cmd=0x${cfg.packetCmd.toString(16).padStart(2, '0').toUpperCase()}, ackWindow=${BLE_OTA_ACK_WINDOW}`, 'D');
    setProgress(0, `[${cfg.label}] 正在读取固件文件...`);
    setBleStage(4, '读取固件...', null);
    const fileBytes = new Uint8Array(await file.arrayBuffer());
    const totalSize = fileBytes.length;
    const packetTotal = Math.ceil(totalSize / payloadSize);
    bleLog('OTA', `固件已读取: totalSize=${totalSize}, packetTotal=${packetTotal}, file.size=${file.size}`, 'D');
    if (totalSize !== file.size) {
      bleLog('OTA', `⚠️ file.size(${file.size}) ≠ arrayBuffer(${totalSize})，以 arrayBuffer 为准，请确认页面已 Ctrl+F5 刷新`, 'W');
    }

    /* 上传前始终按本次读到的内容重算 SHA，不沿用选文件时的旧值 */
    var chipKey = (cfg.label === 'GD32') ? 'gd32' : 'esp32';
    var shaHex = await computeFileSha(file, chipKey, fileBytes.buffer);
    if (!shaHex) throw new Error('SHA-256 计算失败');
    state.fileSha256 = shaHex;
    var fileSha256 = shaHex.toLowerCase();
    var shaElem = document.getElementById('fileSha256');
    if (shaElem) shaElem.textContent = shaHex.toUpperCase();

    setBleStage(4, '写入中...', null);
    setBleStageText(`正在写入 ${packetTotal} 包数据...`);
    // ★ 写入阶段已读出文件大小/包数，记录到 state 供 handleNotify 在 ACK 到达时使用
    state.activeTotalSize = totalSize;
    state.activePacketTotal = packetTotal;
    els.sentCount.textContent = '0';
    setProgress(0, `[${cfg.label}] 等待设备确认 0/${packetTotal} 包`);
    bleLog('OTA', `开始写入 DATA_IN: build=${UI_BUILD}, target=${cfg.label}, file=${file.name}, total=${totalSize}, packets=${packetTotal}, payload=${payloadSize}, windowAck=${BLE_OTA_ACK_WINDOW}`, 'D');

    const MAX_WINDOW_RETRIES = 3;
    // 整分区预擦可长达数十秒；首包 ACK 超时必须覆盖 begin/erase
    const BEGIN_READY_TIMEOUT_MS = 120000;
    const WINDOW_ACK_TIMEOUT_MS = 8000;
    for (let windowStart = 0; windowStart < packetTotal; windowStart += BLE_OTA_ACK_WINDOW) {
      if (state.abortRequested) throw new Error('用户中止');

      const endSeq = Math.min(windowStart + BLE_OTA_ACK_WINDOW, packetTotal) - 1;

      // Windows Web Bluetooth 同一时刻只允许一个 GATT 操作。逐包等待仅保证
      // 提交顺序；实际吞吐由设备请求的高速 BLE 连接参数决定。
      // ★ 不再在循环内 setProgress：进度完全由设备 ACK 后的 state.lastAckBytes 驱动
      //
      // ★ 首窗特殊流控：seq=0 触发设备 begin+整分区预擦（worker 线程，可阻塞很久）。
      //   旧逻辑 sleep(200) 后立刻 withoutResponse 连发 → 队列满丢包 → 整窗重传，体感极慢。
      //   现改为：seq=0 送达后等设备「首包落盘 ACK」再发 1..endSeq。
      for (let seq = windowStart; seq <= endSeq; seq++) {
        if (state.abortRequested) throw new Error('用户中止');
        const start = seq * payloadSize;
        const end = Math.min(start + payloadSize, totalSize);
        const payload = fileBytes.slice(start, end);
        const packet = buildPacket(seq, totalSize, payload, cfg.packetCmd);

        if (seq === 0) {
          try {
            await state.dataInChar.writeValueWithResponse(packet);
            bleLog('OTA', 'seq=0 已用 writeValueWithResponse 确认送达，等待设备 begin/预擦完成…', 'D');
            setBleStageText('设备预擦 Flash 中，请等待…');
          } catch (e) {
            bleLog('OTA', `seq=0 写入失败: ${e.message}`, 'E');
            throw new Error('seq=0 写入失败，OTA 会话无法启动: ' + e.message);
          }
          state.ackGeneration++;
          const firstBytes = Math.min(payloadSize, totalSize);
          try {
            await waitForAck(cfg.packetCmd, 0, BEGIN_READY_TIMEOUT_MS, {
              expectedTotalSize: totalSize,
              payloadSize: payloadSize
            });
          } catch (e) {
            throw new Error('等待设备 begin/预擦完成超时: ' + e.message);
          }
          if (state.lastAckBytes < firstBytes) {
            throw new Error('设备 begin 就绪 ACK 字节不足: got=' + state.lastAckBytes + ' need=' + firstBytes);
          }
          bleLog('OTA', '设备 begin 就绪 (ackBytes=' + state.lastAckBytes + ')，继续发送窗口数据', 'D');
          setBleStageText('正在写入 ' + packetTotal + ' 包数据...');
          continue;
        }

        /* Windows WebBT 同时只允许一个 GATT 操作，必须逐包 await，禁止 Promise.all 并发写 */
        await state.dataInChar.writeValueWithoutResponse(packet);
      }

      state.ackGeneration++;
      try {
        await waitForAck(cfg.packetCmd, endSeq, WINDOW_ACK_TIMEOUT_MS, { expectedTotalSize: totalSize, payloadSize });
      } catch (e) {
        bleLog('OTA', `ACK 等待失败: ${e.message}，进入重试`, 'W');
      }

      let expectedBytes = Math.min((endSeq + 1) * payloadSize, totalSize);
      let retryCount = 0;
      let prevAckBytes = state.lastAckBytes;  // ★ 进度跟踪：检测重试是否有效
      // ★ 仅以字节数判断是否完成，不依赖 ackOk（generation 不匹配时 ackOk 不可靠）
      while (state.lastAckBytes < expectedBytes && retryCount < MAX_WINDOW_RETRIES) {
        // ★ 中止检查：0x20 ABORTED 可能在上一轮 waitForAck 超时期间到达，
        //   或在 sleep/续发过程中到达。立即抛出，避免无谓重试已中止的会话。
        if (state.abortRequested) throw new Error('用户中止');
        // 如果 ESP32 已确认所有数据（ACK 延迟到达），直接跳出
        if (state.lastAckBytes >= expectedBytes) break;

        // ★ 进度检测：如果重试后字节数没有增长，说明续传无效
        if (retryCount > 0 && state.lastAckBytes <= prevAckBytes) {
          bleLog('OTA', `上次重试暂未推进: ackBytes=${state.lastAckBytes} (前次=${prevAckBytes})，继续按缺口重试`, 'W');
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
        await sleep(10);

        for (let seq = resumeSeq; seq <= endSeq; seq++) {
          // ★ 中止检查：续发过程中可能收到 0x20 ABORTED
          if (state.abortRequested) throw new Error('用户中止');
          const start = seq * payloadSize;
          const end = Math.min(start + payloadSize, totalSize);
          const payload = fileBytes.slice(start, end);
          const packet = buildPacket(seq, totalSize, payload, cfg.packetCmd);
          // ★ 重试路径中 seq=0 也必须用 writeValueWithResponse，并等 begin 就绪 ACK
          if (seq === 0) {
            try {
              await state.dataInChar.writeValueWithResponse(packet);
              bleLog('OTA', '重试 seq=0 已送达，等待 begin 就绪 ACK…', 'I');
              state.ackGeneration++;
              try {
                await waitForAck(cfg.packetCmd, 0, BEGIN_READY_TIMEOUT_MS, {
                  expectedTotalSize: totalSize,
                  payloadSize: payloadSize
                });
              } catch (e2) {
                bleLog('OTA', '重试 seq=0 等待 begin ACK 失败: ' + e2.message, 'W');
              }
            } catch (e) {
              bleLog('OTA', `重试 seq=0 写入失败: ${e.message}`, 'E');
            }
          } else {
            await state.dataInChar.writeValueWithoutResponse(packet);
          }
        }

        state.ackGeneration++;
        try {
          await waitForAck(cfg.packetCmd, endSeq, WINDOW_ACK_TIMEOUT_MS, { expectedTotalSize: totalSize, payloadSize });
        } catch (e) {
          bleLog('OTA', `重试 ACK 等待失败: ${e.message}`, 'W');
        }
      }

      if (state.lastAckBytes < expectedBytes) {
        const missing = expectedBytes - state.lastAckBytes;
        throw new Error(`BLE 丢包检测: seq=${endSeq} 期望${expectedBytes}字节 实际${state.lastAckBytes}字节 丢失${missing}字节(${(missing / payloadSize).toFixed(1)}包) 已重试${retryCount}次`);
      }
    }

    // Stage 4 done
    // ★ 不再用本地 seq 推算固定百分比：进度以设备端 ACK 字节数为准
    const confirmedPct = state.activeTotalSize > 0
      ? Math.min(100, (state.lastAckBytes >>> 0) / state.activeTotalSize * 100)
      : 100;
    // 阶段 4 完成后，base 进度：GD32 留 55% 给中继/校验，ESP32 留 10% 给收尾
    const phaseBase = cfg.label === 'GD32' ? 45 : 90;
    // ACK 推进到的进度 = 阶段 4 在整体里占的份额 (phaseBase) × 设备真实确认比例
    const stage4Share = phaseBase * (confirmedPct / 100);
    setBleStage(4, '完成', null);
    setProgress(Math.min(phaseBase, stage4Share), `[${cfg.label}] 固件数据已全部写入 DATA_IN`);

    // Stage 5: 发送 0xF8 + 32B 文件 SHA-256，由设备端执行收尾并完成比对
    if (cfg.label === 'ESP32') {
      state.deviceSha256 = null;
      const frontSha = fileSha256.toUpperCase();
      const frontShaBytes = sha256HexToBytes(fileSha256);
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
        bleLog('OTA', '0xF8 + 32B SHA-256 已发送，等待设备端校验', 'D');
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
    const frontShaBytes = sha256HexToBytes(fileSha256);
    state.deviceSha256 = null;
    setBleStage(5, '发送触发...', 'active');
    setBleStageText('发送 0xF7 + SHA-256，等待设备端缓存校验...');
    showBleVerifyInfo(frontSha, '等待设备端比对...', { text: '已发送文件 SHA-256，等待 GD32 缓存校验' });
    await sleep(30);
    const cmdHex = cfg.ctrlHex;
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
      bleLog('OTA', `0xF7 + 32B SHA-256 已发送，等待设备端缓存校验与转发`, 'D');
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
    // ★ 中止不算失败：0x20 ABORTED handler 已更新 UI（progress/stage/toast），
    //   这里只记录日志并返回 false，避免把"用户中止"误报为写入失败。
    if (state.abortRequested || state.lastUploadStatus === 'aborted') {
      bleLog('OTA', `写入已中止: ${err.message}`, 'W');
      return false;
    }
    state.lastUploadStatus = 'failed';
    state.expectedReboot = false;   // ★ 刷写失败，取消重连退避
    state.reconnecting = false;
    state.reconnectAttempts = 0;
    bleLog('OTA', `写入失败: ${err.message}`, 'E');
    setProgress(0, `写入失败: ${err.message}`);
    setBleStageError(err.message);
    // ★ BLE OTA 失败后不再强制断开 BLE：错误 FINISH 已通过 0xFB 路径或设备端
    //   自动 abort 清理了 ESP32 OTA 会话。保留连接让用户可以快速重试或查看日志。
    //   仅当连接已半死（gatt.connected=false）时由 onDisconnected 走重连路径。
    return false;
  } finally {
    if (state.ackWaiter) {
      clearTimeout(state.ackWaiter.timer);
      state.ackWaiter = null;
    }
    if (state.lastUploadStatus === 'failed' || state.lastUploadStatus === 'aborted') {
      clearOtaResultWaiter();
      // 终态后允许下一次 uploadFirmware 干净起步（abortRequested 在下次入口也会清）
      // 此处不强制清 abortRequested：中止循环可能仍在 unwind
    }
    state.uploading = false;
    state.uploadTargetKey = null;
    setConnectionState(!!state.server);
  }
}

/** @description 自动刷写流程：上传 ESP32 固件 → SHA验证 → 触发 GD32 中继。 */
export async function autoFlash() {
  /* 与 uploadFirmware 一致：有 handle 则重新读盘，SHA 在 upload 内按内容重算 */
  if (state.selectedFileHandle) {
    try {
      const freshFile = await state.selectedFileHandle.getFile();
      await updateFileInfo(freshFile);
    } catch (e) {
      bleLog('FILE', '重新读取文件失败: ' + e.message + '，将使用缓存版本', 'W');
    }
  } else if (!state.selectedFile) {
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
      // ★ 中止/失败后不再断开 BLE：0xFB 已让设备端清理 OTA 会话，保留连接方便重试。
      //   若连接已半死，onDisconnected 会自动走重连路径。
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
