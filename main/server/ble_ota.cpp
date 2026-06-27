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
 *   - CTRL (0x0001, Write):     0xF7=触发GD32中继（含 32B SHA-256 校验）, 0xF8=触发ESP32 OTA升级（含 32B SHA-256 校验并 reboot），0xFA=重启ESP32
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
#include "esp_wifi.h"

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
#define BLE_OTA_NOTIFY_SHA256 0xC1   // 设备端 SHA-256 上报（[0xC1] + [32字节]）
#define BLE_OTA_FINISH_DONE 0x02
#define BLE_OTA_FINISH_REBOOT_NOW 0x07
#define BLE_OTA_FINISH_GD32_RELAY_START 0x10
#define BLE_OTA_FINISH_GD32_RELAY_DONE 0x11
#define BLE_OTA_FINISH_ABORTED       0x20  // 用户主动中止（BLE 不断开）
#define BLE_OTA_FINISH_ERR_FINALIZE 0x81
#define BLE_OTA_ACK_WINDOW 32
#define BLE_ATT_ERR_OTA_BUSY 0x80           // ESP32 OTA 被其他通道占用
#define BLE_OTA_FINISH_ERR_MUTEX 0x83       // 互斥拒绝: OTA 被其他通道占用
#define BLE_OTA_FINISH_ERR_GD32_SIZE 0x84   // GD32 固件超出分区大小
#define BLE_OTA_FINISH_ERR_SHA_MISMATCH 0x85 // OTA SHA-256 不匹配
#define BLE_OTA_FINISH_ERR_GD32_RELAY 0x86  // GD32 SPI OTA 转发失败

// ── 数据队列：将 OTA 数据处理从 NimBLE host task 卸载到工作线程 ──
// 避免 flash 操作（esp_ota_begin / esp_ota_write）阻塞 BLE 协议栈导致连接断开。
#define BLE_OTA_DATA_QUEUE_LEN 16

struct BleOtaDataBatch {
    uint8_t *data;
    size_t   len;
};
static QueueHandle_t s_otaDataQueue = nullptr;

struct BleGd32RelayWaitCtx {
    uint16_t session_id;
};

struct BleEsp32FinishCtx {
    char sha_hex[65];
    uint16_t session_id;
};

// RAII 包装：保证任务所有 return 路径自动释放 ctx，避免历史多次漏 free。
// 注意：若调用 restartEsp32()（内部 esp_restart 不返回），析构不会执行，
// 但这是设备重启路径，资源由系统回收。
struct BleFinishCtxDeleter {
    BleEsp32FinishCtx *p;
    explicit BleFinishCtxDeleter(BleEsp32FinishCtx *ctx) : p(ctx) {}
    ~BleFinishCtxDeleter() { if (p) { free(p); p = nullptr; } }
    BleFinishCtxDeleter(const BleFinishCtxDeleter&) = delete;
    BleFinishCtxDeleter& operator=(const BleFinishCtxDeleter&) = delete;
};

// ============================================================
// 前向声明
// ============================================================
static void ble_ota_start_advertising(void);
static void sendWindowAck(uint8_t cmd, uint16_t seq, uint32_t completed_bytes);
static void sendFinishEvent(uint8_t stage, uint32_t value);
static uint32_t packBleFinishValue(uint16_t session_id, uint16_t code);
static void shaBytesToHexUpper(const uint8_t sha[32], char out_hex[65]);
static void sendSha256Notify(const uint8_t sha[32], uint16_t session_id);
static void esp32_reorder_free();
static void ble_ota_data_worker_task(void *pvParameters);
static void ble_gd32_relay_wait_task(void *pvParameters);

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
 * 实际的 SHA 校验 / esp_ota_end / esp_ota_set_boot_partition 全部由
 * Esp32OtaCore::finalize 完成。本任务仅负责：补发 ACK → 调 finalize → 上报 SHA → 通知前端。
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
    OtaCoordinator::instance().restartEsp32();   // 内部 notify + delay + esp_restart，不返回
}

/**
  * @brief 异步完成 ESP32 OTA 收尾并触发重启。
  * @param pvParameters BleEsp32FinishCtx（堆分配，任务负责释放）。
  */
static void ble_esp32_finish_and_reboot_task(void *pvParameters)
{
    BleEsp32FinishCtx *ctx = (BleEsp32FinishCtx *)pvParameters;
    // RAII：保证所有 return 路径自动释放 ctx（restartEsp32 不返回路径除外）
    BleFinishCtxDeleter ctx_guard(ctx);
    char *frontend_sha_hex = ctx ? ctx->sha_hex : nullptr;
    uint16_t session_id = ctx ? ctx->session_id : 0;
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
        sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                        packBleFinishValue(session_id, 1));
        BleOta::instance().m_esp32RebootPending = false;
        esp32_reorder_free();
        vTaskDelete(NULL);
        return;
    }

    DEBUG_INFO(TAG, "ESP32 OTA 收尾: 共 %u 字节", (unsigned)core.getReceived());

    // ★ 收尾序列（notify + finalize）由协调器统一封装，reboot=false 让 device_sha 可被读取
    Esp32FinalizeResult fr = OtaCoordinator::instance().finalizeForReboot(frontend_sha_hex);

    if (fr.err == ESP_OK) {
        // 1) 把设备端 SHA 通过 SHA256 通知推给前端
        if (fr.device_sha_hex[0] != '\0') {
            uint8_t device_sha[32] = {0};
            for (int i = 0; i < 32; ++i) {
                unsigned int byte = 0;
                sscanf(&fr.device_sha_hex[i * 2], "%02x", &byte);
                device_sha[i] = (uint8_t)byte;
            }
            sendSha256Notify(device_sha, session_id);
        }
        // 2) 等待 SHA 通知从 NimBLE host 队列送出
        vTaskDelay(pdMS_TO_TICKS(150));

        // 3) 推送"OTA 收尾完成"事件
        sendFinishEvent(BLE_OTA_FINISH_DONE, packBleFinishValue(session_id, 0));
        sendFinishEvent(BLE_OTA_FINISH_REBOOT_NOW, packBleFinishValue(session_id, 0));

        // 4) 通知全部发出后由协调器统一重启（notify + delay + esp_restart）
        DEBUG_INFO(TAG, "[FINISH] 通知已发送，重启...");
        OtaCoordinator::instance().restartEsp32();   // 不返回，ctx 由系统回收
    } else if (fr.err == ESP_ERR_INVALID_VERSION) {
        DEBUG_ERR(TAG, "[FINISH] SHA-256 不匹配，OTA 已中止");
        sendFinishEvent(BLE_OTA_FINISH_ERR_SHA_MISMATCH, packBleFinishValue(session_id, 0));
        BleOta::instance().m_esp32RebootPending = false;
        // ★ finalize 内部已 abort 并释放 mutex，这里补充通知协调器复位状态机，
        //   避免 m_sysState 卡在 ESP32_FINALIZING 导致下次 OTA 被假拒绝。
        OtaCoordinator::instance().abortCurrent();
        esp32_reorder_free();
    } else {
        DEBUG_ERR(TAG, "[FINISH] OTA 收尾失败: %s", esp_err_to_name(fr.err));
        sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                        packBleFinishValue(session_id, 1));
        BleOta::instance().m_esp32RebootPending = false;
        // ★ 同上：finalize 已 abort core，这里通知协调器复位状态机
        OtaCoordinator::instance().abortCurrent();
        esp32_reorder_free();
    }

    vTaskDelete(NULL);
}

