/**
 * console-mode.js - 共享模式/目标切换胶水代码（从 index.html 内联脚本抽取）
 *
 * 职责：
 * - 覆盖 app.js 中的 switchStageFlow，支持 BLE 阶段流程
 * - 提供 setConsoleMode / selectFlashTarget 等供 HTML onclick 调用
 * - 维护共享固件文件状态 (sharedFirmwareFiles) 并在 WiFi/BLE 间同步
 * - 页面刷新后恢复上次升级目标与固件选择
 *
 * 依赖：app.js (通过 window.* 或 bare 引用) 提供 switchStageFlow、
 * handleFileSelect、clearFile、pickFileViaFSAPI、addLog、showToast、setActivePhase 等。
 */
import {
  restorePersistedFirmwareSelection,
  persistFlashTarget,
  loadPersistedFlashTarget
} from './file-handle.js';

window.connectionMode = window.connectionMode || 'wifi';
/* 刷新后优先用上次目标，避免默认回到 ESP32 丢掉 GD32 选择上下文 */
window.flashTarget = loadPersistedFlashTarget() || window.flashTarget || 'esp32';
window.sharedFirmwareFiles = window.sharedFirmwareFiles || {
  esp32: { file: null, handle: null },
  gd32: { file: null, handle: null }
};
(function () {
  var originalSwitchStageFlow = window.switchStageFlow;
  window.switchStageFlow = function (target) {
    if (target === 'ble') {
      window.currentOtaTarget = 'ble';
      var esp32El = document.getElementById('esp32Stages');
      var gd32El = document.getElementById('gd32Stages');
      var bleEl = document.getElementById('bleStages');
      if (esp32El) esp32El.style.display = 'none';
      if (gd32El) gd32El.style.display = 'none';
      if (bleEl) bleEl.style.display = '';
      return;
    }
    if (typeof originalSwitchStageFlow === 'function') {
      originalSwitchStageFlow(target);
    }
    var bleEl = document.getElementById('bleStages');
    if (bleEl) bleEl.style.display = 'none';
  };

  window.setConsoleMode = function (mode) {
      // ★ 刷写进行中禁止切换连接模式：setConsoleMode → syncBleFileFromSelectedTarget →
      //   notifyFileChange → updateFileInfo → finalizeFileInfo → setProgress(0) 会覆盖
      //   进行中的 WiFi/BLE 上传进度（与 selectFlashTarget 同样的守卫策略）。
      var bleBusy = window.bleOta && window.bleOta.isUploading && window.bleOta.isUploading();
      if (window.isUploading || bleBusy) {
        var active = bleBusy ? 'BLE' : ((window.activeUploadChip || 'OTA').toUpperCase());
        if (typeof addLog === 'function') addLog('W', 'OTA', active + ' 刷写进行中，无法切换连接模式');
        if (typeof showToast === 'function') showToast('当前有 ' + active + ' 任务在运行，请等待结束后再切换', 'error');
        return;
      }
    var isBle = mode === 'ble';
    window.connectionMode = isBle ? 'ble' : 'wifi';
    var wifiCards = document.getElementById('wifiModeCards');
    var bleCards = document.getElementById('bleModeCards');
    var wifiBtn = document.getElementById('modeWifiBtn');
    var bleBtn = document.getElementById('modeBleBtn');
    if (wifiCards) wifiCards.style.display = isBle ? 'none' : '';
    if (bleCards) bleCards.style.display = isBle ? '' : 'none';
    var headBtns = document.getElementById('bleHeadActions');
    if (headBtns) headBtns.style.display = isBle ? '' : 'none';
    if (wifiBtn) wifiBtn.className = isBle ? 'btn btn-outline' : 'btn btn-primary';
    if (bleBtn) bleBtn.className = isBle ? 'btn btn-primary' : 'btn btn-outline';
    applySharedModeUi();
    syncBleFileFromSelectedTarget();
    if (isBle) {
      window.switchStageFlow('ble');
      if (typeof setActivePhase === 'function') {
        setActivePhase(window.flashTarget === 'gd32' ? 'gd32Ota' : 'esp32Ota');
      }
      if (window.bleOta && typeof window.bleOta.refreshButtons === 'function') {
        window.bleOta.refreshButtons();
      }
    } else {
      window.switchStageFlow(window.flashTarget || (window.activePhase === 'esp32Ota' ? 'esp32' : 'gd32'));
    }
  };

  window.selectFlashTarget = function (target) {
      // OTA 上传进行中禁止切换目标：避免前端状态与后端 relay/upload 不同步导致卡死
      // ★ 兼容 BLE 模式：BLE 上传用 bleOta.state.uploading，window.isUploading 仅 WiFi 通道使用。
      var bleBusy = window.bleOta && window.bleOta.isUploading && window.bleOta.isUploading();
      if (window.isUploading || bleBusy) {
        var active = bleBusy ? 'BLE' : ((window.activeUploadChip || 'OTA').toUpperCase());
        if (typeof addLog === 'function') addLog('W', 'OTA', active + ' 刷写进行中，无法切换目标');
        if (typeof showToast === 'function') showToast('当前有 ' + active + ' 任务在运行，请等待结束后再切换', 'error');
        return;
      }
    window.flashTarget = target === 'gd32' ? 'gd32' : 'esp32';
    persistFlashTarget(window.flashTarget);
    applySharedModeUi();
    /* 与 WiFi 一致：两路文件独立；切换只换「当前目标」面板 + 绑定该路文件到会话 */
    if (window.connectionMode === 'ble') {
      window.switchStageFlow('ble');
      if (typeof setActivePhase === 'function') {
        setActivePhase(window.flashTarget === 'gd32' ? 'gd32Ota' : 'esp32Ota');
      }
      if (window.bleOta && typeof window.bleOta.refreshTargetUi === 'function') {
        window.bleOta.refreshTargetUi();
      }
      /* 把当前目标对应的 esp32/gd32 固件绑到 BLE 会话（无文件则清空，避免误刷另一芯片） */
      syncBleFileFromSelectedTarget();
    } else {
      window.switchStageFlow(window.flashTarget);
    }
  };

  window.handleSharedFileSelect = function (event, chip) {
    window.flashTarget = chip === 'gd32' ? 'gd32' : 'esp32';
    persistFlashTarget(window.flashTarget);
    applySharedModeUi();
    /* 等 processFile（含 HEX→BIN）完成后再写入 shared 并 sync */
    var done = (typeof window.handleFileSelect === 'function')
      ? Promise.resolve(window.handleFileSelect(event, chip))
      : Promise.resolve();
    done.then(function () {
      var prepared = chip === 'gd32' ? window.gd32File : window.esp32File;
      if (prepared) {
        window.sharedFirmwareFiles[chip] = { file: prepared, handle: null };
      }
      syncBleFileFromSelectedTarget();
    }).catch(function (err) {
      if (typeof window.addLog === 'function') {
        window.addLog('W', '文件', chip.toUpperCase() + ' 选择失败: ' + (err && err.message ? err.message : err));
      }
    });
  };

  window.pickSharedFirmware = function (chip) {
    window.flashTarget = chip === 'gd32' ? 'gd32' : 'esp32';
    persistFlashTarget(window.flashTarget);
    applySharedModeUi();
    if (window.showOpenFilePicker && typeof window.pickFileViaFSAPI === 'function') {
      var nativePicker = window.showOpenFilePicker;
      var capturedHandle = null;
      window.showOpenFilePicker = function () {
        return nativePicker.apply(this, arguments).then(function (handles) {
          capturedHandle = handles && handles.length ? handles[0] : null;
          return handles;
        });
      };
      Promise.resolve(window.pickFileViaFSAPI(chip)).then(function () {
        /* 用 processFile 后的结果（GD32 HEX→BIN 后的 prepared），不要再塞回原始 .hex */
        var prepared = chip === 'gd32' ? window.gd32File : window.esp32File;
        var handle = (chip === 'gd32' ? window.gd32FileHandle : window.esp32FileHandle) || capturedHandle;
        if (prepared) {
          window.sharedFirmwareFiles[chip] = { file: prepared, handle: handle || null };
        } else if (capturedHandle) {
          window.sharedFirmwareFiles[chip] = {
            file: window.sharedFirmwareFiles[chip] && window.sharedFirmwareFiles[chip].file,
            handle: capturedHandle
          };
        }
        syncBleFileFromSelectedTarget();
      }).catch(function (err) {
        if (err && err.name !== 'AbortError' && typeof window.addLog === 'function') {
          window.addLog('W', '文件', 'FSAPI 选择失败: ' + (err.message || err));
        }
      }).then(function () {
        /* 无论成败都还原原生 picker，避免二次选择失效 */
        window.showOpenFilePicker = nativePicker;
      });
    } else {
      var input = document.getElementById(chip === 'gd32' ? 'fileInputGd32' : 'fileInputEsp32');
      if (input) input.click();
    }
  };

  window.clearSharedFirmware = function (chip) {
    window.sharedFirmwareFiles[chip] = { file: null, handle: null };
    if (typeof window.clearFile === 'function') {
      window.clearFile(chip);
    }
    if (window.connectionMode === 'ble' && window.flashTarget === chip && window.bleOta && typeof window.bleOta.clearFile === 'function') {
      window.bleOta.clearFile();
    }
  };

  window.setBleFirmwareInputFile = function (file) {
    var input = document.getElementById('firmwareFile');
    if (!input || !file || typeof window.DataTransfer !== 'function') {
      return;
    }
    try {
      var dt = new DataTransfer();
      dt.items.add(file);
      input.files = dt.files;
    } catch (err) {
      // Some browsers restrict assignment to file inputs; BLE state is still synced via notifyFileChange.
    }
  };

  /**
   * 按 flashTarget 把「该芯片一路」固件绑到 BLE 会话（复刻 WiFi：两路独立、切换不串文件）。
   * 优先 window.esp32File/gd32File（含 HEX→BIN 结果），其次 sharedFirmwareFiles / input。
   * 当前目标无文件时清空 BLE 会话，禁止继续显示/上传另一芯片固件。
   */
  window.syncBleFileFromSelectedTarget = function () {
    if (window.connectionMode !== 'ble' || !window.bleOta) {
      return;
    }
    if (window.bleOta.isUploading && window.bleOta.isUploading()) {
      return;
    }
    var chip = window.flashTarget === 'gd32' ? 'gd32' : 'esp32';
    var file = chip === 'gd32' ? window.gd32File : window.esp32File;
    var handle = chip === 'gd32' ? window.gd32FileHandle : window.esp32FileHandle;
    var stored = window.sharedFirmwareFiles && window.sharedFirmwareFiles[chip]
      ? window.sharedFirmwareFiles[chip]
      : null;
    if (!handle && stored && stored.handle) handle = stored.handle;
    if (!file && stored && stored.file) file = stored.file;

    function bindBle(f, h) {
      setBleFirmwareInputFile(f);
      if (typeof window.bleOta.notifyFileChange === 'function') {
        window.bleOta.notifyFileChange(f, h || null);
      }
      /* 会话 SHA 与对应芯片槽对齐（不跨芯片） */
      var slot = chip === 'gd32' ? window._gd32FileSha256 : window._esp32FileSha256;
      if (window.bleOta.state) {
        if (slot) window.bleOta.state.fileSha256 = slot;
      }
    }
    function clearBleSession() {
      if (typeof window.bleOta.clearFile === 'function') {
        window.bleOta.clearFile();
      } else if (typeof window.bleOta.notifyFileChange === 'function') {
        window.bleOta.notifyFileChange(null, null);
      }
    }

    if (file) {
      bindBle(file, handle);
      return;
    }
    if (handle && typeof handle.getFile === 'function') {
      handle.getFile().then(function (fresh) {
        if (chip === 'gd32' && fresh.name.toLowerCase().endsWith('.hex') &&
            typeof window.processFile === 'function') {
          return Promise.resolve(window.processFile(fresh, 'gd32', {
            silent: true,
            skipHandlePersist: true
          })).then(function () {
            if (window.gd32File) bindBle(window.gd32File, handle);
            else bindBle(fresh, handle);
          });
        }
        if (chip === 'esp32') window.esp32File = fresh;
        else window.gd32File = fresh;
        if (stored) stored.file = chip === 'gd32' ? window.gd32File : window.esp32File;
        bindBle(chip === 'gd32' ? window.gd32File : window.esp32File, handle);
      }).catch(function () {
        if (stored && stored.file) bindBle(stored.file, handle);
        else clearBleSession();
      });
      return;
    }
    var suffix = chip === 'gd32' ? 'Gd32' : 'Esp32';
    var input = document.getElementById('fileInput' + suffix);
    var inputFile = input && input.files && input.files[0] ? input.files[0] : null;
    if (inputFile) {
      if (window.sharedFirmwareFiles) {
        window.sharedFirmwareFiles[chip] = { file: inputFile, handle: null };
      }
      bindBle(inputFile, null);
      return;
    }
    clearBleSession();
  };

  window.applySharedModeUi = function () {
    var isBle = window.connectionMode === 'ble';
    var isGd32 = window.flashTarget === 'gd32';
    var targetEsp32 = document.getElementById('targetEsp32Tab');
    var targetGd32 = document.getElementById('targetGd32Tab');
    if (targetEsp32) targetEsp32.className = isGd32 ? 'btn btn-outline' : 'btn btn-primary';
    if (targetGd32) targetGd32.className = isGd32 ? 'btn btn-primary' : 'btn btn-outline';
    var ids = {
      wifiEsp32UploadPane: !isGd32,
      wifiGd32UploadPane: isGd32,
      bleUploadPane: false,
      wifiEsp32ActionPane: !isBle && !isGd32,
      wifiGd32ActionPane: !isBle && isGd32,
      bleActionPane: isBle
    };
    Object.keys(ids).forEach(function (id) {
      var el = document.getElementById(id);
      if (el) el.style.display = ids[id] ? '' : 'none';
    });
    var hint = document.getElementById('readyHint');
    var autoBtn = document.getElementById('autoFlashBtn');
    if (autoBtn && isBle) {
      autoBtn.textContent = isGd32 ? '一键刷写 GD32' : '一键刷写 ESP32';
    }
    if (hint) {
      if (isBle) {
        hint.innerHTML = isGd32
          ? 'BLE 模式 · 目标 GD32：连接设备 → 选择 GD32 固件 → 一键刷写。'
          : 'BLE 模式 · 目标 ESP32：连接设备 → 选择 ESP32 固件 → 一键刷写。';
      } else if (isGd32) {
        hint.innerHTML = 'WiFi 模式：上传 GD32 固件后自动执行 SPI OTA。';
      } else {
        hint.innerHTML = 'WiFi 模式：上传 ESP32 固件后自动执行 OTA 并重启。';
      }
    }
  };

  applySharedModeUi();
  /* 刷新后恢复上次固件与目标（FSAPI handle + localStorage 目标） */
  restorePersistedFirmwareSelection().then(function () {
    if (typeof window.switchStageFlow === 'function' && window.connectionMode !== 'ble') {
      window.switchStageFlow(window.flashTarget || 'esp32');
    }
  }).catch(function () {});
})();
/* HTML onclick 用的 API 已在 IIFE 内挂到 window.*，无需再二次赋值 */
