/*
 * ota_compressor.cpp - OTA 流式解压实现
 *
 * 使用 miniz tinfl API 做分块 zlib/deflate 解压。
 * 采用 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 模式，
 * 输出缓冲尽可能大（最大 32KB），满了就 flush 到 Flash 然后重置。
 * 这是 tinfl_decompress_mem_to_callback 的流式版本。
 *
 * 注意：如果 deflate 流中存在 > out_buf_size 的 back-reference，
 *       解压会失败（理论上可能，实际浏览器 CompressionStream 生成的流几乎不会出现）。
 */
#include "ota_compressor.h"

#include <cstdlib>
#include <cstring>
#include "esp_log.h"
#include "esp_system.h"
// tinfl declarations are in ota_compressor.h

static const char *TAG = "OTA_DECOMP";

/* tinfl LZ 字典大小 = 32KB，输出缓冲的理想大小 */
static constexpr size_t TINFL_LZ_DICT = 32768;  /* back-ref window, not alloc target */
/* 最小可用输出缓冲 */
static constexpr size_t TINFL_MIN_BUF  = 512;   /* 流式解压够用，每 flush 后重置 */
/* 为系统预留的堆空间（WiFi/TCP 栈需要一些余量） */
static constexpr size_t HEAP_MARGIN    = 4096;  /* WiFi+TCP 最低保障 */