// ============================================================
// WiFi 共存策略：BLE OTA 传输期间的处理
//
// 历史：曾在此处把 WiFi 切到 WIFI_PS_MAX_MODEM 以把空中时间让给 BLE。
//   但 MAX_MODEM 下 STA 监听间隔被拉到极大，HTTP server 的 TCP 握手/数据包
//   会大量丢失或延迟到超时 → BLE OTA 期间任何 WiFi 请求（包括互斥拒绝应返回的
//   403）都到不了设备，前端 XHR 长时间无响应，表现为"Wifi 上传既不成功也不报错"。
//
// 现策略：不切换 WiFi 省电模式（保持 WIFI_PS_NONE），让 STA 全程可用，
//   协调器照常以 403 拒绝并发 OTA。ESP32-C3 coex 默认调度已能保证 BLE OTA 可用。
//   仍保留 setBleOtaActive() 暂停 RSSI 轮询（避免 get_ap_info 的射频活动干扰 BLE）。
// ============================================================
static bool s_wifi_ps_boosted = false;

/**
 * @brief BLE OTA 开始时的 WiFi 共存处理（暂停 RSSI 轮询，不切省电模式）。
 */
static void ble_ota_boost_wifi_ps()
{
    if (s_wifi_ps_boosted) return;
    s_wifi_ps_boosted = true;
    // ★ L1: 通过协调器通知 WiFi 层（替代直接调用 WifiOta）
    OtaCoordinator::instance().notifyBleCoexStart();
    DEBUG_INFO(TAG, "BLE OTA 数据传输进行中：暂停 WiFi RSSI 轮询（保持 WIFI_PS_NONE，STA 仍可响应 HTTP）");
}

/**
 * @brief BLE OTA 结束/中止时恢复 WiFi RSSI 轮询。
 */
