/*
 * 编解码模块单元测试
 * 测试: 压缩/加密各算法, 编码/解码往返, 首包协商, 不压缩不加密透传
 */

#include "public_api/ce_types.h"
#include "codec/ce_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- 测试数据 ---- */

static const char* TEST_DATA =
    "Hello ChaosEngine! This is a test message for codec module. "
    "The quick brown fox jumps over the lazy dog. "
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
    "Repeated data repeated data repeated data repeated data.";

/* ---- 测试: 不压缩不加密透传 ---- */

static void test_none_none(void) {
    CeCodecConfig cfg = {
        .compress = CE_CODEC_COMPRESS_NONE,
        .encrypt = CE_CODEC_ENCRYPT_NONE,
        .key_len = 0,
        .compress_level = 0,
    };

    CeCodecCtx* ctx = ce_codec_create(&cfg);
    assert(ctx != NULL);

    uint8_t encoded[4096];
    uint32_t enc_len = sizeof(encoded);
    CeResult ret = ce_codec_encode(ctx, (const uint8_t*)TEST_DATA, strlen(TEST_DATA),
                                     encoded, &enc_len);
    assert(ret == CE_OK);
    assert(enc_len == strlen(TEST_DATA));  /* 透传，长度不变 */
    assert(memcmp(encoded, TEST_DATA, enc_len) == 0);

    uint8_t decoded[4096];
    uint32_t dec_len = sizeof(decoded);
    ret = ce_codec_decode(ctx, encoded, enc_len, decoded, &dec_len);
    assert(ret == CE_OK);
    assert(dec_len == strlen(TEST_DATA));
    assert(memcmp(decoded, TEST_DATA, dec_len) == 0);

    ce_codec_destroy(ctx);
    printf("[OK] test_none_none (透传)\n");
}

/* ---- 测试: XOR 加密 ---- */

static void test_none_xor(void) {
    CeCodecConfig cfg = {
        .compress = CE_CODEC_COMPRESS_NONE,
        .encrypt = CE_CODEC_ENCRYPT_XOR,
        .key_len = 16,
        .compress_level = 0,
    };
    memcpy(cfg.key, "0123456789ABCDEF", 16);

    CeCodecCtx* ctx = ce_codec_create(&cfg);
    assert(ctx != NULL);

    uint8_t encoded[4096];
    uint32_t enc_len = sizeof(encoded);
    CeResult ret = ce_codec_encode(ctx, (const uint8_t*)TEST_DATA, strlen(TEST_DATA),
                                     encoded, &enc_len);
    assert(ret == CE_OK);
    /* 加密后数据应不同 */
    assert(memcmp(encoded, TEST_DATA, enc_len) != 0);

    uint8_t decoded[4096];
    uint32_t dec_len = sizeof(decoded);
    ret = ce_codec_decode(ctx, encoded, enc_len, decoded, &dec_len);
    assert(ret == CE_OK);
    assert(dec_len == strlen(TEST_DATA));
    assert(memcmp(decoded, TEST_DATA, dec_len) == 0);

    ce_codec_destroy(ctx);
    printf("[OK] test_none_xor\n");
}

/* ---- 测试: RC4 加密 ---- */

static void test_none_rc4(void) {
    CeCodecConfig cfg = {
        .compress = CE_CODEC_COMPRESS_NONE,
        .encrypt = CE_CODEC_ENCRYPT_RC4,
        .key_len = 16,
        .compress_level = 0,
    };
    memcpy(cfg.key, "RC4SecretKey12345", 16);

    CeCodecCtx* ctx = ce_codec_create(&cfg);
    assert(ctx != NULL);

    uint8_t encoded[4096];
    uint32_t enc_len = sizeof(encoded);
    CeResult ret = ce_codec_encode(ctx, (const uint8_t*)TEST_DATA, strlen(TEST_DATA),
                                     encoded, &enc_len);
    assert(ret == CE_OK);
    assert(memcmp(encoded, TEST_DATA, enc_len) != 0);

    /* RC4 是流加密，需要重新创建上下文来解密（流状态） */
    CeCodecCtx* dec_ctx = ce_codec_create(&cfg);
    uint8_t decoded[4096];
    uint32_t dec_len = sizeof(decoded);
    ret = ce_codec_decode(dec_ctx, encoded, enc_len, decoded, &dec_len);
    assert(ret == CE_OK);
    assert(dec_len == strlen(TEST_DATA));
    assert(memcmp(decoded, TEST_DATA, dec_len) == 0);

    ce_codec_destroy(ctx);
    ce_codec_destroy(dec_ctx);
    printf("[OK] test_none_rc4\n");
}

