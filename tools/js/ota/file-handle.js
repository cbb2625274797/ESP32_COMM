/**
 * file-handle.js - 文件处理 + Intel HEX → BIN 转换
 *
 * 包含：HEX 解析、文件选择、FSAPI、SHA-256 计算、文件信息 UI、清除。
 *
 * 共享状态（通过 window.* 引用）：
 *   window.esp32File / window.gd32File / window.esp32FileHandle / window.gd32FileHandle
 *   window._esp32FileSha256 / window._gd32FileSha256
 *
 * 复用的函数（通过 window.* 引用）：
 *   window.updateUploadButtonLabel / window.showToast / window.addLog
 */
import { formatBytes, bytesToHex } from '../common/utils.js';
import { showToast } from '../common/toast.js';
import { addLog } from '../common/log.js';

// ==================== 文件处理 ====================
// 文件状态（模块级，但被 upload-esp32/upload-gd32 通过 window.* 读写）
window.esp32File = null;
window.gd32File = null;
window.esp32FileHandle = null;  // FileSystemFileHandle for hot reload
window.gd32FileHandle = null;   // FileSystemFileHandle for hot reload
/** 两路各自一份 SHA：选文件时写入，上传时读取；互不切换、不共享 */
window._esp32FileSha256 = null;
window._gd32FileSha256 = null;

/** localStorage：升级目标等轻量偏好（刷新后恢复） */
var LS_FLASH_TARGET = 'dcu_ota_flash_target';
/** localStorage：上次文件名/大小（无权限时仍可展示，提示一点恢复） */
var LS_FILE_META = 'dcu_ota_fw_meta';
/** IndexedDB：FileSystemFileHandle 可结构化克隆，刷新后可恢复同一文件 */
var IDB_NAME = 'dcu_ota_fw_v1';
var IDB_STORE = 'file_handles';

/**
 * 刷新后权限常为 prompt，requestPermission 必须在用户手势里才成功。
 * 此处暂存待授权 handle，点击上传区时再 requestPermission 并直接 getFile。
 */
window._pendingFwHandles = { esp32: null, gd32: null };

/** 模块级 IndexedDB 连接缓存，避免每次 put/get 都重开 */
var _idbDbPromise = null;
function openHandleDb() {
  if (!_idbDbPromise) {
    _idbDbPromise = new Promise(function (resolve, reject) {
      if (!window.indexedDB) {
        reject(new Error('indexedDB unavailable'));
        return;
      }
      var req = indexedDB.open(IDB_NAME, 1);
      req.onupgradeneeded = function () {
        var db = req.result;
        if (!db.objectStoreNames.contains(IDB_STORE)) {
          db.createObjectStore(IDB_STORE);
        }
      };
      req.onsuccess = function () { resolve(req.result); };
      req.onerror = function () { reject(req.error || new Error('idb open failed')); };
    });
    // 失败时清空缓存，下次允许重试
    _idbDbPromise.catch(function () { _idbDbPromise = null; });
  }
  return _idbDbPromise;
}

function idbPutHandle(chip, handle) {
  return openHandleDb().then(function (db) {
    return new Promise(function (resolve, reject) {
      var tx = db.transaction(IDB_STORE, 'readwrite');
      if (handle) {
        tx.objectStore(IDB_STORE).put(handle, chip);
      } else {
        tx.objectStore(IDB_STORE).delete(chip);
      }
      tx.oncomplete = function () { resolve(); };
      tx.onerror = function () { reject(tx.error); };
    });
  }).catch(function () { /* 无痕/禁用存储时静默 */ });
}

function idbGetHandle(chip) {
  return openHandleDb().then(function (db) {
    return new Promise(function (resolve, reject) {
      var tx = db.transaction(IDB_STORE, 'readonly');
      var req = tx.objectStore(IDB_STORE).get(chip);
      req.onsuccess = function () { resolve(req.result || null); };
      req.onerror = function () { reject(req.error); };
    });
  }).catch(function () { return null; });
}

/** 持久化升级目标（esp32/gd32） */
export function persistFlashTarget(chip) {
  try {
    if (chip === 'esp32' || chip === 'gd32') {
      localStorage.setItem(LS_FLASH_TARGET, chip);
    }
  } catch (_) {}
}

export function loadPersistedFlashTarget() {
  try {
    var t = localStorage.getItem(LS_FLASH_TARGET);
    return (t === 'gd32' || t === 'esp32') ? t : null;
  } catch (_) {
    return null;
  }
}

