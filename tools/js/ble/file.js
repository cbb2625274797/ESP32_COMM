// ==================== BLE OTA 文件准备与分包 ====================
//
// ★ 上传中拦截策略（全文件统一）：file.js 是 BLE 通道文件操作的唯一入口，
//   transfer.js / state.js / session.js / 外部 bleOta.notifyFileChange 都汇聚到这里。
//   isBleUploading()（state.js）= state.uploading || state.autoFlashing；
//   window.isUploading = WiFi 通道 OTA 会话是否处于 UPLOADING（ota/session.js 维护）。
//   两者任一为真即视为上传中，禁止重置进度条/阶段/清空文件，避免覆盖进行中的 OTA。
import { state, els, bleLog, currentTarget, currentPayloadSize, setConnectionState, isBleUploading } from './state.js';
import { setBleStage, setBleStageText, resetBleStages } from './stages.js';
import { setProgress } from './transfer.js';
import { hideBleVerifyInfo } from './verify.js';
import { formatBytes, bytesToHex } from '../common/utils.js';
import { showToast } from '../common/toast.js';

/** 任一通道上传中（BLE 或 WiFi）。focus/change 等不分通道的事件用此判定。 */
function isAnyChannelUploading() {
  return isBleUploading() || (typeof window.isUploading !== 'undefined' && !!window.isUploading);
}

