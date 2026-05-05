/*
 * ble_ota.cpp - BLE OTA GATT 服务器实现 (ESP-IDF 6.0 NimBLE)
 * T5.1 - T5.5
 *
 * 通过 BLE GATT 接收上位机固件，存储到 Flash (gd32_ota 分区)，
 * 接收完成后触发 OTA 中继任务转发到 GD32。
 *
 * GATT 服务:
 *   Service UUID: 0xFFFF
 *   - CTRL (0x0001, Write):     写入 0xF7 触发中继
 *   - DATA_IN (0x0002, Write):  接收固件数据块
 *   - DATA_OUT (0x0003, Notify): 推送 GD32 响应
 *   - VERSION (0x0004, Read):   固件版本
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_err.h"

// NimBLE 协议栈
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_config.h"
#include "ble_ota.h"
#include "ota_relay.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"

#define TAG "BLE_OTA"

// ============================================================
// 静态状态
// ============================================================

static bool   s_ble_ready     = false;            // NimBLE 同步完成
static bool   s_ble_connected = false;            // GATT 客户端已连接
/** @brief 当前 BLE 连接句柄。 */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_data_out_val_handle = 0;        // DATA_OUT 值句柄 (供通知用)

// ============================================================
// NimBLE 工具: 从 mbuf 读取原始数据
/**
  * @brief 将 NimBLE mbuf 中的数据拷贝到普通字节缓冲区。
 * @param om NimBLE mbuf 数据指针。
 * @param buf 输出缓冲区。
 * @param buf_len 缓冲区最大长度。
 * @return 拷贝的字节数，失败返回 -1。
 */
static int ble_ota_mbuf_to_buf(struct os_mbuf *om, uint8_t *buf, size_t buf_len)
{
    if (!om || !buf) return -1;
    uint16_t om_len = OS_MBUF_PKTLEN(om);
    uint16_t copy_len = (om_len > buf_len) ? (uint16_t)buf_len : om_len;
    os_mbuf_copydata(om, 0, copy_len, buf);
    return (int)copy_len;
}

// ============================================================
// GATT 访问回调
// ============================================================

/*
 * CTRL (0x0001) - Write
 * 写入 0xF7 触发 OTA 中继
 */
static int ble_ota_ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    uint8_t buf[4] = {0};
    int len = ble_ota_mbuf_to_buf(ctxt->om, buf, sizeof(buf));
    if (len < 1) {
        DEBUG_WARN(TAG, "CTRL 数据为空");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    DEBUG_INFO(TAG, "CTRL 写入: cmd=0x%02X", buf[0]);

    if (buf[0] == 0xF7) {
        // 检查固件是否存储完毕
        ota_meta_t meta;
        esp_err_t err = meta_load(&meta);
        if (err == ESP_OK &&
            meta.total_size > 0 &&
            meta.stored_size >= meta.total_size) {
            DEBUG_INFO(TAG, "固件就绪 (%" PRIu32 " / %" PRIu32 " 字节)，触发中继",
                       meta.stored_size, meta.total_size);
            ble_ota_signal_relay();
        } else {
            DEBUG_WARN(TAG, "固件未就绪, 未触发中继 (stored=%" PRIu32 ", total=%" PRIu32 ")",
                       meta.stored_size, meta.total_size);
        }
    }

    return 0; // BLE_ATT_ERR_OK 在 NimBLE 中就是 0
}

/*
 * DATA_IN (0x0002) - Write
 * 数据块格式:
 *   [0]      cmd          (0x00=数据)
 *   [1-2]    seq          uint16 LE - 分块序号
 *   [3-6]    total_size   uint32 LE - 固件总大小 (仅 seq=0 时有效)
 *   [7..n-4] payload      固件数据
 *   [n-4..n] crc          校验 (预留)
 */