static void ble_ota_restore_wifi_ps()
{
    if (!s_wifi_ps_boosted) return;
    s_wifi_ps_boosted = false;
    // ★ L1: 通过协调器通知 WiFi 层（替代直接调用 WifiOta）
    OtaCoordinator::instance().notifyBleCoexEnd();
    DEBUG_INFO(TAG, "BLE OTA 数据传输结束：恢复 WiFi RSSI 轮询（GD32 中继仍可能进行中）");
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

// ============================================================
// ESP32 OTA 包序号顺序保护：Reorder Buffer（TCP 接收窗口模式）
//
// 背景：
//   esp_ota_write 是顺序追加写入，不支持 seek。
//   客户端窗口内使用 writeValueWithoutResponse（fire-and-forget），
//   BLE 空口会导致同一窗口的包乱序到达设备端。
//
// 策略（窗口级 reorder buffer）：
//   - seq < expected                → 重复包，丢弃
//   - seq == expected               → 写入 flash，expected++，
//                                     并 flush 连续紧邻的缓存包
//   - seq > expected 且在窗口范围内 → 暂存到 slot，不写 flash
//   - seq 超出窗口范围              → 防御性丢弃 + ACK 报告缺口
//
// 与旧"丢弃策略"的区别：乱序包不再丢弃，而是暂存到 buffer，
// 当缺口（expected）的包到达时按序 flush，从而吸收窗口内乱序，
// 彻底消除"重传包与原发包再次乱序"导致的重传风暴。
//
// 内存：slots 指针/长度数组在 OTA begin 时分配（常驻 ~384B），
//       每个 slot 的 payload 缓冲区按需 lazy malloc（最多 480B），
//       flush 后立即 free。OTA 结束/abort 时统一释放。
//       若 begin 时分配失败 → active=false，自动回退到丢弃策略。
//
// ACK 协议不变：getReceived() 仅在真正按序写入 flash 时增长，
//              客户端 resumeSeq = ceil(bytes/payloadSize) 仍正确。
// ============================================================

/**
  * @brief ESP32 OTA reorder buffer 状态。
  *        窗口级缓存，吸收 BLE 乱序到达的包。
  */
struct Esp32OtaReorderBuf {
    bool      active = false;      ///< 是否已启用（分配成功）
    bool      fallback = false;    ///< 是否回退到丢弃策略（buffer 分配失败时）
    uint32_t  expected = 0;        ///< 下一个期望按序写入的 seq
    uint32_t  windowBase = 0;      ///< 当前窗口起始 seq（= 上次 flush 后的 expected）
    uint32_t  windowSize = 0;      ///< 窗口大小（= BLE_OTA_REORDER_WINDOW）
    uint8_t  **slots = nullptr;    ///< slots[i]：窗口内第 i 个包的 payload 拷贝（nullptr=未占）
    size_t   *lens = nullptr;      ///< lens[i]：slots[i] 的 payload 长度
    size_t    payloadCap = 0;      ///< 每个 slot 的容量（= BLE_OTA_CHUNK_PAYLOAD）
};

static Esp32OtaReorderBuf s_reorder;
static uint32_t s_fallback_ooo_count = 0;  // 回退模式下自上次 ACK 以来的乱序包计数
static uint32_t s_gd32_ooo_count = 0;      // GD32 路径自上次 ACK 以来的乱序/重复包计数

// 前向声明（alloc/free 互相引用）
static void esp32_reorder_free();

/**
 * @brief 分配 reorder buffer（slots 指针/长度数组）。
 *        在 ble_esp32_ota_begin() 中调用。分配失败则置 fallback=true，
 *        后续自动回退到丢弃策略。
 */
static void esp32_reorder_alloc()
{
    // 已分配则先释放（防御性）
    if (s_reorder.slots || s_reorder.lens) {
        esp32_reorder_free();
    }

    s_reorder.expected    = 0;
    s_reorder.windowBase  = 0;
    s_reorder.windowSize  = (uint32_t)BLE_OTA_REORDER_WINDOW;
    s_reorder.payloadCap  = BLE_OTA_CHUNK_PAYLOAD;
    s_reorder.slots = (uint8_t **)calloc(s_reorder.windowSize, sizeof(uint8_t *));
    s_reorder.lens  = (size_t  *)calloc(s_reorder.windowSize, sizeof(size_t));
    s_fallback_ooo_count  = 0;

    if (!s_reorder.slots || !s_reorder.lens) {
        DEBUG_WARN(TAG, "Reorder buffer 分配失败 (slots=%p lens=%p)，回退到丢弃策略。"
                   "heap: free=%u min=%u",
                   s_reorder.slots, s_reorder.lens,
                   (unsigned)esp_get_free_heap_size(),
                   (unsigned)esp_get_minimum_free_heap_size());
        // 释放可能部分成功的分配
        free(s_reorder.slots); s_reorder.slots = nullptr;
        free(s_reorder.lens);  s_reorder.lens  = nullptr;
        s_reorder.active   = false;
        s_reorder.fallback = true;
        return;
    }
    s_reorder.active   = true;
    s_reorder.fallback = false;
    DEBUG_INFO(TAG, "Reorder buffer 已启用: window=%u payloadCap=%u heap=%u",
               (unsigned)s_reorder.windowSize, (unsigned)s_reorder.payloadCap,
               (unsigned)esp_get_free_heap_size());
}

/**
 * @brief 释放 reorder buffer 全部内存（含已占用的 slot）。
 *        同时复位 expected/windowBase 等状态，供下次 OTA begin 重新分配。
 */
static void esp32_reorder_free()
{
    if (s_reorder.slots) {
        for (uint32_t i = 0; i < s_reorder.windowSize; ++i) {
            if (s_reorder.slots[i]) {
                free(s_reorder.slots[i]);
                s_reorder.slots[i] = nullptr;
            }
        }
        free(s_reorder.slots);
        s_reorder.slots = nullptr;
    }
    if (s_reorder.lens) {
        free(s_reorder.lens);
        s_reorder.lens = nullptr;
    }
    s_reorder.expected    = 0;
    s_reorder.windowBase  = 0;
    s_reorder.active      = false;
    s_reorder.fallback    = false;
    s_reorder.windowSize  = 0;
    s_reorder.payloadCap  = 0;
    s_fallback_ooo_count  = 0;
}

/**
 * @brief flush expected 对应的连续缓存包到 flash，并释放对应 slot。
 *        由 esp32_write_ordered 在每次 expected 推进后调用。
 * @return flush 的包数（不含当前已写入的包）。
 */
static uint32_t esp32_reorder_flush()
{
    uint32_t flushed = 0;
    while (s_reorder.active) {
        uint32_t idx = s_reorder.expected - s_reorder.windowBase;
        // idx 应始终 < windowSize（窗口未越界）
        if (idx >= s_reorder.windowSize) break;
        if (!s_reorder.slots[idx]) break;  // 缺口：后续虽有缓存但不连续

        uint8_t *data = s_reorder.slots[idx];
        size_t   len  = s_reorder.lens[idx];
        esp_err_t err = Esp32OtaCore::instance().write(data, len);
        // 释放 slot
        free(s_reorder.slots[idx]);
        s_reorder.slots[idx] = nullptr;
        s_reorder.lens[idx]  = 0;

        if (err != ESP_OK) {
            DEBUG_ERR(TAG, "Reorder flush 写入失败: seq=%u err=%s",
                      (unsigned)s_reorder.expected, esp_err_to_name(err));
            // 写入失败不推进 expected，交由上层 abort
            return flushed;
        }
        s_reorder.expected++;
        flushed++;
    }
    return flushed;
}

/**
 * @brief 按序号顺序写入 ESP32 OTA 数据。
 *
 * 启用 reorder buffer 时：吸收窗口内乱序，缺口填满后按序 flush。
 * 回退模式（buffer 分配失败）：执行旧丢弃策略（乱序包丢弃 + ACK 报告缺口）。
 *
 * @param seq        包序号。
 * @param cmd        数据命令字节（用于 ACK）。
 * @param data       负载数据指针。
 * @param data_len   负载数据长度。
 * @return ESP_OK 写入成功或被安全跳过，其他为失败。
 */
static esp_err_t esp32_write_ordered(uint16_t seq, uint8_t cmd,
                                     const uint8_t *data, size_t data_len)
{
    // ── 回退模式：旧丢弃策略 ──
    if (s_reorder.fallback) {
        // 重复包
        if (seq < s_reorder.expected) {
            DEBUG_DBG(TAG, "ESP32 重复包: seq=%u < expected=%u，跳过",
                      seq, (unsigned)s_reorder.expected);
            return ESP_OK;
        }
        // 乱序包：丢弃 + 节流 ACK 报告缺口
        // ★ 节流策略：每 BLE_OTA_ACK_WINDOW 个乱序包才发一次 ACK，
        //   或在窗口边界处发 ACK。避免大量乱序包涌入时触发 ACK 风暴，
        //   耗尽 NimBLE mbuf 池导致 ESP_ERR_NO_MEM。
        if (seq > s_reorder.expected) {
            s_fallback_ooo_count++;
            bool shouldAck =
                (s_fallback_ooo_count >= BLE_OTA_ACK_WINDOW) ||
                ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U);
            if (shouldAck) {
                uint32_t curReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
                DEBUG_WARN(TAG, "ESP32 乱序包跳过(回退): seq=%u (expected=%u)，节流 ACK bytes=%u (ooo=%u)",
                           seq, (unsigned)s_reorder.expected, (unsigned)curReceived,
                           (unsigned)s_fallback_ooo_count);
                sendWindowAck(cmd, s_reorder.expected > 0 ? (uint16_t)(s_reorder.expected - 1) : 0,
                              curReceived);
                s_fallback_ooo_count = 0;
            } else {
                DEBUG_DBG(TAG, "ESP32 乱序包跳过(回退): seq=%u (expected=%u)，抑制 ACK (ooo=%u)",
                          seq, (unsigned)s_reorder.expected,
                          (unsigned)s_fallback_ooo_count);
            }
            return ESP_OK;
        }
        // seq == expected：正常写入
        esp_err_t err = Esp32OtaCore::instance().write(data, data_len);
        if (err != ESP_OK) return err;
        s_reorder.expected++;
        return ESP_OK;
    }

    // ── Reorder buffer 模式 ──
    // 窗口推进检测：若 expected 已超出当前窗口基址，滑动窗口
    if (s_reorder.expected >= s_reorder.windowBase + s_reorder.windowSize) {
        s_reorder.windowBase = s_reorder.expected;
    }

    // 重复包：seq < expected → 已写入，跳过
    if (seq < s_reorder.expected) {
        DEBUG_DBG(TAG, "ESP32 重复包: seq=%u < expected=%u，跳过",
                  seq, (unsigned)s_reorder.expected);
        return ESP_OK;
    }

    // seq == expected：正常写入 + flush 连续缓存包
    if (seq == s_reorder.expected) {
        esp_err_t err = Esp32OtaCore::instance().write(data, data_len);
        if (err != ESP_OK) return err;
        s_reorder.expected++;
        uint32_t flushed = esp32_reorder_flush();
        if (flushed > 0) {
            DEBUG_DBG(TAG, "Reorder flush: seq=%u 后连续写入 %u 包 (expected→%u)",
                      seq, (unsigned)flushed, (unsigned)s_reorder.expected);
        }
        return ESP_OK;
    }

    // seq > expected：暂存到 slot（若在窗口范围内）
    uint32_t idx = seq - s_reorder.windowBase;
    if (idx < s_reorder.windowSize) {
        if (s_reorder.slots[idx]) {
            // 该 slot 已被占用（重复的乱序包），跳过
            DEBUG_DBG(TAG, "Reorder slot 已占: seq=%u idx=%u，跳过重复包",
                      seq, (unsigned)idx);
            return ESP_OK;
        }
        // data 可能来自静态 buf（ble_ota_data_in_access 的合并缓冲区），
        // 后续包处理会覆盖它，必须拷贝一份
        uint8_t *copy = (uint8_t *)malloc(data_len);
        if (!copy) {
            uint32_t curReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
            DEBUG_ERR(TAG, "Reorder slot malloc 失败: seq=%u expected=%u bytes=%u，"
                       "ACK 报告缺口并 abort",
                       seq, (unsigned)s_reorder.expected, (unsigned)curReceived);
            sendWindowAck(cmd,
                          s_reorder.expected > 0 ? (uint16_t)(s_reorder.expected - 1) : 0,
                          curReceived);
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, data, data_len);
        s_reorder.slots[idx] = copy;
        s_reorder.lens[idx]  = data_len;
        DEBUG_DBG(TAG, "Reorder 缓存乱序包: seq=%u idx=%u (expected=%u)",
                  seq, (unsigned)idx, (unsigned)s_reorder.expected);
        return ESP_OK;
    }

    // seq 超出窗口范围（防御性，理论不会发生）
    uint32_t curReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
    DEBUG_WARN(TAG, "ESP32 包超出 reorder 窗口: seq=%u (expected=%u base=%u win=%u)，ACK bytes=%u",
               seq, (unsigned)s_reorder.expected, (unsigned)s_reorder.windowBase,
               (unsigned)s_reorder.windowSize, (unsigned)curReceived);
    sendWindowAck(cmd, s_reorder.expected > 0 ? (uint16_t)(s_reorder.expected - 1) : 0,
                  curReceived);
    return ESP_OK;
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
    // ★ 通过协调器发起（内部已含 forceCleanup 重试）
    esp_err_t err = OtaCoordinator::instance().beginEsp32OtaWithRetry(total_size, OTA_CHANNEL_BLE);
    if (err != ESP_OK) {
        return err;
    }
    // 复位 BLE 传输相关状态
    BleOta::instance().m_esp32RebootPending = false;
    BleOta::instance().m_esp32LastCmd = 0;
    BleOta::instance().m_esp32LastSeq = 0xFFFFU;
    // ★ 分配 reorder buffer（失败则自动回退丢弃策略）
    esp32_reorder_alloc();
    // ★ BLE OTA 开始：降低 WiFi 射频占用
    ble_ota_boost_wifi_ps();
    return ESP_OK;
}