export function updateFileInfo(file) {
  // ★ 上传进行中禁止重置进度/阶段：目标切换、模式切换、syncBleFileFromSelectedTarget
  //   等事件会在上传期间调用本函数，若不拦截会触发 finalizeFileInfo → setProgress(0, ...)
  //   把进度条从"已确认 N/M 包"错误地刷回"等待选择文件 0%"。
  if (isBleUploading() && file && state.selectedFile) {
    // 上传中且文件未变：仅刷新元数据（文件名/大小等），不碰进度条和阶段
    bleLog('FILE', '上传进行中，跳过 updateFileInfo 进度重置', 'D');
    return Promise.resolve();
  }

  els.fileName.textContent = file ? file.name : '-';
  els.fileType.textContent = file ? (file.name.toLowerCase().includes('.hex') ? 'Intel HEX' : 'Binary') : '-';
  els.fileSize.textContent = file ? formatBytes(file.size) : '0 B';
  const payloadSize = currentPayloadSize();
  const packets = file ? Math.ceil(file.size / payloadSize) : 0;
  els.packetCount.textContent = String(packets);
  els.sentCount.textContent = '0';

  state.selectedFile = file || null;
  state.lastUploadStatus = 'idle';

  if (els.fileInfo) {
    if (file) els.fileInfo.classList.add('show');
    else els.fileInfo.classList.remove('show');
  }

  // ★ HEX → BIN 自动转换：复用 app.js 公共函数 window.convertHexFileToBin
  if (file && file.name.toLowerCase().endsWith('.hex') && typeof window.convertHexFileToBin === 'function') {
    return convertHexFileToBin(file).then(function(result) {
      if (!result.converted) {
        bleLog('FILE', file.name + ' 扩展名为 .hex 但内容不像 HEX 格式，按原始文件处理', 'W');
        finalizeAndHash(file);
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
      finalizeAndHash(result.file);
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
  finalizeAndHash(file);
  return Promise.resolve();
}

/** finalizeFileInfo + computeFileSha256 的统一出口：HEX 转换完成分支与非 HEX 分支共用。 */
function finalizeAndHash(file) {
  finalizeFileInfo(file);
  computeFileSha256(file);
}

function finalizeFileInfo(file) {
  // ★ 上传进行中禁止重置进度条/阶段：防止目标切换等事件在上传期间
  //   通过 finalizeFileInfo → setProgress(0, '等待选择文件') 覆盖上传进度。
  if (isBleUploading()) {
    bleLog('FILE', '上传进行中，跳过 finalizeFileInfo 进度重置', 'D');
    return;
  }
  setConnectionState(!!state.server);
  if (file) {
    setProgress(0, `已选择 ${file.name}`);
    bleLog('FILE', `已选择: ${file.name} (${formatBytes(file.size)}), 目标=${currentTarget().label}`);
  } else {
    setProgress(0, '等待选择文件');
    if (state.server) {
      resetBleStages();
      setBleStage(3, '', null);
      setBleStageText('BLE 连接就绪，请选择固件文件');
    }
  }
}

/**
 * 计算文件 SHA-256 并推进 BLE stage 流。
 * stage 推进逻辑收敛在此函数内，避免与 finalizeFileInfo 隐式耦合：
 *   - 已连接且 file 非空：进入「计算中」态（stage 3 active + 状态栏文案）
 *   - 哈希算完：stage 3 done + 推进 stage 4 + 状态栏「文件就绪」
 *   - file 为空 / 未连接：不碰 stage（由 finalizeFileInfo 的 else 分支 + clearSelectedFile 负责）
 */
function computeFileSha256(file) {
  var shaElem = document.getElementById('fileSha256');
  if (!file || !shaElem) {
    if (shaElem) shaElem.textContent = '-';
    state.fileSha256 = null;
    return;
  }
  // 已连接：进入「计算中」态（旧逻辑放在 finalizeFileInfo 里，现收敛至此）
  if (state.server) {
    setBleStage(3, '计算哈希...', 'active');
    setBleStageText('正在计算文件 SHA-256...');
  }
  shaElem.textContent = '计算中...';
  file.arrayBuffer().then(function(buf) {
    return crypto.subtle.digest('SHA-256', buf);
  }).then(function(hashBuffer) {
    var hashHex = bytesToHex(new Uint8Array(hashBuffer)).toUpperCase();
    shaElem.textContent = hashHex;
    state.fileSha256 = hashHex;
    bleLog('FILE', 'SHA-256: ' + hashHex);
    if (state.server) {
      setBleStage(3, 'SHA-256 已计算', null);
      setBleStage(4, '', null);
      setBleStageText('文件就绪，可以开始刷写');
    }
  }).catch(function(err) {
    shaElem.textContent = '计算失败: ' + err.message;
    state.fileSha256 = null;
  });
}

export async function syncSelectedFileFromInput() {
  // ★ 上传进行中禁止同步：window focus / file input change 事件会在上传期间
  //   触发本函数，若不拦截会通过 updateFileInfo 把进度条重置为"等待选择文件 0%"。
  //   ★ 必须同时拦截 WiFi 通道上传：focus 事件不分通道，WiFi 刷写时点击网页/切标签
  //   回来会触发本函数 → finalizeFileInfo → setProgress(0)，把进度"闪一下"归零，
  //   直到下一次 XHR progress 事件才恢复。
  if (isAnyChannelUploading()) return;
  // FSAPI 选择的文件以 selectedFileHandle 为准，不从 input 元素同步，避免切标签页回来误清空
  if (state.selectedFileHandle) return;
  const file = (els.firmwareFile && els.firmwareFile.files && els.firmwareFile.files[0]) || null;
  await updateFileInfo(file);
}

// 通过 File System Access API 选择文件（支持热重载）
export async function pickFileViaFSAPI() {
  if (!window.showOpenFilePicker) {
    bleLog('FILE', '当前浏览器不支持 File System Access API', 'W');
    return false;
  }
  try {
    const pickerOpts = {
      id: 'dcu-fw-ble',
      types: [{ description: '固件文件', accept: { 'application/octet-stream': ['.bin', '.hex'] } }],
      multiple: false
    };
    /* 再次选择时定位到刷新/上次已选文件所在目录 */
    if (state.selectedFileHandle) {
      pickerOpts.startIn = state.selectedFileHandle;
    }
    const handles = await window.showOpenFilePicker(pickerOpts);
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

export function clearSelectedFile() {
  // ★ 上传进行中禁止清除文件：防止目标切换/模式切换在上传期间清空文件引用，
  //   导致进度条被重置为"等待选择文件 0%"并中断上传数据源。
  if (isBleUploading()) {
    bleLog('FILE', '上传进行中，拒绝清除文件', 'W');
    return;
  }
  state.selectedFile = null;
  state.selectedFileHandle = null;
  state.lastUploadStatus = 'idle';
  state.fileSha256 = null;
  state.deviceSha256 = null;
  if (els.firmwareFile) els.firmwareFile.value = '';
  updateFileInfo(null);  // 内部 finalizeFileInfo(null) 已重置进度条并恢复「连接就绪」stage
  hideBleVerifyInfo();
  bleLog('FILE', '文件已清除');
}

// 别名导出，与 window.bleOta.clearFile 对齐
export { clearSelectedFile as clearFile };

/**
 * 外部文件变更通知（index.html 拖拽 / console-mode 切换等）。
 * ★ 守卫集中在此：file.js 是 BLE 文件操作唯一入口，外部包装层（ble_ota.js）不再重复守卫。
 */
export async function notifyFileChange(file, handle) {
  if (isAnyChannelUploading()) {
    bleLog('FILE', '上传进行中，忽略外部 notifyFileChange', 'D');
    return;
  }
  if (file) {
    if (handle) state.selectedFileHandle = handle;
    await updateFileInfo(file);
  }
}
