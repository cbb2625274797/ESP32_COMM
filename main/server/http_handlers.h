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
esp_err_t can_send_handler(httpd_req_t *req);
esp_err_t spi_send_handler(httpd_req_t *req);
esp_err_t system_status_handler(httpd_req_t *req);
esp_err_t system_probe_handler(httpd_req_t *req);

#endif // HTTP_HANDLERS_H
