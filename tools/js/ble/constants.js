// ==================== BLE OTA 协议常量 ====================
// 与 WiFi OTA 共享界面元素：进度条、阶段流程、日志面板、连接状态、文件选择。
// 仅传输层不同 (GATT vs HTTP)。通过 connectionMode 变量判断当前模式。

export const UUIDS = {
  service: 0xFFFF,
  ctrl: 0x0001,
  dataIn: 0x0002,
  dataOut: 0x0003,
  version: 0x0004,
};

export const UI_BUILD = '2026-07-12-ble-reorder-v6';
export const BLE_OTA_NOTIFY_ACK = 0xA1;
export const BLE_OTA_NOTIFY_FINISH = 0xB1;
export const BLE_OTA_NOTIFY_SHA256 = 0xC1;
export const BLE_OTA_ACK_WINDOW = 32;
export const BLE_OTA_FINISH_ESP32_DONE = 0x02;
export const BLE_OTA_FINISH_REBOOT_NOW = 0x07;
export const BLE_OTA_FINISH_GD32_RELAY_START = 0x10;
export const BLE_OTA_FINISH_GD32_RELAY_DONE = 0x11;
export const BLE_OTA_FINISH_ABORTED = 0x20;           // 用户主动中止（BLE 不断开）
export const BLE_OTA_CTRL_ABORT_OTA = 0xFB;           // 中止当前 OTA（不断开 BLE）
export const BLE_OTA_FINISH_ERR_FINALIZE = 0x81;
export const BLE_OTA_FINISH_ERR_MUTEX = 0x83;
export const BLE_OTA_FINISH_ERR_GD32_SIZE = 0x84;
export const BLE_OTA_FINISH_ERR_SHA_MISMATCH = 0x85;
export const BLE_OTA_FINISH_ERR_GD32_RELAY = 0x86;

export const TARGETS = {
  gd32: {
    label: 'GD32',
    packetCmd: 0x00,
    ctrlHex: 'F7',
    recommendedPayload: 480,
    finishText: '自动触发中继',
    hint: '当前目标：<b>GD32</b>。页面会使用 <b>cmd=0x00</b> 发送数据，写入完成后自动触发 SPI OTA 中继。',
    verifyStage: 6,
    finalizeStage: 5,
    stage5Label: '完成触发',
    stage6Label: '校验比对'
  },
  esp32: {
    label: 'ESP32',
    packetCmd: 0x01,
    ctrlHex: 'F8',
    recommendedPayload: 480,
    finishText: '自动完成 OTA',
    hint: '当前目标：ESP32。页面会使用 <b>cmd=0x01</b> 发送数据，写入完成后发 <b>0xF8</b> + 32 字节 SHA-256 触发设备端收尾：设备端读 OTA 分区算 SHA 并比对，一致则 end+set_boot+reboot；不一致上报 0x85，不重启。',
    verifyStage: 5,
    finalizeStage: 6,
    stage5Label: '校验比对',
    stage6Label: '完成触发'
  }
};

export const MAX_LOG_LINES = 1000;
