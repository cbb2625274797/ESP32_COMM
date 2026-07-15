/**
 * file-handle.js - 文件处理 + Intel HEX → BIN 转换
 *
 * 从 app.js 的「文件处理」「Intel HEX → BIN 转换」「文件处理」段提取。
 * 包含：HEX 解析、文件选择、FSAPI、SHA-256 计算、文件信息 UI、清除。
 *
 * 共享状态（通过 window.* 引用）：
 *   window.esp32File / window.gd32File / window.esp32FileHandle / window.gd32FileHandle
 *   window._esp32FileSha256 / window._gd32FileSha256
 *
 * 未拆分的函数（通过 window.* 引用）：
 *   window.updateUploadButtonLabel / window.showToast / window.addLog
 */
import { formatBytes } from '../common/utils.js';
import { showToast } from '../common/toast.js';
import { addLog } from '../common/log.js';

// ==================== 文件处理 ====================
// 文件状态（模块级，但被 upload-esp32/upload-gd32 通过 window.* 读写）
window.esp32File = null;
window.gd32File = null;
window.esp32FileHandle = null;  // FileSystemFileHandle for hot reload
window.gd32FileHandle = null;   // FileSystemFileHandle for hot reload

export function handleFileSelect(event, chip) {
  var file = event.target.files[0];
  if (file) processFile(file, chip);
}

