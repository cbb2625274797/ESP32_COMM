/**
 * base-url.js - 设备 IP / 基础 URL 公共助手
 *
 * 各页面共用 localStorage key dcu_esp_ip（OTA / CAN / 电机页一致）。
 * 本模块只提供纯 localStorage + 默认值的通用实现；页面特有的输入框回退
 * （如 CAN 页 el.toolIp、OTA 页 #espIp）由各自入口在此基础上包装。
 */
export var LS_ESP_IP = 'dcu_esp_ip';
export var DEFAULT_ESP_IP = '192.168.31.127';

/** @description 读取 localStorage 中保存的设备 IP，无有效值返回空串。 */
export function getSavedDeviceIp() {
  try {
    var saved = localStorage.getItem(LS_ESP_IP);
    if (saved && saved.trim()) return saved.trim();
  } catch (_) {}
  return '';
}

/** @description 保存设备 IP 到 localStorage（空值忽略）。 */
export function saveDeviceIp(ip) {
  if (!ip || !String(ip).trim()) return;
  try { localStorage.setItem(LS_ESP_IP, String(ip).trim()); } catch (_) {}
}

/** @description 设备 IP：localStorage 优先，否则默认值。 */
export function getDeviceIp() {
  return getSavedDeviceIp() || DEFAULT_ESP_IP;
}

/** @description 拼接设备基础 URL（http://<ip>）。 */
export function getBaseUrl() {
  return 'http://' + getDeviceIp();
}