/* ---- 测试: Zlib 压缩（如果可用） ---- */

static void test_zlib_none(void) {
    if (!ce_codec_compress_available(CE_CODEC_COMPRESS_ZLIB)) {
        printf("[SKIP] test_zlib_none (zlib not available)\n");
        return;
    }

    CeCodecConfig cfg = {
        .compress = CE_CODEC_COMPRESS_ZLIB,
        .encrypt = CE_CODEC_ENCRYPT_NONE,
        .key_len = 0,
        .compress_level = 6,
    };

    CeCodecCtx* ctx = ce_codec_create(&cfg);
    assert(ctx != NULL);

    uint8_t encoded[4096];
    uint32_t enc_len = sizeof(encoded);
    CeResult ret = ce_codec_encode(ctx, (const uint8_t*)TEST_DATA, strlen(TEST_DATA),
                                     encoded, &enc_len);
    assert(ret == CE_OK);
    printf("  zlib: %zu -> %u (%.0f%%)\n", strlen(TEST_DATA), enc_len,
           100.0 * enc_len / strlen(TEST_DATA));

    uint8_t decoded[4096];
    uint32_t dec_len = sizeof(decoded);
    ret = ce_codec_decode(ctx, encoded, enc_len, decoded, &dec_len);
    assert(ret == CE_OK);
    assert(dec_len == strlen(TEST_DATA));
    assert(memcmp(decoded, TEST_DATA, dec_len) == 0);

    ce_codec_destroy(ctx);
    printf("[OK] test_zlib_none\n");
}

/* ---- 测试: 压缩+加密组合 ---- */

static void test_zlib_xor(void) {
    if (!ce_codec_compress_available(CE_CODEC_COMPRESS_ZLIB)) {
        printf("[SKIP] test_zlib_xor (zlib not available)\n");
        return;
    }

    CeCodecConfig cfg = {
        .compress = CE_CODEC_COMPRESS_ZLIB,
        .encrypt = CE_CODEC_ENCRYPT_XOR,
        .key_len = 16,
        .compress_level = 6,
    };
    memcpy(cfg.key, "XorKeyForTesting!", 16);

    CeCodecCtx* enc_ctx = ce_codec_create(&cfg);
    CeCodecCtx* dec_ctx = ce_codec_create(&cfg);

    uint8_t encoded[4096];
    uint32_t enc_len = sizeof(encoded);
    ce_codec_encode(enc_ctx, (const uint8_t*)TEST_DATA, strlen(TEST_DATA),
                    encoded, &enc_len);

    uint8_t decoded[4096];
    uint32_t dec_len = sizeof(decoded);
    ce_codec_decode(dec_ctx, encoded, enc_len, decoded, &dec_len);

    assert(dec_len == strlen(TEST_DATA));
    assert(memcmp(decoded, TEST_DATA, dec_len) == 0);

    ce_codec_destroy(enc_ctx);
    ce_codec_destroy(dec_ctx);
    printf("[OK] test_zlib_xor (压缩+加密组合)\n");
}

/* ---- 测试: 首包协商 ---- */

