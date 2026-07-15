/*
 * ChaosEngine 统一编解码接口 - 实现
 *
 * 编码流程: 压缩 → 加密
 * 解码流程: 解密 → 解压
 *
 * 支持不压缩/不加密（直接透传）。
 * 纯 C99。
 */

#define _POSIX_C_SOURCE 200112L

#include "codec/ce_codec.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 条件编译: 检测可用库 ---- */

#ifdef __has_include
#  if __has_include(<lz4.h>)
#    define HAVE_LZ4 1
#    include <lz4.h>
#  endif
#  if __has_include(<zstd.h>)
#    define HAVE_ZSTD 1
#    include <zstd.h>
#  endif
#  if __has_include(<zlib.h>)
#    define HAVE_ZLIB 1
#    include <zlib.h>
#  endif
#  if __has_include(<openssl/evp.h>)
#    define HAVE_OPENSSL 1
#    include <openssl/evp.h>
#    include <openssl/aes.h>
#    include <openssl/rand.h>
#  endif
#endif

#ifndef HAVE_LZ4
#  define HAVE_LZ4 0
#endif
#ifndef HAVE_ZSTD
#  define HAVE_ZSTD 0
#endif
#ifndef HAVE_ZLIB
#  define HAVE_ZLIB 0
#endif
#ifndef HAVE_OPENSSL
#  define HAVE_OPENSSL 0
#endif

/* ---- 编解码上下文 ---- */

struct CeCodecCtx {
    CeCodecConfig  config;
    /* XOR 流加密状态 */
    uint8_t        xor_pad[256];
    /* RC4 状态 */
    uint8_t        rc4_s[256];
    int            rc4_i;
    int            rc4_j;
};

/* ---- 压缩实现 ---- */

static CeResult compress_none(const uint8_t* in, uint32_t in_len,
                                uint8_t* out, uint32_t* out_len) {
    if (*out_len < in_len) return CE_ERR;
    memcpy(out, in, in_len);
    *out_len = in_len;
    return CE_OK;
}

#if HAVE_LZ4
static CeResult compress_lz4(const uint8_t* in, uint32_t in_len,
                               uint8_t* out, uint32_t* out_len, int level) {
    (void)level;
    int bound = LZ4_compressBound((int)in_len);
    if (*out_len < (uint32_t)bound) return CE_ERR;
    int n = LZ4_compress_default((const char*)in, (char*)out, (int)in_len, (int)*out_len);
    if (n <= 0) return CE_ERR;
    *out_len = (uint32_t)n;
    return CE_OK;
}

static CeResult decompress_lz4(const uint8_t* in, uint32_t in_len,
                                 uint8_t* out, uint32_t* out_len) {
    int n = LZ4_decompress_safe((const char*)in, (char*)out, (int)in_len, (int)*out_len);
    if (n <= 0) return CE_ERR;
    *out_len = (uint32_t)n;
    return CE_OK;
}
#endif

#if HAVE_ZSTD
static CeResult compress_zstd(const uint8_t* in, uint32_t in_len,
                                uint8_t* out, uint32_t* out_len, int level) {
    size_t n = ZSTD_compress(out, *out_len, in, in_len, level > 0 ? level : 3);
    if (ZSTD_isError(n)) return CE_ERR;
    *out_len = (uint32_t)n;
    return CE_OK;
}

static CeResult decompress_zstd(const uint8_t* in, uint32_t in_len,
                                  uint8_t* out, uint32_t* out_len) {
    size_t n = ZSTD_decompress(out, *out_len, in, in_len);
    if (ZSTD_isError(n)) return CE_ERR;
    *out_len = (uint32_t)n;
    return CE_OK;
}
#endif

#if HAVE_ZLIB
static CeResult compress_zlib(const uint8_t* in, uint32_t in_len,
                                uint8_t* out, uint32_t* out_len, int level) {
    uLongf dest_len = *out_len;
    int rc = compress2(out, &dest_len, in, in_len, level > 0 ? level : Z_DEFAULT_COMPRESSION);
    if (rc != Z_OK) return CE_ERR;
    *out_len = (uint32_t)dest_len;
    return CE_OK;
}