static int ble_ota_data_in_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    uint8_t buf[BLE_MTU_SIZE] = {0};
    int recv_len = ble_ota_mbuf_to_buf(ctxt->om, buf, sizeof(buf));

    if (recv_len < 12) {
        DEBUG_WARN(TAG, "DATA_IN 数据太短: %d 字节 (最少 12)", recv_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    // 解析包头
    uint8_t  cmd        = buf[0];
    uint16_t seq        = (uint16_t)(buf[1] | (buf[2] << 8));
    uint32_t total_size = (uint32_t)(buf[3] | (buf[4] << 8) |
                                     (buf[5] << 16) | (buf[6] << 24));

    size_t data_offset = 7;
    size_t data_len    = (size_t)recv_len - data_offset;

    // 尾 4 字节 CRC 预留，实际 payload = data_len - 4
    if (data_len < 4) {
        DEBUG_WARN(TAG, "DATA_IN 负载不足: data_len=%zu", data_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    data_len -= 4;  // 减去尾部 CRC 预留

    DEBUG_DBG(TAG, "DATA_IN: cmd=0x%02X seq=%u total=%" PRIu32 " payload=%zu",
              cmd, seq, total_size, data_len);

    // --- 首个分块: 创建存储元数据 ---
    if (seq == 0 && total_size > 0) {
        if (total_size > OTA_MAX_FIRMWARE_SIZE) {
            DEBUG_ERR(TAG, "固件太大: %" PRIu32 " > %zu",
                      total_size, OTA_MAX_FIRMWARE_SIZE);
            return BLE_ATT_ERR_UNLIKELY;
        }

        ota_meta_t meta = {};
        meta.total_size  = total_size;
        meta.stored_size = 0;
        meta.last_seq    = 0;
        meta.chunk_count = (total_size + (OTA_BLOCK_SIZE - 1)) / OTA_BLOCK_SIZE;

        esp_err_t err = meta_save(&meta);
        if (err != ESP_OK) {
            DEBUG_ERR(TAG, "元数据保存失败");
            return BLE_ATT_ERR_UNLIKELY;
        }

        DEBUG_INFO(TAG, "固件存储开始: total=%" PRIu32 " chunk_count=%" PRIu32,
                   meta.total_size, meta.chunk_count);
    }

    // --- 写入 Flash ---
    uint32_t flash_offset = seq * OTA_BLOCK_SIZE;
    esp_err_t err = flash_write_chunk(flash_offset, &buf[data_offset], data_len);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "Flash 写入失败: seq=%u offset=%" PRIu32 " err=%s",
                  seq, flash_offset, esp_err_to_name(err));
        return BLE_ATT_ERR_UNLIKELY;
    }

    err = meta_update(flash_offset + data_len, seq);
    if (err != ESP_OK) {
        DEBUG_WARN(TAG, "元数据更新失败 (seq=%u)", seq);
    }

    // --- 检查是否接收完毕 ---
    ota_meta_t meta;
    if (meta_load(&meta) == ESP_OK &&
        meta.total_size > 0 &&
        meta.stored_size >= meta.total_size) {
        DEBUG_INFO(TAG, "★ 固件接收完毕: %" PRIu32 " / %" PRIu32 " 字节 (共 %u 块)",
                   meta.stored_size, meta.total_size, seq + 1);
    }

    return 0;
}

/*
 * VERSION (0x0004) - Read
 * 返回固件版本字符串
 */
static int ble_ota_version_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    const char *version = "1.0.0";
    int rc = os_mbuf_append(ctxt->om, version, strlen(version));
    return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
}

/*
 * DATA_OUT (0x0003) - Notify only
 * 访问回调不做读写处理，通知由 ble_ota_notify_response() 发送
 */
