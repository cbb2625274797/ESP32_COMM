/*
 * ble_ota.cpp - BLE OTA GATT 服务器实现 (ESP-IDF 6.0 NimBLE)
 * 所有功能封装为 BleOta 类方法。
 * T5.1 - T5.5
 *
 * 通过 BLE GATT 接收上位机固件，支持两种目标:
 *   - GD32: 存入 Flash → 触发 SPI 中继转发到 GD32
 *   - ESP32: 直接写入 OTA 分区 → 触发重启升级自身
 *
 * GATT 服务:
 *   Service UUID: 0xFFFF
 *   - CTRL (0x0001, Write):     0xF7=触发GD32中继, 0xF8=触发ESP32 OTA升级（含 32B SHA-256 校验并 reboot），0xFA=重启ESP32
 *   - DATA_IN (0x0002, Write):  cmd=0x00=GD32数据, cmd=0x01=ESP32数据
 *   - DATA_OUT (0x0003, Notify): 推送响应
 *   - VERSION (0x0004, Read):   固件版本
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

// NimBLE 协议栈
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_att.h"

// SHA-256 计算已抽离到 Esp32OtaCore，本文件不再直接引用 mbedtls

#include "app_config.h"
#include "ota_mutex.h"
#include "ble_ota.h"
#include "esp32_ota_core.h"
#include "ota_relay.h"
#include "health_monitor.h"
#include "monitor/debug_log.h"

#define TAG "BLE_OTA"
#define BLE_OTA_NOTIFY_ACK 0xA1
#define BLE_OTA_NOTIFY_FINISH 0xB1
#define BLE_OTA_NOTIFY_SHA256 0xC1   // 设备端 SHA-256 上报（[0xC1] + [32字节]）
#define BLE_OTA_FINISH_BEGIN_OTA_END 0x01
#define BLE_OTA_FINISH_DONE_OTA_END 0x02
#define BLE_OTA_FINISH_BEGIN_SET_BOOT 0x03
#define BLE_OTA_FINISH_DONE_SET_BOOT 0x04
#define BLE_OTA_FINISH_BEGIN_DELAY 0x05
#define BLE_OTA_FINISH_DONE_DELAY 0x06
#define BLE_OTA_FINISH_REBOOT_NOW 0x07
#define BLE_OTA_FINISH_ERR_OTA_END 0x81
#define BLE_OTA_FINISH_ERR_SET_BOOT 0x82
#define BLE_OTA_ACK_WINDOW 32
#define BLE_ATT_ERR_OTA_BUSY 0x80           // ESP32 OTA 被其他通道占用
#define BLE_OTA_FINISH_ERR_MUTEX 0x83       // 互斥拒绝: OTA 被其他通道占用
#define BLE_OTA_FINISH_ERR_GD32_SIZE 0x84   // GD32 固件超出分区大小
#define BLE_OTA_FINISH_ERR_SHA_MISMATCH 0x85 // ESP32 OTA SHA-256 不匹配

// ============================================================
// 前向声明
// ============================================================
static void ble_ota_start_advertising(void);
static void sendWindowAck(uint8_t cmd, uint16_t seq, uint32_t completed_bytes);
static void sendFinishEvent(uint8_t stage, uint32_t value);

// ============================================================
/**
  * @brief 获取 BleOta 单例实例。
 * @return BleOta 类引用。
 */
BleOta& BleOta::instance()
{
    static BleOta inst;
    return inst;
}

// File-scope val_handle for GATT service definition (struct literal needs compile-time address)
static uint16_t s_data_out_val_handle = 0;

// ESP32 自身 OTA 收尾任务（OTA 写入/校验/重启逻辑委托给 Esp32OtaCore）

/**
 * @brief 异步完成 ESP32 OTA 收尾并触发重启。
 *
 * 实际的 esp_ota_end / SHA 校验 / esp_ota_set_boot_partition / esp_restart 全部
 * 由 Esp32OtaCore::finalize 完成。本任务仅负责：补发 ACK → 调 finalize → 上报 SHA → 通知前端。
 * @param pvParameters 任务参数，当前未使用。
 */
/**
 * @brief 0xFA 重启任务：为了不卡 NimBLE host task，仅在这里推送 FINISH 后 500ms 重启。
 * @param pvParameters 任务参数，未使用。
 */
