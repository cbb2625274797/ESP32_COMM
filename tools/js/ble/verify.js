// ==================== BLE OTA SHA-256 校验展示 ====================

export function showBleVerifyInfo(frontSha, devSha, result) {
  var panel = document.getElementById('stageVerifyInfo');
  if (panel) { panel.className = 'stage-verify-info'; panel.style.display = 'block'; }
  if (frontSha !== undefined) {
    var el = document.getElementById('sviFrontSha');
    if (el) el.textContent = frontSha || '-';
  }
  if (devSha !== undefined) {
    var el = document.getElementById('sviDeviceSha');
    if (el) el.textContent = devSha || '-';
  }
  if (result !== undefined) {
    var el = document.getElementById('sviResult');
    if (el) {
      el.textContent = result.text || '-';
      el.className = 'svi-val' + (result.ok ? ' ok' : result.err ? ' err' : '');
    }
  }
}

export function hideBleVerifyInfo() {
  var panel = document.getElementById('stageVerifyInfo');
  if (panel) { panel.className = 'stage-verify-info'; panel.style.display = 'none'; }
}