/**
 * @brief BLE OTA 会话状态统一复位（共用清理内核）。
 *
 * 集中处理三个清理动作：
 *   1. abortCurrent()：通知协调器按当前状态分发清理
 *      （GD32_STORING→metaClear, ESP32_RECEIVING→core.abort, IDLE→空操作），
 *      确保下次 begin() 不会被残留状态假拒绝。
 *   2. core.abort() 兜底：若 Esp32OtaCore 仍 isActive（异常路径），单独再 abort。
 *   3. BLE OTA 会话状态字段复位：m_esp32LastSeq/m_esp32LastCmd/m_esp32RebootPending
 *      + esp32_reorder_free() 释放 reorder buffer + s_gd32_ooo_count 归零。
 *
 * 不做：OTA 数据队列清理（调用方按需做）、WiFi PS 恢复（调用方按需做）。
 * 这两个动作在不同调用路径下时序/前置条件不同，交给调用方更安全。
 *
 * 调用方：
 *   - ble_esp32_ota_abort(): ESP32 OTA 异常/中止路径
 *   - 0xFB handler: 用户主动中止
 *   - BLE_GAP_EVENT_DISCONNECT handler: BLE 断开（含 GD32 存储断开）
 */
static void ble_ota_session_reset()
{
    // ★ 无条件通知协调器释放通道占用（m_sysState → IDLE）。
    //   abortCurrent() 对 IDLE 状态是空操作，对其他状态均复位到 IDLE，
    //   确保后续 begin() 不会被残留的 ESP32_RECEIVING / ERROR 等状态假拒绝。
    OtaCoordinator::instance().abortCurrent();
    // 若 ESP32 OTA core 仍活跃（兜底），单独再 abort 一次
    if (Esp32OtaCore::instance().isActive()) {
        Esp32OtaCore::instance().abort();
    }
    // 复位 BLE OTA 会话状态，避免下次会话读到旧值
    BleOta::instance().m_esp32LastSeq = 0xFFFFU;
    BleOta::instance().m_esp32LastCmd = 0;
    BleOta::instance().m_esp32RebootPending = false;
    // 释放 ESP32 reorder buffer（无 buffer 时为空操作）
    esp32_reorder_free();
    // 复位 GD32 路径乱序包计数器（节流 ACK 计数），保证下次 GD32 上传从干净状态开始
    s_gd32_ooo_count = 0;
}

/**
 * @brief ESP32 OTA 异常/中止时调用：会话复位 + WiFi PS 恢复。
 *
 * 关键：必须先通知协调器释放 m_sysState（避免下次 begin 被 ESP32_RECEIVING
 *   状态假拒绝），再调 core.abort() 释放 mutex。两者重复 abort 幂等安全。
 */
