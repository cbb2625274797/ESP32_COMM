/**
 * toast.js - Toast 提示组件
 *
 * showToast 函数，操作 DOM 元素 #toast / #toastIco / #toastMsg。
 * 支持 success / error / warning 三类；info 走 warning 同款样式（历史兼容，无专属配色）。
 * 内置 5s 去重防抖，相同 (type|message) 5s 内不重复弹出。
 */
/** @description 显示 Toast 提示消息，支持 success/error/warning（info 同 warning）。 */
const TOAST_DEDUPE_MS = 5000;
const TOAST_AUTOHIDE_MS = 3000;

var _toastHideTimer = null;
var _toastLastKey = '';
var _toastLastAt = 0;
export function showToast(message, type) {
  type = type || 'success';
  var toast = document.getElementById('toast');
  if (!toast) return;
  // 相同文案 5s 内不重复弹，避免轮询把 ✓ 刷成「疯狂跳」
  var key = type + '|' + message;
  var now = Date.now();
  if (key === _toastLastKey && (now - _toastLastAt) < TOAST_DEDUPE_MS) {
    return;
  }
  _toastLastKey = key;
  _toastLastAt = now;

  var ico = document.getElementById('toastIco');
  var msg = document.getElementById('toastMsg');
  toast.className = 'toast ' + (type === 'success' ? 'ok' : type === 'error' ? 'err' : 'warn') + ' show';
  if (ico) ico.textContent = type === 'success' ? '\u2713' : type === 'error' ? '\u2715' : '\u26A0';
  if (msg) msg.textContent = message;
  if (_toastHideTimer) clearTimeout(_toastHideTimer);
  _toastHideTimer = setTimeout(function () {
    toast.classList.remove('show');
    _toastHideTimer = null;
  }, TOAST_AUTOHIDE_MS);
}
