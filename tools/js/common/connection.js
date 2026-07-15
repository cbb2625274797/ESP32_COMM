/**
 * connection.js - 连接管理 + 系统状态显示
 *
 * 从 app.js 的「连接管理」段 + testConnection / refreshSystemStatusNow 提取。
 * 操作 DOM 元素 #pillConn / #pillConnTxt / #connStatus / #connStatusTxt / #bConn 等。
 *
 * 共享状态（通过 window.* 引用，后续 app.js 拆分时会统一挂载）：
 *   window.latestSystemStatus     — 最近一次 /system/status 响应
 *   window.lastSysStateCode       — 上次成功的 sys_state_code（polling.js 读取）
 *   window.lastRelayState         — 上次成功的 relay_state（polling.js 读取）
 *   window.consecutivePollFailures — 连续轮询失败计数（polling.js 读取）
 *
 * 未拆分的 app.js 函数（通过 window.* 引用）：
 *   window.getBaseUrl / window.updateSystemStatusUi / window.diagnoseNetwork / window.formatPercent
 */
import { formatBytes, formatTime, formatUptime, escapeHtml } from './utils.js';
import { addLog } from './log.js';
import { showToast } from './toast.js';

// ==================== 连接管理 ====================
export function updateConnectionUi(connected, text) {
  // header pill
  var pc = document.getElementById('pillConn');
  var pct = document.getElementById('pillConnTxt');
  pc.className = connected ? 'pill online' : 'pill offline';
  pct.textContent = text || (connected ? '已连接' : '未连接');
  // connection card indicator
  var cs = document.getElementById('connStatus');
  var cst = document.getElementById('connStatusTxt');
  if (cs) cs.className = connected ? 'conn-indicator ok' : 'conn-indicator err';
  if (cst) cst.textContent = connected ? (text || '已连接') : '未连接';
  // primary badge
  var bc = document.getElementById('bConn');
  bc.textContent = connected ? '在线' : '离线';
  bc.className = 'v ' + (connected ? 'ok' : 'err');
}
export function updateHealthUi(healthCode, freeHeap) {
  var healthNames = ['正常', '警告', '危险', '关机'];
  var health = healthCode !== undefined ? (healthNames[healthCode] || '未知') : '未知';
  var connStatusTxt = document.getElementById('connStatusTxt');
  if (connStatusTxt && window.latestSystemStatus && window.latestSystemStatus.wifi) {
    connStatusTxt.textContent = (window.latestSystemStatus.wifi.connected ? '设备在线' : '设备离线') + ' · 系统' + health;
  }
  if (freeHeap !== undefined) {
    var mHeapEl = document.getElementById('mHeap');
    if (mHeapEl) mHeapEl.textContent = formatBytes(freeHeap);
  }
  var mh = document.getElementById('mHealth');
  if (mh) {
    mh.textContent = health;
    mh.className = 'v ' + (healthCode === 0 ? 'ok' : healthCode === 1 ? 'warn' : 'err');
  }
}
export function updateHeapAndTaskStatusUi(data) {
  if (!data) return;
  var ota = data.ota || {};
  var wifi = data.wifi || {};
  var targetHost = window.getBaseUrl().replace(/^http:\/\//, '');
  var setText = function (id, value) {
    var el = document.getElementById(id);
    if (el) el.textContent = value;
  };
  // ---- 堆概况：已用 / 总计 + 占用条 ----
  function setFill(id, pct) {
    var el = document.getElementById(id);
    if (!el) return;
    el.style.width = Math.max(0, Math.min(100, pct)) + '%';
    el.className = 'heap-bar-fill' + (pct >= 85 ? ' danger' : pct >= 70 ? ' warn' : '');
  }
  function setBar(usageId, fillId, pctId, free, total) {
    if (free === undefined || total === undefined) {
      setText(usageId, '--'); setFill(fillId, 0); setText(pctId, '--');
      return;
    }
    var used = total - free;
    var pct = total > 0 ? (used * 100 / total) : 0;
    setText(usageId, '已用 ' + formatBytes(used) + ' / 总计 ' + formatBytes(total));
    setFill(fillId, pct);
    setText(pctId, pct.toFixed(1) + '%');
  }
  function setCpuFill(pct) {
    var el = document.getElementById('cpuUsageFill');
    if (!el) return;
    var safePct = Math.max(0, Math.min(100, Number(pct) || 0));
    el.style.width = safePct + '%';
    el.className = 'heap-bar-fill cpu-fill' + (safePct >= 85 ? ' danger' : safePct >= 70 ? ' warn' : '');
  }
  function updateCpuUsageUi(cpu) {
    cpu = cpu || {};
    var taskList = document.getElementById('cpuTaskList');
    if (!cpu.available) {
      setText('cpuUsageText', '未启用 FreeRTOS 统计');
      setText('cpuUsagePct', '--');
      setText('cpuIdlePct', '--');
      setText('cpuSampleMs', '--');
      setText('cpuTaskCount', '--');
      setCpuFill(0);
      if (taskList) taskList.innerHTML = '<div class="status-empty">固件未启用 CPU runtime stats</div>';
      return;
    }
    if (!cpu.valid) {
      setText('cpuUsageText', '等待下一次采样...');
      setText('cpuUsagePct', '--');
      setText('cpuIdlePct', '--');
      setText('cpuSampleMs', '--');
      setText('cpuTaskCount', '--');
      setCpuFill(0);
      if (taskList) taskList.innerHTML = '<div class="status-empty">首次刷新只建立基线，下一次刷新后显示 CPU 占用</div>';
      return;
    }
    var usagePct = Number(cpu.usage_pct) || 0;
    var idlePct = Number(cpu.idle_pct) || 0;
    var sampleMs = Number(cpu.sample_ms) || 0;
    var tasks = Array.isArray(cpu.tasks) ? cpu.tasks.slice() : [];
    tasks.sort(function (a, b) {
      return (Number(b.usage_pct) || 0) - (Number(a.usage_pct) || 0);
    });

    setText('cpuUsageText', 'CPU 已用 ' + window.formatPercent(usagePct));
    setText('cpuUsagePct', window.formatPercent(usagePct));
    setText('cpuIdlePct', window.formatPercent(idlePct));
    setText('cpuSampleMs', sampleMs > 0 ? sampleMs + ' ms' : '--');
    setText('cpuTaskCount', String(tasks.length));
    setCpuFill(usagePct);

    if (taskList) {
      var visibleTasks = tasks.slice(0, 8);
      if (!visibleTasks.length) {
        taskList.innerHTML = '<div class="status-empty">当前采样没有任务增量</div>';
      } else {
        taskList.innerHTML = visibleTasks.map(function (task) {
          var pct = Number(task.usage_pct) || 0;
          var delta = Number(task.runtime_delta) || 0;
          var fillClass = pct >= 85 ? 'err' : pct >= 70 ? 'warn' : '';
          return '' +
            '<div class="status-task cpu-task">' +
              '<div class="status-task-head">' +
                '<div class="status-task-name">' + escapeHtml(task.name || 'task') + '</div>' +
                '<div class="status-task-ratio">' + escapeHtml(window.formatPercent(pct)) + '</div>' +
              '</div>' +
              '<div class="status-task-bar"><div class="status-task-bar-fill ' + fillClass + '" style="width:' + Math.max(0, Math.min(100, pct)) + '%"></div></div>' +
              '<div class="status-task-meta">' +
                '<div><span>任务号</span><b>' + escapeHtml(task.task_number) + '</b></div>' +
                '<div><span>增量</span><b>' + escapeHtml(delta) + '</b></div>' +
                '<div><span>占比</span><b>' + escapeHtml(window.formatPercent(pct)) + '</b></div>' +
              '</div>' +
            '</div>';
        }).join('');
      }
    }
  }
  updateCpuUsageUi(data.cpu);
  setText('mInternalHeap', data.internal_free_heap !== undefined ? formatBytes(data.internal_free_heap) : '--');
  setText('mInternalMinHeap', data.internal_min_free_heap !== undefined ? formatBytes(data.internal_min_free_heap) : '--');
  setText('mInternalLargestBlock', data.internal_largest_free_block !== undefined ? formatBytes(data.internal_largest_free_block) : '--');
  setBar('memInternalUsage', 'memInternalFill', 'memInternalPct', data.internal_free_heap, data.internal_heap_total);

  setText('mLargestBlock', data.largest_free_block !== undefined ? formatBytes(data.largest_free_block) : '--');

  setText('sOtaState', (ota.sys_state || '--') + (Number.isFinite(ota.sys_state_code) ? ' (' + ota.sys_state_code + ')' : ''));
  setText('sRelayState', Number.isFinite(ota.relay_state) ? String(ota.relay_state) : '--');
  // 缓存最近一次成功的 sys_state_code / relay_state，供 pollStatus 失败时判断设备是否在忙
  if (Number.isFinite(ota.sys_state_code)) window.lastSysStateCode = ota.sys_state_code;
  if (Number.isFinite(ota.relay_state)) window.lastRelayState = ota.relay_state;
  var storedText = '--';
  if (ota.total_size || ota.stored_size) {
    storedText = formatBytes(ota.stored_size || 0) + ' / ' + formatBytes(ota.total_size || 0);
  }
  setText('sOtaStored', storedText);
  setText('sStatusUpdated', formatTime(new Date()));

  var taskList = document.getElementById('taskStackList');
  if (taskList) {
    var taskStack = data.task_stack || {};
    var names = Object.keys(taskStack);
    if (!names.length) {
      taskList.innerHTML = '<div class="status-empty">当前状态未包含任务栈信息</div>';
    } else {
      taskList.innerHTML = names.map(function (name) {
        var item = taskStack[name] || {};
        var stackSize = Number(item.stack_size_bytes) || 0;
        var watermark = Number(item.high_watermark_bytes) || 0;
        var peakUsed = Number(item.peak_used_bytes) || 0;
        var usagePct = stackSize > 0 ? (peakUsed * 100 / stackSize) : 0;
        var fillClass = usagePct >= 85 ? 'err' : usagePct >= 70 ? 'warn' : '';
        return '' +
          '<div class="status-task">' +
            '<div class="status-task-head">' +
              '<div class="status-task-name">' + escapeHtml(name) + '</div>' +
              '<div class="status-task-ratio">' + escapeHtml(window.formatPercent(usagePct)) + '</div>' +
            '</div>' +
            '<div class="status-task-bar"><div class="status-task-bar-fill ' + fillClass + '" style="width:' + Math.max(0, Math.min(100, usagePct)) + '%"></div></div>' +
            '<div class="status-task-meta">' +
              '<div><span>栈大小</span><b>' + escapeHtml(formatBytes(stackSize)) + '</b></div>' +
              '<div><span>剩余水位</span><b>' + escapeHtml(formatBytes(watermark)) + '</b></div>' +
              '<div><span>峰值占用</span><b>' + escapeHtml(formatBytes(peakUsed)) + '</b></div>' +
            '</div>' +
          '</div>';
      }).join('');
    }
  }

  var statusJson = document.getElementById('statusJson');
  if (statusJson) {
    statusJson.textContent = JSON.stringify(data, null, 2);
  }
}
/** @description 测试与 ESP32 的连接（GET /status）。 */
export async function testConnection() {
  // 立即发一次请求检测
  // 手动检测使用独立的 AbortController，避免打断递归调度的 pollStatus
  var testAbort = new AbortController();
  var testTimer = setTimeout(function() { try { testAbort.abort(); } catch(_e) {} }, 3000);
  try {
    var resp = await fetch(window.getBaseUrl() + '/system/status', {
      signal: testAbort.signal
    });
    if (resp.ok) {
      var data = await resp.json();
      window.latestSystemStatus = data;
      window.updateSystemStatusUi(data);
      var wifi = data.wifi || {};
      addLog('I', '连接', wifi.connected
        ? '已连接：' + (wifi.mode || 'STA') + ' ' + (wifi.ip || '')
        : '未连接：' + (wifi.mode || 'STA'));
      showToast(wifi.connected ? 'ESP32 当前在线' : 'ESP32 当前未连接',
        wifi.connected ? 'success' : 'warning');
    } else {
      addLog('E', '连接', 'HTTP ' + resp.status);
      showToast('设备响应异常: HTTP ' + resp.status, 'error');
    }
  } catch (e) {
    addLog('E', '连接', '连接失败: ' + e.message);
    // 连接失败时自动诊断
    var diag = await window.diagnoseNetwork();
    addLog('W', '诊断', diag);
    showToast('无法连接: ' + diag, 'error');
  } finally {
    clearTimeout(testTimer);
  }
}
export async function refreshSystemStatusNow(event) {
  if (event) {
    event.preventDefault();
    event.stopPropagation();
  }
  // 手动刷新使用独立的 AbortController，避免与递归调度的 pollStatus 互相干扰
  var manualAbort = new AbortController();
  var manualTimer = setTimeout(function() { try { manualAbort.abort(); } catch(_e) {} }, 3000);
  try {
    var resp = await fetch(window.getBaseUrl() + '/system/status', {
      signal: manualAbort.signal
    });
    if (!resp.ok) {
      addLog('E', '状态', '刷新失败: HTTP ' + resp.status);
      showToast('状态刷新失败: HTTP ' + resp.status, 'error');
      return;
    }
    var data = await resp.json();
    window.latestSystemStatus = data;
    window.updateSystemStatusUi(data);
    window.consecutivePollFailures = 0;
    addLog('I', '状态', '已手动刷新 /system/status');
  } catch (e) {
    addLog('E', '状态', '刷新失败: ' + e.message);
    showToast('状态刷新失败', 'error');
  } finally {
    clearTimeout(manualTimer);
  }
}