static void ble_esp32_ota_abort()
{
    ble_ota_session_reset();
    // ★ BLE OTA 中止：恢复 WiFi 射频
    ble_ota_restore_wifi_ps();
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

/** @brief 将 session_id 和 code 打包为 32 位 BLE FINISH 事件值。 */
static uint32_t packBleFinishValue(uint16_t session_id, uint16_t code)
{
    return ((uint32_t)code << 16) | (uint32_t)session_id;
}

/** @brief 将 32 字节 SHA-256 转为大写十六进制字符串（65字节含'\0'）。 */
static void shaBytesToHexUpper(const uint8_t sha[32], char out_hex[65])
{
    if (!sha || !out_hex) return;
    for (int i = 0; i < 32; ++i) {
        snprintf(&out_hex[i * 2], 3, "%02X", sha[i]);
    }
    out_hex[64] = '\0';
}

/** @brief 通过 BLE Notify 推送设备端 SHA-256 给前端（用于比对验证）。 */
static void sendSha256Notify(const uint8_t sha[32], uint16_t session_id)
{
    if (!sha) return;

    uint8_t sha_pkt[35] = {0};
    sha_pkt[0] = BLE_OTA_NOTIFY_SHA256;
    memcpy(&sha_pkt[1], sha, 32);
    sha_pkt[33] = (uint8_t)(session_id & 0xFFU);
    sha_pkt[34] = (uint8_t)((session_id >> 8) & 0xFFU);
    BleOta::instance().notifyResponse(sha_pkt, sizeof(sha_pkt));
}

/** @brief BLE 触发 GD32 中继后等待结果的 FreeRTOS 任务。 */
static void ble_gd32_relay_wait_task(void *pvParameters)
{
    BleGd32RelayWaitCtx *ctx = (BleGd32RelayWaitCtx *)pvParameters;
    uint16_t session_id = ctx ? ctx->session_id : 0;
    if (ctx) free(ctx);

    uint32_t result_msg = (uint32_t)OTA_RESULT_ERR_TIMEOUT;
    bool got_result = false;
    QueueHandle_t rq = OtaRelay::getResultQueue();
    if (rq) {
        got_result = (xQueueReceive(rq, &result_msg, pdMS_TO_TICKS(120000)) == pdTRUE);
    }

    if (!got_result) {
        DEBUG_ERR(TAG, "GD32 BLE OTA 等待中继结果超时");
        sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                        packBleFinishValue(session_id, (uint16_t)(-OTA_RESULT_ERR_TIMEOUT)));
        vTaskDelete(NULL);
        return;
    }

    ota_result_t result = (ota_result_t)((int32_t)result_msg);
    if (result == OTA_RESULT_OK) {
        DEBUG_INFO(TAG, "GD32 BLE OTA 中继完成，通知前端 (stage=0x11, session=%u)", session_id);
        sendFinishEvent(BLE_OTA_FINISH_GD32_RELAY_DONE,
                        packBleFinishValue(session_id, 0));
    } else {
        uint16_t code = (uint16_t)(result < 0 ? -result : result);
        DEBUG_ERR(TAG, "GD32 BLE OTA 中继失败: result=%d", (int)result);
        sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                        packBleFinishValue(session_id, code));
    }
    vTaskDelete(NULL);
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
        uint16_t session_id = 0;
        if (len >= 35) {
            session_id = (uint16_t)(buf[33] | (buf[34] << 8));
        } else if (len >= 3) {
            session_id = (uint16_t)(buf[1] | (buf[2] << 8));
        }

        if (len < 35) {
            DEBUG_WARN(TAG, "CTRL 0xF7 数据长度不足 35（前端 SHA-256 必传），拒绝触发 GD32 中继");
            sendFinishEvent(BLE_OTA_FINISH_ERR_SHA_MISMATCH,
                            packBleFinishValue(session_id, 1));
            return 0;
        }

        // 检查固件是否存储完毕 → 通过协调器触发 GD32 OTA 中继
        ota_meta_t meta = {};
        esp_err_t err = OtaCoordinator::instance().loadGd32Meta(&meta);
        if (err == ESP_OK &&
            meta.total_size > 0 &&
            meta.stored_size >= meta.total_size) {
            const uint8_t *frontend_sha = &buf[1];
            char frontend_sha_hex[65] = {0};
            char device_sha_hex[65] = {0};
            shaBytesToHexUpper(frontend_sha, frontend_sha_hex);

            // ★ 统一走共享 helper：计算设备端 SHA → 比对前端 SHA
            uint8_t device_sha[32] = {0};
            err = OtaCoordinator::instance().verifyGd32Firmware(frontend_sha, device_sha);
            shaBytesToHexUpper(device_sha, device_sha_hex);
            sendSha256Notify(device_sha, session_id);
            DEBUG_INFO(TAG, "GD32 前端 SHA-256: %s, session=%u", frontend_sha_hex, session_id);
            DEBUG_INFO(TAG, "GD32 缓存 SHA-256: %s", device_sha_hex);

            if (err == ESP_ERR_INVALID_CRC) {
                DEBUG_ERR(TAG, "GD32 缓存 SHA-256 与前端文件不匹配，拒绝触发 SPI 中继");
                sendFinishEvent(BLE_OTA_FINISH_ERR_SHA_MISMATCH,
                                packBleFinishValue(session_id, 0));
                return 0;
            } else if (err != ESP_OK) {
                DEBUG_ERR(TAG, "GD32 SHA-256 计算/读取失败: %s", esp_err_to_name(err));
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                                packBleFinishValue(session_id, 3));
                return 0;
            }

            DEBUG_INFO(TAG, "GD32 固件就绪且 SHA-256 匹配 (%" PRIu32 " / %" PRIu32 " 字节)，通过协调器触发中继",
                       meta.stored_size, meta.total_size);
            if (OtaRelay::getResultQueue()) {
                uint32_t stale = 0;
                while (xQueueReceive(OtaRelay::getResultQueue(), &stale, 0) == pdTRUE) {
                    DEBUG_WARN(TAG, "清理旧 GD32 中继结果: %" PRIu32, stale);
                }
            }
            esp_err_t trig_err = OtaCoordinator::instance().triggerGd32Relay();
            if (trig_err != ESP_OK) {
                DEBUG_ERR(TAG, "GD32 中继触发失败: %s", esp_err_to_name(trig_err));
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                                packBleFinishValue(session_id, 1));
                return 0;
            }

            sendFinishEvent(BLE_OTA_FINISH_GD32_RELAY_START,
                            packBleFinishValue(session_id, 0));
            DEBUG_INFO(TAG, "已通知前端: GD32 中继已开始 (stage=0x10, session=%u)", session_id);

            BleGd32RelayWaitCtx *ctx = (BleGd32RelayWaitCtx *)malloc(sizeof(BleGd32RelayWaitCtx));
            if (!ctx) {
                DEBUG_ERR(TAG, "GD32 中继等待任务上下文分配失败");
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                                packBleFinishValue(session_id, 2));
                return 0;
            }
            ctx->session_id = session_id;
            BaseType_t ok = xTaskCreate(ble_gd32_relay_wait_task,
                                        "ble_gd32_wait",
                                        3072,
                                        ctx,
                                        5,
                                        NULL);
            if (ok != pdPASS) {
                DEBUG_ERR(TAG, "创建 GD32 中继等待任务失败");
                free(ctx);
                sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                                packBleFinishValue(session_id, 2));
            }
        } else {
            DEBUG_WARN(TAG, "GD32 固件未就绪 (stored=%" PRIu32 ", total=%" PRIu32 ")",
                       meta.stored_size, meta.total_size);
            sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                            packBleFinishValue(session_id, 1));
        }
    } else if (buf[0] == BLE_OTA_CTRL_TRIGGER_ESP32) {
        uint16_t session_id = 0;
        if (len >= 35) {
            session_id = (uint16_t)(buf[33] | (buf[34] << 8));
        }
        if (!Esp32OtaCore::instance().isActive()) {
            DEBUG_WARN(TAG, "ESP32 OTA 未激活，无法触发升级");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, 1));
            return 0;
        }
        if (BleOta::instance().m_esp32RebootPending) {
            DEBUG_WARN(TAG, "ESP32 OTA 收尾已在进行中，忽略重复 CTRL");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, 1));
            return 0;
        }

        // 强制要求 32B 前端 SHA-256：与 WiFi 流程对齐，设备端读回分区计算并比对，
        // 一致走 end+set_boot+reboot，不一致返回 0x85 ERR_SHA_MISMATCH。
        if (len < 33) {
            DEBUG_WARN(TAG, "CTRL 0xF8 数据长度不足 33（前端 SHA-256 必传），拒绝触发");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE, (uint32_t)ESP_ERR_INVALID_ARG);
            return 0;
        }

        BleEsp32FinishCtx *finish_ctx = (BleEsp32FinishCtx *)malloc(sizeof(BleEsp32FinishCtx));
        if (!finish_ctx) {
            DEBUG_ERR(TAG, "CTRL 0xF8 申请内存失败");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, (uint16_t)ESP_ERR_NO_MEM));
            return 0;
        }
        for (int i = 0; i < 32; ++i) {
            snprintf(&finish_ctx->sha_hex[i * 2], 3, "%02x", buf[1 + i]);
        }
        finish_ctx->sha_hex[64] = '\0';
        finish_ctx->session_id = session_id;
        DEBUG_INFO(TAG, "前端 SHA-256: %s, session=%u", finish_ctx->sha_hex, session_id);

        BleOta::instance().m_esp32RebootPending = true;
        BaseType_t ok = xTaskCreate(ble_esp32_finish_and_reboot_task,
                                    "ble_ota_finish",
                                    4096,
                                    finish_ctx,  // heap context; task 负责 free
                                    5,
                                    NULL);
        if (ok != pdPASS) {
            DEBUG_ERR(TAG, "创建 ESP32 OTA 收尾任务失败");
            free(finish_ctx);
            BleOta::instance().m_esp32RebootPending = false;
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, (uint16_t)ESP_ERR_NO_MEM));
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
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE, 0);
        }
    } else if (buf[0] == BLE_OTA_CTRL_ABORT_OTA) {
        // 0xFB: 中止当前 OTA（保持 BLE 连接）
        //   - 数据接收阶段（GD32_STORING / ESP32_RECEIVING）：立即中止 + 清队列 + 回复 ABORTED
        //   - 中继/收尾阶段（GD32_RELAYING / ESP32_FINALIZING）：拒绝中止（SPI 转发不可中断）
        //   - IDLE/已完成状态：幂等回复 ABORTED
        uint16_t session_id = 0;
        if (len >= 3) {
            session_id = (uint16_t)(buf[1] | (buf[2] << 8));
        }

        ota_system_state_t cur_state = OtaCoordinator::instance().getSystemState();
        DEBUG_WARN(TAG, "收到 0xFB 中止请求: state=%s session=%u",
                   OtaCoordinator::stateToString(cur_state), session_id);

        if (cur_state == OTA_SYS_GD32_RELAYING) {
            DEBUG_WARN(TAG, "GD32 SPI 中继进行中，无法中止（请等待完成或断开 BLE）");
            sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_RELAY,
                            packBleFinishValue(session_id, 2));
            return 0;
        }
        if (cur_state == OTA_SYS_ESP32_FINALIZING) {
            DEBUG_WARN(TAG, "ESP32 OTA 收尾进行中，无法中止");
            sendFinishEvent(BLE_OTA_FINISH_ERR_FINALIZE,
                            packBleFinishValue(session_id, 2));
            return 0;
        }

        // ── 可中止状态：清队列 + abort core + 复位协调器 ──
        if (s_otaDataQueue) {
            BleOtaDataBatch stale;
            size_t dropped = 0;
            while (xQueueReceive(s_otaDataQueue, &stale, 0) == pdTRUE) {
                free(stale.data);
                dropped++;
            }
            if (dropped > 0) {
                DEBUG_INFO(TAG, "中止时清理 OTA 数据队列: %zu 项", dropped);
            }
        }

        // abortCurrent 内部按状态分发：GD32_STORING→metaClear, ESP32_RECEIVING→core.abort, IDLE→空操作
        // ble_ota_session_reset() 统一负责 abortCurrent + core.abort 兜底 + 会话状态复位
        // （m_esp32LastSeq/m_esp32LastCmd/m_esp32RebootPending/reorder/s_gd32_ooo_count）
        ble_ota_session_reset();

        ble_ota_restore_wifi_ps();

        DEBUG_INFO(TAG, "OTA 已中止（BLE 保持连接），通知前端 (stage=0x20)");
        sendFinishEvent(BLE_OTA_FINISH_ABORTED,
                        packBleFinishValue(session_id, 0));
    }
    //   0xF7 = 触发 GD32 中继
    //   0xF8 = 触发 ESP32 本机 OTA 收尾：设备端读 OTA 分区算 SHA 并与前端 32B SHA 比对，
    //         一致走 end+set_boot+reboot，不一致返回 0x85 ERR_SHA_MISMATCH（不重启）
    //   0xFA = 重启 ESP32（配套前端 expectedReboot 自动重连）
    //   0xFB = 中止当前 OTA（不断开 BLE）：仅数据接收阶段有效，中继/收尾阶段拒绝

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

    // 将数据拷贝到堆上并送入工作队列，避免在 NimBLE host task 中执行
    // 耗时的 flash 操作（esp_ota_begin / esp_ota_write）导致 BLE 连接断开。
    if (s_otaDataQueue) {
        uint8_t *heap_buf = (uint8_t *)malloc((size_t)recv_len);
        if (heap_buf) {
            memcpy(heap_buf, buf, (size_t)recv_len);
            BleOtaDataBatch item = { heap_buf, (size_t)recv_len };
            if (xQueueSend(s_otaDataQueue, &item, 0) != pdTRUE) {
                DEBUG_WARN(TAG, "DATA_IN 工作队列满，丢弃 %d 字节（等待客户端重传）", recv_len);
                free(heap_buf);
            }
        } else {
            DEBUG_ERR(TAG, "DATA_IN 堆内存不足 (%d 字节)，丢弃", recv_len);
        }
    }

    return 0;
}

