/*
 * ChaosEngine RPC Channel — 实现
 *
 * 远程过程调用通道实现：
 *   - Handler 注册/分发
 *   - 二进制序列化: [1B msg_type][4B call_id][2B method_len][N method][4B params_len][N params]
 *   - MERGE_ATTRS: 将脏属性附加到 RPC 数据
 *   - 可靠 RPC 确认/超时 (MVP: 占位)
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 */

#include "replication/ce_rpc_channel.h"
#include "replication/ce_replication_internal.h"
#include "public_api/ce_log.h"
#include <stdlib.h>
#include <string.h>

/* ---- 二进制协议常量 ---- */

#define CE_RPC_MSG_TYPE_CALL    0x01   /* RPC 调用 */
#define CE_RPC_MSG_TYPE_REPLY   0x02   /* RPC 回复 */
#define CE_RPC_HEADER_SIZE      11     /* 1B type + 4B call_id + 2B method_len + 4B params_len */

/* ---- RPC 发送 ---- */

CeResult ce_repl_rpc_send(CeReplContext* ctx, uint64_t entity_id,
                          CeRpcTarget target, CeRpcReliability reliability,
                          CeRpcSendFlag send_flags,
                          const char* method,
                          const uint8_t* params, uint32_t params_len) {
    if (!ctx || !method) return CE_ERR;

    uint32_t method_len = (uint32_t)strlen(method);
    if (method_len == 0 || method_len > CE_RPC_MAX_METHOD_NAME) {
        CE_LOG_ERROR("RPC", "invalid method name length: %u", method_len);
        return CE_ERR;
    }

    /* 分配 call_id */
    uint32_t call_id = ctx->rpc_call_id_counter++;

    /* 构建二进制 payload:
     * [1B msg_type][4B call_id][2B method_len][N method][4B params_len][N params]
     */
    uint32_t total_len = CE_RPC_HEADER_SIZE + method_len + params_len;

    if (total_len > CE_RPC_MAX_PAYLOAD) {
        CE_LOG_ERROR("RPC", "payload too large: %u > %u", total_len, CE_RPC_MAX_PAYLOAD);
        return CE_ERR;
    }

    uint8_t payload[CE_RPC_MAX_PAYLOAD];
    uint8_t* p = payload;

    /* msg_type */
    *p++ = CE_RPC_MSG_TYPE_CALL;

    /* call_id (little-endian) */
    *p++ = (uint8_t)(call_id & 0xFF);
    *p++ = (uint8_t)((call_id >> 8) & 0xFF);
    *p++ = (uint8_t)((call_id >> 16) & 0xFF);
    *p++ = (uint8_t)((call_id >> 24) & 0xFF);

    /* method_len (little-endian) */
    *p++ = (uint8_t)(method_len & 0xFF);
    *p++ = (uint8_t)((method_len >> 8) & 0xFF);

    /* method name */
    memcpy(p, method, method_len);
    p += method_len;

    /* params_len (little-endian) */
    *p++ = (uint8_t)(params_len & 0xFF);
    *p++ = (uint8_t)((params_len >> 8) & 0xFF);
    *p++ = (uint8_t)((params_len >> 16) & 0xFF);
    *p++ = (uint8_t)((params_len >> 24) & 0xFF);

    /* params */
    if (params && params_len > 0) {
        memcpy(p, params, params_len);
        p += params_len;
    }

    total_len = (uint32_t)(p - payload);

    /* MERGE_ATTRS: 将当前脏字段附加到 RPC 数据
     * MVP: 遍历脏实体表，将 entity_id 匹配的脏字段信息追加
     */
    if (send_flags & CE_RPC_SEND_MERGE_ATTRS) {
        /* 查找该实体的脏条目 */
        for (uint32_t di = 0; di < ctx->dirty_count; di++) {
            if (ctx->dirty_entities[di].entity_id == entity_id) {
                CeReplDirtyEntry* entry = &ctx->dirty_entities[di];

                /* 追加脏字段信息: [4B num_fields][for each: 2B comp_idx][2B field_idx] */
                uint32_t num_fields = 0;
                uint8_t* attr_start = p;
                p += 4; /* 预留 num_fields 位置 */

                for (uint32_t ci = 0; ci < ctx->component_count; ci++) {
                    const CeReplComponent* comp = &ctx->components[ci];
                    for (uint32_t fi = 0; fi < comp->field_count; fi++) {
                        uint32_t bit_idx = ci * CE_REPL_MAX_FIELDS_PER_COMP + fi;
                        uint32_t word = bit_idx / 64;
                        uint32_t bit  = bit_idx % 64;

                        if (word >= 4) continue;
                        if (!(entry->mask.bits[word] & (1ULL << bit))) continue;

                        /* 跳过 SERVER_ONLY 字段 */
                        if (comp->fields[fi].flags & CE_FLAG_SERVER_ONLY) continue;

                        if ((p + 4) > (payload + CE_RPC_MAX_PAYLOAD)) break;

                        *p++ = (uint8_t)(ci & 0xFF);
                        *p++ = (uint8_t)((ci >> 8) & 0xFF);
                        *p++ = (uint8_t)(fi & 0xFF);
                        *p++ = (uint8_t)((fi >> 8) & 0xFF);
                        num_fields++;
                    }
                }

                /* 回填 num_fields */
                attr_start[0] = (uint8_t)(num_fields & 0xFF);
                attr_start[1] = (uint8_t)((num_fields >> 8) & 0xFF);
                attr_start[2] = (uint8_t)((num_fields >> 16) & 0xFF);
                attr_start[3] = (uint8_t)((num_fields >> 24) & 0xFF);

                total_len = (uint32_t)(p - payload);
                break;
            }
        }
    }

    /* 加入 pending 队列 (用于可靠 RPC 的 ack/timeout 跟踪) */
    if (reliability == CE_RPC_RELIABLE) {
        if (!ce_repl_ensure_rpc_pending_capacity(ctx, ctx->rpc_pending_count + 1)) {
            CE_LOG_ERROR("RPC", "rpc_pending queue full and expand failed (count=%u)",
                         ctx->rpc_pending_count);
            /* 非致命: RPC 仍会发送, 只是无法跟踪 ack/timeout */
        } else {
            uint32_t idx = ctx->rpc_pending_count++;
            ctx->rpc_pending[idx].call_id = call_id;
            ctx->rpc_pending[idx].entity_id = entity_id;
            ctx->rpc_pending[idx].target = target;
            ctx->rpc_pending[idx].reliability = reliability;
            ctx->rpc_pending[idx].payload_len = total_len;
            /* 动态分配 payload 副本 */
            ctx->rpc_pending[idx].payload = (uint8_t*)malloc(total_len);
            if (ctx->rpc_pending[idx].payload) {
                memcpy(ctx->rpc_pending[idx].payload, payload, total_len);
            } else {
                ctx->rpc_pending[idx].payload_len = 0;
            }
            ctx->rpc_pending[idx].timeout = 1.0f; /* 1 second timeout */
        }
    }

    CE_LOG_INFO("RPC", "send method='%s' entity=%llu target=%d reliable=%d "
                "flags=0x%02x call_id=%u payload=%u bytes",
                method, (unsigned long long)entity_id,
                (int)target, (int)reliability,
                (unsigned int)send_flags, call_id, total_len);

    /* MVP: 实际网络发送由 Gateway 层处理，这里仅序列化并记录 */
    (void)target;
    return CE_OK;
}

