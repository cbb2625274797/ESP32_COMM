/**
 * @file spi_speed_test.h
 * @brief 【临时功能 SPI-SPEEDTEST】SPI 链路测速端点声明。
 *        删除方法见 spi_speed_test.cpp 文件头。
 */
#ifndef SPI_SPEED_TEST_H
#define SPI_SPEED_TEST_H

#include "esp_http_server.h"

/** @brief GET /spi/speedtest — 0xA1 探针测 SPI RTT/吞吐（临时功能）。 */
esp_err_t spi_speedtest_handler(httpd_req_t *req);

#endif // SPI_SPEED_TEST_H