static void test_negotiate(void) {
    /* 客户端声明支持的算法 */
    uint8_t client_buf[16];
    uint32_t client_len = sizeof(client_buf);
    ce_codec_negotiate_encode(
        ce_codec_supported_compress(),
        ce_codec_supported_encrypt(),
        client_buf, &client_len);

    /* 服务端选择算法 */
    CeCodecConfig srv_config;
    ce_codec_negotiate_select(client_buf, client_len, &srv_config);
    printf("  协商结果: compress=%s, encrypt=%s, key_len=%d\n",
           ce_codec_compress_name(srv_config.compress),
           ce_codec_encrypt_name(srv_config.encrypt),
           srv_config.key_len);

    /* 服务端发送选定配置给客户端 */
    uint8_t resp_buf[64];
    uint32_t resp_len = sizeof(resp_buf);
    ce_codec_negotiate_resp_encode(&srv_config, resp_buf, &resp_len);

    /* 客户端解析 */
    CeCodecConfig client_config;
    ce_codec_negotiate_resp_decode(resp_buf, resp_len, &client_config);
    assert(client_config.compress == srv_config.compress);
    assert(client_config.encrypt == srv_config.encrypt);
    assert(client_config.key_len == srv_config.key_len);
    assert(memcmp(client_config.key, srv_config.key, srv_config.key_len) == 0);

    printf("[OK] test_negotiate (首包协商)\n");
}

/* ---- 测试: 空数据 ---- */

static void test_empty_data(void) {
    CeCodecConfig cfg = {
        .compress = CE_CODEC_COMPRESS_NONE,
        .encrypt = CE_CODEC_ENCRYPT_NONE,
    };
    CeCodecCtx* ctx = ce_codec_create(&cfg);

    uint8_t encoded[16];
    uint32_t enc_len = sizeof(encoded);
    ce_codec_encode(ctx, (const uint8_t*)"", 0, encoded, &enc_len);
    assert(enc_len == 0);

    uint8_t decoded[16];
    uint32_t dec_len = sizeof(decoded);
    ce_codec_decode(ctx, encoded, enc_len, decoded, &dec_len);
    assert(dec_len == 0);

    ce_codec_destroy(ctx);
    printf("[OK] test_empty_data\n");
}

/* ---- 测试: 大数据 ---- */

static void test_large_data(void) {
    CeCodecConfig cfg = {
        .compress = CE_CODEC_COMPRESS_NONE,
        .encrypt = CE_CODEC_ENCRYPT_XOR,
        .key_len = 16,
    };
    memcpy(cfg.key, "LargeDataTestKey!", 16);

    CeCodecCtx* enc_ctx = ce_codec_create(&cfg);
    CeCodecCtx* dec_ctx = ce_codec_create(&cfg);

    /* 生成 32KB 重复数据 */
    uint8_t large_data[32768];
    for (int i = 0; i < 32768; i++) {
        large_data[i] = (uint8_t)(i & 0xFF);
    }

    uint8_t encoded[49152];
    uint32_t enc_len = sizeof(encoded);
    ce_codec_encode(enc_ctx, large_data, 32768, encoded, &enc_len);

    uint8_t decoded[32768];
    uint32_t dec_len = sizeof(decoded);
    ce_codec_decode(dec_ctx, encoded, enc_len, decoded, &dec_len);

    assert(dec_len == 32768);
    assert(memcmp(decoded, large_data, 32768) == 0);

    ce_codec_destroy(enc_ctx);
    ce_codec_destroy(dec_ctx);
    printf("[OK] test_large_data (32KB XOR)\n");
}

/* ---- 工具函数测试 ---- */

static void test_utils(void) {
    assert(ce_codec_compress_available(CE_CODEC_COMPRESS_NONE) == CE_TRUE);
    assert(ce_codec_encrypt_available(CE_CODEC_ENCRYPT_NONE) == CE_TRUE);
    assert(ce_codec_encrypt_available(CE_CODEC_ENCRYPT_XOR) == CE_TRUE);

    uint32_t comp_mask = ce_codec_supported_compress();
    assert(comp_mask & 1);  /* NONE 始终在 bit0 */

    uint32_t enc_mask = ce_codec_supported_encrypt();
    assert(enc_mask & 1);  /* NONE 始终在 bit0 */
    assert(enc_mask & 2);  /* XOR 始终在 bit1 */

    printf("[OK] test_utils\n");
}

int main(void) {
    printf("=== test_codec ===\n");
    test_none_none();
    test_none_xor();
    test_none_rc4();
    test_zlib_none();
    test_zlib_xor();
    test_negotiate();
    test_empty_data();
    test_large_data();
    test_utils();
    printf("=== All tests passed ===\n");
    return 0;
}