/** 保存 FSAPI handle，刷新后可默认恢复该文件（仅模块内使用，不导出） */
function persistFileHandle(chip, handle) {
  if (chip !== 'esp32' && chip !== 'gd32') return Promise.resolve();
  return idbPutHandle(chip, handle || null);
}

function readFileMetaMap() {
  try {
    var raw = localStorage.getItem(LS_FILE_META);
    if (!raw) return {};
    var o = JSON.parse(raw);
    return o && typeof o === 'object' ? o : {};
  } catch (_) {
    return {};
  }
}

function persistFileMeta(chip, file) {
  if (chip !== 'esp32' && chip !== 'gd32') return;
  try {
    var map = readFileMetaMap();
    if (file && file.name) {
      map[chip] = {
        name: file.name,
        size: file.size || 0,
        lastModified: file.lastModified || 0
      };
    } else {
      delete map[chip];
    }
    localStorage.setItem(LS_FILE_META, JSON.stringify(map));
  } catch (_) {}
}

function loadFileMeta(chip) {
  var map = readFileMetaMap();
  return map[chip] || null;
}

/** 安全调用 window.updateUploadButtonLabel（恢复路径可能在 app.js 初始化前触发） */
function safeUpdateUploadButtonLabel() {
  if (typeof window.updateUploadButtonLabel === 'function') {
    window.updateUploadButtonLabel();
  }
}

/** 仅 query，不 request（刷新无用户手势时 request 会失败） */
async function queryHandlePermission(handle) {
  if (!handle || typeof handle.getFile !== 'function') return 'denied';
  try {
    if (typeof handle.queryPermission === 'function') {
      return await handle.queryPermission({ mode: 'read' });
    }
    /* 无 query API 时尝试直接 getFile */
    await handle.getFile();
    return 'granted';
  } catch (_) {
    return 'denied';
  }
}

/**
 * 在用户点击手势中：对上次 handle 重新授权并恢复文件（无需再走选文件对话框）。
 * @returns {Promise<boolean>} 是否已成功恢复
 */
export async function resumePersistedFirmwareWithGesture(chip) {
  if (chip !== 'esp32' && chip !== 'gd32') return false;
  var handle = (window._pendingFwHandles[chip]) ||
    (chip === 'esp32' ? window.esp32FileHandle : window.gd32FileHandle) ||
    null;
  if (!handle) {
    try {
      handle = await idbGetHandle(chip);
    } catch (_) {
      handle = null;
    }
  }
  if (!handle || typeof handle.getFile !== 'function') return false;

  try {
    var perm = 'granted';
    if (typeof handle.queryPermission === 'function') {
      perm = await handle.queryPermission({ mode: 'read' });
    }
    if (perm !== 'granted' && typeof handle.requestPermission === 'function') {
      perm = await handle.requestPermission({ mode: 'read' });
    }
    if (perm !== 'granted') {
      addLog('W', '文件', chip.toUpperCase() + ' 未授予读取权限，请在弹窗中允许，或重新选择文件');
      return false;
    }
    var file = await handle.getFile();
    await applyRestoredFile(chip, handle, file, { silent: true });
    window._pendingFwHandles[chip] = null;
    addLog('I', '文件', chip.toUpperCase() + ' 已重新授权并恢复: ' + file.name);
    showToast(chip.toUpperCase() + ' 已恢复上次固件', 'success');
    return true;
  } catch (e) {
    addLog('W', '文件', chip.toUpperCase() + ' 恢复失败: ' + (e && e.message ? e.message : e));
    return false;
  }
}

function applyRestoredFile(chip, handle, file, options) {
  if (chip === 'esp32') {
    window.esp32FileHandle = handle;
    window.esp32File = file;
  } else {
    window.gd32FileHandle = handle;
    window.gd32File = file;
  }
  if (window.sharedFirmwareFiles) {
    window.sharedFirmwareFiles[chip] = { file: file, handle: handle };
  }
  /* 等 SHA 写入对应槽后再 sync BLE，避免二次重算 */
  return Promise.resolve(processFile(file, chip, Object.assign({ skipHandlePersist: true }, options || {})))
    .then(function () {
      if (typeof window.syncBleFileFromSelectedTarget === 'function') {
        window.syncBleFileFromSelectedTarget();
      }
      safeUpdateUploadButtonLabel();
    });
}

