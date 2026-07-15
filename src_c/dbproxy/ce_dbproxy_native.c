/*
 * ChaosEngine DBProxy 原生 MongoDB 驱动 - 实现
 *
 * 使用 libmongoc-1.0 C 驱动直接操作 MongoDB。
 * 连接池 + 批量写入 + upsert。
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 */

#ifdef HAVE_MONGOC

#define _POSIX_C_SOURCE 200112L

#include "dbproxy/ce_dbproxy_native.h"
#include "public_api/ce_log.h"

#include <mongoc/mongoc.h>
#include <stdlib.h>
#include <string.h>

/* ---- 内部结构 ---- */

#define CE_DBPROXY_DB_NAME       "chaos_engine"
#define CE_DBPROXY_COLL_NAME     "players"

/** 驱动上下文 */
struct CeDbproxyNativeCtx {
    mongoc_client_pool_t*  pool;          /* 连接池 */
    mongoc_uri_t*          uri;           /* MongoDB URI */
    char                   db_name[64];   /* 数据库名 */
    char                   coll_name[64]; /* 集合名 */
    CeDbproxyNativeStats   stats;         /* 统计 */
};

/* ---- 生命周期 ---- */

CeDbproxyNativeCtx* ce_dbproxy_native_init(const char* mongo_uri, int pool_size) {
    if (!mongo_uri || pool_size <= 0) {
        CE_LOG_ERROR("DBPROXY", "native_init: invalid params");
        return NULL;
    }

    /* 初始化 mongoc 全局（幂等，多次调用安全） */
    mongoc_init();

    CeDbproxyNativeCtx* ctx = (CeDbproxyNativeCtx*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        CE_LOG_ERROR("DBPROXY", "native_init: calloc failed");
        return NULL;
    }

    /* 解析 URI */
    bson_error_t err;
    ctx->uri = mongoc_uri_new_with_error(mongo_uri, &err);
    if (!ctx->uri) {
        CE_LOG_ERROR("DBPROXY", "native_init: invalid URI '%s': %s", mongo_uri, err.message);
        free(ctx);
        return NULL;
    }

    /* 创建连接池 */
    ctx->pool = mongoc_client_pool_new(ctx->uri);
    if (!ctx->pool) {
        CE_LOG_ERROR("DBPROXY", "native_init: pool_new failed");
        mongoc_uri_destroy(ctx->uri);
        free(ctx);
        return NULL;
    }

    /* 设置连接池大小 */
    mongoc_client_pool_max_size(ctx->pool, (uint32_t)pool_size);
    mongoc_client_pool_min_size(ctx->pool, 1);

    strncpy(ctx->db_name, CE_DBPROXY_DB_NAME, sizeof(ctx->db_name) - 1);
    strncpy(ctx->coll_name, CE_DBPROXY_COLL_NAME, sizeof(ctx->coll_name) - 1);

    CE_LOG_INFO("DBPROXY", "native_init: pool_size=%d, uri=%s, db=%s, coll=%s",
                pool_size, mongo_uri, ctx->db_name, ctx->coll_name);

    return ctx;
}

void ce_dbproxy_native_shutdown(CeDbproxyNativeCtx* ctx) {
    if (!ctx) return;

    if (ctx->pool) {
        mongoc_client_pool_destroy(ctx->pool);
    }
    if (ctx->uri) {
        mongoc_uri_destroy(ctx->uri);
    }

    CE_LOG_INFO("DBPROXY", "native_shutdown: saves=%lu, loads=%lu, errors=%lu",
                (unsigned long)ctx->stats.total_saves,
                (unsigned long)ctx->stats.total_loads,
                (unsigned long)ctx->stats.total_errors);

    free(ctx);
    /* 注意：不调用 mongoc_cleanup()，因为可能其他模块也在用 */
}

/* ---- 单条操作 ---- */

