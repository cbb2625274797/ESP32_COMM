/*
 * ble_ota_priv.h - BLE OTA 内部共享声明（仅 server/ble_ota*.cpp 使用）
 *
 * 拆分：
 *   ble_ota.cpp      — 单例/会话/收尾/公开 API
 *   ble_ota_data.cpp — 乱序缓冲、数据写路径、worker
 *   ble_ota_host.cpp — GATT/GAP/广播/Host
 */
#pragma once

#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "nimble/ble.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_att.h"

#include "app_config.h"
#include "version.h"
#include "ble_ota.h"
#include "esp32_ota_core.h"
#include "ota_coordinator.h"
#include "ota_relay.h"
#include "storage/gd32_flash_store.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"

#define TAG "BLE_OTA"
#define BLE_OTA_NOTIFY_ACK 0xA1
#define BLE_OTA_NOTIFY_FINISH 0xB1
#define BLE_OTA_NOTIFY_SHA256 0xC1
#define BLE_OTA_FINISH_DONE 0x02
#define BLE_OTA_FINISH_REBOOT_NOW 0x07
#define BLE_OTA_FINISH_GD32_RELAY_START 0x10
#define BLE_OTA_FINISH_GD32_RELAY_DONE 0x11
#define BLE_OTA_FINISH_ABORTED       0x20
#define BLE_ATT_ERR_OTA_BUSY 0x80
#define BLE_OTA_FINISH_ERR_FINALIZE 0x81
#define BLE_OTA_FINISH_ERR_MUTEX 0x83
#define BLE_OTA_FINISH_ERR_GD32_SIZE 0x84
#define BLE_OTA_FINISH_ERR_SHA_MISMATCH 0x85
#define BLE_OTA_FINISH_ERR_GD32_RELAY 0x86

struct BleOtaDataBatch {
    uint8_t *data;
    size_t   len;
};

struct BleGd32RelayWaitCtx {
    uint16_t session_id;
};

struct BleEsp32FinishCtx {
    char sha_hex[65];
    uint16_t session_id;
};

struct BleFinishCtxDeleter {
    BleEsp32FinishCtx *p;
    explicit BleFinishCtxDeleter(BleEsp32FinishCtx *ctx) : p(ctx) {}
    ~BleFinishCtxDeleter() { if (p) { free(p); p = nullptr; } }
    BleFinishCtxDeleter(const BleFinishCtxDeleter&) = delete;
    BleFinishCtxDeleter& operator=(const BleFinishCtxDeleter&) = delete;
};

extern QueueHandle_t s_otaDataQueue;
extern uint16_t s_data_out_val_handle;
extern bool s_wifi_ps_boosted;
/** GAP 连接参数回退请求标志（定义在 ble_ota_data.cpp，host 连接事件读写） */
extern bool s_conn_fallback_requested;

void drainOtaDataQueue();
void ble_ota_start_advertising(void);
void sendWindowAck(uint8_t cmd, uint16_t seq, uint32_t completed_bytes);
void sendFinishEvent(uint8_t stage, uint32_t value);
uint32_t packBleFinishValue(uint16_t session_id, uint16_t code);
void shaBytesToHexUpper(const uint8_t sha[32], char out_hex[65]);
void sendSha256Notify(const uint8_t sha[32], uint16_t session_id);
void esp32_reorder_free();
void ble_ota_session_reset(void);
void ble_ota_restore_wifi_ps(void);
void ble_ota_boost_wifi_ps(void);
void ble_ota_data_worker_task(void *pvParameters);
void ble_gd32_relay_wait_task(void *pvParameters);
int ble_ota_mbuf_to_buf(struct os_mbuf *om, uint8_t *buf, size_t buf_len);
void ble_ota_on_sync(void);
void ble_ota_on_reset(int reason);
void ble_ota_nimble_host_task(void *param);
int ble_ota_gap_event(struct ble_gap_event *event, void *arg);
uint16_t ble_ota_log_conn_params(uint16_t conn_handle, const char *phase);
int ble_ota_request_conn_params(uint16_t conn_handle, bool windows_compatible);
void ble_esp32_restart_task(void *pvParameters);
void ble_esp32_finish_and_reboot_task(void *pvParameters);
int ble_ota_ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg);
int ble_ota_data_in_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);
int ble_ota_data_out_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);
int ble_ota_version_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);

extern const struct ble_gatt_svc_def gatt_ota_svcs[];