/** 无权限时仍展示上次文件名，提示点上传区一键恢复 */
function showPendingReauthUi(chip, handle, meta) {
  if (chip === 'esp32') window.esp32FileHandle = handle;
  else window.gd32FileHandle = handle;
  window._pendingFwHandles[chip] = handle;
  if (window.sharedFirmwareFiles) {
    window.sharedFirmwareFiles[chip] = { file: null, handle: handle };
  }

  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  var info = document.getElementById('fileInfo' + suffix);
  var nameEl = document.getElementById('fileName' + suffix);
  var typeEl = document.getElementById('fileType' + suffix);
  var sizeEl = document.getElementById('fileSize' + suffix);
  var name = (meta && meta.name) || (handle && handle.name) || '上次固件';
  if (info) info.classList.add('show');
  if (nameEl) nameEl.textContent = name + '（点击上传区重新授权）';
  if (typeEl) typeEl.textContent = '待授权';
  if (sizeEl) sizeEl.textContent = meta && meta.size ? formatBytes(meta.size) : '--';
  var shaEl = document.getElementById('fileSha256' + suffix);
  if (shaEl) shaEl.textContent = '刷新后需点击授权';
  safeUpdateUploadButtonLabel();
}

/**
 * 页面刷新后：恢复升级目标 + 上次 FSAPI 固件。
 * 权限仍 granted → 静默恢复；否则保留 handle，等用户点击上传区再授权。
 */
export async function restorePersistedFirmwareSelection() {
  var target = loadPersistedFlashTarget();
  if (target) {
    window.flashTarget = target;
  }

  var chips = ['esp32', 'gd32'];
  for (var i = 0; i < chips.length; i++) {
    var chip = chips[i];
    try {
      var handle = await idbGetHandle(chip);
      if (!handle) continue;
      var meta = loadFileMeta(chip);
      var perm = await queryHandlePermission(handle);
      if (perm === 'granted') {
        var file = await handle.getFile();
        await applyRestoredFile(chip, handle, file, { silent: true, skipHandlePersist: true });
        window._pendingFwHandles[chip] = null;
        addLog('I', '文件', chip.toUpperCase() + ' 已恢复刷新前的固件: ' + file.name);
        continue;
      }
      /* prompt/denied：不在此处 requestPermission（无用户手势会失败） */
      showPendingReauthUi(chip, handle, meta);
      addLog('I', '文件',
        chip.toUpperCase() + ' 上次固件「' +
        ((meta && meta.name) || handle.name || '未知') +
        '」需点击上传区重新授权（浏览器安全限制）');
    } catch (e) {
      addLog('W', '文件', chip.toUpperCase() + ' 恢复上次固件失败: ' + (e && e.message ? e.message : e));
    }
  }

  if (typeof window.applySharedModeUi === 'function') {
    window.applySharedModeUi();
  }
  if (typeof window.syncBleFileFromSelectedTarget === 'function') {
    window.syncBleFileFromSelectedTarget();
  }
  safeUpdateUploadButtonLabel();
}

export function handleFileSelect(event, chip) {
  var file = event.target.files[0];
  if (!file) return Promise.resolve();
  /* 传统 input 无 handle：清掉旧 FSAPI 句柄，避免热重载读到过期路径 */
  if (chip === 'esp32') window.esp32FileHandle = null;
  else window.gd32FileHandle = null;
  window._pendingFwHandles[chip] = null;
  persistFileHandle(chip, null);
  return processFile(file, chip);
}

