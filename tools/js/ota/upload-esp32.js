/**
 * upload-esp32.js - ESP32 本机 OTA 上传流程 + 完成升级
 *
 * 包含：startUpload 入口（处理 esp32 + gd32 共用互斥/重读逻辑）、
 *       XHR 上传、SHA 校验、verifyEsp32Firmware、restartEsp32。
 *
 * 共享状态（通过 window.* 引用）：
 *   window.esp32File / window.gd32File / window.esp32FileHandle / window.gd32FileHandle
 *   window._esp32FileSha256 / window._gd32FileSha256 / window.activeXhr
 *   window.isUploading / window.uploadBtnOrigText / window.activeUploadChip
 *
 * 复用的函数（通过 window.* 引用）：
 *   window.otaSessionActive / window.otaSessionTerminated / window.otaSessionReset
 *   window.otaSessionTransition / window.OtaSessionState / window.otaSessionTarget
 *   window.cleanupAfterOtaFailure / window.pausePollingForUpload
 *   window.resumePollingAfterUpload / window.updateEsp32StagesFromPhase
 *   window.switchStageFlow / window.setActivePhase / window.setProgress
 *   window.markPhaseDone / window.startUploadWaitHeartbeat / window.stopUploadWaitHeartbeat
 *   window.updateUploadButtonLabel / window.processFile / window.clearFile
 *   window.triggerGd32Relay / window.rebootObservedDown / window.rebootStartTime
 *   window.isEsp32Rebooting / window.getBaseUrl / window.formatBytes
 *   window.showToast / window.addLog
 */
import { formatBytes } from '../common/utils.js';
import { showToast } from '../common/toast.js';
import { addLog } from '../common/log.js';
import {
  otaSessionActive, otaSessionTerminated, otaSessionReset,
  otaSessionTransition, OtaSessionState, stopUploadWaitHeartbeat,
  startUploadWaitHeartbeat
} from './session.js';
import { updateEsp32StagesFromPhase, switchStageFlow, setActivePhase, setProgress, markPhaseDone, cleanupAfterOtaFailure } from './progress.js';
import { ensureFileShaCached } from './file-handle.js';

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

function formatHttpErrorDetail(text) {
  var detail = (text || '').replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ').trim();
  if (detail.length > 180) detail = detail.slice(0, 177) + '...';
  return detail;
}

