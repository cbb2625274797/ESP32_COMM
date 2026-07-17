/**
 * ble_ota.js - BLE OTA 控制台入口
 *
 * 薄入口：import 所有 ble/* 模块，组装并导出 window.bleOta API 对象，
 * 调用 init() 完成事件绑定与 UI 初始化。
 */
import { UI_BUILD } from './ble/constants.js';
import {
  state, els, bleLog, refreshTargetUi, setConnectionState,
  resetUiAfterDisconnect
} from './ble/state.js';
import { resetBleStages } from './ble/stages.js';
import { connectBle, disconnectBle } from './ble/gatt.js';
import { clearSelectedFile, notifyFileChange } from './ble/file.js';
import { autoFlash } from './ble/transfer.js';
import { abortTask, bindEvents } from './ble/session.js';
import { showToast } from './common/toast.js';

window.bleOta = {
  init: function() {
    bindEvents();
    refreshTargetUi();
    setConnectionState(false);
    resetBleStages();
    bleLog('SYS', `Web Bluetooth 刷写控制台已就绪 (${UI_BUILD})`);
  },
  disconnect: disconnectBle,
  showToast: showToast,
  notifyFileChange: notifyFileChange,
  clearFile: clearSelectedFile,
  refreshButtons: function() {
    setConnectionState(!!state.server);
  },
  refreshTargetUi: refreshTargetUi,
  // ★ 暴露上传状态查询，供 index.html 的 selectFlashTarget 等外部守卫使用
  isUploading: function() {
    return !!(state.uploading || state.autoFlashing);
  },
};

// 脚本以 type="module" 加载（默认 defer），DOM 元素已就绪，直接初始化
window.bleOta.init();