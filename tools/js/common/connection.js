/**
 * connection.js - 连接管理 + 系统状态显示
 *
 * 操作 DOM 元素 #pillConn / #pillConnTxt / #bConn 等（连接文案仅顶栏）。
 *
 * 共享状态（通过 window.* 引用）：
 *   window.latestSystemStatus     — 最近一次 /system/status 响应
 *   window.lastSysStateCode       — 上次成功的 sys_state_code（polling.js 读取）
 *   window.lastRelayState         — 上次成功的 relay_state（polling.js 读取）
 *   window.consecutivePollFailures — 连续轮询失败计数（polling.js 读取）
 *
 * 复用的函数（通过 window.* 引用）：
 *   window.getBaseUrl / window.updateSystemStatusUi / window.formatPercent
 */
import { formatBytes, formatTime, formatUptime, escapeHtml } from './utils.js';
import { addLog } from './log.js';
import { showToast } from './toast.js';
import {
  appendHeapLowRecords, loadHeapLowHistory, clearHeapLowHistory, HEAP_HISTORY_MAX
} from './heap-low-history.js';

// ==================== 连接管理 ====================
export function updateConnectionUi(connected, text) {
  var label = text || (connected ? '已连接' : '未连接');
  // 仅顶栏连接胶囊（下方连接卡不再重复显示文案）
  var pc = document.getElementById('pillConn');
  var pct = document.getElementById('pillConnTxt');
  if (pc) {
    pc.className = connected ? 'pill online' : 'pill offline';
    if (pct) pct.textContent = label;
  }
  // 系统状态卡徽章
  var bc = document.getElementById('bConn');
  if (bc) {
    bc.textContent = connected ? '在线' : '离线';
    bc.className = 'v ' + (connected ? 'ok' : 'err');
  }
}
export function updateHealthUi(healthCode, freeHeap) {
  var healthNames = ['正常', '警告', '危险', '关机'];
  var health = healthCode !== undefined ? (healthNames[healthCode] || '未知') : '未知';
  /* 连接文案由 updateConnectionUi / 顶栏 pill 负责，不在此拼「设备在线 · 系统正常」 */
  if (freeHeap !== undefined) {
    var mHeapEl = document.getElementById('mHeap');
    if (mHeapEl) mHeapEl.textContent = formatBytes(freeHeap);
  }
  var mh = document.getElementById('mHealth');
  if (mh) {
    mh.textContent = health;
    mh.className = 'v ' + (healthCode === 0 ? 'ok' : healthCode === 1 ? 'warn' : 'err');
  }
  ensureHealthBootToggleBound();
}

/** 健康状态 badge 点击展开/收起启动自检面板（只绑一次）。 */
function ensureHealthBootToggleBound() {
  var badge = document.getElementById('healthBadge');
  var panel = document.getElementById('healthBootPanel');
  if (!badge || !panel || badge.dataset.bootToggleBound === '1') return;
  badge.dataset.bootToggleBound = '1';

  function toggle() {
    var open = panel.classList.contains('collapsed') || panel.hasAttribute('hidden');
    if (open) {
      panel.hidden = false;
      panel.classList.remove('collapsed');
      badge.classList.add('is-open');
      badge.setAttribute('aria-expanded', 'true');
    } else {
      panel.hidden = true;
      panel.classList.add('collapsed');
      badge.classList.remove('is-open');
      badge.setAttribute('aria-expanded', 'false');
    }
  }
  badge.addEventListener('click', function (e) {
    e.preventDefault();
    toggle();
  });
  badge.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      toggle();
    }
  });
}

/** 启动自检内容（/system/status.boot）；挂在健康状态展开区，非常驻。 */
function updateBootInitUi(data) {
  var boot = data && data.boot ? data.boot : null;
  var resultEl = document.getElementById('mBootResult');
  var failedEl = document.getElementById('mBootFailed');
  var listEl = document.getElementById('bootModuleList');
  if (!boot) {
    if (resultEl) {
      resultEl.textContent = '--';
      resultEl.className = 'health-boot-summary';
    }
    if (failedEl) {
      failedEl.textContent = '--';
      failedEl.className = '';
    }
    if (listEl) listEl.innerHTML = '';
    return;
  }

  var criticalOk = !!boot.critical_ok;
  var allOk = !!boot.all_ok;
  var summary = boot.summary || (allOk ? 'OK (all modules)' : (criticalOk ? 'OK (critical)' : 'FAIL'));
  var failed = (boot.failed && String(boot.failed).trim()) ? String(boot.failed) : '';
  var tone = allOk ? 'ok' : (criticalOk ? 'warn' : 'err');

  if (resultEl) {
    resultEl.textContent = summary;
    resultEl.className = 'health-boot-summary ' + tone;
    resultEl.title = 'mask=0x' + (Number(boot.mask) || 0).toString(16);
  }
  if (failedEl) {
    failedEl.textContent = failed || (allOk ? '无' : '--');
    failedEl.className = failed ? tone : 'ok';
  }

  if (listEl) {
    var modules = Array.isArray(boot.modules) ? boot.modules : [];
    if (!modules.length) {
      listEl.innerHTML = '';
    } else {
      listEl.innerHTML = modules.map(function (m) {
        var ok = !!m.ok;
        var critical = !!m.critical;
        var cls = 'boot-mod ' + (ok ? 'ok' : (critical ? 'fail' : 'fail optional'));
        var tip = (m.name || '') + (ok ? ' 成功' : ' 失败') + (critical ? ' · 关键' : ' · 非关键');
        return '<span class="' + cls + '" title="' + escapeHtml(tip) + '">' +
          '<span class="boot-mod-dot"></span>' +
          escapeHtml(m.name || '?') +
          '</span>';
      }).join('');
    }
  }

  if (!window._bootInitLogged) {
    window._bootInitLogged = true;
    var logLine = '启动自检: ' + summary + (failed ? ' · 失败/跳过: ' + failed : '');
    addLog(allOk ? 'I' : (criticalOk ? 'W' : 'E'), 'BOOT', logLine);
  }
  ensureHealthBootToggleBound();
}