/**
 * @brief 处理单个 ESP32 OTA 数据包（ble_ota_process_data_batch 子步骤）。
 * @return true 中止整个 batch；false 继续下一包（offset 由外层统一推进）。
 */
static bool process_esp32_packet(uint8_t cmd, uint16_t seq, uint32_t total_size,
                                  const uint8_t *buf, size_t data_offset, size_t data_len,
                                  uint32_t expected_payload)
{
    if (seq == 0) {
        if (total_size == 0) {
            DEBUG_ERR(TAG, "ESP32 固件大小非法: %" PRIu32, total_size);
            return true;
        }
        // ★ 会话判定：若已有活跃 ESP32 OTA 会话，根据已接收字节数区分两种情况：
        //   ① m_received == 0：seq=0 尚未写入 flash，说明是客户端 ACK 超时后
        //     重传的 seq=0（同一会话续传），跳过 begin。
        //   ② m_received > 0：seq=0 已写入且有后续数据，说明这是新的上传会话
        //     （前端 abort 后 BLE 断开未完成就发起了新传输），需 abort 旧会话
        //     再 begin，否则 reorder buffer 以旧 expected 去重导致全部丢弃。
        if (Esp32OtaCore::instance().isActive()) {
            if (Esp32OtaCore::instance().getReceived() > 0) {
                // ★ 跨通道保护：当前活跃会话可能是 WiFi 通道发起的，
                //   BLE 不应无条件 abort 别人的会话。只有当活跃会话也是 BLE
                //   通道时才允许 abort + rebegin（同会话重传/新 BLE 会话场景）；
                //   否则直接互斥拒绝，让前端看到"被其他通道占用"。
                ota_channel_t curCh = OtaCoordinator::instance().getOtaChannel();
                if (curCh != OTA_CHANNEL_BLE) {
                    DEBUG_WARN(TAG, "ESP32 OTA 被其他通道占用(ch=%d)，BLE 拒绝抢占",
                               (int)curCh);
                    sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
                    return true;
                }
                DEBUG_WARN(TAG, "ESP32 OTA 新会话检测: received=%u > 0, "
                           "中止旧会话并重新开始",
                           (unsigned)Esp32OtaCore::instance().getReceived());
                ble_esp32_ota_abort();
                esp_err_t err = ble_esp32_ota_begin(total_size);
                if (err != ESP_OK) {
                    if (err == ESP_ERR_INVALID_STATE) {
                        sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
                    }
                    return true;
                }
            } else {
                DEBUG_INFO(TAG, "ESP32 OTA 会话已激活，seq=0 视为重传，跳过 begin");
            }
        } else {
            // 分区选择/大小校验/互斥由 Esp32OtaCore::begin 内部完成
            esp_err_t err = ble_esp32_ota_begin(total_size);
            if (err != ESP_OK) {
                if (err == ESP_ERR_INVALID_STATE) {
                    sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
                }
                return true;
            }
        }
    }

    // ★ 会话有效性检查：如果 begin() 失败（如互斥拒绝），后续包也应丢弃
    if (!Esp32OtaCore::instance().isActive()) {
        DEBUG_WARN(TAG, "ESP32 OTA 会话无效，丢弃 seq=%u", seq);
        return false;  // 外层推进 offset，继续下一包
    }

    // ★ 防重复写入保护：如果已接收字节 >= 声明大小，跳过（重试引起的重复包）
    uint32_t curReceived = (uint32_t)Esp32OtaCore::instance().getReceived();
    if (total_size > 0 && curReceived >= total_size) {
        DEBUG_WARN(TAG, "ESP32 OTA 已收满 (%u >= %" PRIu32 ")，跳过重复 seq=%u",
                   (unsigned)curReceived, total_size, seq);
        sendWindowAck(cmd, seq, curReceived);
        return false;  // 外层推进 offset，继续下一包
    }

    // ★ 使用带序号顺序保护的写入：拒绝乱序包并立即 ACK 通知发送端
    esp_err_t err = esp32_write_ordered(seq, cmd, &buf[data_offset], data_len);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "ESP32 OTA 写入失败: seq=%u err=%s",
                  seq, esp_err_to_name(err));
        ble_esp32_ota_abort();  // core 内部完成 abort + mutex release + 状态复位
        return true;
    }

    DEBUG_DBG(TAG, "ESP32 OTA 写入成功: seq=%u received=%u",
              seq, (unsigned int)Esp32OtaCore::instance().getReceived());

    // 完成判定：以实际按序写入字节数为准。
    bool isLastPacket = (expected_payload < BLE_OTA_CHUNK_PAYLOAD);
    bool transferComplete = (total_size > 0 &&
                             Esp32OtaCore::instance().getReceived() >= total_size);

    // ACK 触发条件
    if ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U ||
        transferComplete ||
        isLastPacket) {
        if (isLastPacket || transferComplete) {
            DEBUG_INFO(TAG, "ESP32 尾包/完成检测: seq=%u data_len=%zu received=%u total=%" PRIu32,
                       seq, data_len, (unsigned int)Esp32OtaCore::instance().getReceived(), total_size);
        }
        sendWindowAck(cmd, seq, (uint32_t)Esp32OtaCore::instance().getReceived());
    }

    if (transferComplete) {
        DEBUG_INFO(TAG, "★ ESP32 固件接收完毕: %u / %" PRIu32 " 字节 (共 %u 块)",
                   (unsigned int)Esp32OtaCore::instance().getReceived(), total_size, seq + 1);
        OtaCoordinator::instance().notifyEsp32ReceiveComplete();
        BleOta::instance().m_esp32LastCmd = cmd;
        BleOta::instance().m_esp32LastSeq = seq;
        ble_ota_restore_wifi_ps();
    }
    return false;
}