static int ble_ota_data_out_access(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

// ============================================================
// GATT 服务定义
// ============================================================

// BLE_UUID16_DECLARE 在 C++ 中产生 "taking address of rvalue" 编译错误。
// 改用 static ble_uuid16_t 变量，再取 .u 地址。
/** @brief OTA GATT 服务 UUID */
static const ble_uuid16_t GATT_SVC_UUID      = BLE_UUID16_INIT(BLE_OTA_SERVICE_UUID);
/** @brief CTRL 特征 UUID，写入 0xF7 触发中继 */
static const ble_uuid16_t GATT_CTRL_UUID     = BLE_UUID16_INIT(BLE_OTA_CTRL_UUID);
/** @brief DATA_IN 特征 UUID，接收固件数据块 */
static const ble_uuid16_t GATT_DATA_IN_UUID  = BLE_UUID16_INIT(BLE_OTA_DATA_IN_UUID);
/** @brief DATA_OUT 特征 UUID，Notify 推送 GD32 响应 */
static const ble_uuid16_t GATT_DATA_OUT_UUID = BLE_UUID16_INIT(BLE_OTA_DATA_OUT_UUID);
/** @brief VERSION 特征 UUID，返回固件版本字符串 */
static const ble_uuid16_t GATT_VERSION_UUID  = BLE_UUID16_INIT(BLE_OTA_VERSION_UUID);

// NimBLE C 结构体在 C++ designated initializer 不能显式初始化全部字段。
// 用 pragma 局部关闭 -Wmissing-field-initializers 警告。
#ifdef __cplusplus
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

static const struct ble_gatt_chr_def gatt_ota_chrs[] = {
    {
        .uuid       = &GATT_CTRL_UUID.u,
        .access_cb  = ble_ota_ctrl_access,
        .flags      = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid       = &GATT_DATA_IN_UUID.u,
        .access_cb  = ble_ota_data_in_access,
        .flags      = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid       = &GATT_DATA_OUT_UUID.u,
        .access_cb  = ble_ota_data_out_access,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_data_out_val_handle,
    },
    {
        .uuid       = &GATT_VERSION_UUID.u,
        .access_cb  = ble_ota_version_access,
        .flags      = BLE_GATT_CHR_F_READ,
    },
    {}  // 终止符
};

static const struct ble_gatt_svc_def gatt_ota_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &GATT_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def *)gatt_ota_chrs,
    },
    {}  // 终止符
};

#ifdef __cplusplus
#pragma GCC diagnostic pop
#endif

// ============================================================
// GAP 事件回调
// ============================================================
/**
   * @brief BLE GAP 事件回调，处理连接和断开事件。
 * @param event
 * @param arg
 * @return 0 成功。
 */