static CeResult decompress_zlib(const uint8_t* in, uint32_t in_len,
                                  uint8_t* out, uint32_t* out_len) {
    uLongf dest_len = *out_len;
    int rc = uncompress(out, &dest_len, in, in_len);
    if (rc != Z_OK) return CE_ERR;
    *out_len = (uint32_t)dest_len;
    return CE_OK;
}
#endif

/* ---- 加密实现 ---- */

static void xor_init(uint8_t* pad, const uint8_t* key, int key_len) {
    for (int i = 0; i < 256; i++) {
        pad[i] = key[i % key_len] ^ (uint8_t)i;
    }
}

static void xor_crypt(uint8_t* data, uint32_t len, const uint8_t* pad) {
    for (uint32_t i = 0; i < len; i++) {
        data[i] ^= pad[i % 256];
    }
}

static void rc4_init(CeCodecCtx* ctx, const uint8_t* key, int key_len) {
    for (int i = 0; i < 256; i++) ctx->rc4_s[i] = (uint8_t)i;
    ctx->rc4_i = 0; ctx->rc4_j = 0;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + ctx->rc4_s[i] + key[i % key_len]) & 0xFF;
        uint8_t tmp = ctx->rc4_s[i];
        ctx->rc4_s[i] = ctx->rc4_s[j];
        ctx->rc4_s[j] = tmp;
    }
}

static void rc4_crypt(CeCodecCtx* ctx, uint8_t* data, uint32_t len) {
    for (uint32_t k = 0; k < len; k++) {
        ctx->rc4_i = (ctx->rc4_i + 1) & 0xFF;
        ctx->rc4_j = (ctx->rc4_j + ctx->rc4_s[ctx->rc4_i]) & 0xFF;
        uint8_t tmp = ctx->rc4_s[ctx->rc4_i];
        ctx->rc4_s[ctx->rc4_i] = ctx->rc4_s[ctx->rc4_j];
        ctx->rc4_s[ctx->rc4_j] = tmp;
        data[k] ^= ctx->rc4_s[(ctx->rc4_s[ctx->rc4_i] + ctx->rc4_s[ctx->rc4_j]) & 0xFF];
    }
}