/* ---- Handler 注册 ---- */

CeResult ce_repl_rpc_register_handler(CeReplContext* ctx, const char* method,
                                      CeRpcHandler handler, void* user_data) {
    if (!ctx || !method || !handler) return CE_ERR;

    uint32_t method_len = (uint32_t)strlen(method);
    if (method_len == 0 || method_len >= CE_REPL_MAX_FIELD_NAME) {
        CE_LOG_ERROR("RPC", "invalid method name length: %u", method_len);
        return CE_ERR;
    }

    /* 检查重复 */
    for (uint32_t i = 0; i < ctx->rpc_handler_count; i++) {
        if (strcmp(ctx->rpc_handlers[i].method, method) == 0) {
            CE_LOG_ERROR("RPC", "handler already registered for method '%s'", method);
            return CE_ERR;
        }
    }

    /* 检查容量 */
    if (ctx->rpc_handler_count >= CE_REPL_MAX_RPC_HANDLERS) {
        CE_LOG_ERROR("RPC", "handler table full (%u max)", CE_REPL_MAX_RPC_HANDLERS);
        return CE_ERR;
    }

    CeRpcHandlerEntry* entry = &ctx->rpc_handlers[ctx->rpc_handler_count];
    strncpy(entry->method, method, CE_REPL_MAX_FIELD_NAME - 1);
    entry->method[CE_REPL_MAX_FIELD_NAME - 1] = '\0';
    entry->handler = handler;
    entry->user_data = user_data;
    ctx->rpc_handler_count++;

    CE_LOG_INFO("RPC", "registered handler for method '%s' (total: %u)",
                method, ctx->rpc_handler_count);

    return CE_OK;
}

