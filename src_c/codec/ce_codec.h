/*
 * ChaosEngine 统一编解码接口 - 头文件
 *
 * 引擎层封装压缩/加密，提供统一接口。
 * 首包协商后，客户端和服务端使用相同的编解码配置。
 *
 * 支持的压缩算法:
 *   CE_CODEC_COMPRESS_NONE  - 不压缩
 *   CE_CODEC_COMPRESS_LZ4   - LZ4 (高性能，需 liblz4)
 *   CE_CODEC_COMPRESS_ZSTD  - Zstd (高压缩率，需 libzstd)
 *   CE_CODEC_COMPRESS_ZLIB  - Zlib (经典，系统自带)
 *
 * 支持的加密算法:
 *   CE_CODEC_ENCRYPT_NONE   - 不加密
 *   CE_CODEC_ENCRYPT_XOR    - XOR 流加密 (轻量，无外部依赖)
 *   CE_CODEC_ENCRYPT_AES    - AES-128-CTR (需 libcrypto)
 *   CE_CODEC_ENCRYPT_RC4    - RC4 (经典，系统自带)
 *
 * 纯 C99，ce_ 前缀。
 */

#ifndef CE_CODEC_H
#define CE_CODEC_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 压缩算法 ---- */

typedef enum {
    CE_CODEC_COMPRESS_NONE = 0,
    CE_CODEC_COMPRESS_LZ4  = 1,
    CE_CODEC_COMPRESS_ZSTD = 2,
    CE_CODEC_COMPRESS_ZLIB = 3,
} CeCompressType;

/* ---- 加密算法 ---- */

typedef enum {
    CE_CODEC_ENCRYPT_NONE = 0,
    CE_CODEC_ENCRYPT_XOR  = 1,
    CE_CODEC_ENCRYPT_AES  = 2,
    CE_CODEC_ENCRYPT_RC4  = 3,
} CeEncryptType;

/* ---- 编解码配置 ---- */

/** 编解码配置（首包协商后确定） */
typedef struct CeCodecConfig {
    CeCompressType  compress;     /* 压缩算法 */
    CeEncryptType   encrypt;      /* 加密算法 */
    uint8_t         key[32];      /* 密钥（最长 32 字节，AES 用 16） */
    int             key_len;      /* 密钥长度 */
    int             compress_level; /* 压缩级别 (1-9, 0=默认) */
} CeCodecConfig;

/* ---- 编解码上下文 ---- */

typedef struct CeCodecCtx CeCodecCtx;

/**
 * 创建编解码上下文
 *
 * @param config  编解码配置
 * @return        上下文，NULL 失败
 */
CeCodecCtx* ce_codec_create(const CeCodecConfig* config);

/**
 * 销毁编解码上下文
 */
void ce_codec_destroy(CeCodecCtx* ctx);

/**
 * 编码（压缩 → 加密）
 *
 * @param ctx       编解码上下文
 * @param input     原始数据
 * @param input_len 原始数据长度
 * @param output    输出缓冲区（调用方分配，建议 input_len * 1.5 + 128）
 * @param output_len 输入时为输出缓冲区大小，输出时为实际编码后长度
 * @return          CE_OK 成功
 */
CeResult ce_codec_encode(CeCodecCtx* ctx,
                           const uint8_t* input, uint32_t input_len,
                           uint8_t* output, uint32_t* output_len);

/**
 * 解码（解密 → 解压）
 *
 * @param ctx       编解码上下文
 * @param input     编码后数据
 * @param input_len 编码后数据长度
 * @param output    输出缓冲区（调用方分配）
 * @param output_len 输入时为输出缓冲区大小，输出时为实际解码后长度
 * @return          CE_OK 成功
 */
CeResult ce_codec_decode(CeCodecCtx* ctx,
                           const uint8_t* input, uint32_t input_len,
                           uint8_t* output, uint32_t* output_len);

/* ---- 首包协商 ---- */

/**
 * 将客户端支持的算法列表编码为协商数据（放入 LOGIN 包 payload）
 *
 * @param supported_compress  支持的压缩算法位数组（bit0=LZ4, bit1=Zstd, bit2=Zlib）
 * @param supported_encrypt   支持的加密算法位数组（bit0=XOR, bit1=AES, bit2=RC4）
 * @param out_buf             输出缓冲区
 * @param out_len             输出长度
 * @return                    CE_OK 成功
 */
CeResult ce_codec_negotiate_encode(uint32_t supported_compress,
                                     uint32_t supported_encrypt,
                                     uint8_t* out_buf, uint32_t* out_len);

/**
 * 服务端从客户端协商数据中选择算法
 *
 * @param client_buf       客户端协商数据
 * @param client_len       客户端协商数据长度
 * @param out_config       输出选定的配置
 * @return                 CE_OK 成功
 */
CeResult ce_codec_negotiate_select(const uint8_t* client_buf, uint32_t client_len,
                                     CeCodecConfig* out_config);

/**
 * 将服务端选定的配置编码为协商响应（放入 LOGIN_RESP 包 payload）
 */
CeResult ce_codec_negotiate_resp_encode(const CeCodecConfig* config,
                                          uint8_t* out_buf, uint32_t* out_len);

/**
 * 客户端从服务端响应中解析选定配置
 */
CeResult ce_codec_negotiate_resp_decode(const uint8_t* resp_buf, uint32_t resp_len,
                                          CeCodecConfig* out_config);

/* ---- 工具函数 ---- */

/** 获取算法名称 */
const char* ce_codec_compress_name(CeCompressType type);
const char* ce_codec_encrypt_name(CeEncryptType type);

/** 检查算法是否可用（编译时是否包含对应库） */
CeBool ce_codec_compress_available(CeCompressType type);
CeBool ce_codec_encrypt_available(CeEncryptType type);

/** 获取引擎支持的算法位数组 */
uint32_t ce_codec_supported_compress(void);
uint32_t ce_codec_supported_encrypt(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_CODEC_H */