// 通过 File System Access API 选择文件（支持热重载）
export async function pickFileViaFSAPI(chip) {
  if (!window.showOpenFilePicker) {
    addLog('W', '文件', '当前浏览器不支持 File System Access API，请手动重新选择文件');
    return false;
  }
  try {
    var acceptMap = chip === 'esp32'
      ? { 'application/octet-stream': ['.bin'] }
      : { 'application/octet-stream': ['.bin', '.hex'] };
    var handles = await window.showOpenFilePicker({
      types: [{ description: '固件文件', accept: acceptMap }],
      multiple: false
    });
    if (handles && handles.length > 0) {
      var handle = handles[0];
      var file = await handle.getFile();
      if (chip === 'esp32') window.esp32FileHandle = handle;
      else window.gd32FileHandle = handle;
      processFile(file, chip);
      addLog('I', '文件', '已通过 FSAPI 选择文件（支持热重载）: ' + file.name);
      return true;
    }
  } catch (e) {
    if (e.name !== 'AbortError') {
      addLog('W', '文件', 'FSAPI 选择失败: ' + e.message);
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
  var baseAddr = 0;     // 当前扩展地址
  var segments = [];     // { offset, data } 列表
  var minAddr = 0xFFFFFFFF;
  var maxAddr = 0;
  var lines = hexText.split(/\r?\n/);
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].trim();
    if (!line || line[0] !== ':') continue;
    // 解析行: :LLAAAATT[DD...]CC
    var byteCount = parseInt(line.substr(1, 2), 16);
    var addr16   = parseInt(line.substr(3, 4), 16);
    var recType  = parseInt(line.substr(7, 2), 16);
    // 校验和验证
    var sum = 0;
    for (var j = 1; j < line.length; j += 2) {
      sum += parseInt(line.substr(j, 2), 16);
    }
    if ((sum & 0xFF) !== 0) {
      throw new Error('HEX 行 ' + (i + 1) + ' 校验和错误: ' + line);
    }
    if (recType === 0x01) {
      // EOF
      break;
    } else if (recType === 0x04) {
      // 扩展线性地址 (upper 16 bits)
      baseAddr = parseInt(line.substr(9, 4), 16) << 16;
    } else if (recType === 0x02) {
      // 扩展段地址
      baseAddr = (parseInt(line.substr(9, 4), 16) << 4) & 0xFFFFF;
    } else if (recType === 0x00) {
      // 数据记录
      var absAddr = baseAddr + addr16;
      var data = new Uint8Array(byteCount);
      for (var k = 0; k < byteCount; k++) {
        data[k] = parseInt(line.substr(9 + k * 2, 2), 16);
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
 * @returns {Promise<{file: File, converted: boolean, baseAddr: number|null}>}
 */
export async function convertHexFileToBin(file) {
  if (!file || !file.name.toLowerCase().endsWith('.hex')) {
    return { file: file, converted: false, baseAddr: null };
  }
  var text = await file.text();
  if (!isIntelHexText(text)) {
    return { file: file, converted: false, baseAddr: null };
  }
  var result = parseIntelHex(text);
  var binName = file.name.replace(/\.hex$/i, '.bin');
  var binBlob = new Blob([result.data], { type: 'application/octet-stream' });
  var binFile = new File([binBlob], binName, { type: 'application/octet-stream', lastModified: file.lastModified });
  return { file: binFile, converted: true, baseAddr: result.baseAddr, endAddr: result.endAddr };
}

// ==================== 文件处理 ====================
/** @description 处理用户选择的固件文件（HEX→BIN 转换、SHA-256 计算、文件校验）。 */
export function processFile(file, chip) {
  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  var isHexByName = file.name.toLowerCase().endsWith('.hex');
  if (chip === 'gd32' && isHexByName) {
    // GD32 固件: .hex 文件自动转换为 .bin，然后替换文件对象
    convertHexFileToBin(file).then(function(result) {
      if (!result.converted) {
        // 文件名 .hex 但内容不像，直接用原始文件
        addLog('W', '文件', file.name + ' 扩展名为 .hex 但内容不像 HEX 格式，按原始文件处理');
        window.gd32File = file;
        document.getElementById('fileInfo' + suffix).classList.add('show');
        document.getElementById('fileName' + suffix).textContent = file.name;
        document.getElementById('fileType' + suffix).textContent = 'Binary(?)';
        document.getElementById('fileSize' + suffix).textContent = formatBytes(file.size);
        window.updateUploadButtonLabel();
        return;
      }
      window.gd32File = result.file;
      var baseHex = '0x' + result.baseAddr.toString(16).toUpperCase().padStart(8, '0');
      addLog('I', '文件', 'HEX → BIN 转换完成: ' + file.name + ' → ' + result.file.name);
      addLog('I', '文件', '基地址: ' + baseHex + ', 数据大小: ' + formatBytes(result.file.size));
      showToast('HEX 已自动转换为 BIN', 'success');
      document.getElementById('fileInfo' + suffix).classList.add('show');
      document.getElementById('fileName' + suffix).textContent = result.file.name + ' (由 ' + file.name + ' 转换)';
      document.getElementById('fileType' + suffix).textContent = 'Binary (HEX转换)';
      document.getElementById('fileSize' + suffix).textContent = formatBytes(result.file.size);
      window.updateUploadButtonLabel();
    }).catch(function(err) {
      addLog('E', '文件', 'HEX 解析失败: ' + err.message);
      showToast('HEX 文件解析失败: ' + err.message, 'error');
      window.gd32File = null;
      document.getElementById('fileInfo' + suffix).classList.remove('show');
      window.updateUploadButtonLabel();
    });
    return; // 异步，回调里设置 gd32File
  }
  // .bin 文件或 ESP32 文件: 直接使用
  if (chip === 'esp32') window.esp32File = file;
  else window.gd32File = file;
  document.getElementById('fileInfo' + suffix).classList.add('show');
  document.getElementById('fileName' + suffix).textContent = file.name;
  document.getElementById('fileType' + suffix).textContent =
    isHexByName ? 'Intel HEX' : 'Binary';
  document.getElementById('fileSize' + suffix).textContent = formatBytes(file.size);
  window.updateUploadButtonLabel();
  addLog('I', '文件', chip.toUpperCase() + ' 已选择: ' + file.name + ' (' + formatBytes(file.size) + ')');
  showToast(chip.toUpperCase() + ' 文件已选择', 'success');
  // ESP32 文件: 计算并显示 SHA-256
  if (chip === 'esp32') {
    var shaElem = document.getElementById('fileSha256' + suffix);
    if (shaElem) {
      shaElem.textContent = '计算中...';
      file.arrayBuffer().then(function(buf) {
        return crypto.subtle.digest('SHA-256', buf);
      }).then(function(hashBuffer) {
        var hashArray = Array.from(new Uint8Array(hashBuffer));
        var hashHex = hashArray.map(function(b) { return b.toString(16).padStart(2, '0'); }).join('');
        shaElem.textContent = hashHex.toUpperCase();
        // 存储到全局变量供上传时使用
        window._esp32FileSha256 = hashHex.toUpperCase();
        addLog('I', '文件', 'ESP32 固件 SHA-256: ' + hashHex.toUpperCase());
      }).catch(function(err) {
        shaElem.textContent = '计算失败: ' + err.message;
      });
    }
  }
}

/** @description 清除已选固件文件并重置对应 UI 状态。 */
export function clearFile(chip) {
  var suffix = chip === 'esp32' ? 'Esp32' : 'Gd32';
  if (chip === 'esp32') { window.esp32File = null; window.esp32FileHandle = null; window._esp32FileSha256 = null; }
  else { window.gd32File = null; window.gd32FileHandle = null; }
  document.getElementById('fileInput' + suffix).value = '';
  document.getElementById('fileInfo' + suffix).classList.remove('show');
  var shaElem = document.getElementById('fileSha256' + suffix);
  if (shaElem) shaElem.textContent = '-';
  window.updateUploadButtonLabel();
  addLog('I', '文件', chip.toUpperCase() + ' 文件已清除');
}