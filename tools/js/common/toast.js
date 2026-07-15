/**
 * toast.js - Toast 提示组件
 *
 * 从 app.js 提取的 showToast 函数，操作 DOM 元素 #toast / #toastIco / #toastMsg。
 * 支持 info / success / warning / error 类型，内置 5s 去重防抖。
 */
/** @description 显示 Toast 提示消息，支持 info/success/warning/error 类型。 */
var _toastHideTimer = null;
var _toastLastKey = '';
var _toastLastAt = 0;
export function showToast(message, type) {
  type = type || 'success';
  // 相同文案 5s 内不重复弹，避免轮询把 ✓ 刷成「疯狂跳」
  var key = type + '|' + message;
  var now = Date.now();
  if (key === _toastLastKey && (now - _toastLastAt) < 5000) {
    return;
  }
  _toastLastKey = key;
  _toastLastAt = now;

  const toast = document.getElementById('toast');
  const ico = document.getElementById('toastIco');
  const msg = document.getElementById('toastMsg');
  toast.className = 'toast ' + (type === 'success' ? 'ok' : type === 'error' ? 'err' : 'warn') + ' show';
  ico.textContent = type === 'success' ? '\u2713' : type === 'error' ? '\u2715' : '\u26A0';
  msg.textContent = message;
  if (_toastHideTimer) clearTimeout(_toastHideTimer);
  _toastHideTimer = setTimeout(function () {
    toast.classList.remove('show');
    _toastHideTimer = null;
  }, 3000);
}