CeResult ce_dbproxy_native_save(CeDbproxyNativeCtx* ctx,
                                  uint64_t player_id,
                                  const uint8_t* data, uint32_t len) {
    if (!ctx || !data || len == 0) return CE_ERR;

    mongoc_client_t* client = mongoc_client_pool_pop(ctx->pool);
    if (!client) {
        CE_LOG_ERROR("DBPROXY", "native_save: pool_pop failed for player %lu",
                     (unsigned long)player_id);
        ctx->stats.total_errors++;
        return CE_ERR;
    }

    mongoc_collection_t* coll = mongoc_client_get_collection(client,
                                                               ctx->db_name, ctx->coll_name);

    /* 构造查询条件: { _id: player_id } */
    bson_t* query = BCON_NEW("_id", BCON_INT64((int64_t)player_id));

    /* 构造更新: { $set: { data: <binary> } } */
    bson_t* update = bson_new();
    bson_t  set_doc;
    bson_append_document_begin(update, "$set", 4, &set_doc);
    bson_append_binary(&set_doc, "data", 4, BSON_SUBTYPE_BINARY, data, (uint32_t)len);
    bson_append_document_end(update, &set_doc);

    bson_error_t err;
    bool ok = mongoc_collection_update(coll,
                                        MONGOC_UPDATE_UPSERT,
                                        query, update, &err);

    if (!ok) {
        CE_LOG_ERROR("DBPROXY", "native_save: update failed for player %lu: %s",
                     (unsigned long)player_id, err.message);
        ctx->stats.total_errors++;
    } else {
        ctx->stats.total_saves++;
    }

    bson_destroy(query);
    bson_destroy(update);
    mongoc_collection_destroy(coll);
    mongoc_client_pool_push(ctx->pool, client);

    return ok ? CE_OK : CE_ERR;
}

CeResult ce_dbproxy_native_load(CeDbproxyNativeCtx* ctx,
                                  uint64_t player_id,
                                  uint8_t** out_data, uint32_t* out_len) {
    if (!ctx || !out_data || !out_len) return CE_ERR;
    *out_data = NULL;
    *out_len = 0;

    mongoc_client_t* client = mongoc_client_pool_pop(ctx->pool);
    if (!client) {
        CE_LOG_ERROR("DBPROXY", "native_load: pool_pop failed for player %lu",
                     (unsigned long)player_id);
        ctx->stats.total_errors++;
        return CE_ERR;
    }

    mongoc_collection_t* coll = mongoc_client_get_collection(client,
                                                               ctx->db_name, ctx->coll_name);

    bson_t* query = BCON_NEW("_id", BCON_INT64((int64_t)player_id));
    bson_t* opts = BCON_NEW("projection", "{", "data", BCON_INT32(1), "}");

    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(coll, query, opts, NULL);

    const bson_t* doc;
    bool found = false;
    CeResult result = CE_ERR;

    while (mongoc_cursor_next(cursor, &doc)) {
        found = true;
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, "data") &&
            BSON_ITER_HOLDS_BINARY(&iter)) {
            bson_subtype_t subtype;
            uint32_t bin_len = 0;
            const uint8_t* bin_data = NULL;
            bson_iter_binary(&iter, &subtype, &bin_len, &bin_data);

            if (bin_data && bin_len > 0) {
                *out_data = (uint8_t*)malloc(bin_len);
                if (*out_data) {
                    memcpy(*out_data, bin_data, bin_len);
                    *out_len = bin_len;
                    result = CE_OK;
                    ctx->stats.total_loads++;
                }
            }
        }
        break;  /* 只取第一条 */
    }

    if (!found) {
        CE_LOG_WARN("DBPROXY", "native_load: player %lu not found",
                    (unsigned long)player_id);
        ctx->stats.total_errors++;
    }

    bson_error_t cerr;
    if (mongoc_cursor_error(cursor, &cerr)) {
        CE_LOG_ERROR("DBPROXY", "native_load: cursor error for player %lu: %s",
                     (unsigned long)player_id, cerr.message);
        ctx->stats.total_errors++;
        result = CE_ERR;
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    bson_destroy(opts);
    mongoc_collection_destroy(coll);
    mongoc_client_pool_push(ctx->pool, client);

    return result;
}

/* ---- 批量操作 ---- */

