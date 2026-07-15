/*
 * ChaosEngine DBProxy 原生 MongoDB 驱动 - 头文件
 *
 * 使用 libmongoc-1.0 原生 C 驱动直接操作 MongoDB，
 * 替代旧的 mongosh 命令行方式（性能提升 1000x+）。
 *
 * 特性：
 *   - 连接池（mongoc_client_pool_t）
 *   - 批量写入（mongoc_bulk_operation_t）
 *   - 线程安全（client_pool 天然线程安全）
 *   - upsert 语义（存在则更新，不存在则插入）
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 *
 * 条件编译：需安装 libmongoc-dev 且 CMake 检测到 HAVE_MONGOC=1
 */

#ifndef CE_DBPROXY_NATIVE_H
#define CE_DBPROXY_NATIVE_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

/** 不透明 DBProxy 原生驱动上下文 */
typedef struct CeDbproxyNativeCtx CeDbproxyNativeCtx;

/* ---- 统计 ---- */

/** DBProxy 原生驱动统计信息 */
typedef struct CeDbproxyNativeStats {
    uint64_t  total_saves;      /* 总保存次数 */
    uint64_t  total_loads;      /* 总加载次数 */
    uint64_t  total_batch_saves;/* 总批量保存次数 */
    uint64_t  total_errors;     /* 总错误次数 */
} CeDbproxyNativeStats;

/* ---- 生命周期 ---- */

/**
 * 初始化原生 MongoDB 驱动
 *
 * @param mongo_uri   MongoDB 连接 URI (如 "mongodb://localhost:27017")
 * @param pool_size   连接池大小（建议 4-16）
 * @return            驱动上下文，NULL 失败
 */
CeDbproxyNativeCtx* ce_dbproxy_native_init(const char* mongo_uri, int pool_size);

/**
 * 关闭驱动，释放连接池等资源
 */
void ce_dbproxy_native_shutdown(CeDbproxyNativeCtx* ctx);

/* ---- 单条操作 ---- */

/**
 * 保存玩家数据（upsert 语义）
 *
 * @param ctx        驱动上下文
 * @param player_id  玩家 ID
 * @param data       二进制数据
 * @param len        数据长度
 * @return           CE_OK 成功
 */
CeResult ce_dbproxy_native_save(CeDbproxyNativeCtx* ctx,
                                  uint64_t player_id,
                                  const uint8_t* data, uint32_t len);

/**
 * 加载玩家数据
 *
 * @param ctx        驱动上下文
 * @param player_id  玩家 ID
 * @param out_data   输出数据（调用方负责 free）
 * @param out_len    输出数据长度
 * @return           CE_OK 成功，CE_ERR 未找到或失败
 */
CeResult ce_dbproxy_native_load(CeDbproxyNativeCtx* ctx,
                                  uint64_t player_id,
                                  uint8_t** out_data, uint32_t* out_len);

/* ---- 批量操作 ---- */

/**
 * 批量保存玩家数据（使用 bulk operation）
 *
 * @param ctx        驱动上下文
 * @param ids        玩家 ID 数组
 * @param datas      数据数组
 * @param lens       数据长度数组
 * @param count      数量
 * @return           CE_OK 成功
 */
CeResult ce_dbproxy_native_batch_save(CeDbproxyNativeCtx* ctx,
                                        const uint64_t* ids,
                                        const uint8_t** datas,
                                        const uint32_t* lens,
                                        int count);

/* ---- 统计 ---- */

/**
 * 获取驱动统计信息
 */
CeResult ce_dbproxy_native_get_stats(CeDbproxyNativeCtx* ctx,
                                       CeDbproxyNativeStats* out);

#ifdef __cplusplus
}
#endif

#endif /* CE_DBPROXY_NATIVE_H */
