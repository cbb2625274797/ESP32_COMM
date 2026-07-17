// ==================== BLE OTA SHA-256 校验展示 ====================

export function showBleVerifyInfo(frontSha, devSha, result) {
  var panel = document.getElementById('stageVerifyInfo');
  if (panel) { panel.className = 'stage-verify-info'; panel.style.display = 'block'; }
  if (frontSha !== undefined) {
    var frontEl = document.getElementById('sviFrontSha');
    if (frontEl) frontEl.textContent = frontSha || '-';
  }
  if (devSha !== undefined) {
    var devEl = document.getElementById('sviDeviceSha');
    if (devEl) devEl.textContent = devSha || '-';
  }
  if (result) {
    var resultEl = document.getElementById('sviResult');
    if (resultEl) {
      resultEl.textContent = result.text || '-';
      resultEl.className = 'svi-val' + (result.ok ? ' ok' : result.err ? ' err' : '');
    }
  }
}

export function hideBleVerifyInfo() {
  var panel = document.getElementById('stageVerifyInfo');
  if (panel) { panel.className = 'stage-verify-info'; panel.style.display = 'none'; }
}