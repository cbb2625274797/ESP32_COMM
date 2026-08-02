/*
 * json_parse.h - 极简 JSON 扁平对象解析器（cJSON 零堆分配替代）
 *
 * 项目中对 JSON 的使用全部是"解析前端发来的扁平 JSON 对象"：
 *   { "sha256": "...", "verify_only": true, "total_size": 12345, "type": "esp32" }
 * 无嵌套对象、无数组（can_send_batch 已自研流式解析）。
 * 本解析器直接在调用方提供的可写字符串缓冲上做扫描，
 * 不分配堆内存，字符串值通过就地 NUL 终止返回指针。
 *
 * 用法：
 *   JsonView root = jsonParse(buf);       // buf 须为可写 char[]
 *   if (!root.valid) return error;
 *   JsonView sha = jsonGet(&root, "sha256");
 *   if (sha.type == JSON_STRING) { use(sha.str); }
 *   JsonView sz  = jsonGet(&root, "total_size");
 *   if (sz.type == JSON_NUMBER) { uint32_t n = (uint32_t)sz.num; }
 *   // 无需 free —— 指针指向 buf，buf 由调用方管理
 */
#ifndef JSON_PARSE_H
#define JSON_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSON_INVALID = 0,
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_OBJECT,
} JsonType;

typedef struct {
    JsonType type;
    bool     valid;
    char    *str;          /* JSON_STRING：指向 buf 内字符串（不一定 NUL 终止！） */
    size_t   str_len;      /* JSON_STRING：字符串长度（配合 str 使用，避免就地写 NUL） */
    double   num;          /* JSON_NUMBER / JSON_BOOL */
    char    *obj_start;    /* JSON_OBJECT：'{' 后的内容区 */
    size_t   obj_len;
} JsonView;

/**
 * @brief 解析 JSON 字符串为根对象视图（就地扫描，可能写入 NUL 终止符）。
 * @param json 可写 NUL 终止 JSON 字符串（须为顶层 { ... } 扁平对象）
 * @return type=JSON_OBJECT 且 valid=true，或 valid=false 解析失败
 */
static inline JsonView jsonParse(char *json)
{
    JsonView v = {};
    if (!json) return v;

    char *p = json;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '{') return v;

    p++;
    char *content = p;
    int depth = 1;
    while (*p) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p) p++;
        } else if (*p == '{') {
            depth++; p++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) break;
            p++;
        } else {
            p++;
        }
    }
    if (depth != 0) return v;

    v.type = JSON_OBJECT;
    v.valid = true;
    v.obj_start = content;
    v.obj_len = (size_t)(p - content);
    return v;
}

/**
 * @brief 在扁平 JSON 对象中查找指定 key 的值。
 * @return valid=true 表示找到；字符串值经就地 NUL 终止后通过 .str 返回
 */
static inline JsonView jsonGet(const JsonView *obj, const char *key)
{
    JsonView result = {};
    if (!obj || !obj->valid || obj->type != JSON_OBJECT || !key) return result;

    char *p = obj->obj_start;
    char *end = obj->obj_start + obj->obj_len;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
        if (p >= end) break;

        /* "key" */
        if (*p != '"') break;
        p++;
        char *key_start = p;
        while (p < end && *p != '"') p++;
        if (p >= end) break;
        size_t klen = (size_t)(p - key_start);
        p++;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= end || *p != ':') break;
        p++;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= end) break;

        bool match = (strlen(key) == klen && strncmp(key_start, key, klen) == 0);

        if (*p == '"') {
            p++;
            char *val_start = p;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
            if (p >= end) break;
            if (match) {
                /* 不就地写 NUL（会破坏后续 jsonGet 扫描）；用 str_len 返回长度 */
                result.type = JSON_STRING;
                result.valid = true;
                result.str = val_start;
                result.str_len = (size_t)(p - val_start);
                return result;
            }
            p++;
        } else if (*p == 't' || *p == 'f') {
            if (match) {
                result.type = JSON_BOOL;
                result.valid = true;
                result.num = (*p == 't') ? 1.0 : 0.0;
                return result;
            }
            while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && *p != '\r') p++;
        } else if (*p == 'n') {
            if (match) {
                result.type = JSON_NULL;
                result.valid = true;
                return result;
            }
            while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && *p != '\r') p++;
        } else {
            char *val_start = p;
            if (*p == '-') p++;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) p++;
            if (match) {
                /* 不就地写 NUL；strtod 可在非 NUL 终止串上工作（指定长度）。
                 * 但 strtod 需要终止符——用临时方式：记录起止，用 sscanf 或手动解析。
                 * 更简方案：buf 本身是 NUL 终止的（jsonParse 要求 NUL 终止输入），
                 * 而 *p 此处是数字后的分隔符（',' 或 '}' 或空白），不是 NUL。
                 * 我们临时保存 *p、写 NUL、解析、恢复 *p。 */
                char saved = *p;
                *p = '\0';
                result.type = JSON_NUMBER;
                result.valid = true;
                result.num = strtod(val_start, nullptr);
                *p = saved;  /* 恢复，不影响后续 jsonGet 扫描 */
                return result;
            }
        }
    }
    return result;
}

/* ── 便捷谓词（兼容 cJSON 调用风格）── */

static inline bool jsonIsString(const JsonView *item) {
    return item && item->valid && item->type == JSON_STRING;
}
static inline bool jsonIsNumber(const JsonView *item) {
    return item && item->valid && item->type == JSON_NUMBER;
}
static inline bool jsonIsBool(const JsonView *item) {
    return item && item->valid && item->type == JSON_BOOL;
}
static inline bool jsonIsTrue(const JsonView *item) {
    return item && item->valid && item->type == JSON_BOOL && item->num != 0.0;
}

/**
 * @brief 获取字符串值指针和长度（字符串不一定 NUL 终止，须配合 str_len 使用）。
 * @param item   JSON 值视图
 * @param out_len 可选，输出字符串长度
 * @return 字符串指针（指向 buf 内部），或 nullptr
 */
static inline const char *jsonGetStringValue(const JsonView *item, size_t *out_len = nullptr) {
    if (item && item->valid && item->type == JSON_STRING) {
        if (out_len) *out_len = item->str_len;
        return item->str;
    }
    if (out_len) *out_len = 0;
    return nullptr;
}

/**
 * @brief 比较字符串值是否等于指定 C 字符串。
 *        因 jsonGet 不写 NUL 终止符，不能用 strcmp，须用此函数。
 */
static inline bool jsonStringEquals(const JsonView *item, const char *cstr) {
    if (!jsonIsString(item) || !cstr) return false;
    size_t clen = strlen(cstr);
    return clen == item->str_len && strncmp(item->str, cstr, clen) == 0;
}

/**
 * @brief 将字符串值拷贝到 NUL 终止的输出缓冲（安全截断）。
 * @return 实际拷贝长度（不含 NUL）
 */
static inline size_t jsonGetStringCopy(const JsonView *item, char *out, size_t out_max) {
    if (!jsonIsString(item) || !out || out_max == 0) return 0;
    size_t n = (item->str_len < out_max - 1) ? item->str_len : out_max - 1;
    memcpy(out, item->str, n);
    out[n] = '\0';
    return n;
}

static inline double jsonGetNumberValue(const JsonView *item) {
    return (item && item->valid && item->type == JSON_NUMBER) ? item->num : 0.0;
}

#ifdef __cplusplus
}
#endif

#endif // JSON_PARSE_H