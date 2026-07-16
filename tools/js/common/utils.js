/**
 * utils.js - 通用工具函数
 *
 * 格式化 / 转义工具函数，供各子模块和 can.js / can_monitor.js 共享。
 */
// ==================== 工具函数 ====================
/** @description 格式化字节大小为人类可读字符串（B/KB/MB）。 */
/** @description 字节格式化为 B / KB（不升到 MB，避免 1024KB 显示成 1.00MB）。 */
export function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return '--';
  if (bytes < 1024) return bytes + ' B';
  var kb = bytes / 1024;
  if (Math.abs(kb - Math.round(kb)) < 1e-9) return Math.round(kb) + ' KB';
  return kb.toFixed(1) + ' KB';
}
/** @description 格式化 Date 对象为 HH:MM:SS.mmm 时间字符串。 */
export function formatTime(date) {
  var h = String(date.getHours()).padStart(2, '0');
  var m = String(date.getMinutes()).padStart(2, '0');
  var s = String(date.getSeconds()).padStart(2, '0');
  var ms = String(date.getMilliseconds()).padStart(3, '0');
  return h + ':' + m + ':' + s + '.' + ms;
}
/** @description 格式化秒数为人类可读运行时间（d/h/m/s）。 */
export function formatUptime(sec) {
  if (!Number.isFinite(sec) || sec < 0) return '--';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
  if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
  if (m > 0) return m + 'm ' + s + 's';
  return s + 's';
}
/** @description 转义 HTML 特殊字符，防止 XSS 注入。 */
export function escapeHtml(text) {
  return String(text == null ? '' : text)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}
