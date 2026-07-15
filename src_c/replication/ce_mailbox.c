/*
 * ChaosEngine Replication Mailbox - 实现
 *
 * entity_id -> server_id 映射表，基于线性探测哈希表。
 * 动态扩容: 初始 CE_REPL_INITIAL_MAILBOX_CAPACITY，满时 realloc 翻倍。
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 */

#include "replication/ce_mailbox.h"
#include "replication/ce_replication_internal.h"
#include "public_api/ce_log.h"
#include <string.h>

#define CE_MAILBOX_EMPTY_KEY    UINT64_MAX   /* 空槽位标记 */

/* ---- 辅助: FNV-1a 哈希 (基于当前容量) ---- */

static uint32_t mailbox_hash(uint64_t key, uint32_t capacity) {
    uint64_t h = 14695981039346656037ULL;
    h ^= key;
    h *= 1099511628211ULL;
    return (uint32_t)(h % capacity);
}

/* ---- 公共 API ---- */

CeResult ce_mailbox_register(CeReplContext* ctx, uint64_t entity_id, uint32_t server_id) {
    if (!ctx) return CE_ERR;

    uint32_t cap = ctx->mailbox_capacity;
    uint32_t idx = mailbox_hash(entity_id, cap);

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t probe = (idx + i) % cap;

        if (ctx->mailbox_keys[probe] == entity_id) {
            /* 已存在，覆盖 */
            ctx->mailbox_values[probe] = server_id;
            CE_LOG_INFO("MAILBOX", "overwrite entity %lu -> server %u", (unsigned long)entity_id, server_id);
            return CE_OK;
        }

        if (ctx->mailbox_keys[probe] == CE_MAILBOX_EMPTY_KEY) {
            /* 空槽位，插入 */
            ctx->mailbox_keys[probe] = entity_id;
            ctx->mailbox_values[probe] = server_id;
            ctx->mailbox_count++;
            CE_LOG_INFO("MAILBOX", "register entity %lu -> server %u (count=%u)",
                        (unsigned long)entity_id, server_id, ctx->mailbox_count);
            return CE_OK;
        }
    }

    /* 表满: 动态扩容后重试 */
    if (!ce_repl_ensure_mailbox_capacity(ctx, ctx->mailbox_count + 1)) {
        CE_LOG_ERROR("MAILBOX", "table full (%u entries) and expand failed, cannot register entity %lu",
                     ctx->mailbox_capacity, (unsigned long)entity_id);
        return CE_ERR;
    }
    /* 扩容后重新插入 */
    return ce_mailbox_register(ctx, entity_id, server_id);
}

void ce_mailbox_unregister(CeReplContext* ctx, uint64_t entity_id) {
    if (!ctx) return;

    uint32_t cap = ctx->mailbox_capacity;
    uint32_t idx = mailbox_hash(entity_id, cap);

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t probe = (idx + i) % cap;

        if (ctx->mailbox_keys[probe] == entity_id) {
            /* 找到，标记为删除 */
            ctx->mailbox_keys[probe] = CE_MAILBOX_EMPTY_KEY;
            ctx->mailbox_values[probe] = 0;
            ctx->mailbox_count--;

            CE_LOG_INFO("MAILBOX", "unregister entity %lu (count=%u)",
                        (unsigned long)entity_id, ctx->mailbox_count);

            /*
             * 线性探测哈希表删除后的 rehash:
             * 将后续连续的非空条目重新插入，避免产生空洞导致查找失败。
             */
            uint32_t next = (probe + 1) % cap;
            while (ctx->mailbox_keys[next] != CE_MAILBOX_EMPTY_KEY) {
                uint64_t rehash_key = ctx->mailbox_keys[next];
                uint32_t rehash_val = ctx->mailbox_values[next];

                /* 从原位置移除 */
                ctx->mailbox_keys[next] = CE_MAILBOX_EMPTY_KEY;
                ctx->mailbox_values[next] = 0;
                ctx->mailbox_count--;

                /* 重新插入 */
                uint32_t rehash_idx = mailbox_hash(rehash_key, cap);
                for (uint32_t j = 0; j < cap; j++) {
                    uint32_t rehash_probe = (rehash_idx + j) % cap;
                    if (ctx->mailbox_keys[rehash_probe] == CE_MAILBOX_EMPTY_KEY) {
                        ctx->mailbox_keys[rehash_probe] = rehash_key;
                        ctx->mailbox_values[rehash_probe] = rehash_val;
                        ctx->mailbox_count++;
                        break;
                    }
                }

                next = (next + 1) % cap;
            }

            return;
        }

        if (ctx->mailbox_keys[probe] == CE_MAILBOX_EMPTY_KEY) {
            /* 遇到空槽位，实体不存在 */
            return;
        }
    }
}

CeBool ce_mailbox_lookup(CeReplContext* ctx, uint64_t entity_id, uint32_t* out_server_id) {
    if (!ctx) return CE_FALSE;

    uint32_t cap = ctx->mailbox_capacity;
    uint32_t idx = mailbox_hash(entity_id, cap);

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t probe = (idx + i) % cap;

        if (ctx->mailbox_keys[probe] == entity_id) {
            if (out_server_id) {
                *out_server_id = ctx->mailbox_values[probe];
            }
            return CE_TRUE;
        }

        if (ctx->mailbox_keys[probe] == CE_MAILBOX_EMPTY_KEY) {
            /* 遇到空槽位，实体不存在 */
            return CE_FALSE;
        }
    }

    /* 表满且未找到 */
    return CE_FALSE;
}

uint32_t ce_mailbox_count(CeReplContext* ctx) {
    if (!ctx) return 0;
    return ctx->mailbox_count;
}
