/**
 * log.js - 日志系统（带去重 + 自动滚动）
 *
 * 日志环形缓冲、去重、过滤、自动滚动逻辑。
 * 操作 DOM 元素 #logScroll / #logCount / #autoScrollBtn / .log-filter。
 */
import { formatTime } from './utils.js';

// ==================== 日志系统状态 ====================
let logFilter = 'all';
let logEntries = [];
let lastLogHash = null;
let autoScroll = true;

// ==================== 日志系统 (带去重 + 自动滚动) ====================
function makeLogHash(level, tag, message) {
  return level + '|' + tag + '|' + message;
}
/** @description 添加一条日志条目到环形缓冲区和日志面板。 */
export function addLog(level, tag, message) {
  var scroll = document.getElementById('logScroll');
  var time = formatTime(new Date());
  var hash = makeLogHash(level, tag, message);
  if (hash === lastLogHash) return; // 去重
  lastLogHash = hash;
  var entry = { time: time, level: level, tag: tag, message: message };
  logEntries.push(entry);
  if (logFilter !== 'all' && level !== logFilter) return;
  var div = document.createElement('div');
  div.className = 'log-line';
  div.innerHTML =
    '<span class="lt">[' + time + ']</span>' +
    '<span class="lv ' + level + '">' + level + '</span>' +
    '<span class="lg">[' + tag + ']</span>' +
    '<span class="lm">' + message + '</span>';
  scroll.appendChild(div);
  if (autoScroll) scroll.scrollTop = scroll.scrollHeight;
  document.getElementById('logCount').textContent = logEntries.length;
}
export function clearLogs() {
  document.getElementById('logScroll').innerHTML = '';
  logEntries = [];
  lastLogHash = null;
  document.getElementById('logCount').textContent = '0';
}
export function setLogFilter(level, btn) {
  document.querySelectorAll('.log-filter').forEach(function (b) { b.classList.remove('active'); });
  btn.classList.add('active');
  logFilter = level;
  var scroll = document.getElementById('logScroll');
  scroll.innerHTML = '';
  logEntries.forEach(function (e) {
    if (level === 'all' || e.level === level) {
      var div = document.createElement('div');
      div.className = 'log-line';
      div.innerHTML =
        '<span class="lt">[' + e.time + ']</span>' +
        '<span class="lv ' + e.level + '">' + e.level + '</span>' +
        '<span class="lg">[' + e.tag + ']</span>' +
        '<span class="lm">' + e.message + '</span>';
      scroll.appendChild(div);
    }
  });
  if (autoScroll) scroll.scrollTop = scroll.scrollHeight;
}
export function toggleAutoScroll() {
  autoScroll = !autoScroll;
  var btn = document.getElementById('autoScrollBtn');
  btn.textContent = autoScroll ? '\uD83D\uDCCC 自动滚动' : '\u23F8\uFE0F 暂停滚动';
}
