/*
 * ota_compressor.h - OTA 流式解压模块
 *
 * 支持 zlib/deflate 压缩固件的流式解压，边接收边解压边写 Flash。
 * 采用 tinfl 非包装模式 + 自适应缓冲：满了就 flush 到 Flash 然后重置。
 * tinfl 函数在 ESP32 ROM 中始终可用，此处仅声明原型，无需 esp_rom 组件依赖。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "esp_err.h"

/* ================================================================
 * tinfl ROM API 声明（摘自 esp_rom/miniz.h，避免引入 esp_rom 依赖）
 * ================================================================ */

typedef unsigned char  mz_uint8;
typedef signed short   mz_int16;
typedef unsigned int   mz_uint32;
typedef unsigned int   mz_uint;

#define MZ_MACRO_END while (0)

enum {
    TINFL_FLAG_PARSE_ZLIB_HEADER            = 1,
    TINFL_FLAG_HAS_MORE_INPUT               = 2,
    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4,
    TINFL_FLAG_COMPUTE_ADLER32              = 8
};

typedef enum {
    TINFL_STATUS_BAD_PARAM       = -3,
    TINFL_STATUS_ADLER32_MISMATCH = -2,
    TINFL_STATUS_FAILED          = -1,
    TINFL_STATUS_DONE            =  0,
    TINFL_STATUS_NEEDS_MORE_INPUT =  1,
    TINFL_STATUS_HAS_MORE_OUTPUT  =  2
} tinfl_status;

enum {
    TINFL_MAX_HUFF_TABLES     = 3,
    TINFL_MAX_HUFF_SYMBOLS_0  = 288,
    TINFL_MAX_HUFF_SYMBOLS_1  = 32,
    TINFL_MAX_HUFF_SYMBOLS_2  = 19,
    TINFL_FAST_LOOKUP_BITS     = 10,
    TINFL_FAST_LOOKUP_SIZE     = 1 << TINFL_FAST_LOOKUP_BITS
};

typedef struct {
    mz_uint8  m_code_size[TINFL_MAX_HUFF_SYMBOLS_0];
    mz_int16  m_look_up[TINFL_FAST_LOOKUP_SIZE];
    mz_int16  m_tree[TINFL_MAX_HUFF_SYMBOLS_0 * 2];
} tinfl_huff_table;

/* ESP32-C3: 32-bit bitbuf */
typedef mz_uint32 tinfl_bit_buf_t;

struct tinfl_decompressor_tag {
    mz_uint32 m_state, m_num_bits, m_zhdr0, m_zhdr1, m_z_adler32,
              m_final, m_type, m_check_adler32, m_dist, m_counter,
              m_num_extra, m_table_sizes[TINFL_MAX_HUFF_TABLES];
    tinfl_bit_buf_t m_bit_buf;
    size_t m_dist_from_out_buf_start;
    tinfl_huff_table m_tables[TINFL_MAX_HUFF_TABLES];
    mz_uint8 m_raw_header[4];
    mz_uint8 m_len_codes[TINFL_MAX_HUFF_SYMBOLS_0 + TINFL_MAX_HUFF_SYMBOLS_1 + 137];
};

typedef struct tinfl_decompressor_tag tinfl_decompressor;

#define tinfl_init(r) do { (r)->m_state = 0; } MZ_MACRO_END

#ifdef __cplusplus
extern "C" {
#endif
tinfl_status tinfl_decompress(tinfl_decompressor *r,
    const mz_uint8 *pIn_buf_next, size_t *pIn_buf_size,
    mz_uint8 *pOut_buf_start, mz_uint8 *pOut_buf_next,
    size_t *pOut_buf_size, const mz_uint32 decomp_flags);
#ifdef __cplusplus
}
#endif

/* ================================================================
 * OTA 解压上下文
 * ================================================================ */

enum class OtaCompress : uint8_t {
    NONE = 0,
    ZLIB = 1,
};

struct OtaDecompCtx {
    OtaCompress   type;
    tinfl_decompressor *tinfl;  // 仅 ZLIB 模式
    uint8_t      *out_buf;
    size_t        out_buf_size;
    size_t        out_write_pos;
    size_t        total_in;
    size_t        total_out;
    bool          finished;
};

typedef esp_err_t (*ota_decomp_write_fn)(const uint8_t *data, size_t len, void *arg);

esp_err_t ota_decomp_init(OtaDecompCtx *ctx, OtaCompress type, size_t out_buf_size);
esp_err_t ota_decomp_process(OtaDecompCtx *ctx,
                             const uint8_t *chunk, size_t chunk_len,
                             ota_decomp_write_fn write_cb, void *user_arg,
                             size_t *out_bytes);
esp_err_t ota_decomp_finish(OtaDecompCtx *ctx);
void      ota_decomp_free(OtaDecompCtx *ctx);