/**
 * @brief 处理单个 GD32 OTA 数据包（ble_ota_process_data_batch 子步骤）。
 * @return true 中止整个 batch；false 继续下一包（offset 由外层统一推进）。
 */
static bool process_gd32_packet(uint8_t cmd, uint16_t seq, uint32_t total_size,
                                 const uint8_t *buf, size_t data_offset, size_t data_len,
                                 uint32_t expected_payload)
{
    if (seq == 0 && total_size > 0) {
        // ★ 统一调用 prepareGd32Storage 完成：validate + begin + erase + clearMeta + saveMeta
        //   原 BLE 路径未擦除分区，现通过 prepareGd32Storage 与 HTTP 路径对齐。
        esp_err_t prep_err = OtaCoordinator::instance().prepareGd32Storage(total_size, OTA_CHANNEL_BLE);
        if (prep_err == ESP_ERR_INVALID_SIZE) {
            DEBUG_ERR(TAG, "GD32 固件大小非法: raw=%" PRIu32 " max=%zu",
                      total_size, OTA_MAX_FIRMWARE_SIZE);
            sendFinishEvent(BLE_OTA_FINISH_ERR_GD32_SIZE, total_size);
            return true;
        } else if (prep_err == ESP_ERR_INVALID_STATE) {
            DEBUG_ERR(TAG, "GD32 OTA 协调器拒绝存储（系统忙）");
            sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
            return true;
        } else if (prep_err != ESP_OK) {
            DEBUG_ERR(TAG, "GD32 OTA 准备失败: %s", esp_err_to_name(prep_err));
            sendFinishEvent(BLE_OTA_FINISH_ERR_MUTEX, 0);
            return true;
        }

        DEBUG_INFO(TAG, "GD32 固件存储开始: total=%" PRIu32 ", payload=%zu",
                   total_size, data_len);
        ble_ota_boost_wifi_ps();
    }

    ota_meta_t meta = {};
    if (OtaCoordinator::instance().loadGd32Meta(&meta) != ESP_OK || meta.total_size == 0) {
        DEBUG_ERR(TAG, "GD32 元数据无效，拒绝写入: seq=%u", seq);
        return true;
    }

    // ── 顺序检查：GD32 Flash 必须按序写入，无 reorder buffer ──
    // 改进：乱序/重复包不再中止整个 batch，而是丢弃当前包 + 节流 ACK 报告缺口，
    //       让前端尽快知道设备端实际进度（last_seq）并重传缺口。
    //       策略与 ESP32 fallback 路径一致（参考 esp32_write_ordered 行 459-477）。
    if (seq > 0 && meta.last_seq != (uint32_t)(seq - 1)) {
        // 重复包：seq <= last_seq（已写入过）
        if (seq <= meta.last_seq) {
            DEBUG_DBG(TAG, "GD32 重复包: seq=%u <= last_seq=%" PRIu32 "，跳过",
                      seq, meta.last_seq);
            // 重复包不触发 ACK 风暴，只在窗口边界发一次
            if ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U) {
                sendWindowAck(cmd, (uint16_t)meta.last_seq, meta.stored_size);
            }
            return false;  // 外层推进 offset，继续下一包
        }

        // 乱序包：seq > last_seq + 1（缺口）
        // 节流策略：每 BLE_OTA_ACK_WINDOW 个乱序包才发一次 ACK，或在窗口边界处发 ACK。
        // 避免大量乱序包涌入时触发 ACK 风暴，耗尽 NimBLE mbuf 池导致 ESP_ERR_NO_MEM。
        s_gd32_ooo_count++;
        bool shouldAck =
            (s_gd32_ooo_count >= BLE_OTA_ACK_WINDOW) ||
            ((((uint32_t)seq + 1U) % BLE_OTA_ACK_WINDOW) == 0U);
        if (shouldAck) {
            DEBUG_WARN(TAG, "GD32 乱序包跳过: seq=%u (last_seq=%" PRIu32 ")，节流 ACK bytes=%" PRIu32 " (ooo=%u)",
                       seq, meta.last_seq, meta.stored_size, (unsigned)s_gd32_ooo_count);
            sendWindowAck(cmd, (uint16_t)meta.last_seq, meta.stored_size);
            s_gd32_ooo_count = 0;
        } else {
            DEBUG_DBG(TAG, "GD32 乱序包跳过: seq=%u (last_seq=%" PRIu32 ")，抑制 ACK (ooo=%u)",
                      seq, meta.last_seq, (unsigned)s_gd32_ooo_count);
        }
        return false;  // 外层推进 offset，继续下一包
    }

    // 顺序包到达，复位乱序计数器
    if (s_gd32_ooo_count != 0) {
        s_gd32_ooo_count = 0;
    }

    // --- GD32 写入 Flash ---
    uint32_t flash_offset = meta.stored_size;
    if (flash_offset >= meta.total_size) {
        DEBUG_WARN(TAG, "GD32 OTA 已收满 (%" PRIu32 " >= %" PRIu32 ")，跳过重复 seq=%u",
                   flash_offset, meta.total_size, seq);
        sendWindowAck(cmd, seq, flash_offset);
        return false;  // 外层推进 offset，继续下一包
    }
    if (flash_offset + (uint32_t)data_len > meta.total_size) {
        DEBUG_ERR(TAG, "GD32 写入越界: offset=%" PRIu32 " len=%zu total=%" PRIu32,
                  flash_offset, data_len, meta.total_size);
        return true;
    }

    esp_err_t err = OtaCoordinator::instance().writeGd32Chunk(flash_offset, &buf[data_offset], data_len);
    if (err != ESP_OK) {
        DEBUG_ERR(TAG, "Flash 写入失败: seq=%u offset=%" PRIu32 " err=%s",
                  seq, flash_offset, esp_err_to_name(err));
        return true;
    }

    uint32_t stored_size = flash_offset + (uint32_t)data_len;
    err = OtaCoordinator::instance().updateGd32Meta(stored_size, seq);
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
    if (OtaCoordinator::instance().loadGd32Meta(&meta) == ESP_OK &&
        meta.total_size > 0 &&
        meta.stored_size >= meta.total_size) {
        DEBUG_INFO(TAG, "★ GD32 固件接收完毕: %" PRIu32 " / %" PRIu32 " 字节 (共 %u 块)",
                   meta.stored_size, meta.total_size, seq + 1);
        OtaCoordinator::instance().completeGd32Storage();
        ble_ota_restore_wifi_ps();
        esp_err_t sha_err = OtaCoordinator::instance().finalizeGd32Checksum();
        if (sha_err != ESP_OK) {
            DEBUG_ERR(TAG, "GD32 固件 SHA-256 计算失败: %s", esp_err_to_name(sha_err));
        }
    }
    return false;
}