export function updateHeapAndTaskStatusUi(data) {
  if (!data) return;
  var formatPercent = window.formatPercent;
  var ota = data.ota || {};
  var setText = function (id, value) {
    var el = document.getElementById(id);
    if (el) el.textContent = value;
  };

  updateBootInitUi(data);

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
  function updateCpuUsageUi(cpuData) {
    cpuData = cpuData || {};
    var taskList = document.getElementById('cpuTaskList');
    if (!cpuData.available) {
      setText('cpuUsageText', '未启用 FreeRTOS 统计');
      setText('cpuUsagePct', '--');
      setText('cpuIdlePct', '--');
      setText('cpuSampleMs', '--');
      setText('cpuTaskCount', '--');
      setCpuFill(0);
      if (taskList) taskList.innerHTML = '<div class="status-empty">固件未启用 CPU runtime stats</div>';
      return;
    }
    if (!cpuData.valid) {
      setText('cpuUsageText', '等待下一次采样...');
      setText('cpuUsagePct', '--');
      setText('cpuIdlePct', '--');
      setText('cpuSampleMs', '--');
      setText('cpuTaskCount', '--');
      setCpuFill(0);
      if (taskList) taskList.innerHTML = '<div class="status-empty">首次刷新只建立基线，下一次刷新后显示 CPU 占用</div>';
      return;
    }
    var usagePct = Number(cpuData.usage_pct) || 0;
    var idlePct = Number(cpuData.idle_pct) || 0;
    var sampleMs = Number(cpuData.sample_ms) || 0;
    var tasks = Array.isArray(cpuData.tasks) ? cpuData.tasks.slice() : [];
    tasks.sort(function (a, b) {
      return (Number(b.usage_pct) || 0) - (Number(a.usage_pct) || 0);
    });

    setText('cpuUsageText', 'CPU 已用 ' + formatPercent(usagePct));
    setText('cpuUsagePct', formatPercent(usagePct));
    setText('cpuIdlePct', formatPercent(idlePct));
    setText('cpuSampleMs', sampleMs > 0 ? sampleMs + ' ms' : '--');
    setText('cpuTaskCount', String(tasks.length));
    setCpuFill(usagePct);

    if (taskList) {
      /* 按占用降序渲染全部任务；列表容器 CSS 默认露出约 3 条，其余滚动 */
      if (!tasks.length) {
        taskList.innerHTML = '<div class="status-empty">当前采样没有任务增量</div>';
      } else {
        taskList.innerHTML = tasks.map(function (task) {
          var pct = Number(task.usage_pct) || 0;
          var delta = Number(task.runtime_delta) || 0;
          var fillClass = pct >= 85 ? 'err' : pct >= 70 ? 'warn' : '';
          return '' +
            '<div class="status-task cpu-task">' +
              '<div class="status-task-head">' +
                '<div class="status-task-name">' + escapeHtml(task.name || 'task') + '</div>' +
                '<div class="status-task-ratio">' + escapeHtml(formatPercent(pct)) + '</div>' +
              '</div>' +
              '<div class="status-task-bar"><div class="status-task-bar-fill ' + fillClass + '" style="width:' + Math.max(0, Math.min(100, pct)) + '%"></div></div>' +
              '<div class="status-task-meta">' +
                '<div><span>任务号</span><b>' + escapeHtml(task.task_number) + '</b></div>' +
                '<div><span>增量</span><b>' + escapeHtml(delta) + '</b></div>' +
                '<div><span>占比</span><b>' + escapeHtml(formatPercent(pct)) + '</b></div>' +
              '</div>' +
            '</div>';
        }).join('');
      }
    }
  }
  updateCpuUsageUi(data.cpu);

  /* 合并 memory：layout（?layout=1，缓存）+ 动态 heap_*（每次轮询） */
  function mergeMemoryView(rawMem) {
    rawMem = rawMem || {};
    if (rawMem.layout && typeof rawMem.layout === 'object') {
      window._memLayout = rawMem.layout;
    }
    var L = window._memLayout || {};
    /* 兼容旧固件：静态字段仍在 memory 根上 */
    function pick(key) {
      if (rawMem[key] !== undefined) return rawMem[key];
      if (L[key] !== undefined) return L[key];
      return undefined;
    }
    return {
      chip: pick('chip'),
      sram_total: pick('sram_total'),
      sram_soc: pick('sram_soc'),
      static_reserved: pick('static_reserved'),
      reserved_tail: pick('reserved_tail'),
      iram: pick('iram'),
      data: pick('data'),
      bss: pick('bss'),
      heap_total: pick('heap_total'),
      dma_total: pick('dma_total'),
      rtc_total: pick('rtc_total'),
      task_stack_total: pick('task_stack_total'),
      default_heap_total: pick('default_heap_total'),
      heap_used: rawMem.heap_used,
      heap_free: rawMem.heap_free,
      heap_min_free: rawMem.heap_min_free,
      heap_largest_free: rawMem.heap_largest_free,
      alloc_blocks: rawMem.alloc_blocks,
      free_blocks: rawMem.free_blocks,
      dma_free: rawMem.dma_free,
      rtc_free: rawMem.rtc_free,
      default_heap_free: rawMem.default_heap_free
    };
  }

  /* 芯片 SRAM 全景：IRAM + .data + .bss + 堆已用 + 堆空闲（链接符号拆分静态段） */
  function updateSramOverview(mem, fallback) {
    mem = mergeMemoryView(mem);
    var sramTotal = Number(mem.sram_total);
    var sramSoc = Number(mem.sram_soc);
    var staticRes = Number(mem.static_reserved);
    var iram = Number(mem.iram);
    var dataSz = Number(mem.data);
    var bss = Number(mem.bss);
    var heapTotal = Number(mem.heap_total);
    var heapUsed = Number(mem.heap_used);
    var heapFree = Number(mem.heap_free);
    var reservedTail = Number(mem.reserved_tail);
    var hasSections = Number.isFinite(iram) || Number.isFinite(dataSz) || Number.isFinite(bss);

    /* 旧固件无 memory 字段时，用 internal_* 兜底 */
    if (!Number.isFinite(sramTotal) || sramTotal <= 0) {
      heapFree = Number(fallback.internal_free_heap);
      heapTotal = Number(fallback.internal_heap_total);
      if (!Number.isFinite(heapTotal) || heapTotal <= 0) {
        setText('memSramUsage', '--');
        setText('memSramPct', '--');
        setText('mIram', '--');
        setText('mData', '--');
        setText('mBss', '--');
        setText('mHeapUsed', '--');
        setText('mHeapFree', '--');
        setText('mSramSoc', '--');
        setText('mReservedTail', '--');
        setText('mTaskStackTotal', '--');
        setText('mDmaFree', '--');
        setText('mRtcFree', '--');
        setText('mAllocBlocks', '--');
        ['memSegIram', 'memSegData', 'memSegBss', 'memSegUsed', 'memSegFree'].forEach(function (id) {
          var el = document.getElementById(id);
          if (el) el.style.width = '0%';
        });
        return;
      }
      heapUsed = heapTotal - heapFree;
      sramTotal = heapTotal;
      staticRes = 0;
      iram = dataSz = bss = 0;
      hasSections = false;
    }
    if (!Number.isFinite(heapUsed) || heapUsed < 0) {
      heapUsed = Math.max(0, (Number.isFinite(heapTotal) ? heapTotal : 0) - (Number.isFinite(heapFree) ? heapFree : 0));
    }
    if (!Number.isFinite(heapFree) || heapFree < 0) heapFree = 0;
    if (!Number.isFinite(staticRes) || staticRes < 0) staticRes = 0;
    if (!Number.isFinite(heapTotal) || heapTotal < 0) heapTotal = heapUsed + heapFree;
    if (!Number.isFinite(iram) || iram < 0) iram = 0;
    if (!Number.isFinite(dataSz) || dataSz < 0) dataSz = 0;
    if (!Number.isFinite(bss) || bss < 0) bss = 0;
    /* 无分段字段时，整块静态灰条仍可用 static_reserved */
    if (!hasSections && staticRes > 0) {
      bss = staticRes;
    }

    var usedAll = staticRes + heapUsed;
    if (!Number.isFinite(staticRes) || staticRes <= 0) {
      usedAll = iram + dataSz + bss + heapUsed;
      staticRes = iram + dataSz + bss;
    }
    var pctAll = sramTotal > 0 ? (usedAll * 100 / sramTotal) : 0;
    setText('memSramUsage', '已占 ' + formatBytes(usedAll) + ' / 应用 ' + formatBytes(sramTotal));
    setText('memSramPct', pctAll.toFixed(1) + '%');
    setText('mIram', formatBytes(iram));
    setText('mData', formatBytes(dataSz));
    setText('mBss', formatBytes(bss));
    setText('mHeapUsed', formatBytes(heapUsed));
    setText('mHeapFree', formatBytes(heapFree));
    setText('mSramSoc', Number.isFinite(sramSoc) && sramSoc > 0 ? formatBytes(sramSoc) : '--');
    setText('mReservedTail', Number.isFinite(reservedTail) ? formatBytes(reservedTail) : '--');
    setText('mTaskStackTotal', Number.isFinite(Number(mem.task_stack_total))
      ? formatBytes(Number(mem.task_stack_total)) : '--');

    function setSeg(id, bytes) {
      var el = document.getElementById(id);
      if (!el) return;
      var pct = sramTotal > 0 ? (bytes * 100 / sramTotal) : 0;
      el.style.width = Math.max(0, Math.min(100, pct)) + '%';
    }
    setSeg('memSegIram', iram);
    setSeg('memSegData', dataSz);
    setSeg('memSegBss', bss);
    setSeg('memSegUsed', heapUsed);
    setSeg('memSegFree', heapFree);

    if (Number.isFinite(Number(mem.dma_free)) && Number.isFinite(Number(mem.dma_total))) {
      setText('mDmaFree', formatBytes(Number(mem.dma_free)) + ' / ' + formatBytes(Number(mem.dma_total)));
    } else {
      setText('mDmaFree', '--');
    }
    if (Number.isFinite(Number(mem.rtc_free)) && Number.isFinite(Number(mem.rtc_total))) {
      var rt = Number(mem.rtc_total);
      setText('mRtcFree', rt > 0
        ? (formatBytes(Number(mem.rtc_free)) + ' / ' + formatBytes(rt))
        : '未并入堆');
    } else {
      setText('mRtcFree', '--');
    }
    setText('mAllocBlocks', Number.isFinite(Number(mem.alloc_blocks)) ? String(Number(mem.alloc_blocks)) : '--');
  }
  updateSramOverview(data.memory, data);

  setText('mInternalHeap', data.internal_free_heap !== undefined ? formatBytes(data.internal_free_heap) : '--');
  setText('mInternalMinHeap', data.internal_min_free_heap !== undefined ? formatBytes(data.internal_min_free_heap) : '--');
  setText('mInternalLargestBlock', data.internal_largest_free_block !== undefined ? formatBytes(data.internal_largest_free_block) : '--');
  setBar('memInternalUsage', 'memInternalFill', 'memInternalPct', data.internal_free_heap, data.internal_heap_total);
  /* 最低空闲再创新低 → 尽快把设备 1 槽快照拉进 IndexedDB（多条历史靠浏览器攒） */
  onStatusHeapMinMaybeChanged(data);

  setText('mLargestBlock', data.largest_free_block !== undefined ? formatBytes(data.largest_free_block) : '--');

  bindHeapLowTooltip();

  setText('sOtaState', (ota.sys_state || '--') + (Number.isFinite(ota.sys_state_code) ? ' (' + ota.sys_state_code + ')' : ''));
  setText('sRelayState', Number.isFinite(ota.relay_state) ? String(ota.relay_state) : '--');
  if (Number.isFinite(ota.sys_state_code)) window.lastSysStateCode = ota.sys_state_code;
  if (Number.isFinite(ota.relay_state)) window.lastRelayState = ota.relay_state;
  setText('sStatusUpdated', formatTime(new Date()));

  var taskList = document.getElementById('taskStackList');
  if (taskList) {
    var taskStack = data.task_stack || {};
    var names = Object.keys(taskStack);
    if (!names.length) {
      taskList.innerHTML = '<div class="status-empty">当前状态未包含任务栈信息</div>';
    } else {
      /* 峰值占用比例降序；容器默认露出最高 3 条，其余滚动 */
      names.sort(function (a, b) {
        var ia = taskStack[a] || {};
        var ib = taskStack[b] || {};
        var sa = Number(ia.stack_size_bytes) || 0;
        var sb = Number(ib.stack_size_bytes) || 0;
        var pa = sa > 0 ? ((Number(ia.peak_used_bytes) || 0) * 100 / sa) : 0;
        var pb = sb > 0 ? ((Number(ib.peak_used_bytes) || 0) * 100 / sb) : 0;
        return pb - pa;
      });
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
              '<div class="status-task-ratio">' + escapeHtml(formatPercent(usagePct)) + '</div>' +
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
/** 从设备 IP 推导网关 IP（取前三段 + .1） */
function getGatewayIp() {
  var parts = (window.getEspIp ? window.getEspIp() : '192.168.31.127').split('.');
  if (parts.length === 4) return parts[0] + '.' + parts[1] + '.' + parts[2] + '.1';
  return '192.168.31.1';
}

/**
 * 网络诊断：区分浏览器离线 / 网关不通 / 仅设备不通。
 * 放在 connection.js，避免与 polling.js 形成循环 import。
 */
export async function diagnoseNetwork() {
  var result = [];
  if (navigator.onLine === false) {
    result.push('浏览器无网络连接');
    return result.join('；');
  }
  var gwIp = getGatewayIp();
  var gwOk = false;
  try {
    await fetch('http://' + gwIp + '/', { mode: 'no-cors', signal: AbortSignal.timeout(2000) });
    gwOk = true;
  } catch (_) {}
  if (!gwOk) {
    try {
      await fetch('http://' + gwIp + '/favicon.ico', { mode: 'no-cors', signal: AbortSignal.timeout(1500) });
      gwOk = true;
    } catch (_) {}
  }
  if (!gwOk) {
    result.push('网关 ' + gwIp + ' 不可达（本地网络不通）');
  } else {
    result.push('网关 ' + gwIp + ' 可达');
    var espIp = window.getEspIp ? window.getEspIp() : '设备';
    result.push('设备 ' + espIp + ' 不响应（可能断电、重启中或 WiFi 断开）');
  }
  return result.join('；');
}

/**
 * /system/status URL：尚无缓存布局时带 ?layout=1（静态 IRAM/.data/.bss/堆容量只取一次）。
 * 设备重启/换固件后清 window._memLayout 再拉。
 */
export function systemStatusUrl(forceLayout) {
  var base = (window.getBaseUrl ? window.getBaseUrl() : '') + '/system/status';
  if (forceLayout || !window._memLayout) {
    return base + (base.indexOf('?') >= 0 ? '&' : '?') + 'layout=1';
  }
  return base;
}

/**
 * 统一的 /system/status 抓取器：独立 AbortController，超时后中断，
 * 避免与递归调度的 pollStatus 互相干扰。
 * 成功返回 data，HTTP 非 2xx 抛 Error(含 status)，网络失败抛原 Error。
 */
async function fetchSystemStatus(timeoutMs, forceLayout) {
  var ac = new AbortController();
  var timer = setTimeout(function () { ac.abort(); }, timeoutMs || 3000);
  try {
    var resp = await fetch(systemStatusUrl(!!forceLayout), { signal: ac.signal });
    if (!resp.ok) {
      var err = new Error('HTTP ' + resp.status);
      err.status = resp.status;
      throw err;
    }
    return await resp.json();
  } finally {
    clearTimeout(timer);
  }
}

/** @description 测试与 ESP32 的连接（GET /status）。 */
export async function testConnection() {
  try {
    var data = await fetchSystemStatus(3000);
    window.latestSystemStatus = data;
    window.updateSystemStatusUi(data);
    var wifi = data.wifi || {};
    addLog('I', '连接', wifi.connected
      ? '已连接：' + (wifi.mode || 'STA') + ' ' + (wifi.ip || '')
      : '未连接：' + (wifi.mode || 'STA'));
    showToast(wifi.connected ? 'ESP32 当前在线' : 'ESP32 当前未连接',
      wifi.connected ? 'success' : 'warning');
  } catch (e) {
    if (e && e.status) {
      addLog('E', '连接', 'HTTP ' + e.status);
      showToast('设备响应异常: HTTP ' + e.status, 'error');
      return;
    }
    addLog('E', '连接', '连接失败: ' + e.message);
    // 连接失败时自动诊断
    var diag = await diagnoseNetwork();
    addLog('W', '诊断', diag);
    showToast('无法连接: ' + diag, 'error');
  }
}
export async function refreshSystemStatusNow(event) {
  if (event) {
    event.preventDefault();
    event.stopPropagation();
  }
  try {
    /* 手动刷新强制带 layout，便于换固件后更新静态段 */
    var data = await fetchSystemStatus(3000, true);
    window.latestSystemStatus = data;
    window.updateSystemStatusUi(data);
    window.consecutivePollFailures = 0;
    addLog('I', '状态', '已手动刷新 /system/status');
  } catch (e) {
    if (e && e.status) {
      addLog('E', '状态', '刷新失败: HTTP ' + e.status);
      showToast('状态刷新失败: HTTP ' + e.status, 'error');
      return;
    }
    addLog('E', '状态', '刷新失败: ' + e.message);
    showToast('状态刷新失败', 'error');
  }
}

// ============================================================
// 最低空闲 hover 弹窗：拉 /system/heap_low_log（设备 1 条）+ IndexedDB 历史
// ============================================================
// 行为：鼠标移到 #mInternalMinHeap 上时浮现 tooltip，离开时关闭。
//   首次进入懒加载一次接口，1s 内复用缓存防鼠标抖动反复请求。
//   弹窗固定在元素右下，越界时自动翻到左侧/上侧。

var heapLowLogCache = null;        // null=未加载，[]/data=已加载
var heapLowLogLoading = false;
var heapLowLogLoadAt = 0;          // 上次成功加载时间戳（ms）
var heapLowTooltipEl = null;       // 当前显示中的 tooltip 元素

var HEAP_LOW_STATE_NAMES = ['IDLE','GD32_STORING','GD32_STORED','GD32_RELAYING','GD32_COMPLETE','ESP32_RECEIVING','ESP32_FINALIZING','ESP32_REBOOTING','ERROR'];
var HEAP_LOW_CHANNEL_NAMES = ['WIFI','BLE'];

function fmtUptime(ms) {
  if (!Number.isFinite(ms) || ms <= 0) return '--';
  var s = Math.floor(ms / 1000);
  var h = Math.floor(s / 3600);
  var m = Math.floor((s % 3600) / 60);
  var sec = s % 60;
  if (h > 0) return h + 'h' + String(m).padStart(2,'0') + 'm' + String(sec).padStart(2,'0') + 's';
  if (m > 0) return m + 'm' + String(sec).padStart(2,'0') + 's';
  return sec + 's';
}

/** OTA 忙态：HTTP socket/堆紧张，勿后台拉 heap_low_log（与 status 抢缓冲/超时 abort）。 */
function isOtaBusyForHeapLowPoll() {
  if (window.pollingPausedForUpload) return true;
  var c = Number(window.lastSysStateCode);
  /* STORING=1 RELAYING=3 RECEIVING=5 FINALIZING=6 REBOOTING=7 */
  return c === 1 || c === 3 || c === 5 || c === 6 || c === 7;
}

function isFetchAbortError(e) {
  if (!e) return false;
  if (e.name === 'AbortError') return true;
  var msg = String(e.message || e);
  return /aborted|AbortError/i.test(msg);
}

/** 拉取 /system/heap_low_log。点击触发频率低，不缓存，保证每次看到最新数据
 *  （与每秒轮询的 #mInternalMinHeap 表格保持一致）。仅对"请求中"去重避免重复并发。
 *  拉到后增量写入 IndexedDB 持久化，突破设备仅 1 条槽位的限制。 */
async function loadHeapLowLog(force) {
  if (heapLowLogLoading) return heapLowLogCache;
  /* 用户点开 tooltip 时 force=true 仍允许；后台/创新低轮询勿在 OTA 中抢 HTTP */
  if (!force && isOtaBusyForHeapLowPoll()) {
    return heapLowLogCache;
  }
  heapLowLogLoading = true;
  try {
    var ac = new AbortController();
    var timer = setTimeout(function(){ ac.abort(); }, 3000);
    var resp;
    try {
      resp = await fetch(window.getBaseUrl() + '/system/heap_low_log', { signal: ac.signal });
    } finally { clearTimeout(timer); }
    if (!resp.ok) throw new Error('HTTP ' + resp.status);
    heapLowLogCache = await resp.json();
    heapLowLogLoadAt = Date.now();
    // 增量持久化：把设备当前记录与 IndexedDB 已有的去重后写入。
    //   失败/不可用静默降级（appendHeapLowRecords 内部已 catch）。
    if (heapLowLogCache && Array.isArray(heapLowLogCache.records) && heapLowLogCache.records.length) {
      appendHeapLowRecords(heapLowLogCache.records).then(function (n) {
        if (n > 0) {
          addLog('I', '内存', '堆低水位历史已存 ' + n + ' 条新记录');
        }
      });
    }
  } catch (e) {
    /* 超时 abort / 导航取消：OTA 或 socket 紧时常见，不刷 WARN */
    if (!isFetchAbortError(e)) {
      addLog('W', '内存', '拉取 /system/heap_low_log 失败: ' + (e && e.message ? e.message : e));
    }
    // 失败时不清缓存，避免下次再次进入就重打接口
  } finally {
    heapLowLogLoading = false;
  }
  return heapLowLogCache;
}

function buildHeapLowRowHtml(r, idx, segTag) {
  var ts = fmtUptime(r.ts_ms);
  var state = HEAP_LOW_STATE_NAMES[r.sys_state] || ('#' + r.sys_state);
  var conn = '';
  if (r.ble_connected) conn += 'BLE ';
  if (r.wifi_connected) conn += 'WiFi';
  if (!conn) conn = '--';
  var otaProgress = '--';
  if (r.ota_total_size > 0) {
    otaProgress = formatBytes(r.ota_stored_size) + '/' + formatBytes(r.ota_total_size);
  }
  var curTask = escapeHtml(r.current_task || 'health_mon');
  // 排除法诊断：OTA 资源优先；否则按 hook 最后一击类别（勿把「连接=WiFi」当触发任务）
  var diag = '--';
  var diagCls = '';
  var suspects = [];
  if (Number(r.ble_data_queue_depth) > 0) suspects.push('BLE队列:' + r.ble_data_queue_depth);
  if (Number(r.ble_reorder_cached) > 0) suspects.push('reorder:' + r.ble_reorder_cached);
  if (Number(r.spi_pending_jobs) > 0) suspects.push('SPI队列:' + r.spi_pending_jobs);
  var hookCls = (r.hook_task || '').toString();
  var hookSz = Number(r.hook_alloc_size) || 0;
  if (suspects.length) {
    diag = escapeHtml(suspects.join(' '));
    diagCls = 'has-suspect';
  } else if (Number(r.hook_fired) === 1 && (hookCls === 'net?' || (hookSz >= 48 && hookSz < 2048))) {
    // 最后一击是小/中块：HTTP 回包 / UDP 日志 send 前 lwip 会再 alloc pbuf，常被误当成 WiFi 主因
    diag = '最后一击≈网络发送';
    diagCls = 'idle-suspect';
  } else if (Number(r.hook_fired) === 1 && (hookCls === 'bulk' || hookSz >= 2048)) {
    diag = '最后一击≈大块alloc';
    diagCls = 'has-suspect';
  } else {
    var stateName = HEAP_LOW_STATE_NAMES[r.sys_state] || ('#' + r.sys_state);
    if (stateName === 'IDLE' || stateName === 'GD32_COMPLETE' || stateName === 'GD32_STORED' || stateName === 'ERROR') {
      diag = '空闲期(主因未定位)';
      diagCls = 'idle-suspect';
    } else {
      diag = '非数据路径';
      diagCls = 'idle-suspect';
    }
  }
  // 任务栈子表（默认隐藏，点击主行展开）
  var stacks = Array.isArray(r.task_stacks) ? r.task_stacks : [];
  var stackRows = stacks.map(function(s) {
    var nm = escapeHtml(s.name || '?');
    var rem = Number.isFinite(s.remaining) ? formatBytes(s.remaining) : '--';
    var cls = (Number(s.remaining) < 1024) ? 'crit' :
              (Number(s.remaining) < 2048) ? 'warn' : '';
    return '<tr class="heap-low-sub-row ' + cls + '"><td colspan="8" class="heap-low-sub-cell">' +
           nm + '<span class="heap-low-stack-rem">' + rem + '</span></td></tr>';
  }).join('');
  var stackSummary = stacks.length
    ? (stacks[0].name + ' ' + formatBytes(stacks[0].remaining) + ' <span class="heap-low-expand-hint">▾' + stacks.length + '</span>')
    : '--';
  // ★ 行 id 用 segTag 前缀避免设备当前段与历史段 idx 冲突
  var rowId = segTag + '_' + idx;
  return '<tr class="heap-low-main-row" data-idx="' + rowId + '" data-seg="' + segTag + '">' +
    '<td>' + ts + '</td>' +
    '<td>' + formatBytes(r.min_free_heap) + '</td>' +
    '<td>' + formatBytes(r.free_heap) + '</td>' +
    '<td>' + formatBytes(r.largest_free_block) + '</td>' +
    '<td>' + state + '</td>' +
    '<td>' + conn + '</td>' +
    '<td>' + otaProgress + '</td>' +
    '<td>' + formatBytes(r.default_free_heap) + '</td>' +
    '<td class="' + diagCls + '">' + diag + '</td>' +
    '<td>' + stackSummary + '</td>' +
  '</tr>' +
  '<tr class="heap-low-stack-detail" data-idx="' + rowId + '" hidden><td colspan="10">' +
    '<div class="heap-low-detail-meta">' +
      '<span>采样任务: <b>' + curTask + '</b> <span class="heap-low-caller-hint">（巡检发现者，不是 alloc 触发者）</span></span>' +
      '<span>内部堆分配数: <b>' + (Number.isFinite(r.alloc_count) ? r.alloc_count : '--') + '</b></span>' +
      '<span>IP事件计数: <b>' + (Number.isFinite(r.ip_event_count) ? r.ip_event_count : '--') + '</b></span>' +
      (r.saved_at_ms ? '<span>保存于: <b>' + formatTime(new Date(r.saved_at_ms)) + '</b></span>' : '') +
    '</div>' +
    '<div class="heap-low-hook-meta">' +
      '<div class="heap-low-hook-title">heap hook：破新低时的「最后一击」alloc' +
        (Number(r.hook_fired) === 1
          ? ''
          : ' <span class="heap-low-caller-hint">（hook 未命中）</span>') +
      '</div>' +
      '<span>分配类别: <b>' + escapeHtml(r.hook_task || '--') + '</b> <span class="heap-low-caller-hint">net?=小中块/疑网络 · bulk=大块 · tiny=更小</span></span>' +
      '<span>本次alloc: <b>' + (Number.isFinite(r.hook_alloc_size) && Number(r.hook_alloc_size) > 0 ? formatBytes(r.hook_alloc_size) : '--') + '</b></span>' +
      '<span>alloc后est空闲: <b>' + (Number.isFinite(r.hook_free_after) && Number(r.hook_free_after) > 0 ? formatBytes(r.hook_free_after) : '--') + '</b></span>' +
      '<span>调用者地址: <b class="heap-low-caller">0x' + (Number.isFinite(r.hook_caller_addr) && Number(r.hook_caller_addr) > 0 ? r.hook_caller_addr.toString(16).toUpperCase() : '--') + '</b>' +
        ' <span class="heap-low-caller-hint">（addr2line）</span></span>' +
    '</div>' +
    '<table class="heap-low-stack-table"><thead><tr><th>任务</th><th>剩余栈</th></tr></thead>' +
    '<tbody>' + (stackRows || '<tr><td colspan="2">无</td></tr>') + '</tbody>' +
    '</table>' +
  '</td></tr>';
}

function buildHeapLowSectionHtml(records, segTag, title, totalCount) {
  if (!records || !records.length) return '';
  var rows = records.map(function (r, i) { return buildHeapLowRowHtml(r, i, segTag); }).join('');
  return '<div class="heap-low-section heap-low-section-' + segTag + '">' +
    '<div class="heap-low-section-title">' + title +
      ' <span class="heap-low-sub">' + records.length + ' 条' +
      (totalCount !== undefined && totalCount > records.length ? ' / 共 ' + totalCount : '') +
      '</span>' +
    '</div>' +
    '<table class="heap-low-table">' +
      '<thead><tr>' +
        '<th>启动后</th><th>最低空闲</th><th>当时空闲</th><th>最大块</th>' +
        '<th>OTA状态</th><th>连接</th><th>OTA进度</th><th>默认堆</th>' +
        '<th>诊断</th><th>最危险栈 ▾</th>' +
      '</tr></thead>' +
      '<tbody>' + rows + '</tbody>' +
    '</table>' +
  '</div>';
}

function buildHeapLowTooltipHtml(data, history) {
  var live = (data && Array.isArray(data.records)) ? data.records : [];
  var hist = Array.isArray(history) ? history : [];
  var bodyHtml;
  if (!live.length && !hist.length) {
    bodyHtml = '<div class="heap-low-empty">尚无"最低空闲创新低"记录</div>';
  } else {
    // 设备当前段：live 已是最新在前
    var liveHtml = buildHeapLowSectionHtml(live, 'live', '设备当前（仅最新 1 条）', hist.length + live.length);
    // 历史段：hist 升序（旧→新），显示时反转成新的在前
    var histDesc = hist.slice().reverse();
    var liveKeys = {};
    live.forEach(function (r) {
      liveKeys[r.ts_ms + '|' + r.min_free_heap + '|' + r.free_heap + '|' + (r.alloc_count || 0)] = true;
    });
    var histOnly = histDesc.filter(function (r) {
      return !liveKeys[r.ts_ms + '|' + r.min_free_heap + '|' + r.free_heap + '|' + (r.alloc_count || 0)];
    });
    var histHtml = histOnly.length
      ? buildHeapLowSectionHtml(histOnly, 'hist', '前端累积历史', hist.length)
      : '';
    bodyHtml = liveHtml + histHtml;
  }
  return '' +
    '<div class="heap-low-title">堆"最低空闲"创新低记录' +
      '<span class="heap-low-sub">设备 ' + live.length + ' 条 + 历史 ' + hist.length + ' 条</span>' +
      '<button class="heap-low-clear" type="button" title="清空浏览器历史（不访问设备）">清空</button>' +
      '<button class="heap-low-close" type="button" aria-label="关闭" title="关闭">&times;</button>' +
    '</div>' +
    '<div class="heap-low-table-wrap">' + bodyHtml + '</div>' +
    '<div class="heap-low-tip">设备只保留最新 1 条；多条历史在浏览器 IndexedDB（最多 '
      + HEAP_HISTORY_MAX
      + ' 条，约 5s 拉取 + status 新低时立即拉取）。「清空」只清本地历史，不访问设备。状态栏「最低空闲」需重启才清。</div>';
}

function positionHeapLowTooltip(anchorEl, tipEl) {
  // 默认放右下；越界时翻左侧/上侧。tip 先 measure 再定位。
  tipEl.style.visibility = 'hidden';
  tipEl.style.left = '0px';
  tipEl.style.top  = '0px';
  document.body.appendChild(tipEl);
  var r = anchorEl.getBoundingClientRect();
  var tw = tipEl.offsetWidth;
  var th = tipEl.offsetHeight;
  var pad = 8;
  var left = r.right + pad;
  if (left + tw > window.innerWidth - 8) left = r.left - tw - pad;
  if (left < 8) left = Math.max(8, r.right - tw);
  var top = r.bottom + pad;
  if (top + th > window.innerHeight - 8) top = r.top - th - pad;
  if (top < 8) top = 8;
  tipEl.style.left = left + 'px';
  tipEl.style.top  = top + 'px';
  tipEl.style.visibility = 'visible';
}

function showHeapLowTooltip(anchorEl) {
  // 并行加载：设备当前 1 条 + 浏览器 IndexedDB 累积历史
  // force=true：用户主动查看，OTA 期也允许试一次（失败静默）
  Promise.all([
    loadHeapLowLog(true).catch(function () { return { records: [] }; }),
    loadHeapLowHistory().catch(function () { return []; })
  ]).then(function (results) {
    var data = results[0] || { records: [] };
    var history = results[1] || [];
    if (heapLowTooltipEl) heapLowTooltipEl.remove();
    var el = document.createElement('div');
    el.className = 'heap-low-tooltip';
    el.innerHTML = buildHeapLowTooltipHtml(data, history);
    heapLowTooltipEl = el;
    positionHeapLowTooltip(anchorEl, el);
    // 关闭按钮：显式关闭并阻止 click 冒泡触发"点击外部关闭"
    var closeBtn = el.querySelector('.heap-low-close');
    if (closeBtn) {
      closeBtn.addEventListener('click', function(e) {
        e.preventDefault();
        e.stopPropagation();
        hideHeapLowTooltip();
      });
    }
    var clearBtn = el.querySelector('.heap-low-clear');
    if (clearBtn) {
      clearBtn.addEventListener('click', function(e) {
        e.preventDefault();
        e.stopPropagation();
        clearHeapLowRecordsUi(clearBtn, anchorEl);
      });
    }
    // 点击主行展开/收起对应任务栈详情子表
    var mainRows = el.querySelectorAll('.heap-low-main-row');
    mainRows.forEach(function(row) {
      row.addEventListener('click', function(e) {
        e.preventDefault();
        e.stopPropagation();
        var idx = row.getAttribute('data-idx');
        if (idx === null) return;
        var detail = el.querySelector('.heap-low-stack-detail[data-idx="' + idx + '"]');
        if (detail) detail.hidden = !detail.hidden;
      });
    });
  });
}

/**
 * 清空浏览器侧堆低水位历史（IndexedDB）。
 * 不再 POST 设备：设备仅 1 槽，多条历史全在本地；状态栏 min_free 仍要重启才清。
 */
async function clearHeapLowRecordsUi(btn, anchorEl) {
  if (btn && btn.disabled) return;
  if (!window.confirm(
    '清空浏览器中的堆低水位历史（IndexedDB，最多 '
    + HEAP_HISTORY_MAX
    + ' 条）？\n\n不会访问设备。状态栏「最低空闲」芯片累计需重启才清零。'
  )) {
    return;
  }
  if (btn) {
    btn.disabled = true;
    btn.textContent = '清空中…';
  }
  var localOk = false;
  try {
    localOk = !!(await clearHeapLowHistory());
  } catch (e2) {
    localOk = false;
  }
  heapLowLogCache = null;
  heapLowLogLoadAt = 0;
  lastSeenInternalMinHeap = null;

  if (localOk) {
    addLog('I', '内存', '已清空堆低水位本地历史');
    showToast('已清空本地历史', 'success');
  } else {
    addLog('E', '内存', '清空本地历史失败');
    showToast('清空失败', 'error');
    if (btn) {
      btn.disabled = false;
      btn.textContent = '清空';
    }
    return;
  }
  if (anchorEl) {
    showHeapLowTooltip(anchorEl);
  } else {
    hideHeapLowTooltip();
  }
}

function hideHeapLowTooltip() {
  if (heapLowTooltipEl) { heapLowTooltipEl.remove(); heapLowTooltipEl = null; }
}

/** 绑定到 #mInternalMinHeap 元素（仅绑一次）。click 切换显示/隐藏。 */
export function bindHeapLowTooltip() {
  var el = document.getElementById('mInternalMinHeap');
  if (!el || el.dataset.heapLowBound === '1') return;
  el.dataset.heapLowBound = '1';
  el.style.cursor = 'pointer';
  el.title = '点击查看堆低水位创新低记录';

  // 点击触发：已显示则关闭，未显示则拉接口显示
  el.addEventListener('click', function(e) {
    e.preventDefault();
    e.stopPropagation();
    if (heapLowTooltipEl) {
      hideHeapLowTooltip();
    } else {
      showHeapLowTooltip(el);
    }
  });

  // 点击 tooltip 外部关闭（点击 tooltip 自身或其子元素不关闭）
  document.addEventListener('click', function(e) {
    if (!heapLowTooltipEl) return;
    if (e.target === el) return;  // 触发元素自身已单独处理
    var node = e.target;
    while (node && node !== document) {
      if (node === heapLowTooltipEl) return;
      node = node.parentNode;
    }
    hideHeapLowTooltip();
  }, true);

  // 页面整体滚动/resize 时关闭（tooltip 内部滚动不触发，由 e.target 父链判断）
  window.addEventListener('scroll', function(e) {
    if (!heapLowTooltipEl) return;
    var node = e.target;
    while (node && node !== document) {
      if (node === heapLowTooltipEl) return;
      node = node.parentNode;
    }
    hideHeapLowTooltip();
  }, true);
  window.addEventListener('resize', function() {
    if (heapLowTooltipEl) hideHeapLowTooltip();
  });

  // 后台拉取 heap_low_log → IndexedDB 多条历史（设备仅 1 槽）
  startHeapLowBackgroundPoll();
}

// ==================== 堆低水位历史：后台拉取 + 弹窗合并显示 ====================
var heapLowBgTimerId = null;
/** 与 health_mon 5s 巡检对齐，尽量在下一次创新低覆盖前把当前槽拉进 IDB */
var HEAP_LOW_BG_INTERVAL_MS = 5000;
/** 上次从 /system/status 见到的 min_free，用于检测新低并立刻拉 log */
var lastSeenInternalMinHeap = null;

/**
 * status 轮询里调用：若 internal_min_free_heap 再创新低，立刻拉设备槽写入 IDB。
 * 这样不必等 5s 后台定时，连续创新低时也能多攒几条（每条仍须设备侧先记一次）。
 */
export function onStatusHeapMinMaybeChanged(data) {
  if (!data) return;
  var minH = data.internal_min_free_heap;
  if (minH === undefined) minH = data.min_free_heap;
  minH = Number(minH);
  if (!Number.isFinite(minH)) return;
  if (lastSeenInternalMinHeap === null) {
    lastSeenInternalMinHeap = minH;
    return;
  }
  if (minH < lastSeenInternalMinHeap) {
    lastSeenInternalMinHeap = minH;
    /* force=false：OTA 忙态跳过，避免与上传/status 抢 socket 后 abort 刷屏 */
    if (!document.hidden && !isOtaBusyForHeapLowPoll()) {
      loadHeapLowLog(false).catch(function () {});
    }
  } else if (minH > lastSeenInternalMinHeap) {
    /* 设备重启后 min 回升：同步基线，不强制拉 */
    lastSeenInternalMinHeap = minH;
  }
}

/** 启动后台拉取。多次调用幂等。 */
function startHeapLowBackgroundPoll() {
  if (heapLowBgTimerId) return;
  function tick() {
    heapLowBgTimerId = null;
    if (!document.hidden && !isOtaBusyForHeapLowPoll()) {
      loadHeapLowLog(false).catch(function () {});
    }
    heapLowBgTimerId = setTimeout(tick, HEAP_LOW_BG_INTERVAL_MS);
  }
  heapLowBgTimerId = setTimeout(tick, 3000);
}
