// ==================== BLE OTA Stage Flow Management ====================

export var bleStageIds = ['bs1','bs2','bs3','bs4','bs5','bs6'];
export var bleStageDetailIds = ['bsd1','bsd2','bsd3','bsd4','bsd5','bsd6'];
export var bleStageLabelIds = ['bsl5', 'bsl6'];

export function updateBleStageLabels(cfg) {
  var stage5 = document.getElementById(bleStageLabelIds[0]);
  var stage6 = document.getElementById(bleStageLabelIds[1]);
  if (stage5) stage5.textContent = cfg.stage5Label || '完成触发';
  if (stage6) stage6.textContent = cfg.stage6Label || '校验比对';
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
  var bar = document.getElementById('stageStatusBar');
  if (bar) { bar.className = 'stage-status-bar'; bar.innerHTML = '<span class="ssb-dot"></span><span class="ssb-text" id="stageStatusText">待机</span>'; }
}

export function setBleStage(stageNum, detail, barState) {
  // stageNum: 1-6, detail: sub-step text
  // 终态 stageNum===6：全部 done（绿）；中间步当前为 live（蓝）
  var finished = stageNum >= 6;
  for (var i = 0; i < bleStageIds.length; i++) {
    var el = document.getElementById(bleStageIds[i]);
    if (!el) continue;
    if (i + 1 < stageNum) el.className = 'stage-node done';
    else if (i + 1 === stageNum) el.className = finished ? 'stage-node done' : 'stage-node live';
    else el.className = 'stage-node';
  }
  // Set detail text
  if (stageNum >= 1 && stageNum <= 6) {
    var detailEl = document.getElementById(bleStageDetailIds[stageNum - 1]);
    if (detailEl && detail !== undefined) detailEl.textContent = detail;
  }
  // Update status bar
  var bar = document.getElementById('stageStatusBar');
  if (bar) {
    var cls = barState || (finished ? 'ok' : (stageNum > 0 ? 'active' : ''));
    bar.className = 'stage-status-bar' + (cls ? ' ' + cls : '');
  }
}

export function setBleStageText(text) {
  var txt = document.getElementById('stageStatusText');
  if (txt) txt.textContent = text;
}

export function setBleStageError(detail) {
  // Mark current active stage as error
  for (var i = 0; i < bleStageIds.length; i++) {
    var el = document.getElementById(bleStageIds[i]);
    if (el && el.className === 'stage-node live') {
      el.className = 'stage-node err';
      var detailEl = document.getElementById(bleStageDetailIds[i]);
      if (detailEl && detail) detailEl.textContent = detail;
    }
  }
  var bar = document.getElementById('stageStatusBar');
  if (bar) bar.className = 'stage-status-bar err';
  setBleStageText('错误: ' + (detail || ''));
}