static int ble_ota_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            // 连接成功
            s_ble_connected = true;
            /** @brief 当前 BLE 连接句柄。 */
            s_conn_handle   = event->connect.conn_handle;
            DEBUG_INFO(TAG, "BLE 客户端已连接: handle=%d", s_conn_handle);
        } else {
            DEBUG_WARN(TAG, "BLE 连接失败: status=%d", event->connect.status);
            /** @brief BLE GATT 客户端连接状态。 */
            s_ble_connected = false;
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        DEBUG_INFO(TAG, "BLE 客户端已断开: reason=%d",
                   event->disconnect.reason);
        /** @brief BLE GATT 客户端连接状态。 */
        s_ble_connected = false;
        /** @brief 当前 BLE 连接句柄。 */
        s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
        // 恢复广播以接受新连接
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                          NULL, ble_ota_gap_event, NULL);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        DEBUG_INFO(TAG, "广播周期结束，重新开始");
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                          NULL, ble_ota_gap_event, NULL);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        DEBUG_INFO(TAG, "CCC 描述符更新: conn=%d attr=%d notify=%d indicate=%d",
                   event->subscribe.conn_handle,
                   event->subscribe.attr_handle,
                   event->subscribe.cur_notify,
                   event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_MTU:
        DEBUG_INFO(TAG, "MTU 协商: conn=%d mtu=%d",
                   event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

// ============================================================
// BLE 广播
// ============================================================
/**
   * @brief 启动 BLE 广播，使设备可被发现和连接。
 */
static void ble_ota_start_advertising(void)
{
    uint8_t adv_data[31] = {0};
    int     adv_pos = 0;

    // AD Flags: LE General Discoverable + BR/EDR Not Supported
    adv_data[adv_pos++] = 0x02;   // 长度
    adv_data[adv_pos++] = 0x01;   // 类型 = Flags
    adv_data[adv_pos++] = 0x06;   // 值

    // Complete Local Name
    const char *name = BLE_DEVICE_NAME;
    size_t name_len  = strlen(name);
    if (name_len > 29 - adv_pos - 2) {
        name_len = 29 - adv_pos - 2;  // 截断以适配广播包
    }
    adv_data[adv_pos++] = (uint8_t)(name_len + 1);
    adv_data[adv_pos++] = 0x09;   // 类型 = Complete Local Name
    memcpy(&adv_data[adv_pos], name, name_len);
    adv_pos += name_len;

    int rc = ble_gap_adv_set_data(adv_data, adv_pos);
    if (rc != 0) {
        DEBUG_ERR(TAG, "设置广播数据失败: %d", rc);
        return;
    }

    // 扫描响应: 包含 Service UUID 以帮助发现
    uint8_t scan_rsp[31] = {0};
    int     rsp_pos = 0;
    scan_rsp[rsp_pos++] = 0x11;   // 长度 (2 + 15)
    scan_rsp[rsp_pos++] = 0x09;   // 类型 = Complete Local Name
    name_len = strlen(name);
    if (name_len > (size_t)(31 - rsp_pos)) {
        name_len = 31 - rsp_pos;
    }
    memcpy(&scan_rsp[rsp_pos], name, name_len);
    rsp_pos += name_len;

    rc = ble_gap_adv_rsp_set_data(scan_rsp, rsp_pos);
    if (rc != 0) {
        DEBUG_WARN(TAG, "设置扫描响应数据失败: %d", rc);
    }

    // 开始广播
    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 从 kconfig 获取 BLE 地址类型，默认 Public
#ifdef CONFIG_BLE_ADDR_TYPE_PUBLIC
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
#else
    uint8_t own_addr_type = BLE_OWN_ADDR_RANDOM;
#endif

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_ota_gap_event, NULL);
    if (rc != 0) {
        DEBUG_ERR(TAG, "启动广播失败: %d", rc);
    } else {
        DEBUG_INFO(TAG, "BLE 广播已启动: \"%s\"", name);
    }
}

// ============================================================
// NimBLE 同步/重置回调
// ============================================================
/**
   * @brief NimBLE 协议栈同步完成回调，初始化广播。
 */
static void ble_ota_on_sync(void)
{
    DEBUG_INFO(TAG, "NimBLE 协议栈同步完成");

    // GATT 服务已在 ble_ota_server_start() 中注册，不再重复注册
    // 重复调用 ble_gatts_count_cfg / ble_gatts_add_svcs 会导致 BLE_HS_EBUSY

    s_ble_ready = true;
    ble_ota_start_advertising();
}

/**
  * @brief NimBLE 协议栈重置回调。
 * @param reason
 */
static void ble_ota_on_reset(int reason)
{
    DEBUG_WARN(TAG, "NimBLE 协议栈重置: reason=%d", reason);
    /** @brief NimBLE 协议栈同步完成标志。 */
    s_ble_ready = false;
}

// ============================================================
// NimBLE Host 任务（独立 FreeRTOS 任务）
// ============================================================
/**
   * @brief NimBLE 主机任务入口，处理协议栈事件循环。
 * @param param
 */
static void ble_ota_nimble_host_task(void *param)
{
    (void)param;
    DEBUG_INFO(TAG, "NimBLE Host 任务启动");
    nimble_port_run();
    // 正常情况下不会到达这里
    DEBUG_INFO(TAG, "NimBLE Host 任务退出");
}

// ============================================================
// 公开 API
// ============================================================
/**
  * @brief 启动 BLE GATT 服务器并注册 OTA 服务。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t ble_ota_server_start(void)
{
    int rc;

    DEBUG_INFO(TAG, "=== 初始化 BLE OTA GATT 服务器 ===");
    DEBUG_INFO(TAG, "  Device: %s", BLE_DEVICE_NAME);
    DEBUG_INFO(TAG, "  MTU:    %" PRIu16, BLE_MTU_SIZE);
    DEBUG_INFO(TAG, "  Svc:    0x%04X", BLE_OTA_SERVICE_UUID);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    DEBUG_INFO(TAG, "nimble_port_init 完成");

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        DEBUG_WARN(TAG, "设置设备名称失败: %d", rc);
    }

    // 5. 配置 Host 回调
    //    sync_cb: NimBLE 事件循环就绪时调用 (开始广播)
    //    reset_cb: NimBLE 重置时调用
    ble_hs_cfg.sync_cb  = ble_ota_on_sync;
    ble_hs_cfg.reset_cb = ble_ota_on_reset;
    // 不做 bonding，不需要 store 回调

    rc = ble_gatts_count_cfg(gatt_ota_svcs);
    if (rc != 0) {
        DEBUG_ERR(TAG, "GATT 服务计数失败: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_ota_svcs);
    if (rc != 0) {
        DEBUG_ERR(TAG, "GATT 服务注册失败: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(ble_ota_nimble_host_task);

    DEBUG_INFO(TAG, "BLE OTA 服务器初始化完成");
    return ESP_OK;
}

/**
  * @brief 通过 BLE Notify 将 GD32 响应推送给上位机。
 * @param data 待推送的数据指针。
 * @param len 数据长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t ble_ota_notify_response(const uint8_t *data, size_t len)
{
    if (!s_ble_connected) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_data_out_val_handle == 0) {
        DEBUG_WARN(TAG, "DATA_OUT 值句柄未设置");
        return ESP_ERR_INVALID_STATE;
    }

    if (len == 0) return ESP_OK;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        DEBUG_ERR(TAG, "分配 mbuf 失败 (%zu 字节)", len);
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gattc_notify_custom(s_conn_handle, s_data_out_val_handle, om);
    if (rc != 0) {
        DEBUG_WARN(TAG, "通知发送失败: rc=%d", rc);
        return ESP_FAIL;
    }

    DEBUG_DBG(TAG, "通知已发送: %zu 字节", len);
    return ESP_OK;
}

/**
  * @brief 检查当前是否有 BLE 客户端连接。
 * @return true 已连接，false 未连接。
 */
bool ble_ota_is_connected(void)
{
    /** @brief BLE GATT 客户端连接状态。 */
    return s_ble_connected;
}

/**
  * @brief 通知中继模块开始向 GD32 转发固件。
 */
void ble_ota_signal_relay(void)
{
    if (g_ota_dispatch_queue == NULL) {
        DEBUG_ERR(TAG, "OTA 调度队列未初始化");
        return;
    }

    uint32_t msg = 1;
    BaseType_t ret = xQueueSend(g_ota_dispatch_queue, &msg, pdMS_TO_TICKS(200));
    if (ret != pdTRUE) {
        DEBUG_ERR(TAG, "OTA 调度信号发送失败: 队列满");
        return;
    }

    DEBUG_INFO(TAG, "OTA 调度信号已发送 → ota_relay_task");
}

// ============================================================
// BLE OTA FreeRTOS 任务入口
//
// 注意: 此任务的职责是初始化 BLE 服务器并保持存活。
// 实际的 BLE 事件循环在独立的 NimBLE Host 任务中运行。
// 为避免与 ota_relay_task 竞争，此任务不从 g_ota_dispatch_queue 消费。
// ============================================================
/**
  * @brief BLE OTA FreeRTOS 任务，初始化 NimBLE 并运行 GATT 服务。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void ble_ota_task(void *pvParameters)
{
    (void)pvParameters;
    DEBUG_INFO(TAG, "=== BLE OTA 任务启动 ===");

    esp_err_t err = ble_ota_server_start();
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "BLE 服务器启动失败: %s → BLE OTA 不可用",
                  esp_err_to_name(err));
    }

    // 保持任务存活 — 所有 BLE 事件由 NimBLE Host 任务处理
    uint32_t heartbeat = 0;
    while (1) {
        heartbeat++;

        // 每 60 秒打印一次心跳（确认任务存活）
        if (heartbeat % 6 == 0) {
            DEBUG_DBG(TAG, "心跳 #%lu | BLE %s | 连接: %s",
                      (unsigned long)(heartbeat / 6),
                      s_ble_ready ? "就绪" : "未就绪",
                      s_ble_connected ? "已连接" : "未连接");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
