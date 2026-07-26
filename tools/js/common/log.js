/**
 * log.js - 日志系统（带去重 + 自动滚动 + 上限裁剪）
 *
 * 日志环形缓冲、去重、过滤、自动滚动逻辑。
 * 操作 DOM 元素 #logScroll / #logCount / #autoScrollBtn / .log-filter。
 */
import { formatTime, escapeHtml } from './utils.js';

// ==================== 日志系统状态 ====================
var MAX_LOGS = 1000;          // 缓冲上限，超出后丢弃最旧条目
var logFilter = 'all';
var logEntries = [];
var lastLogHash = null;
var autoScroll = true;

// 缓存 DOM 引用，避免高频日志下重复 getElementById
var scrollEl = document.getElementById('logScroll');
var countEl = document.getElementById('logCount');
var scrollPending = false;

/** rAF 批量自动滚动：/logs 一轮可能追加几十行，逐行读 scrollHeight 会每行
 *  触发一次强制布局；合并到下一帧只滚一次，行为不变。 */
function scheduleAutoScroll() {
  if (!autoScroll || !scrollEl || scrollPending) return;
  scrollPending = true;
  requestAnimationFrame(function () {
    scrollPending = false;
    if (autoScroll && scrollEl) scrollEl.scrollTop = scrollEl.scrollHeight;
  });
}

// ==================== 日志系统 (带去重 + 自动滚动) ====================
function makeLogHash(level, tag, message) {
  return level + '|' + tag + '|' + message;
}
/** 构造一条日志 DOM 节点（tag/message 经 HTML 转义，防 XSS）。 */
function createLogLine(entry) {
  var div = document.createElement('div');
  div.className = 'log-line';
  div.innerHTML =
    '<span class="lt">[' + escapeHtml(entry.time) + ']</span>' +
    '<span class="lv ' + escapeHtml(entry.level) + '">' + escapeHtml(entry.level) + '</span>' +
    '<span class="lg">[' + escapeHtml(entry.tag) + ']</span>' +
    '<span class="lm">' + escapeHtml(entry.message) + '</span>';
  return div;
}
/** 同步 #logCount 为当前可见条目数。 */
function syncCount() {
  if (countEl) countEl.textContent = logEntries.length;
}
/** 超过上限时丢弃最旧条目，并同步移除其 DOM 节点。 */
function trimIfNeeded() {
  while (logEntries.length > MAX_LOGS) {
    var old = logEntries.shift();
    if (old && old.div && old.div.parentNode) old.div.parentNode.removeChild(old.div);
  }
}
/** @description 添加一条日志条目到环形缓冲区和日志面板。 */
export function addLog(level, tag, message) {
  var hash = makeLogHash(level, tag, message);
  if (hash === lastLogHash) return; // 去重
  lastLogHash = hash;
  var entry = {
    time: formatTime(new Date()),
    level: level,
    tag: tag,
    message: message,
    div: null
  };
  logEntries.push(entry);
  trimIfNeeded();
  if (logFilter !== 'all' && level !== logFilter) {
    syncCount();
    return;
  }
  var div = createLogLine(entry);
  entry.div = div;
  if (scrollEl) {
    scrollEl.appendChild(div);
    scheduleAutoScroll();
  }
  syncCount();
}
export function clearLogs() {
  if (scrollEl) scrollEl.innerHTML = '';
  logEntries = [];
  lastLogHash = null;
  syncCount();
}
export function setLogFilter(level, btn) {
  document.querySelectorAll('.log-filter').forEach(function (b) { b.classList.remove('active'); });
  if (btn) btn.classList.add('active');
  logFilter = level;
  if (!scrollEl) return;
  var frag = document.createDocumentFragment();
  logEntries.forEach(function (e) {
    if (level === 'all' || e.level === level) {
      var div = createLogLine(e);
      e.div = div;
      frag.appendChild(div);
    } else {
      e.div = null;
    }
  });
  scrollEl.innerHTML = '';
  scrollEl.appendChild(frag);
  scheduleAutoScroll();
}
export function toggleAutoScroll() {
  autoScroll = !autoScroll;
  var btn = document.getElementById('autoScrollBtn');
  if (btn) btn.textContent = autoScroll ? '\uD83D\uDCCC 自动滚动' : '\u23F8\uFE0F 暂停滚动';
}