esp_err_t ota_decomp_init(OtaDecompCtx *ctx, OtaCompress type, size_t out_buf_size)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    memset(ctx, 0, sizeof(*ctx));
    ctx->type = type;

    if (type == OtaCompress::NONE) {
        ESP_LOGI(TAG, "passthrough mode (no compression)");
        return ESP_OK;
    }

    /* ① 先分配 tinfl 状态（~12KB），它不可压缩 */
    size_t decomp_size = sizeof(tinfl_decompressor);
    ctx->tinfl = (tinfl_decompressor *)calloc(1, decomp_size);
    if (!ctx->tinfl) {
        ESP_LOGE(TAG, "tinfl alloc failed: need %u B, free heap=%u",
                 (unsigned)decomp_size, (unsigned)esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    tinfl_init((tinfl_decompressor *)ctx->tinfl);

    /* ② 根据剩余堆自适应分配输出缓冲 */
    size_t free_now  = esp_get_free_heap_size();
    size_t max_alloc = (free_now > HEAP_MARGIN) ? (free_now - HEAP_MARGIN) : 0;
    size_t buf_size  = out_buf_size;
    if (buf_size > TINFL_LZ_DICT) buf_size = TINFL_LZ_DICT;   /* 超过 32KB 没意义 */
    if (buf_size > max_alloc)     buf_size = max_alloc;         /* 受限于可用堆 */
    if (buf_size < TINFL_MIN_BUF) buf_size = TINFL_MIN_BUF;    /* 最低保障 */

    ctx->out_buf = (uint8_t *)malloc(buf_size);
    if (!ctx->out_buf) {
        ESP_LOGE(TAG, "out_buf alloc failed (%u B), free heap=%u",
                 (unsigned)buf_size, (unsigned)free_now);
        free(ctx->tinfl);
        ctx->tinfl = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ctx->out_buf_size  = buf_size;
    ctx->out_write_pos = 0;
    ctx->finished      = false;

    ESP_LOGI(TAG, "zlib decompressor ready: tinfl=%u B, out_buf=%u B, free_heap=%u B",
             (unsigned)decomp_size, (unsigned)buf_size,
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

esp_err_t ota_decomp_process(OtaDecompCtx *ctx,
                             const uint8_t *chunk, size_t chunk_len,
                             ota_decomp_write_fn write_cb, void *user_arg,
                             size_t *out_bytes)
{
    if (!ctx || !chunk || chunk_len == 0) return ESP_ERR_INVALID_ARG;
    if (out_bytes) *out_bytes = 0;

    /* ---- passthrough ---- */
    if (ctx->type == OtaCompress::NONE) {
        if (write_cb) {
            esp_err_t err = write_cb(chunk, chunk_len, user_arg);
            if (err != ESP_OK) return err;
        }
        ctx->total_in  += chunk_len;
        ctx->total_out += chunk_len;
        if (out_bytes) *out_bytes = chunk_len;
        return ESP_OK;
    }

    /* ---- ZLIB decompression ---- */
    if (ctx->finished) {
        ESP_LOGW(TAG, "decomp already finished, ignoring %u extra bytes", (unsigned)chunk_len);
        return ESP_OK;
    }

    const uint8_t *in_ptr  = chunk;
    size_t         in_left = chunk_len;

    /*
     * 非包装模式 (TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF)：
     * - pOut_buf_start = pOut_buf_next = out_buf + out_write_pos
     * - tinfl 将缓冲视为线性空间，写满时返回 HAS_MORE_OUTPUT
     * - 我们 flush 已写数据到 Flash，然后将写指针重置为 0 继续
     * - 每次 flush 后旧数据从缓冲中消失；如果后续 deflate 流
     *   引用了已 flush 的数据（back-reference > buf_size），会失败
     * - buf_size 越大越安全，32KB 覆盖所有情况
     */

    while (in_left > 0 && !ctx->finished) {
        size_t in_bytes    = in_left;
        size_t out_space   = ctx->out_buf_size - ctx->out_write_pos;
        size_t out_written = out_space;

        tinfl_status status = tinfl_decompress(
            (tinfl_decompressor *)ctx->tinfl,
            (const mz_uint8 *)in_ptr, &in_bytes,
            ctx->out_buf + ctx->out_write_pos,     /* pOut_buf_start */
            ctx->out_buf + ctx->out_write_pos,     /* pOut_buf_next  */
            &out_written,
            TINFL_FLAG_HAS_MORE_INPUT |
            TINFL_FLAG_PARSE_ZLIB_HEADER |
            TINFL_FLAG_COMPUTE_ADLER32 |
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

        /* advance input cursor */
        in_ptr  += in_bytes;
        in_left -= in_bytes;
        ctx->total_in += in_bytes;

        /* flush newly decompressed data to flash */
        if (out_written > 0 && write_cb) {
            esp_err_t err = write_cb(ctx->out_buf + ctx->out_write_pos, out_written, user_arg);
            if (err != ESP_OK) return err;
        }
        ctx->total_out += out_written;
        if (out_bytes) *out_bytes += out_written;

        /* advance write pointer; reset to 0 when segment is full */
        ctx->out_write_pos += out_written;
        if (ctx->out_write_pos >= ctx->out_buf_size) {
            ctx->out_write_pos = 0;   /* flush-complete: reset for next block */
        }

        /* status dispatch */
        if (status == TINFL_STATUS_DONE) {
            ctx->finished = true;
            ESP_LOGI(TAG, "decomp done: in=%u out=%u",
                     (unsigned)ctx->total_in, (unsigned)ctx->total_out);
        } else if (status == TINFL_STATUS_HAS_MORE_OUTPUT) {
            /* segment full → reset and continue */
            ctx->out_write_pos = 0;
            continue;
        } else if (status == TINFL_STATUS_NEEDS_MORE_INPUT) {
            /* consumed all input, wait for next chunk */
            break;
        } else {
            ESP_LOGE(TAG, "tinfl failed, status=%d, in_left=%u, total_in=%u",
                     (int)status, (unsigned)in_left, (unsigned)ctx->total_in);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t ota_decomp_finish(OtaDecompCtx *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;

    if (ctx->type == OtaCompress::NONE) {
        ESP_LOGI(TAG, "passthrough done: %u bytes", (unsigned)ctx->total_out);
        return ESP_OK;
    }

    if (!ctx->finished) {
        ESP_LOGW(TAG, "zlib stream not finished, attempting final flush...");

        size_t out_space   = ctx->out_buf_size - ctx->out_write_pos;
        size_t out_written = out_space;
        tinfl_status status = tinfl_decompress(
            (tinfl_decompressor *)ctx->tinfl,
            nullptr, nullptr,
            ctx->out_buf + ctx->out_write_pos,
            ctx->out_buf + ctx->out_write_pos,
            &out_written,
            TINFL_FLAG_PARSE_ZLIB_HEADER |
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

        if (out_written > 0) {
            ctx->total_out += out_written;
        }

        if (status == TINFL_STATUS_DONE) {
            ctx->finished = true;
        } else {
            ESP_LOGE(TAG, "zlib verify failed, status=%d", (int)status);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "decomp verified: compressed=%u B -> decompressed=%u B (%.1f%%)",
             (unsigned)ctx->total_in, (unsigned)ctx->total_out,
             ctx->total_in > 0 ? (float)ctx->total_out * 100.0f / (float)ctx->total_in : 0.0f);
    return ESP_OK;
}

void ota_decomp_free(OtaDecompCtx *ctx)
{
    if (!ctx) return;
    if (ctx->tinfl) {
        free(ctx->tinfl);
        ctx->tinfl = nullptr;
    }
    if (ctx->out_buf) {
        free(ctx->out_buf);
        ctx->out_buf = nullptr;
    }
    ctx->type = OtaCompress::NONE;
}