// 通过 File System Access API 选择文件（支持热重载）
export async function pickFileViaFSAPI(chip) {
  /*
   * 仅在「刷新后待授权」或「尚无已加载 File」时尝试一键恢复。
   * 已有可用文件时再点上传区 → 打开选文件对话框（可换文件）。
   */
  var pending = window._pendingFwHandles[chip];
  var hasLoadedFile = !!(chip === 'esp32' ? window.esp32File : window.gd32File);
  if (pending || !hasLoadedFile) {
    var resumed = await resumePersistedFirmwareWithGesture(chip);
    if (resumed) {
      return true;
    }
  }

  if (!window.showOpenFilePicker) {
    addLog('W', '文件', '当前浏览器不支持 File System Access API，请手动重新选择文件');
    return false;
  }
  try {
    /* GD32 常为 .hex：同时声明 octet-stream / text，避免对话框里灰掉选不了 */
    var acceptMap = chip === 'esp32'
      ? { 'application/octet-stream': ['.bin'] }
      : {
          'application/octet-stream': ['.bin', '.hex'],
          'text/plain': ['.hex'],
          'application/x-intel-hex': ['.hex']
        };
    var prevHandle = chip === 'esp32' ? window.esp32FileHandle : window.gd32FileHandle;
    if (!prevHandle) {
      prevHandle = window._pendingFwHandles[chip] || null;
    }
    var opts = {
      /* 记住该 id 上次目录；再配合 startIn 定位到刷新前的文件 */
      id: chip === 'esp32' ? 'dcu-fw-esp32' : 'dcu-fw-gd32',
      types: [{ description: '固件文件', accept: acceptMap }],
      multiple: false
    };
    if (prevHandle) {
      opts.startIn = prevHandle;
    }
    var handles;
    try {
      handles = await window.showOpenFilePicker(opts);
    } catch (pickErr) {
      /* 失效 handle 作 startIn 时部分浏览器直接抛错，去掉后重试 */
      if (opts.startIn && pickErr && pickErr.name !== 'AbortError') {
        delete opts.startIn;
        handles = await window.showOpenFilePicker(opts);
      } else {
        throw pickErr;
      }
    }
    if (handles && handles.length > 0) {
      var handle = handles[0];
      var file = await handle.getFile();
      if (chip === 'esp32') window.esp32FileHandle = handle;
      else window.gd32FileHandle = handle;
      window._pendingFwHandles[chip] = null;
      await processFile(file, chip);
      persistFileHandle(chip, handle);
      persistFileMeta(chip, file);
      persistFlashTarget(chip);
      addLog('I', '文件', '已通过 FSAPI 选择文件（支持热重载）: ' + file.name);
      return true;
    }
  } catch (e) {
    if (e && e.name !== 'AbortError') {
      addLog('W', '文件', 'FSAPI 选择失败: ' + (e.message || e));
    }
  }
  return false;
}

export function setupDropZone(zoneId, chip) {
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

// ==================== Intel HEX → BIN 转换 ====================
/**
 * 将 Intel HEX 字符串解析为原始 BIN 字节数组。
 *
 * 支持记录类型: 00(数据), 01(EOF), 02(扩展段地址), 04(扩展线性地址)。
 * 地址间隙填充 0xFF（Flash 擦除态）。
 *
 * @param {string} hexText  Intel HEX 文件的完整文本内容。
 * @returns {{ data: Uint8Array, baseAddr: number, endAddr: number }}
 */
export function parseIntelHex(hexText) {
  var baseAddr = 0;     // 当前扩展地址（uint32）
  var segments = [];     // { offset, data } 列表
  var minAddr = 0xFFFFFFFF;
  var maxAddr = 0;
  var lines = hexText.split(/\r?\n/);
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].trim();
    if (!line || line[0] !== ':') continue;
    // 解析行: :LLAAAATT[DD...]CC
    var byteCount = parseInt(line.slice(1, 3), 16);
    var addr16   = parseInt(line.slice(3, 7), 16);
    var recType  = parseInt(line.slice(7, 9), 16);
    // 校验和验证
    var sum = 0;
    for (var j = 1; j < line.length; j += 2) {
      sum += parseInt(line.slice(j, j + 2), 16);
    }
    if ((sum & 0xFF) !== 0) {
      throw new Error('HEX 行 ' + (i + 1) + ' 校验和错误: ' + line);
    }
    if (recType === 0x01) {
      // EOF
      break;
    } else if (recType === 0x04) {
      // 扩展线性地址 (upper 16 bits)，>>>0 归整为 uint32
      baseAddr = (parseInt(line.slice(9, 13), 16) << 16) >>> 0;
    } else if (recType === 0x02) {
      // 扩展段地址：seg << 4，不加掩码（避免截断 0x10FFEF 等合法地址）
      baseAddr = (parseInt(line.slice(9, 13), 16) << 4) >>> 0;
    } else if (recType === 0x00) {
      // 数据记录
      var absAddr = (baseAddr + addr16) >>> 0;
      var data = new Uint8Array(byteCount);
      for (var k = 0; k < byteCount; k++) {
        data[k] = parseInt(line.slice(9 + k * 2, 11 + k * 2), 16);
      }
      segments.push({ offset: absAddr, data: data });
      if (absAddr < minAddr) minAddr = absAddr;
      var endAddr = absAddr + byteCount;
      if (endAddr > maxAddr) maxAddr = endAddr;
    }
    // 其他记录类型 (03, 05 等) 忽略
  }
  if (segments.length === 0) {
    throw new Error('HEX 文件中没有有效数据记录');
  }
  // 合并为连续 bin，间隙填 0xFF
  var totalSize = maxAddr - minAddr;
  var bin = new Uint8Array(totalSize);
  // 先填充 0xFF
  for (var m = 0; m < totalSize; m++) bin[m] = 0xFF;
  for (var s = 0; s < segments.length; s++) {
    var seg = segments[s];
    var off = seg.offset - minAddr;
    bin.set(seg.data, off);
  }
  return { data: bin, baseAddr: minAddr, endAddr: maxAddr };
}