/* ---- RPC 分发 ---- */

void ce_repl_rpc_dispatch(CeReplContext* ctx, uint64_t source_entity,
                          const uint8_t* data, uint32_t data_len) {
    if (!ctx || !data || data_len < CE_RPC_HEADER_SIZE) {
        CE_LOG_WARN("RPC", "dispatch: invalid data (len=%u, min=%u)",
                    data_len, CE_RPC_HEADER_SIZE);
        return;
    }

    const uint8_t* p = data;

    /* msg_type */
    uint8_t msg_type = *p++;

    /* call_id */
    uint32_t call_id = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    /* method_len */
    uint32_t method_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    p += 2;

    if (method_len == 0 || method_len >= CE_REPL_MAX_FIELD_NAME) {
        CE_LOG_ERROR("RPC", "dispatch: invalid method_len=%u", method_len);
        return;
    }

    if ((p + method_len + 4) > (data + data_len)) {
        CE_LOG_ERROR("RPC", "dispatch: truncated data (method)");
        return;
    }

    /* method name */
    char method[CE_REPL_MAX_FIELD_NAME];
    memcpy(method, p, method_len);
    method[method_len] = '\0';
    p += method_len;

    /* params_len */
    uint32_t params_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;

    if ((p + params_len) > (data + data_len)) {
        CE_LOG_ERROR("RPC", "dispatch: truncated data (params)");
        return;
    }

    /* 查找 handler */
    CeRpcHandlerFn handler_fn = NULL;
    void* user_data = NULL;

    for (uint32_t i = 0; i < ctx->rpc_handler_count; i++) {
        if (strcmp(ctx->rpc_handlers[i].method, method) == 0) {
            handler_fn = ctx->rpc_handlers[i].handler;
            user_data = ctx->rpc_handlers[i].user_data;
            break;
        }
    }

    if (!handler_fn) {
        CE_LOG_WARN("RPC", "dispatch: no handler for method '%s' (type=%u, call_id=%u)",
                    method, (unsigned int)msg_type, call_id);
        return;
    }

    CE_LOG_INFO("RPC", "dispatch method='%s' source=%llu type=%u call_id=%u params_len=%u",
                method, (unsigned long long)source_entity,
                (unsigned int)msg_type, call_id, params_len);

    /* 调用 handler */
    handler_fn(source_entity, p, params_len, user_data);
}

/* ---- RPC Tick ---- */

void ce_repl_rpc_tick(CeReplContext* ctx, float dt) {
    if (!ctx) return;

    /* MVP: 占位 — 可靠 RPC 的 ack/timeout 处理将在后续 Phase 实现 */
    (void)dt;

    /* 清理已超时的 pending RPC */
    uint32_t write_idx = 0;
    for (uint32_t i = 0; i < ctx->rpc_pending_count; i++) {
        ctx->rpc_pending[i].timeout -= dt;
        if (ctx->rpc_pending[i].timeout <= 0.0f) {
            /* 超时: 释放 payload, 在 MVP 中丢弃，后续 Phase 实现重传 */
            CE_LOG_WARN("RPC", "pending RPC call_id=%u timed out",
                        ctx->rpc_pending[i].call_id);
            free(ctx->rpc_pending[i].payload);
            ctx->rpc_pending[i].payload = NULL;
            /* 不保留此项 (通过不复制到 write_idx 来丢弃) */
        } else {
            /* 保留 */
            if (write_idx != i) {
                ctx->rpc_pending[write_idx] = ctx->rpc_pending[i];
                /* 清空原位置以避免 double-free */
                ctx->rpc_pending[i].payload = NULL;
            }
            write_idx++;
        }
    }
    ctx->rpc_pending_count = write_idx;
}