static void ble_esp32_restart_task(void *pvParameters)
{
    (void)pvParameters;
    DEBUG_INFO(TAG, "[RESTART] 推送 FINISH_REBOOT_NOW 后 500ms 重启");
    sendFinishEvent(BLE_OTA_FINISH_REBOOT_NOW, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    // 不会返回
}

/**
  * @brief 异步完成 ESP32 OTA 收尾并触发重启。
 * @param pvParameters 前端 SHA-256 十六进制字符串（堆分配，任务负责释放）。
 */
static void ble_esp32_finish_and_reboot_task(void *pvParameters)
{
    // pvParameters: heap-allocated char[65] frontend SHA hex string (or nullptr)
    char *frontend_sha_hex = (char *)pvParameters;
    DEBUG_INFO(TAG, "ESP32 OTA 异步收尾任务启动");

    Esp32OtaCore &core = Esp32OtaCore::instance();

    // 补发最终 ACK，让前端尽快得知最后一包已收到
    if (BleOta::instance().m_esp32LastSeq != 0xFFFFU) {
        sendWindowAck(BleOta::instance().m_esp32LastCmd,
                      BleOta::instance().m_esp32LastSeq,
                      (uint32_t)core.getReceived());
        DEBUG_INFO(TAG, "ESP32 最终 ACK 已补发: seq=%u", BleOta::instance().m_esp32LastSeq);
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    if (!core.isActive()) {
        DEBUG_WARN(TAG, "异步收尾时 OTA 状态无效，放弃重启");
        if (frontend_sha_hex) free(frontend_sha_hex);
        BleOta::instance().m_esp32RebootPending = false;
        vTaskDelete(NULL);
        return;
    }

    DEBUG_INFO(TAG, "ESP32 OTA 收尾: 共 %u 字节", (unsigned)core.getReceived());

    // 通知前端：开始 finalize
    sendFinishEvent(BLE_OTA_FINISH_BEGIN_OTA_END, 0);

    // 关键：reboot=false，让 finalize 真正返回、拿到 device_sha_hex，
    // 再依次推送 SHA 通知 + FINISH_DONE_OTA_END + FINISH_REBOOT_NOW，
    // 最后延迟重启。若 reboot=true，core 内部直接 esp_restart()，
    // 下面所有 notifyResponse / sendFinishEvent 都不会执行，
    // 前端也就拿不到设备端 SHA 与"收尾完成/即将重启"事件。
    char device_sha_hex[65] = {0};
    esp_err_t err = core.finalize(frontend_sha_hex,
                                  /*reboot=*/false,
                                  device_sha_hex);

    if (err == ESP_OK) {
        // 1) 把设备端 SHA 通过 SHA256 通知推给前端
        if (device_sha_hex[0] != '\0') {
            uint8_t sha_pkt[33] = {0};
            sha_pkt[0] = BLE_OTA_NOTIFY_SHA256;
            for (int i = 0; i < 32; ++i) {
                unsigned int byte = 0;
                sscanf(&device_sha_hex[i * 2], "%02x", &byte);
                sha_pkt[1 + i] = (uint8_t)byte;
            }
            BleOta::instance().notifyResponse(sha_pkt, sizeof(sha_pkt));
        }
        // 2) 等待 SHA 通知从 NimBLE host 队列送出
        vTaskDelay(pdMS_TO_TICKS(150));

        // 3) 推送"OTA 收尾完成"与"即将重启"事件
        sendFinishEvent(BLE_OTA_FINISH_DONE_OTA_END, 0);
        sendFinishEvent(BLE_OTA_FINISH_REBOOT_NOW, 0);

        // 4) 通知全部发出后延迟 500ms 重启
        DEBUG_INFO(TAG, "[FINISH] 通知已发送，500ms 后重启...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        // 不会返回
    } else if (err == ESP_ERR_INVALID_VERSION) {
        DEBUG_ERR(TAG, "[FINISH] SHA-256 不匹配，OTA 已中止");
        sendFinishEvent(BLE_OTA_FINISH_ERR_SHA_MISMATCH, 0);
        BleOta::instance().m_esp32RebootPending = false;
    } else {
        DEBUG_ERR(TAG, "[FINISH] OTA 收尾失败: %s", esp_err_to_name(err));
        sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)err);
        BleOta::instance().m_esp32RebootPending = false;
    }

    if (frontend_sha_hex) free(frontend_sha_hex);
    vTaskDelete(NULL);
}

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

// ─────────────────────────────────────────────────────────────
// ESP32 OTA 薄包装：实际 begin/write/finalize 已抽离到 Esp32OtaCore
// 此处仅保留 BLE 侧的状态复位与日志，供 data_in_access 调用。
// ─────────────────────────────────────────────────────────────

/**
 * @brief 初始化 ESP32 本机 OTA 写入会话。
 *        委托 Esp32OtaCore::begin，并复位 BLE 传输相关状态。
 * @param total_size 固件总大小（字节）。
 * @return ESP_OK 成功，其他为失败错误码。
 */
static esp_err_t ble_esp32_ota_begin(size_t total_size)
{
    esp_err_t err = Esp32OtaCore::instance().begin(total_size);
    if (err != ESP_OK) {
        return err;
    }
    // 复位 BLE 传输相关状态
    BleOta::instance().m_esp32RebootPending = false;
    BleOta::instance().m_esp32LastCmd = 0;
    BleOta::instance().m_esp32LastSeq = 0xFFFFU;
    return ESP_OK;
}

/**
 * @brief 中止 ESP32 OTA 会话并清理（薄包装）。
 */
static void ble_esp32_ota_abort()
{
    Esp32OtaCore::instance().abort();
    BleOta::instance().m_esp32RebootPending = false;
}

/**
 * @brief 发送窗口 ACK 通知，回报已处理的分块序号与累计字节数。
 * @param cmd 对应的数据命令字节。
 * @param seq 当前确认的分块序号。
 * @param completed_bytes 已完成写入的累计字节数。
 */
static void sendWindowAck(uint8_t cmd, uint16_t seq, uint32_t completed_bytes)
{
    uint8_t ack[8] = {0};
    ack[0] = BLE_OTA_NOTIFY_ACK;
    ack[1] = cmd;
    ack[2] = (uint8_t)(seq & 0xFFU);
    ack[3] = (uint8_t)((seq >> 8) & 0xFFU);
    ack[4] = (uint8_t)(completed_bytes & 0xFFU);
    ack[5] = (uint8_t)((completed_bytes >> 8) & 0xFFU);
    ack[6] = (uint8_t)((completed_bytes >> 16) & 0xFFU);
    ack[7] = (uint8_t)((completed_bytes >> 24) & 0xFFU);

    esp_err_t err = BleOta::instance().notifyResponse(ack, sizeof(ack));
    if (err != ESP_OK) {
        DEBUG_WARN(TAG, "发送 ACK 失败: cmd=0x%02X seq=%u err=%s",
                   cmd, seq, esp_err_to_name(err));
    } else {
        DEBUG_DBG(TAG, "已发送 ACK: cmd=0x%02X seq=%u bytes=%lu",
                  cmd, seq, (unsigned long)completed_bytes);
    }
}

/**
 * @brief 发送收尾阶段事件通知（开始/完成/失败/重启等）。
 * @param stage 阶段标识码。
 * @param value 阶段附带值（错误码或耗时等）。
 */
static void sendFinishEvent(uint8_t stage, uint32_t value)
{
    uint8_t pkt[6] = {0};
    pkt[0] = BLE_OTA_NOTIFY_FINISH;
    pkt[1] = stage;
    pkt[2] = (uint8_t)(value & 0xFFU);
    pkt[3] = (uint8_t)((value >> 8) & 0xFFU);
    pkt[4] = (uint8_t)((value >> 16) & 0xFFU);
    pkt[5] = (uint8_t)((value >> 24) & 0xFFU);
    esp_err_t err = BleOta::instance().notifyResponse(pkt, sizeof(pkt));
    if (err != ESP_OK) {
        DEBUG_WARN(TAG, "发送 FINISH 事件失败: stage=0x%02X err=%s",
                   stage, esp_err_to_name(err));
    }
}


// ============================================================
// GATT 访问回调
// ============================================================

/*
 * CTRL (0x0001) - Write
 * 写入 0xF7 触发 OTA 中继
 */
/**
 * @brief GATT CTRL 特征访问回调，处理控制命令写入。
 * @param conn_handle BLE 连接句柄。
 * @param attr_handle GATT 属性句柄。
 * @param ctxt GATT 访问上下文。
 * @param arg 回调用户参数。
 * @return NimBLE ATT 错误码，0 表示成功。
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

    uint8_t buf[36] = {0};
    int len = ble_ota_mbuf_to_buf(ctxt->om, buf, sizeof(buf));
    if (len < 1) {
        DEBUG_WARN(TAG, "CTRL 数据为空");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    DEBUG_INFO(TAG, "CTRL 写入: cmd=0x%02X len=%d", buf[0], len);

    if (buf[0] == BLE_OTA_CTRL_TRIGGER_GD32) {
        // 检查固件是否存储完毕 → 触发 GD32 OTA 中继
        ota_meta_t meta;
        esp_err_t err = OtaRelay::metaLoad(&meta);
        if (err == ESP_OK &&
            meta.total_size > 0 &&
            meta.stored_size >= meta.total_size) {
            DEBUG_INFO(TAG, "GD32 固件就绪 (%" PRIu32 " / %" PRIu32 " 字节)，触发中继",
                       meta.stored_size, meta.total_size);
            BleOta::instance().signalRelay();
        } else {
            DEBUG_WARN(TAG, "GD32 固件未就绪 (stored=%" PRIu32 ", total=%" PRIu32 ")",
                       meta.stored_size, meta.total_size);
        }
    } else if (buf[0] == BLE_OTA_CTRL_TRIGGER_ESP32) {
        if (!Esp32OtaCore::instance().isActive()) {
            DEBUG_WARN(TAG, "ESP32 OTA 未激活，无法触发升级");
            return 0;
        }
        if (BleOta::instance().m_esp32RebootPending) {
            DEBUG_WARN(TAG, "ESP32 OTA 收尾已在进行中，忽略重复 CTRL");
            return 0;
        }

        // 强制要求 32B 前端 SHA-256：与 WiFi 流程对齐，设备端读回分区计算并比对，
        // 一致走 end+set_boot+reboot，不一致返回 0x85 ERR_SHA_MISMATCH。
        if (len < 33) {
            DEBUG_WARN(TAG, "CTRL 0xF8 数据长度不足 33（前端 SHA-256 必传），拒绝触发");
            sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)ESP_ERR_INVALID_ARG);
            return 0;
        }

        char *frontend_sha_hex = (char *)malloc(65);
        if (!frontend_sha_hex) {
            DEBUG_ERR(TAG, "CTRL 0xF8 申请内存失败");
            sendFinishEvent(BLE_OTA_FINISH_ERR_OTA_END, (uint32_t)ESP_ERR_NO_MEM);
            return 0;
        }
        for (int i = 0; i < 32; ++i) {
            snprintf(&frontend_sha_hex[i * 2], 3, "%02x", buf[1 + i]);
        }
        frontend_sha_hex[64] = '\0';
        DEBUG_INFO(TAG, "前端 SHA-256: %s", frontend_sha_hex);

        BleOta::instance().m_esp32RebootPending = true;
        BaseType_t ok = xTaskCreate(ble_esp32_finish_and_reboot_task,
                                    "ble_ota_finish",
                                    4096,
                                    frontend_sha_hex,  // heap string; task 负责 free
                                    5,
                                    NULL);
        if (ok != pdPASS) {
            DEBUG_ERR(TAG, "创建 ESP32 OTA 收尾任务失败");
            free(frontend_sha_hex);
            BleOta::instance().m_esp32RebootPending = false;
        } else {
            DEBUG_INFO(TAG, "ESP32 OTA 收尾任务已创建（前端 SHA 已传入）");
        }
    } else if (buf[0] == BLE_OTA_CTRL_RESTART_ESP32) {
        // 重启 ESP32：GATT 回调中仅推送 FINISH，延时 + 重启交给独立 task，避免卡 NimBLE host
        DEBUG_INFO(TAG, "收到 0xFA 重启请求，创建 restart task（500ms 后 esp_restart()）");
        if (BleOta::instance().m_esp32RebootPending) {
            DEBUG_WARN(TAG, "0xFA 重启已在进行中，忽略重复 CTRL");
            return 0;
        }
        BleOta::instance().m_esp32RebootPending = true;
        BaseType_t ok = xTaskCreate(ble_esp32_restart_task,
                                    "ble_ota_restart",
                                    2048,
                                    NULL,
                                    5,
                                    NULL);
        if (ok != pdPASS) {
            DEBUG_ERR(TAG, "创建 0xFA 重启任务失败");
            BleOta::instance().m_esp32RebootPending = false;
        }
    }
    //   0xF7 = 触发 GD32 中继
    //   0xF8 = 触发 ESP32 本机 OTA 收尾：设备端读 OTA 分区算 SHA 并与前端 32B SHA 比对，
    //         一致走 end+set_boot+reboot，不一致返回 0x85 ERR_SHA_MISMATCH（不重启）
    //   0xFA = 重启 ESP32（配套前端 expectedReboot 自动重连）

    return 0; // BLE_ATT_ERR_OK 在 NimBLE 中就是 0
}

/*
 * DATA_IN (0x0002) - Write
 * 数据块格式:
 *   [0]      cmd          (0x00=数据)
 *   [1-2]    seq          uint16 LE - 分块序号
 *   [3-6]    total_size   uint32 LE - 固件总大小 (仅 seq=0 时有效)
 *   [7..n]   payload      固件数据
 * 注: 已移除CRC字段（BLE链路层已有CRC校验）
 */
/**
 * @brief GATT DATA_IN 特征访问回调，接收并处理 OTA 数据块。
 * @param conn_handle BLE 连接句柄。
 * @param attr_handle GATT 属性句柄。
 * @param ctxt GATT 访问上下文。
 * @param arg 回调用户参数。
 * @return NimBLE ATT 错误码，0 表示成功。
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

    // 使用较大静态缓冲区：writeValueWithoutResponse 快速发送时，
    // NimBLE 可能将多个写请求合并到同一个 mbuf 链中
    // ★ static: NimBLE host task 栈只有 4096 字节，不能用栈分配
    // ★ 4096 字节可容纳 8 个合并包（8×491=3928），避免截断丢包
    static constexpr size_t BLE_OTA_BUF_SIZE = 4096;
    static uint8_t buf[BLE_OTA_BUF_SIZE];
    int recv_len = ble_ota_mbuf_to_buf(ctxt->om, buf, sizeof(buf));

    if (recv_len < 8) {
        DEBUG_WARN(TAG, "DATA_IN 数据太短: %d 字节 (最少 8: 7头+1数据)", recv_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    // 循环处理 mbuf 中的所有完整数据包
    size_t offset = 0;
    while (offset + 7 <= (size_t)recv_len) {
        // 解析包头
        uint8_t  cmd        = buf[offset + 0];
        uint16_t seq        = (uint16_t)(buf[offset + 1] | (buf[offset + 2] << 8));
        uint32_t total_size = (uint32_t)(buf[offset + 3] | (buf[offset + 4] << 8) |
                                         (buf[offset + 5] << 16) | (buf[offset + 6] << 24));

        size_t data_offset = offset + 7;

        // 根据包序号和固件总大小计算期望 payload，而非依赖 recv_len
        // 这样即使 NimBLE mbuf 合并导致缓冲区截断，也不会误判包长度
        uint32_t expected_payload = (total_size > 0)
            ? ((uint32_t)BLE_OTA_CHUNK_PAYLOAD < (total_size - (uint32_t)seq * (uint32_t)BLE_OTA_CHUNK_PAYLOAD)
               ? (uint32_t)BLE_OTA_CHUNK_PAYLOAD
               : (total_size - (uint32_t)seq * (uint32_t)BLE_OTA_CHUNK_PAYLOAD))
            : (uint32_t)BLE_OTA_CHUNK_PAYLOAD;

        size_t data_len = (size_t)expected_payload;
        size_t remaining_after_header = (size_t)recv_len - data_offset;

        // 如果缓冲区中实际可用数据不足，截断 data_len
        if (remaining_after_header < expected_payload) {
            if (remaining_after_header == 0) {
                // 没有更多数据，退出循环
                break;
            }
            // 缓冲区截断，使用实际可用数据
            data_len = remaining_after_header;
        }

        // 当前包总长度: header(7) + payload(data_len)
        size_t packet_len = 7 + data_len;

        DEBUG_DBG(TAG, "DATA_IN: cmd=0x%02X seq=%u total=%" PRIu32 " payload=%zu offset=%zu",
                  cmd, seq, total_size, data_len, offset);

        // --- ESP32 自身 OTA：直接写 OTA 分区 ---
        if (cmd == BLE_OTA_CMD_ESP32_DATA) {
            if (seq == 0) {
                if (total_size == 0) {
                    DEBUG_ERR(TAG, "ESP32 固件大小非法: %" PRIu32, total_size);
                    return BLE_ATT_ERR_UNLIKELY;
                }
                // 分区选择/大小校验/互斥由 Esp32OtaCore::begin 内部完成
                esp_err_t err = ble_esp32_ota_begin(total_size);
                if (err != ESP_OK) {
                    if (err == ESP_ERR_INVALID_STATE) {
                        sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
                        return BLE_ATT_ERR_OTA_BUSY;
                    }
                    return BLE_ATT_ERR_UNLIKELY;
                }
            }

            esp_err_t err = Esp32OtaCore::instance().write(&buf[data_offset], data_len);
            if (err != ESP_OK) {
                DEBUG_ERR(TAG, "ESP32 OTA 写入失败: seq=%u err=%s",
                          seq, esp_err_to_name(err));
                ble_esp32_ota_abort();  // core 内部完成 abort + mutex release + 状态复位
                return BLE_ATT_ERR_UNLIKELY;
            }

            DEBUG_DBG(TAG, "ESP32 OTA 写入成功: seq=%u received=%u",
                      seq, (unsigned int)Esp32OtaCore::instance().getReceived());

            // 尾包检测: 期望 payload < 标准负载说明是真正的最后一包
            // 使用 expected_payload 而非 data_len，避免缓冲区截断导致误触发重启
            bool isLastPacket = (expected_payload < BLE_OTA_CHUNK_PAYLOAD);

            if ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U ||
                (total_size > 0 && Esp32OtaCore::instance().getReceived() >= total_size) ||
                isLastPacket) {
                if (isLastPacket) {
                    DEBUG_INFO(TAG, "ESP32 尾包检测: seq=%u data_len=%zu received=%u total=%" PRIu32,
                               seq, data_len, (unsigned int)Esp32OtaCore::instance().getReceived(), total_size);
                }
                sendWindowAck(cmd, seq, (uint32_t)Esp32OtaCore::instance().getReceived());
            }

            if ((total_size > 0 && Esp32OtaCore::instance().getReceived() >= total_size) || isLastPacket) {
                DEBUG_INFO(TAG, "★ ESP32 固件接收完毕: %u / %" PRIu32 " 字节 (共 %u 块)%s",
                           (unsigned int)Esp32OtaCore::instance().getReceived(), total_size, seq + 1,
                           isLastPacket ? " [尾包]" : "");
                BleOta::instance().m_esp32LastCmd = cmd;
                BleOta::instance().m_esp32LastSeq = seq;
                // 注：与 WiFi 流程对齐，传输完成后由前端发送 0xF8 + SHA-256 触发 finalize，
                //     设备端不再自动创建收尾任务。
            }
        }
        // --- GD32 OTA：首个分块时创建存储元数据 ---
        else if (cmd == BLE_OTA_CMD_GD32_DATA) {
            if (seq == 0 && total_size > 0) {
                uint32_t rounded_size = (uint32_t)(((total_size + (uint32_t)GD32_OTA_PACKET_SIZE - 1U) / (uint32_t)GD32_OTA_PACKET_SIZE) * (uint32_t)GD32_OTA_PACKET_SIZE);
                if (total_size == 0U || total_size > OTA_MAX_FIRMWARE_SIZE || rounded_size > OTA_MAX_FIRMWARE_SIZE) {
                    DEBUG_ERR(TAG, "GD32 固件大小非法: raw=%" PRIu32 " rounded48=%" PRIu32 " max=%zu",
                              total_size, rounded_size, OTA_MAX_FIRMWARE_SIZE);
                    sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_SIZE, total_size);
                    return BLE_ATT_ERR_UNLIKELY;
                }

                ota_meta_t meta = {};
                meta.total_size  = total_size;
                meta.stored_size = 0;
                meta.last_seq    = 0;

                esp_err_t err = OtaRelay::metaSave(&meta);
                if (err != ESP_OK) {
                    DEBUG_ERR(TAG, "元数据保存失败");
                    return BLE_ATT_ERR_UNLIKELY;
                }

                DEBUG_INFO(TAG, "GD32 固件存储开始: total=%" PRIu32 ", payload=%zu",
                           meta.total_size, data_len);
            }

            ota_meta_t meta = {};
            if (OtaRelay::metaLoad(&meta) != ESP_OK || meta.total_size == 0) {
                DEBUG_ERR(TAG, "GD32 元数据无效，拒绝写入: seq=%u", seq);
                return BLE_ATT_ERR_UNLIKELY;
            }

            if (seq > 0 && meta.last_seq != (uint32_t)(seq - 1)) {
                DEBUG_ERR(TAG, "GD32 分包序号不连续: seq=%u last_seq=%" PRIu32,
                          seq, meta.last_seq);
                return BLE_ATT_ERR_UNLIKELY;
            }

            // --- GD32 写入 Flash ---
            uint32_t flash_offset = meta.stored_size;
            if (flash_offset + (uint32_t)data_len > meta.total_size) {
                DEBUG_ERR(TAG, "GD32 写入越界: offset=%" PRIu32 " len=%zu total=%" PRIu32,
                          flash_offset, data_len, meta.total_size);
                return BLE_ATT_ERR_UNLIKELY;
            }

            esp_err_t err = OtaRelay::flashWriteChunk(flash_offset, &buf[data_offset], data_len);
            if (err != ESP_OK) {
                DEBUG_ERR(TAG, "Flash 写入失败: seq=%u offset=%" PRIu32 " err=%s",
                          seq, flash_offset, esp_err_to_name(err));
                return BLE_ATT_ERR_UNLIKELY;
            }

            uint32_t stored_size = flash_offset + (uint32_t)data_len;
            err = OtaRelay::metaUpdate(stored_size, seq);
            if (err != ESP_OK) {
                DEBUG_WARN(TAG, "元数据更新失败 (seq=%u)", seq);
            }

            if ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U ||
                (meta.total_size > 0 && stored_size >= meta.total_size) ||
                (expected_payload < BLE_OTA_CHUNK_PAYLOAD)) {
                sendWindowAck(cmd, seq, stored_size);
            }

            DEBUG_DBG(TAG, "Flash 写入成功: seq=%u offset=%" PRIu32 " stored=%" PRIu32,
                      seq, flash_offset, stored_size);

            // --- 检查 GD32 固件是否接收完毕 ---
            if (OtaRelay::metaLoad(&meta) == ESP_OK &&
                meta.total_size > 0 &&
                meta.stored_size >= meta.total_size) {
                DEBUG_INFO(TAG, "★ GD32 固件接收完毕: %" PRIu32 " / %" PRIu32 " 字节 (共 %u 块)",
                           meta.stored_size, meta.total_size, seq + 1);
                // 计算并存储 SHA-256 校验和
                esp_err_t sha_err = OtaRelay::metaFinalizeChecksum();
                if (sha_err != ESP_OK) {
                    DEBUG_ERR(TAG, "GD32 固件 SHA-256 计算失败: %s", esp_err_to_name(sha_err));
                }
            }
        } else {
            DEBUG_WARN(TAG, "DATA_IN 未知 cmd: 0x%02X offset=%zu", cmd, offset);
        }

        // 推进到下一个包
        offset += packet_len;

        // 检查剩余数据是否足够组成一个完整的包头
        if (offset + 7 > (size_t)recv_len) break;
    }

    return 0;
}

/*
 * VERSION (0x0004) - Read
 * 返回固件版本字符串
 */
/**
 * @brief GATT VERSION 特征访问回调，返回固件版本字符串。
 * @param conn_handle BLE 连接句柄。
 * @param attr_handle GATT 属性句柄。
 * @param ctxt GATT 访问上下文。
 * @param arg 回调用户参数。
 * @return NimBLE ATT 错误码，0 表示成功。
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

    const char *version = FW_VERSION;
    int rc = os_mbuf_append(ctxt->om, version, strlen(version));
    return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
}

/*
 * DATA_OUT (0x0003) - Notify only
 * 访问回调不做读写处理，通知由 BleOta::instance().notifyResponse() 发送
 */
/**
 * @brief GATT DATA_OUT 特征访问回调（Notify-only，占位实现）。
 * @param conn_handle BLE 连接句柄。
 * @param attr_handle GATT 属性句柄。
 * @param ctxt GATT 访问上下文。
 * @param arg 回调用户参数。
 * @return 0。
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
            BleOta::instance().m_bleConnected = true;
                        BleOta::instance().m_connHandle   = event->connect.conn_handle;
            DEBUG_INFO(TAG, "BLE 客户端已连接: handle=%d", BleOta::instance().m_connHandle);

            // 向对端请求更快的连接参数，提升 OTA 吞吐
            struct ble_gap_upd_params up = {};
            up.itvl_min        = 6;      // 7.5 ms 最小连接间隔
            up.itvl_max        = 6;      // 7.5 ms 固定间隔（避免对端选大值）
            up.latency         = 0;      // 无延迟
            up.supervision_timeout = 2000; // 2 秒监控超时
            up.min_ce_len      = 12;     // 最小连接事件长度 12×0.625=7.5ms，尽量多传包
            up.max_ce_len      = 12;
            int rc = ble_gap_update_params(BleOta::instance().m_connHandle, &up);
            if (rc != 0) {
                DEBUG_WARN(TAG, "请求更新连接参数失败: rc=%d（对端不一定接受，继续运行）", rc);
            } else {
                DEBUG_INFO(TAG, "已请求更快的连接参数: itvl=%.1fms ce_len=%.1fms latency=0",
                           up.itvl_min * 1.25f, up.min_ce_len * 0.625f);
            }
        } else {
            DEBUG_WARN(TAG, "BLE 连接失败: status=%d", event->connect.status);
            /** @brief BLE GATT 客户端连接状态。 */
            BleOta::instance().m_bleConnected = false;
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        DEBUG_INFO(TAG, "BLE 客户端已断开: reason=%d",
                   event->disconnect.reason);
        /** @brief BLE GATT 客户端连接状态。 */
        BleOta::instance().m_bleConnected = false;
                BleOta::instance().m_connHandle   = BLE_HS_CONN_HANDLE_NONE;
        // 恢复广播以接受新连接
        ble_ota_start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        DEBUG_INFO(TAG, "广播周期结束，重新开始");
        ble_ota_start_advertising();
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
static void ble_ota_start_advertising(void)  // kept as free function for BLE callback compatibility
{
    int rc;

    // 自动推断可用地址类型（优先 Public，不可用则生成 Random）
    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        DEBUG_ERR(TAG, "推断地址类型失败: rc=%d, 尝试生成随机地址", rc);
        // 生成不可解析随机地址作为 fallback
        ble_addr_t rnd_addr;
        rc = ble_hs_id_gen_rnd(0, &rnd_addr);
        if (rc != 0) {
            DEBUG_ERR(TAG, "生成随机地址也失败: rc=%d, BLE 广播无法启动", rc);
            return;
        }
        own_addr_type = rnd_addr.type;
    }

    DEBUG_INFO(TAG, "使用地址类型: %s", own_addr_type == BLE_OWN_ADDR_PUBLIC ? "Public" : "Random");

    // --- 构建广播数据 ---
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

    rc = ble_gap_adv_set_data(adv_data, adv_pos);
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
    // own_addr_type 已在上面由 ble_hs_id_infer_auto 推断

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

    // GATT 服务已在 serverStart() 中注册，不再重复注册
    // 重复调用 ble_gatts_count_cfg / ble_gatts_add_svcs 会导致 BLE_HS_EBUSY

    DEBUG_INFO(TAG, "DATA_OUT 值句柄: %u", s_data_out_val_handle);
    if (s_data_out_val_handle == 0) {
        DEBUG_WARN(TAG, "DATA_OUT 值句柄为 0，通知功能不可用");
    }

    BleOta::instance().m_bleReady = true;
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
    BleOta::instance().m_bleReady = false;
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
esp_err_t BleOta::serverStart()
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

    rc = ble_att_set_preferred_mtu(BLE_MTU_SIZE);
    if (rc != 0) {
        DEBUG_WARN(TAG, "设置首选 MTU 失败: %d", rc);
    } else {
        DEBUG_INFO(TAG, "首选 ATT MTU 已设置: %u", (unsigned)BLE_MTU_SIZE);
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

    // s_data_out_val_handle 由 on_sync 回调填充，此处不复制
    DEBUG_INFO(TAG, "BLE OTA 服务器初始化完成");
    return ESP_OK;
}

/**
  * @brief 通过 BLE Notify 将 GD32 响应推送给上位机。
 * @param data 待推送的数据指针。
 * @param len 数据长度（字节）。
 * @return ESP_OK 成功，其他失败。
 */
esp_err_t BleOta::notifyResponse(const uint8_t *data, size_t len)
{
    if (!BleOta::instance().m_bleConnected) {
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

    int rc = ble_gattc_notify_custom(BleOta::instance().m_connHandle, s_data_out_val_handle, om);
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
bool BleOta::isConnected()
{
    /** @brief BLE GATT 客户端连接状态。 */
    return BleOta::instance().m_bleConnected;
}

/**
  * @brief 通知中继模块开始向 GD32 转发固件。
 */
void BleOta::signalRelay()
{
    if (OtaRelay::dispatchQueue == NULL) {
        DEBUG_ERR(TAG, "OTA 调度队列未初始化");
        return;
    }

    uint32_t msg = 1;
    BaseType_t ret = xQueueSend(OtaRelay::dispatchQueue, &msg, pdMS_TO_TICKS(200));
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
// 为避免与 ota_relay_task 竞争，此任务不从 OtaRelay::dispatchQueue 消费。
// ============================================================
/**
  * @brief BLE OTA FreeRTOS 任务，初始化 NimBLE 并运行 GATT 服务。
 * @param pvParameters FreeRTOS 任务参数，未使用。
 */
void BleOta::taskEntry(void *pvParameters)
{
    (void)pvParameters;
    DEBUG_INFO(TAG, "=== BLE OTA 任务启动 ===");

    esp_err_t err = BleOta::instance().serverStart();
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
                      BleOta::instance().m_bleReady ? "就绪" : "未就绪",
                      BleOta::instance().m_bleConnected ? "已连接" : "未连接");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}