/**
 * 检测文件内容是否为 Intel HEX 格式。
 * 看前几个非空行是否以 ':' 开头。
 */
export function isIntelHexText(text) {
  var lines = text.split(/\r?\n/);
  for (var i = 0; i < Math.min(lines.length, 5); i++) {
    var line = lines[i].trim();
    if (!line) continue;
    return line[0] === ':';
  }
  return false;
}

/**
 * 公共 HEX→BIN 转换：如果文件是 .hex 格式，自动解析为 bin。
 * BLE / WiFi 共用此函数，避免转换逻辑重复。
 * @param {File} file 原始文件
 * @returns {Promise<{file: File, converted: boolean, baseAddr: number|null, endAddr: number|null}>}
 */
export async function convertHexFileToBin(file) {
  if (!file || !file.name.toLowerCase().endsWith('.hex')) {
    return { file: file, converted: false, baseAddr: null, endAddr: null };
  }
  var text = await file.text();
  if (!isIntelHexText(text)) {
    return { file: file, converted: false, baseAddr: null, endAddr: null };
  }
  var result = parseIntelHex(text);
  var binName = file.name.replace(/\.hex$/i, '.bin');
  var binBlob = new Blob([result.data], { type: 'application/octet-stream' });
  var binFile = new File([binBlob], binName, { type: 'application/octet-stream', lastModified: file.lastModified });
  return { file: binFile, converted: true, baseAddr: result.baseAddr, endAddr: result.endAddr };
}

// ==================== SHA-256：ESP32 / GD32 各一份，互不共用、不互切 ====================
function paintShaUi(chip, hashHex, statusText) {
  var t = statusText != null ? statusText : (hashHex || '-');
  if (chip === 'esp32') {
    var e = document.getElementById('fileSha256Esp32');
    if (e) e.textContent = t;
  } else if (chip === 'gd32') {
    var g = document.getElementById('fileSha256Gd32');
    if (g) g.textContent = t;
  }
  /* BLE 面板只显示当前目标槽，避免算 ESP32 时盖掉 GD32 的展示 */
  var cur = (window.flashTarget === 'gd32') ? 'gd32' : 'esp32';
  if (chip === cur) {
    var ble = document.getElementById('fileSha256');
    if (ble) ble.textContent = t;
  }
}

/**
 * 选文件时算一次，写入对应芯片槽；上传时直接读槽。
 * 不做跨芯片复用、不与另一路互切。
 */
export function computeFileSha(file, chip, optArrayBuffer) {
  if (!file || (chip !== 'esp32' && chip !== 'gd32')) {
    return Promise.resolve(null);
  }
  paintShaUi(chip, null, '计算中...');
  var bufPromise = optArrayBuffer ? Promise.resolve(optArrayBuffer) : file.arrayBuffer();
  return bufPromise.then(function (buf) {
    return crypto.subtle.digest('SHA-256', buf);
  }).then(function (hashBuffer) {
    var hashHex = bytesToHex(new Uint8Array(hashBuffer)).toUpperCase();
    if (chip === 'esp32') window._esp32FileSha256 = hashHex;
    else window._gd32FileSha256 = hashHex;
    paintShaUi(chip, hashHex);
    addLog('I', '文件', chip.toUpperCase() + ' 固件 SHA-256: ' + hashHex);
    return hashHex;
  }).catch(function (err) {
    paintShaUi(chip, null, '计算失败: ' + err.message);
    if (chip === 'esp32') window._esp32FileSha256 = null;
    else window._gd32FileSha256 = null;
    addLog('W', '文件', chip.toUpperCase() + ' SHA-256 计算失败: ' + err.message);
    return null;
  });
}

