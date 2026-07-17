// ==================== BLE OTA Stage Flow Management ====================

export var bleStageIds = ['bs1','bs2','bs3','bs4','bs5','bs6'];
export var bleStageDetailIds = ['bsd1','bsd2','bsd3','bsd4','bsd5','bsd6'];
export var bleStageLabelIds = ['bsl5', 'bsl6'];

/* 状态栏 DOM 元素懒缓存（BLE OTA 高频调用路径用，避免每次 getElementById）。
 * DOM 未就绪或节点被外部重建时回退到 getElementById（与 ota/progress.js 的 setProgress 同款模式）。 */
var stageStatusBarEl = null;
var stageStatusTextEl = null;
/* 当前激活阶段序号（1-based，0=无激活）。由 setBleStage 维护，供 setBleStageError
 * 直接定位错误节点，避免依赖 className 字符串匹配（脆弱且易被外部改动打破）。 */
var currentBleStageIndex = 0;

function getStageStatusBar() {
  if (!stageStatusBarEl || !stageStatusBarEl.isConnected) {
    stageStatusBarEl = document.getElementById('stageStatusBar');
  }
  return stageStatusBarEl;
}

function getStageStatusText() {
  if (!stageStatusTextEl || !stageStatusTextEl.isConnected) {
    stageStatusTextEl = document.getElementById('stageStatusText');
  }
  return stageStatusTextEl;
}

export function updateBleStageLabels(cfg) {
  /* JS 兜底默认值与 constants.js TARGETS.esp32/gd32 的 stage5Label/stage6Label 对齐：
   *   esp32: stage5Label='校验比对', stage6Label='完成触发'
   *   gd32:   stage5Label='完成触发', stage6Label='校验比对'
   * 实际 cfg 由 currentTarget() 返回，始终带 label，此分支仅为防御性兜底。 */
  var stage5 = document.getElementById(bleStageLabelIds[0]);
  var stage6 = document.getElementById(bleStageLabelIds[1]);
  if (stage5) stage5.textContent = cfg.stage5Label || '校验比对';
  if (stage6) stage6.textContent = cfg.stage6Label || '完成触发';
}

export function resetBleStages() {
  bleStageIds.forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.className = 'stage-node';
  });
  bleStageDetailIds.forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.textContent = '';
  });
  currentBleStageIndex = 0;
  /* ★ 只改 className + textContent，不重写 innerHTML：
   *   1) 保留 #stageStatusDot（CSS dotPulse 动画依赖该节点，重写后 id 丢失）；
   *   2) 保留 #stageStatusText 节点引用，避免外部缓存引用失效；
   *   3) 与 ota/progress.js、ota/stages.js、app.js 的状态栏操作约定一致。 */
  var bar = getStageStatusBar();
  if (bar) bar.className = 'stage-status-bar';
  var txt = getStageStatusText();
  if (txt) txt.textContent = '待机';
}

export function setBleStage(stageNum, detail, barState) {
  // stageNum: 1-6, detail: sub-step text
  // 终态 stageNum===末步：全部 done（绿）；中间步当前为 live（蓝）
  var stageCount = bleStageIds.length;
  // 边界守卫：stageNum<1 直接清空所有节点（不推进 currentBleStageIndex），避免误判 active
  if (!Number.isFinite(stageNum) || stageNum < 1) {
    bleStageIds.forEach(function(id) {
      var el = document.getElementById(id);
      if (el) el.className = 'stage-node';
    });
    currentBleStageIndex = 0;
    return;
  }
  if (stageNum > stageCount) stageNum = stageCount;
  currentBleStageIndex = stageNum;
  var finished = stageNum >= stageCount;
  for (var i = 0; i < stageCount; i++) {
    var el = document.getElementById(bleStageIds[i]);
    if (!el) continue;
    if (i + 1 < stageNum) el.className = 'stage-node done';
    else if (i + 1 === stageNum) el.className = finished ? 'stage-node done' : 'stage-node live';
    else el.className = 'stage-node';
  }
  // Set detail text
  if (stageNum >= 1 && stageNum <= stageCount) {
    var detailEl = document.getElementById(bleStageDetailIds[stageNum - 1]);
    if (detailEl && detail !== undefined) detailEl.textContent = detail;
  }
  // Update status bar
  var bar = getStageStatusBar();
  if (bar) {
    var cls = barState || (finished ? 'ok' : 'active');
    bar.className = 'stage-status-bar' + (cls ? ' ' + cls : '');
  }
}

export function setBleStageText(text) {
  var txt = getStageStatusText();
  if (txt) txt.textContent = text;
}

/** @description 一次性更新状态栏的样式与文本（避免每次通知都重复 getElementById）。 */
export function setBleStageStatus(state, text) {
  var bar = getStageStatusBar();
  if (bar) bar.className = 'stage-status-bar' + (state ? ' ' + state : '');
  var txt = getStageStatusText();
  if (txt && text !== undefined) txt.textContent = text;
}

export function setBleStageError(detail) {
  // 直接用 currentBleStageIndex 定位错误节点（由 setBleStage 维护），
  // 不再依赖 className === 'stage-node live' 字符串匹配——后者在 className
  // 顺序变化或被外部改动时会漏标错误阶段。
  var stageCount = bleStageIds.length;
  if (currentBleStageIndex >= 1 && currentBleStageIndex <= stageCount) {
    var el = document.getElementById(bleStageIds[currentBleStageIndex - 1]);
    if (el) el.className = 'stage-node err';
    var detailEl = document.getElementById(bleStageDetailIds[currentBleStageIndex - 1]);
    if (detailEl && detail) detailEl.textContent = detail;
  }
  var bar = getStageStatusBar();
  if (bar) bar.className = 'stage-status-bar err';
  setBleStageText('错误: ' + (detail || '未知错误'));
}