CeResult ce_dbproxy_native_batch_save(CeDbproxyNativeCtx* ctx,
                                        const uint64_t* ids,
                                        const uint8_t** datas,
                                        const uint32_t* lens,
                                        int count) {
    if (!ctx || !ids || !datas || !lens || count <= 0) return CE_ERR;

    mongoc_client_t* client = mongoc_client_pool_pop(ctx->pool);
    if (!client) {
        CE_LOG_ERROR("DBPROXY", "native_batch_save: pool_pop failed");
        ctx->stats.total_errors++;
        return CE_ERR;
    }

    mongoc_collection_t* coll = mongoc_client_get_collection(client,
                                                               ctx->db_name, ctx->coll_name);

    mongoc_bulk_operation_t* bulk = mongoc_collection_create_bulk_operation_with_opts(coll);
    if (!bulk) {
        CE_LOG_ERROR("DBPROXY", "native_batch_save: create_bulk failed");
        mongoc_collection_destroy(coll);
        mongoc_client_pool_push(ctx->pool, client);
        ctx->stats.total_errors++;
        return CE_ERR;
    }

    /* 添加 upsert 操作 */
    for (int i = 0; i < count; i++) {
        bson_t* query = BCON_NEW("_id", BCON_INT64((int64_t)ids[i]));

        bson_t* update = bson_new();
        bson_t  set_doc;
        bson_append_document_begin(update, "$set", 4, &set_doc);
        if (datas[i] && lens[i] > 0) {
            bson_append_binary(&set_doc, "data", 4, BSON_SUBTYPE_BINARY,
                               datas[i], (uint32_t)lens[i]);
        }
        bson_append_document_end(update, &set_doc);

        mongoc_bulk_operation_update(bulk, query, update, true);  /* true = upsert */

        bson_destroy(query);
        bson_destroy(update);
    }

    bson_error_t err;
    bson_t* reply = bson_new();
    bool ok = mongoc_bulk_operation_execute(bulk, reply, &err);

    if (!ok) {
        CE_LOG_ERROR("DBPROXY", "native_batch_save: execute failed: %s", err.message);
        ctx->stats.total_errors++;
    } else {
        ctx->stats.total_batch_saves++;
        ctx->stats.total_saves += (uint64_t)count;
        CE_LOG_INFO("DBPROXY", "native_batch_save: %d records saved", count);
    }

    bson_destroy(reply);
    mongoc_bulk_operation_destroy(bulk);
    mongoc_collection_destroy(coll);
    mongoc_client_pool_push(ctx->pool, client);

    return ok ? CE_OK : CE_ERR;
}

/* ---- 统计 ---- */

CeResult ce_dbproxy_native_get_stats(CeDbproxyNativeCtx* ctx,
                                       CeDbproxyNativeStats* out) {
    if (!ctx || !out) return CE_ERR;
    *out = ctx->stats;
    return CE_OK;
}

#else  /* !HAVE_MONGOC */

/* 无 libmongoc 时提供空实现，避免链接错误 */

#include "dbproxy/ce_dbproxy_native.h"
#include "public_api/ce_log.h"

CeDbproxyNativeCtx* ce_dbproxy_native_init(const char* mongo_uri, int pool_size) {
    (void)mongo_uri; (void)pool_size;
    CE_LOG_WARN("DBPROXY", "native_init: libmongoc not available, compiled out");
    return NULL;
}

void ce_dbproxy_native_shutdown(CeDbproxyNativeCtx* ctx) {
    (void)ctx;
}

CeResult ce_dbproxy_native_save(CeDbproxyNativeCtx* ctx, uint64_t player_id,
                                  const uint8_t* data, uint32_t len) {
    (void)ctx; (void)player_id; (void)data; (void)len;
    return CE_ERR;
}

CeResult ce_dbproxy_native_load(CeDbproxyNativeCtx* ctx, uint64_t player_id,
                                  uint8_t** out_data, uint32_t* out_len) {
    (void)ctx; (void)player_id; (void)out_data; (void)out_len;
    return CE_ERR;
}

CeResult ce_dbproxy_native_batch_save(CeDbproxyNativeCtx* ctx,
                                        const uint64_t* ids, const uint8_t** datas,
                                        const uint32_t* lens, int count) {
    (void)ctx; (void)ids; (void)datas; (void)lens; (void)count;
    return CE_ERR;
}

CeResult ce_dbproxy_native_get_stats(CeDbproxyNativeCtx* ctx,
                                       CeDbproxyNativeStats* out) {
    (void)ctx; (void)out;
    return CE_ERR;
}

#endif /* HAVE_MONGOC */