// ==================== 文件处理 ====================
/**
 * @description 处理用户选择的固件文件（HEX→BIN、算 SHA 写入对应芯片槽）。
 * @param {File} file
 * @param {'esp32'|'gd32'} chip
 * @param {{silent?: boolean, skipHandlePersist?: boolean}=} options
 * @returns {Promise<void>}
 */
export function processFile(file, chip, options) {
  var opts = options || {};
  var silent = !!opts.silent;
  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  var isHexByName = file.name.toLowerCase().endsWith('.hex');
  if (!opts.skipHandlePersist) {
    persistFlashTarget(chip);
  }
  if (file && file.name) {
    persistFileMeta(chip, file);
  }

  function paintFileInfo(name, type, sizeText) {
    var info = document.getElementById('fileInfo' + suffix);
    var nameEl = document.getElementById('fileName' + suffix);
    var typeEl = document.getElementById('fileType' + suffix);
    var sizeEl = document.getElementById('fileSize' + suffix);
    if (info) info.classList.add('show');
    if (nameEl) nameEl.textContent = name;
    if (typeEl) typeEl.textContent = type;
    if (sizeEl) sizeEl.textContent = sizeText;
  }

  if (chip === 'gd32' && isHexByName) {
    return convertHexFileToBin(file).then(function (result) {
      if (!result.converted) {
        addLog('W', '文件', file.name + ' 扩展名为 .hex 但内容不像 HEX 格式，按原始文件处理');
        window.gd32File = file;
        paintFileInfo(file.name, 'Binary(?)', formatBytes(file.size));
        safeUpdateUploadButtonLabel();
        return computeFileSha(file, 'gd32');
      }
      window.gd32File = result.file;
      var baseHex = '0x' + result.baseAddr.toString(16).toUpperCase().padStart(8, '0');
      addLog('I', '文件', 'HEX → BIN 转换完成: ' + file.name + ' → ' + result.file.name);
      addLog('I', '文件', '基地址: ' + baseHex + ', 数据大小: ' + formatBytes(result.file.size));
      if (!silent) showToast('HEX 已自动转换为 BIN', 'success');
      paintFileInfo(
        result.file.name + ' (由 ' + file.name + ' 转换)',
        'Binary (HEX转换)',
        formatBytes(result.file.size)
      );
      safeUpdateUploadButtonLabel();
      return computeFileSha(result.file, 'gd32');
    }).catch(function (err) {
      addLog('E', '文件', 'HEX 解析失败: ' + (err && err.message ? err.message : err));
      if (!silent) showToast('HEX 文件解析失败: ' + (err && err.message ? err.message : err), 'error');
      window.gd32File = null;
      window._gd32FileSha256 = null;
      var info = document.getElementById('fileInfo' + suffix);
      if (info) info.classList.remove('show');
      safeUpdateUploadButtonLabel();
      throw err;
    });
  }

  if (chip === 'esp32') window.esp32File = file;
  else window.gd32File = file;
  paintFileInfo(
    file.name,
    isHexByName ? 'Intel HEX' : 'Binary',
    formatBytes(file.size)
  );
  safeUpdateUploadButtonLabel();
  if (silent) {
    addLog('I', '文件', chip.toUpperCase() + ' 已恢复: ' + file.name + ' (' + formatBytes(file.size) + ')');
  } else {
    addLog('I', '文件', chip.toUpperCase() + ' 已选择: ' + file.name + ' (' + formatBytes(file.size) + ')');
    showToast(chip.toUpperCase() + ' 文件已选择', 'success');
  }
  return Promise.resolve(computeFileSha(file, chip));
}

/** @description 清除已选固件文件并重置对应 UI 状态。 */
export function clearFile(chip) {
  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  if (chip === 'esp32') {
    window.esp32File = null;
    window.esp32FileHandle = null;
    window._esp32FileSha256 = null;
  } else {
    window.gd32File = null;
    window.gd32FileHandle = null;
    window._gd32FileSha256 = null;
  }
  window._pendingFwHandles[chip] = null;
  persistFileHandle(chip, null);
  persistFileMeta(chip, null);
  document.getElementById('fileInput' + suffix).value = '';
  document.getElementById('fileInfo' + suffix).classList.remove('show');
  var shaEl = document.getElementById('fileSha256' + suffix);
  if (shaEl) shaEl.textContent = '-';
  safeUpdateUploadButtonLabel();
  addLog('I', '文件', chip.toUpperCase() + ' 文件已清除');
}