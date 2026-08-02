/*
 * http_util.h - HTTP handler 共享工具（JSON 追加、query 解析、hex 编解码）
 *
 * 消除 http_handlers.cpp / health_monitor.cpp / spi_speed_test.cpp 中的重复样板。
 */
#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── JSON buffer 追加 ──────────────────────────────────────────
 * 统一替代 feedback_append / append_jsonf / appendJsonfSafe 三份拷贝。
 * 截断时不越界，返回新 offset；出错返回 -1 或原 offset（视调用方约定）。
 * 返回 (int)cap-1 表示已截断填满。 */
static inline int http_json_append(char *buf, size_t cap, int offset, const char *fmt, ...)
{
    if (!buf || cap == 0 || offset < 0 || (size_t)offset >= cap) return offset;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + offset, cap - (size_t)offset, fmt, ap);
    va_end(ap);
    if (n < 0) return offset;
    if ((size_t)n >= cap - (size_t)offset) return (int)cap - 1;  /* 截断 */
    return offset + n;
}

/* ── Query 参数解析 ────────────────────────────────────────────
 * 统一替代 6 处内联 ?lite=1 / ?bin=1 / ?ch=N 解析块。 */

/** 查询布尔型参数：值为 "1"/"y"/"t"/"T" 时返回 true，缺失或其它返回 false。 */
static inline bool http_query_bool(httpd_req_t *req, const char *key)
{
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    char val[8] = {};
    if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) return false;
    return val[0] == '1' || val[0] == 'y' || val[0] == 'Y' ||
           val[0] == 't' || val[0] == 'T';
}

/** 查询无符号整型参数：缺失/非法时返回 defval。 */
static inline uint32_t http_query_u32(httpd_req_t *req, const char *key, uint32_t defval)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return defval;
    char val[16] = {};
    if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) return defval;
    return (uint32_t)strtoul(val, NULL, 10);
}

/* ── Hex 编解码 ────────────────────────────────────────────────
 * 统一替代内联 strtoul 循环和 snprintf("%02X") 循环。 */

/** hex 字符串 → 字节数组，返回写入字节数。 */
static inline size_t http_hex_to_bytes(const char *hex, uint8_t *out, size_t max_out)
{
    size_t hex_len = strlen(hex);
    size_t n = 0;
    for (size_t i = 0; i + 1 < hex_len && n < max_out; i += 2) {
        char byte_str[3] = { hex[i], hex[i + 1], 0 };
        out[n++] = (uint8_t)strtol(byte_str, NULL, 16);
    }
    return n;
}

/** hex 字符串（带长度）→ 字节数组，返回写入字节数。
 *  供 json_parse.h 的非 NUL 终止字符串值使用。 */
static inline size_t http_hex_to_bytes_n(const char *hex, size_t hex_len, uint8_t *out, size_t max_out)
{
    size_t n = 0;
    for (size_t i = 0; i + 1 < hex_len && n < max_out; i += 2) {
        char byte_str[3] = { hex[i], hex[i + 1], 0 };
        out[n++] = (uint8_t)strtol(byte_str, NULL, 16);
    }
    return n;
}

/** 字节数组 → 大写 hex 字符串（out 需至少 2*n+1 字节）。 */
static inline void http_bytes_to_hex_upper(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; ++i) {
        snprintf(out + i * 2, 3, "%02X", in[i]);
    }
    out[n * 2] = '\0';
}

/** 字节数组 → 小写 hex 字符串（out 需至少 2*n+1 字节）。 */
static inline void http_bytes_to_hex_lower(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; ++i) {
        snprintf(out + i * 2, 3, "%02x", in[i]);
    }
    out[n * 2] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif // HTTP_UTIL_H