#if HAVE_OPENSSL
static CeResult aes_ctr_crypt(const uint8_t* in, uint32_t in_len,
                                uint8_t* out, const uint8_t* key, int key_len,
                                const uint8_t* iv) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return CE_ERR;

    const EVP_CIPHER* cipher = (key_len == 32) ? EVP_aes_256_ctr() : EVP_aes_128_ctr();
    if (EVP_EncryptInit_ex(ctx, cipher, NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return CE_ERR;
    }

    int out_len = 0;
    if (EVP_EncryptUpdate(ctx, out, &out_len, in, (int)in_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return CE_ERR;
    }
    int final_len = 0;
    EVP_EncryptFinal_ex(ctx, out + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);
    (void)final_len;
    return CE_OK;
}
#endif

/* ---- 公开 API ---- */

CeCodecCtx* ce_codec_create(const CeCodecConfig* config) {
    if (!config) return NULL;

    CeCodecCtx* ctx = (CeCodecCtx*)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->config = *config;

    /* 初始化加密状态 */
    if (config->encrypt == CE_CODEC_ENCRYPT_XOR && config->key_len > 0) {
        xor_init(ctx->xor_pad, config->key, config->key_len);
    } else if (config->encrypt == CE_CODEC_ENCRYPT_RC4 && config->key_len > 0) {
        rc4_init(ctx, config->key, config->key_len);
    }

    return ctx;
}

void ce_codec_destroy(CeCodecCtx* ctx) {
    if (ctx) free(ctx);
}

CeResult ce_codec_encode(CeCodecCtx* ctx,
                           const uint8_t* input, uint32_t input_len,
                           uint8_t* output, uint32_t* output_len) {
    if (!ctx || !input || !output || !output_len) return CE_ERR;

    uint8_t  tmp_buf[65536];
    uint32_t tmp_len;

    /* Step 1: 压缩 */
    tmp_len = sizeof(tmp_buf);
    switch (ctx->config.compress) {
    case CE_CODEC_COMPRESS_NONE:
        if (compress_none(input, input_len, tmp_buf, &tmp_len) != CE_OK) return CE_ERR;
        break;
#if HAVE_LZ4
    case CE_CODEC_COMPRESS_LZ4:
        if (compress_lz4(input, input_len, tmp_buf, &tmp_len, ctx->config.compress_level) != CE_OK) return CE_ERR;
        break;
#endif
#if HAVE_ZSTD
    case CE_CODEC_COMPRESS_ZSTD:
        if (compress_zstd(input, input_len, tmp_buf, &tmp_len, ctx->config.compress_level) != CE_OK) return CE_ERR;
        break;
#endif
#if HAVE_ZLIB
    case CE_CODEC_COMPRESS_ZLIB:
        if (compress_zlib(input, input_len, tmp_buf, &tmp_len, ctx->config.compress_level) != CE_OK) return CE_ERR;
        break;
#endif
    default:
        /* 库未编译，回退到不压缩 */
        if (compress_none(input, input_len, tmp_buf, &tmp_len) != CE_OK) return CE_ERR;
        break;
    }

    /* Step 2: 加密 */
    if (*output_len < tmp_len) return CE_ERR;
    memcpy(output, tmp_buf, tmp_len);

    switch (ctx->config.encrypt) {
    case CE_CODEC_ENCRYPT_NONE:
        /* 不加密，直接输出 */
        break;
    case CE_CODEC_ENCRYPT_XOR:
        xor_crypt(output, tmp_len, ctx->xor_pad);
        break;
    case CE_CODEC_ENCRYPT_RC4:
        rc4_crypt(ctx, output, tmp_len);
        break;
#if HAVE_OPENSSL
    case CE_CODEC_ENCRYPT_AES: {
        uint8_t iv[16] = {0}; /* 简化: 用固定 IV（生产环境应随机生成并随包发送） */
        uint8_t enc_buf[65536];
        if (aes_ctr_crypt(tmp_buf, tmp_len, enc_buf,
                          ctx->config.key, ctx->config.key_len, iv) != CE_OK) return CE_ERR;
        memcpy(output, enc_buf, tmp_len);
        break;
    }
#endif
    default:
        /* 库未编译，回退到不加密 */
        break;
    }

    *output_len = tmp_len;
    return CE_OK;
}

CeResult ce_codec_decode(CeCodecCtx* ctx,
                           const uint8_t* input, uint32_t input_len,
                           uint8_t* output, uint32_t* output_len) {
    if (!ctx || !input || !output || !output_len) return CE_ERR;

    uint8_t  tmp_buf[65536];
    uint32_t tmp_len;

    /* Step 1: 解密 */
    if (input_len > sizeof(tmp_buf)) return CE_ERR;
    memcpy(tmp_buf, input, input_len);
    tmp_len = input_len;

    switch (ctx->config.encrypt) {
    case CE_CODEC_ENCRYPT_NONE:
        break;
    case CE_CODEC_ENCRYPT_XOR:
        xor_crypt(tmp_buf, tmp_len, ctx->xor_pad);
        break;
    case CE_CODEC_ENCRYPT_RC4:
        rc4_crypt(ctx, tmp_buf, tmp_len);
        break;
#if HAVE_OPENSSL
    case CE_CODEC_ENCRYPT_AES: {
        uint8_t iv[16] = {0};
        uint8_t dec_buf[65536];
        if (aes_ctr_crypt(tmp_buf, tmp_len, dec_buf,
                          ctx->config.key, ctx->config.key_len, iv) != CE_OK) return CE_ERR;
        memcpy(tmp_buf, dec_buf, tmp_len);
        break;
    }
#endif
    default:
        break;
    }

    /* Step 2: 解压 */
    switch (ctx->config.compress) {
    case CE_CODEC_COMPRESS_NONE:
        if (*output_len < tmp_len) return CE_ERR;
        memcpy(output, tmp_buf, tmp_len);
        *output_len = tmp_len;
        break;
#if HAVE_LZ4
    case CE_CODEC_COMPRESS_LZ4:
        if (decompress_lz4(tmp_buf, tmp_len, output, output_len) != CE_OK) return CE_ERR;
        break;
#endif
#if HAVE_ZSTD
    case CE_CODEC_COMPRESS_ZSTD:
        if (decompress_zstd(tmp_buf, tmp_len, output, output_len) != CE_OK) return CE_ERR;
        break;
#endif
#if HAVE_ZLIB
    case CE_CODEC_COMPRESS_ZLIB:
        if (decompress_zlib(tmp_buf, tmp_len, output, output_len) != CE_OK) return CE_ERR;
        break;
#endif
    default:
        if (*output_len < tmp_len) return CE_ERR;
        memcpy(output, tmp_buf, tmp_len);
        *output_len = tmp_len;
        break;
    }

    return CE_OK;
}

/* ---- 首包协商 ---- */

/*
 * 协商数据格式 (简单二进制):
 * [1B version][1B supported_compress][1B supported_encrypt][1B reserved]
 * 响应格式:
 * [1B version][1B selected_compress][1B selected_encrypt][1B key_len][N key]
 */

CeResult ce_codec_negotiate_encode(uint32_t supported_compress,
                                     uint32_t supported_encrypt,
                                     uint8_t* out_buf, uint32_t* out_len) {
    if (!out_buf || !out_len || *out_len < 4) return CE_ERR;
    out_buf[0] = 1; /* version */
    out_buf[1] = (uint8_t)(supported_compress & 0xFF);
    out_buf[2] = (uint8_t)(supported_encrypt & 0xFF);
    out_buf[3] = 0; /* reserved */
    *out_len = 4;
    return CE_OK;
}

CeResult ce_codec_negotiate_select(const uint8_t* client_buf, uint32_t client_len,
                                     CeCodecConfig* out_config) {
    if (!client_buf || client_len < 4 || !out_config) return CE_ERR;

    uint8_t client_compress = client_buf[1];
    uint8_t client_encrypt = client_buf[2];
    uint32_t srv_compress = ce_codec_supported_compress();
    uint32_t srv_encrypt = ce_codec_supported_encrypt();

    memset(out_config, 0, sizeof(*out_config));

    /* 选择双方都支持的算法（优先级从高到低） */
    if (client_compress & srv_compress & (1 << 1)) out_config->compress = CE_CODEC_COMPRESS_LZ4;
    else if (client_compress & srv_compress & (1 << 2)) out_config->compress = CE_CODEC_COMPRESS_ZSTD;
    else if (client_compress & srv_compress & (1 << 3)) out_config->compress = CE_CODEC_COMPRESS_ZLIB;
    else out_config->compress = CE_CODEC_COMPRESS_NONE;

    if (client_encrypt & srv_encrypt & (1 << 2)) out_config->encrypt = CE_CODEC_ENCRYPT_AES;
    else if (client_encrypt & srv_encrypt & (1 << 3)) out_config->encrypt = CE_CODEC_ENCRYPT_RC4;
    else if (client_encrypt & srv_encrypt & (1 << 1)) out_config->encrypt = CE_CODEC_ENCRYPT_XOR;
    else out_config->encrypt = CE_CODEC_ENCRYPT_NONE;

    /* 生成随机密钥 */
    out_config->key_len = 16;
    for (int i = 0; i < 16; i++) {
        out_config->key[i] = (uint8_t)(rand() & 0xFF);
    }
    out_config->compress_level = 0;

    return CE_OK;
}

CeResult ce_codec_negotiate_resp_encode(const CeCodecConfig* config,
                                          uint8_t* out_buf, uint32_t* out_len) {
    if (!config || !out_buf || !out_len || *out_len < (uint32_t)(4 + config->key_len)) return CE_ERR;
    out_buf[0] = 1; /* version */
    out_buf[1] = (uint8_t)config->compress;
    out_buf[2] = (uint8_t)config->encrypt;
    out_buf[3] = (uint8_t)config->key_len;
    memcpy(out_buf + 4, config->key, config->key_len);
    *out_len = 4 + config->key_len;
    return CE_OK;
}

CeResult ce_codec_negotiate_resp_decode(const uint8_t* resp_buf, uint32_t resp_len,
                                          CeCodecConfig* out_config) {
    if (!resp_buf || resp_len < 4 || !out_config) return CE_ERR;

    memset(out_config, 0, sizeof(*out_config));
    out_config->compress = (CeCompressType)resp_buf[1];
    out_config->encrypt = (CeEncryptType)resp_buf[2];
    out_config->key_len = resp_buf[3];
    if (out_config->key_len > 32) return CE_ERR;
    if (resp_len < (uint32_t)(4 + out_config->key_len)) return CE_ERR;
    memcpy(out_config->key, resp_buf + 4, out_config->key_len);
    out_config->compress_level = 0;

    return CE_OK;
}

/* ---- 工具函数 ---- */

const char* ce_codec_compress_name(CeCompressType type) {
    switch (type) {
    case CE_CODEC_COMPRESS_NONE: return "none";
    case CE_CODEC_COMPRESS_LZ4:  return "lz4";
    case CE_CODEC_COMPRESS_ZSTD: return "zstd";
    case CE_CODEC_COMPRESS_ZLIB: return "zlib";
    default: return "unknown";
    }
}

const char* ce_codec_encrypt_name(CeEncryptType type) {
    switch (type) {
    case CE_CODEC_ENCRYPT_NONE: return "none";
    case CE_CODEC_ENCRYPT_XOR:  return "xor";
    case CE_CODEC_ENCRYPT_AES:  return "aes";
    case CE_CODEC_ENCRYPT_RC4:  return "rc4";
    default: return "unknown";
    }
}

CeBool ce_codec_compress_available(CeCompressType type) {
    switch (type) {
    case CE_CODEC_COMPRESS_NONE: return CE_TRUE;
    case CE_CODEC_COMPRESS_LZ4:  return HAVE_LZ4 ? CE_TRUE : CE_FALSE;
    case CE_CODEC_COMPRESS_ZSTD: return HAVE_ZSTD ? CE_TRUE : CE_FALSE;
    case CE_CODEC_COMPRESS_ZLIB: return HAVE_ZLIB ? CE_TRUE : CE_FALSE;
    default: return CE_FALSE;
    }
}

CeBool ce_codec_encrypt_available(CeEncryptType type) {
    switch (type) {
    case CE_CODEC_ENCRYPT_NONE: return CE_TRUE;
    case CE_CODEC_ENCRYPT_XOR:  return CE_TRUE;
    case CE_CODEC_ENCRYPT_AES:  return HAVE_OPENSSL ? CE_TRUE : CE_FALSE;
    case CE_CODEC_ENCRYPT_RC4:  return CE_TRUE;
    default: return CE_FALSE;
    }
}

uint32_t ce_codec_supported_compress(void) {
    uint32_t mask = 1; /* NONE 始终支持 */
#if HAVE_LZ4
    mask |= (1 << 1);
#endif
#if HAVE_ZSTD
    mask |= (1 << 2);
#endif
#if HAVE_ZLIB
    mask |= (1 << 3);
#endif
    return mask;
}

uint32_t ce_codec_supported_encrypt(void) {
    uint32_t mask = 1; /* NONE 始终支持 */
    mask |= (1 << 1);  /* XOR 始终支持 */
#if HAVE_OPENSSL
    mask |= (1 << 2);  /* AES */
#endif
    mask |= (1 << 3);  /* RC4 始终支持 */
    return mask;
}