/** @description 启动 ESP32 本机 OTA 上传流程（互斥获取→分块上传→SHA验证→重启）。 */
export async function startUpload(chip) {
  var file = chip === 'esp32' ? window.esp32File : window.gd32File;
  if (!file) return;
  // ★ 状态机守卫：用统一的会话活跃判断替代裸 isUploading。
  //   FAILED/DONE 会话需要先复位才能发起新上传（下方 otaSessionReset）。
  //   RELAYING 可能因 ERROR 被设备软清为 IDLE 而前端未收尾，先探测再决定。
  if (otaSessionActive()) {
    var busyState = window.otaSessionState;
    if (busyState === OtaSessionState.RELAYING || busyState === OtaSessionState.VERIFYING) {
      try {
        var staleProbe = await fetch(window.getBaseUrl() + '/system/status', {
          signal: AbortSignal.timeout(2000)
        });
        if (staleProbe.ok) {
          var staleSt = await staleProbe.json();
          var so = staleSt.ota || {};
          var ssc0 = Number.isFinite(so.sys_state_code) ? so.sys_state_code : -1;
          var rs0 = Number.isFinite(so.relay_state) ? so.relay_state : -1;
          /* 设备已空闲/完成/错误残留：前端会话是陈旧活跃态，强制清掉 */
          var deviceFree = (ssc0 === 0 || ssc0 === 2 || ssc0 === 4 || ssc0 === 8) &&
                           (rs0 === 0 || rs0 === 5 || rs0 === 255 || rs0 < 0);
          if (deviceFree) {
            addLog('W', '上传', '检测到陈旧会话 ' + busyState +
              '（设备已 ' + (so.sys_state || ssc0) + '），自动清理后继续');
            otaSessionReset();
          }
        }
      } catch (_) { /* 探测失败则仍按占用拒绝 */ }
    }
  }
  if (otaSessionActive()) {
    var busyChip = (window.otaSessionTarget || window.activeUploadChip || window.flashTarget || chip || 'ota').toUpperCase();
    addLog('W', '上传', '已有 ' + busyChip + ' OTA 会话进行中 (' + window.otaSessionState + ')，拒绝新的 ' + chip.toUpperCase() + ' 上传');
    showToast('OTA 通道正在占用，请等待当前刷写结束', 'error');
    return;
  }
  // 上一次会话已终结（DONE/FAILED）：发起新会话前先复位状态机
  if (otaSessionTerminated()) otaSessionReset();
  // 记录本会话目标
  window.otaSessionTarget = chip;

  window.pausePollingForUpload();
  /* 先进入 UPLOADING（同步 isUploading），再 await 读文件，防止重复提交 */
  otaSessionTransition(OtaSessionState.UPLOADING);
  window.activeUploadChip = chip;
  /* 新上传：清掉上次完成标志，避免立刻被 status 残留判成“又完成了” */
  window.gd32CompleteAnnounced = false;
  window.relayWatchLastSeq = -1;
  window.gd32RelaySessionLive = false;
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
      var probe = await fetch(window.getBaseUrl() + '/system/status', {
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
        // ERROR(8) 放行给后端清理；REBOOTING(7) 不可逆。
        // FINALIZING(6)：真收尾时后端会 403；SHA 失败后可能假占用（core 已空），
        // status 自愈 / begin 残留清理需要请求触达设备，前端同通道不硬拦。
        if ((ssc === 5 || ssc === 6) && otaChannel === 1 && _pi < 3) continue;
        var esp32Busy = (ssc === 7 ||
          (ssc === 6 && otaChannel !== 0) ||
          (ssc === 5 && otaChannel !== 0));
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
  var handle = chip === 'esp32' ? window.esp32FileHandle : window.gd32FileHandle;
  if (handle) {
    // 有 FileSystemFileHandle，调用 getFile() 获取最新内容
    try {
      var freshFile = await handle.getFile();
      if (chip === 'esp32') {
        window.esp32File = freshFile;
        file = freshFile;
      } else {
        if (freshFile.name.toLowerCase().endsWith('.hex')) {
          window.processFile(freshFile, 'gd32');
          await new Promise(function(resolve) { setTimeout(resolve, 200); });
          file = window.gd32File;
        } else {
          window.gd32File = freshFile;
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
      window.clearFile(chip);
      // ★ 状态机：文件失效属于会话失败
      cleanupAfterOtaFailure('本地文件失效');
      return;
    }
  }
  var uploadBtn = document.getElementById(chip === 'esp32' ? 'uploadEsp32Btn' : 'uploadGd32Btn');
  window.uploadBtnOrigText = uploadBtn.textContent;
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
    // SHA-256：processFile 阶段已算好并缓存；这里若 handle.getFile() 拿回的
    // fresh file meta 未变就直接复用（命中 30–100ms/MB 的计算）；
    // 用户 rebuild 使 lastModified 变化时 ensureFileShaCached 自动 fallback 到重算。
    await ensureFileShaCached(file, chip, arrayBuffer);
    var fileSize = file.size;
    var body, url = window.getBaseUrl() + '/ota/upload';
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
    window.activeXhr = new XMLHttpRequest();
    var xhr = window.activeXhr;
    xhr.upload.addEventListener('progress', function (e) {
      if (e.lengthComputable) {
        var pct = Math.round((e.loaded / e.total) * 100);
        window.uploadProgress = pct;
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
    xhr.addEventListener('load', function () {
      stopUploadWaitHeartbeat();
      /* 成功路径摘掉 activeXhr；失败路径留给 cleanupAfterOtaFailure 统一摘掉并 abort 幂等 */
      if (window.activeXhr === xhr) window.activeXhr = null;
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
        /* 成功：恢复轮询/按钮（会话仍 VERIFYING/RELAYING，isUploading 由 transition 清） */
        window.resumePollingAfterUpload();
        uploadBtn.disabled = false;
        uploadBtn.textContent = window.uploadBtnOrigText;
        window.updateUploadButtonLabel();
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
          window.triggerGd32Relay();
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
      // cleanup 会先摘 activeXhr 再 abort；仅当本 xhr 仍是活动请求或会话仍在 UPLOADING 时清理
      if (window.activeXhr === xhr || window.otaSessionState === OtaSessionState.UPLOADING) {
        showToast('上传过程发生网络错误', 'error');
        cleanupAfterOtaFailure('网络错误');
      }
    });
    xhr.addEventListener('abort', function () {
      stopUploadWaitHeartbeat();
      // clearOtaSessionResiduals 先置 activeXhr=null 再 abort → 此处勿二次 cleanup
      if (window.activeXhr === xhr) {
        showToast('上传已中止', 'error');
        cleanupAfterOtaFailure('已中止');
      }
    });
    xhr.addEventListener('timeout', function () {
      stopUploadWaitHeartbeat();
      if (window.activeXhr === xhr || window.otaSessionState === OtaSessionState.UPLOADING) {
        showToast('上传超时：设备可能忙或连接被踢，请重试', 'error');
        cleanupAfterOtaFailure('上传超时（设备未在 5 分钟内响应）');
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
//
// 竞态说明（易误报“应用失败”）：
//   1) finalize 成功 → 回 JSON ok → 100ms 后 esp_restart
//   2) 响应丢失 / 浏览器只收到半截 / 请求迟到打到已重启的新固件
//      → 正文可能是明文或 JSON 的 "OTA not started"
//   3) 此时 /system/status 已是 IDLE，uptime 往往很小
// 旧逻辑：resp.json() 抛错 → 探测到 IDLE → cleanupAfterOtaFailure
// 正确：上传已成功的 VERIFYING 会话里，优先按「已应用并重启」推进 REBOOTING。

/** @description 进入 ESP32 重启等待（校验成功或推断已应用）。 */
function enterEsp32RebootingUi(reason, opts) {
  opts = opts || {};
  var resultEl = document.getElementById('sviResult');
  if (resultEl && !opts.keepResult) {
    resultEl.textContent = opts.resultText || '已应用，等待重启';
    resultEl.className = 'svi-val ok';
  }
  addLog('I', 'OTA', reason || '设备重启中…');
  showToast(opts.toast || '设备重启中…', 'success');
  updateEsp32StagesFromPhase(4);
  updateEsp32StagesFromPhase(5);
  window.rebootObservedDown = !!opts.observedDown;
  window.rebootStartTime = Date.now();
  otaSessionTransition(OtaSessionState.REBOOTING);
}

/**
 * /ota/update 结果不明确时探测设备真实状态。
 * @param {string} detail 原始错误摘要
 * @param {{notStarted?: boolean}} flags
 */
async function resolveEsp32VerifyAmbiguous(detail, flags) {
  flags = flags || {};
  addLog('W', 'OTA', '应用结果不明确: ' + (detail || 'unknown') + '，探测设备状态…');

  var sawOffline = false;
  var delays = [0, 800, 1500, 2500, 4000];
  for (var i = 0; i < delays.length; i++) {
    if (delays[i] > 0) {
      await new Promise(function (r) { setTimeout(r, delays[i]); });
    }
    try {
      var probe = await fetch(window.getBaseUrl() + '/system/status', {
        signal: AbortSignal.timeout(4000)
      });
      if (!probe.ok) {
        sawOffline = true;
        continue;
      }
      var st = await probe.json();
      var o = st.ota || {};
      var ssc = Number.isFinite(o.sys_state_code) ? o.sys_state_code : -1;
      var uptime = Number(st.uptime_sec);
      if (!Number.isFinite(uptime)) uptime = -1;

      // 7=REBOOTING
      if (ssc === 7) {
        enterEsp32RebootingUi('探测到设备 REBOOTING，按重启中处理', {
          toast: '设备正在重启…',
          observedDown: false
        });
        return;
      }
      // 5=RECEIVING / 6=FINALIZING：仍在收尾，再等几轮
      if (ssc === 5 || ssc === 6) {
        addLog('I', 'OTA', '设备仍在 ESP32 OTA (sys=' + ssc + ')，继续等待…');
        continue;
      }
      // IDLE/其它：上传本会话已成功过；短 uptime 或曾离线 → 几乎一定是已 finalize 并重启
      // 「OTA not started」在 VERIFYING 上下文下同理（新固件无会话）。
      var freshBoot = uptime >= 0 && uptime < 120;
      if (flags.notStarted || freshBoot || sawOffline) {
        enterEsp32RebootingUi(
          flags.notStarted
            ? ('设备回报 OTA not started（uptime=' + uptime +
               's）— 上传已完成，按已应用并重启处理')
            : ('探测到设备已在线 (sys=' + ssc + ', uptime=' + uptime +
               's) — 按已应用并重启处理'),
          {
            toast: '固件可能已应用，等待启动完成…',
            observedDown: sawOffline,
            resultText: '已应用（响应未完整返回，按重启完成确认）'
          }
        );
        return;
      }
      // 长 uptime + IDLE 且非 notStarted：更像请求失败、设备从未进入 finalize
      addLog('E', 'OTA', '应用请求失败且设备非重启态 (sys=' + ssc +
        ', uptime=' + uptime + 's): ' + (detail || ''));
      var resultElFail = document.getElementById('sviResult');
      if (resultElFail) {
        resultElFail.textContent = '✗ 请求失败: ' + (detail || 'unknown');
        resultElFail.className = 'svi-val err';
      }
      showToast('应用请求失败: ' + (detail || 'unknown'), 'error');
      cleanupAfterOtaFailure('应用请求失败');
      return;
    } catch (_probeErr) {
      sawOffline = true;
    }
  }

  // 多轮无响应，或一直停在 FINALIZING
  if (sawOffline) {
    enterEsp32RebootingUi('设备无响应，按重启中观察（可能 TCP 被 esp_restart 掐断）', {
      toast: '设备无响应，等待重启恢复…',
      observedDown: true,
      resultText: '等待设备重启…'
    });
    return;
  }
  addLog('E', 'OTA', '应用阶段设备长时间仍忙或状态不明: ' + (detail || ''));
  var resultElBusy = document.getElementById('sviResult');
  if (resultElBusy) {
    resultElBusy.textContent = '✗ 请求中断，设备仍忙';
    resultElBusy.className = 'svi-val err';
  }
  cleanupAfterOtaFailure('应用请求中断（设备仍忙）');
}

/** @description 验证 ESP32 固件 SHA-256 是否与设备端计算值一致。 */
export async function verifyEsp32Firmware() {
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

    var resp = await fetch(window.getBaseUrl() + '/ota/update', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(requestBody),
      signal: AbortSignal.timeout(120000)
    });

    // 禁止裸 resp.json()：失败体可能是 httpd 明文或半截响应
    var bodyTxt = '';
    try {
      bodyTxt = await resp.text();
    } catch (te) {
      await resolveEsp32VerifyAmbiguous('读取响应失败: ' + te.message, {});
      return;
    }
    var data = null;
    if (bodyTxt && bodyTxt.trim()) {
      try {
        data = JSON.parse(bodyTxt);
      } catch (_pe) {
        data = null;
      }
    }

    var notStarted =
      (data && (data.code === 'ota_not_started' ||
        /OTA not started/i.test(String(data.message || '')))) ||
      /OTA not started/i.test(bodyTxt || '');

    if (resp.ok && data && data.status === 'ok') {
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
      enterEsp32RebootingUi('SHA-256 校验通过，设备自动重启中...', {
        toast: '校验通过，设备重启中...',
        keepResult: true,
        observedDown: false
      });
      return;
    }

    // SHA 明确不匹配
    if (data && (data.status === 'error' || !resp.ok) &&
        /mismatch|不匹配/i.test(String(data.message || bodyTxt || ''))) {
      var detail = data.message || 'SHA-256 不匹配';
      addLog('E', 'OTA', '校验失败: ' + detail);
      showToast('校验失败: ' + detail, 'error');
      if (data.device) {
        var dSha = document.getElementById('sviDeviceSha');
        if (dSha) dSha.textContent = data.device;
      }
      var resultElMm = document.getElementById('sviResult');
      if (resultElMm) {
        resultElMm.textContent = '✗ ' + detail;
        resultElMm.className = 'svi-val err';
      }
      cleanupAfterOtaFailure('校验失败');
      return;
    }

    // 迟到请求打到已重启固件 / 非 JSON 响应：勿当硬失败
    if (notStarted) {
      await resolveEsp32VerifyAmbiguous(
        bodyTxt.slice(0, 120) || 'OTA not started',
        { notStarted: true }
      );
      return;
    }

    var failDetail = (data && data.message) ||
      formatHttpErrorDetail(bodyTxt) ||
      ('HTTP ' + resp.status);
    await resolveEsp32VerifyAmbiguous(failDetail, {});
  } catch (e) {
    // fetch 异常 ≠ 一定失败：finalize 成功后 esp_restart 会掐断 TCP
    await resolveEsp32VerifyAmbiguous(e && e.message ? e.message : String(e), {});
  }
}

export async function restartEsp32() {
  if (!confirm('确定要重启 ESP32 吗？')) return;
  addLog('I', '系统', '=== 请求重启 ESP32 ===');
  showToast('ESP32 正在重启...', 'success');
  try {
    var resp = await fetch(window.getBaseUrl() + '/restart', {
      method: 'POST', signal: AbortSignal.timeout(5000)
    });
    if (resp.ok) {
      var data = await resp.json();
      addLog('I', '系统', '重启已触发: ' + data.message);
      showToast('ESP32 重启中，页面将自动恢复...', 'success');
      window.isEsp32Rebooting = true;
      window.rebootObservedDown = false;    // 重置：尚未观察到设备实际下线
      window.rebootStartTime = Date.now();
    }
  } catch (e) {
    addLog('E', '系统', '重启请求失败: ' + e.message);
    showToast('重启失败', 'error');
  }
}