/**
 * @brief 处理一批 OTA 数据包（在工作线程上下文中调用）。
 *        包含 ESP32 和 GD32 的数据接收逻辑。
 * @param buf      数据缓冲区（包含一个或多个合并的包）。
 * @param recv_len 数据长度。
 */
static void ble_ota_process_data_batch(const uint8_t *buf, size_t recv_len)
{
    // 循环处理 mbuf 中的所有完整数据包
    size_t offset = 0;
    while (offset + 7 <= recv_len) {
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
        size_t remaining_after_header = recv_len - data_offset;

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

        // 分发到对应通道的包处理器
        bool abort_batch = false;
        if (cmd == BLE_OTA_CMD_ESP32_DATA) {
            abort_batch = process_esp32_packet(cmd, seq, total_size,
                                               buf, data_offset, data_len, expected_payload);
        } else if (cmd == BLE_OTA_CMD_GD32_DATA) {
            abort_batch = process_gd32_packet(cmd, seq, total_size,
                                              buf, data_offset, data_len, expected_payload);
        } else {
            DEBUG_WARN(TAG, "DATA_IN 未知 cmd: 0x%02X offset=%zu", cmd, offset);
        }

        if (abort_batch) {
            return;
        }

        // 推进到下一个包
        offset += packet_len;

        // 检查剩余数据是否足够组成一个完整的包头
        if (offset + 7 > recv_len) break;
    }
}

/**
 * @brief OTA 数据工作线程：从队列接收数据并调用处理函数。
 *        独立于 NimBLE host task，避免 flash 操作阻塞 BLE 协议栈。
 */
static void ble_ota_data_worker_task(void *pvParameters)
{
    (void)pvParameters;
    BleOta::instance().m_workerTaskHandle = xTaskGetCurrentTaskHandle();
    DEBUG_INFO(TAG, "OTA 数据工作线程已启动");

    while (1) {
        BleOtaDataBatch item;
        if (xQueueReceive(s_otaDataQueue, &item, portMAX_DELAY) == pdTRUE) {
            if (!item.data || item.len == 0) {
                free(item.data);
                continue;
            }

            // 若 BLE 已断开，丢弃队列中的残留数据
            if (!BleOta::instance().isConnected()) {
                DEBUG_DBG(TAG, "BLE 已断开，丢弃 %zu 字节队列数据", item.len);
                free(item.data);
                continue;
            }

            ble_ota_process_data_batch(item.data, item.len);
            free(item.data);
        }
    }
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
    // 版本字符串: "vX.Y.Z YYYY-MM-DD HH:MM:SS"
    char ver_buf[64];
    int ver_len = snprintf(ver_buf, sizeof(ver_buf), "%s %s", FW_VERSION, FW_BUILD_TIME);
    if (ver_len < 0 || ver_len >= (int)sizeof(ver_buf)) {
        ver_len = strlen(version);
        int rc = os_mbuf_append(ctxt->om, version, ver_len);
        return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    int rc = os_mbuf_append(ctxt->om, ver_buf, ver_len);
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
            BleOta::instance().m_bleConnected = false;
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        DEBUG_INFO(TAG, "BLE 客户端已断开: reason=%d",
                   event->disconnect.reason);

        // ★ 断开时清空数据队列，丢弃属于旧会话的残留包
        if (s_otaDataQueue) {
            BleOtaDataBatch stale;
            while (xQueueReceive(s_otaDataQueue, &stale, 0) == pdTRUE) {
                free(stale.data);
            }
        }

        BleOta::instance().m_bleConnected = false;
        BleOta::instance().m_connHandle   = BLE_HS_CONN_HANDLE_NONE;
        // ★ BLE 断开时恢复 WiFi 射频
        ble_ota_restore_wifi_ps();

        // ★ 断开时清理"BLE 数据接收阶段"残留的 OTA 会话状态，避免下次刷写被
        //   系统 busy 假拒绝。注意区分可中止 / 不可中止状态：
        //   - GD32_STORING / ESP32_RECEIVING / ESP32_FINALIZING：BLE 是唯一数据源，
        //     断开即等价于中止，必须 abortCurrent() + 清理底层 core。
        //   - GD32_RELAYING / ESP32_REBOOTING：SPI 中继/重启不依赖 BLE，不能打断。
        //     这些状态保留，等中继/重启自然结束。
        //   - IDLE / STORED / COMPLETE / ERROR：abortCurrent 幂等安全，直接调即可。
        {
            ota_system_state_t cur = OtaCoordinator::instance().getSystemState();
            if (cur == OTA_SYS_GD32_RELAYING || cur == OTA_SYS_ESP32_REBOOTING) {
                DEBUG_WARN(TAG, "BLE 断开但 %s 进行中，保留状态等待自然结束",
                           OtaCoordinator::stateToString(cur));
            } else {
                if (cur == OTA_SYS_GD32_STORING) {
                    DEBUG_WARN(TAG, "BLE 断开，中止未完成的 GD32 固件存储");
                } else if (cur == OTA_SYS_ESP32_RECEIVING ||
                           cur == OTA_SYS_ESP32_FINALIZING) {
                    DEBUG_WARN(TAG, "BLE 断开，中止未完成的 ESP32 OTA");
                }
                // ble_ota_session_reset() 统一负责 abortCurrent + core.abort 兜底 +
                // 会话状态复位（m_esp32LastSeq/m_esp32LastCmd/m_esp32RebootPending/
                // reorder/s_gd32_ooo_count）
                ble_ota_session_reset();
            }
        }
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
    return BleOta::instance().m_bleConnected;
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
    BleOta::instance().m_taskHandle = xTaskGetCurrentTaskHandle();
    DEBUG_INFO(TAG, "=== BLE OTA 任务启动 ===");

    // 创建 OTA 数据队列和工作线程（在 NimBLE 初始化之前完成）
    s_otaDataQueue = xQueueCreate(BLE_OTA_DATA_QUEUE_LEN, sizeof(BleOtaDataBatch));
    if (s_otaDataQueue) {
        BaseType_t wk_ok = xTaskCreate(ble_ota_data_worker_task,
                                       "ble_ota_worker",
                                       4096,   // 峰值仅 ~1.9KB，4KB 留有充足余量
                                       NULL,
                                       10,     // 中等优先级，避免被 NimBLE host task 过度抢占
                                       NULL);
        if (wk_ok != pdPASS) {
            DEBUG_ERR(TAG, "OTA 数据工作线程创建失败，BLE OTA 数据传输不可用");
            vQueueDelete(s_otaDataQueue);
            s_otaDataQueue = nullptr;
        }
    } else {
        DEBUG_ERR(TAG, "OTA 数据队列创建失败，BLE OTA 数据传输不可用");
    }

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
