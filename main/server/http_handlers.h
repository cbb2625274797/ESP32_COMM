#ifndef HTTP_HANDLERS_H
#define HTTP_HANDLERS_H

#include "esp_http_server.h"

// HTTP CORS and options
void http_set_cors_headers(httpd_req_t *req);
esp_err_t http_options_handler(httpd_req_t *req);

// OTA handlers
esp_err_t esp32_ota_update_handler(httpd_req_t *req);
esp_err_t ota_upload_handler(httpd_req_t *req);
esp_err_t ota_relay_trigger_handler(httpd_req_t *req);
esp_err_t restart_handler(httpd_req_t *req);

// Utility handlers
esp_err_t logs_handler(httpd_req_t *req);
/** @brief CAN 发送（唯一入口）：同通道 1~N 帧，单帧即 frames 长度为 1。 */
esp_err_t can_send_batch_handler(httpd_req_t *req);
esp_err_t spi_send_handler(httpd_req_t *req);
esp_err_t system_status_handler(httpd_req_t *req);
esp_err_t system_probe_handler(httpd_req_t *req);

/** @brief GET /system/heap_low_log — 返回设备侧最新 1 条创新低上下文。 */
esp_err_t system_heap_low_log_handler(httpd_req_t *req);

// CAN 反馈快照（被动 0xC1）与接收模式
esp_err_t can_feedback_handler(httpd_req_t *req);
esp_err_t can_mode_handler(httpd_req_t *req);

#endif // HTTP_HANDLERS_H
