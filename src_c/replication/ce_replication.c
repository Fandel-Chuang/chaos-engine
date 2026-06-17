/*
 * ChaosEngine Replication Manager — 核心实现
 *
 * 统一属性复制管线：
 *   - 字段级同步策略 (AOI_BROADCAST / OWNER_ONLY / SERVER_ONLY / PERSIST)
 *   - 脏标收集 + 帧末批量 flush
 *   - 启动时值域校验 (不 clamp，越界直接拒绝启动)
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 */

#include "replication/ce_replication_internal.h"
#include "public_api/ce_log.h"
#include "core/ce_time.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 辅助: 从字段类型读取 int64_t 值 ---- */

int64_t ce_repl_read_field_value(CeReplFieldType type, const uint8_t* src) {
    switch (type) {
    case CE_REPL_TYPE_I8:   return (int64_t)(*(int8_t*)src);
    case CE_REPL_TYPE_I16:  return (int64_t)(*(int16_t*)src);
    case CE_REPL_TYPE_I32:  return (int64_t)(*(int32_t*)src);
    case CE_REPL_TYPE_I64:  return (*(int64_t*)src);
    case CE_REPL_TYPE_U8:   return (int64_t)(*(uint8_t*)src);
    case CE_REPL_TYPE_U16:  return (int64_t)(*(uint16_t*)src);
    case CE_REPL_TYPE_U32:  return (int64_t)(*(uint32_t*)src);
    case CE_REPL_TYPE_U64:  return (int64_t)(*(uint64_t*)src);  /* 可能溢出，但约束通常不会设到 U64_MAX */
    case CE_REPL_TYPE_F32:  return (int64_t)(*(float*)src);
    case CE_REPL_TYPE_F64:  return (int64_t)(*(double*)src);
    case CE_REPL_TYPE_BOOL: return (int64_t)(*(bool*)src);
    default:                return 0;  /* STRING/VEC/BLOB 不支持值域约束 */
    }
}

/* ---- 辅助: 简单哈希 ---- */

static uint32_t hash_entity_id(uint64_t id, uint32_t capacity) {
    /* FNV-1a 风格 */
    uint64_t h = 14695981039346656037ULL;
    h ^= id;
    h *= 1099511628211ULL;
    return (uint32_t)(h % capacity);
}

/* ---- 辅助: 在脏标表中查找或创建条目 ---- */

CeReplDirtyEntry* ce_repl_find_or_create_dirty(CeReplContext* ctx, uint64_t entity_id) {
    if (ctx->dirty_hash_capacity == 0) return NULL;

    uint32_t idx = hash_entity_id(entity_id, ctx->dirty_hash_capacity);

    /* 线性探测 */
    for (uint32_t i = 0; i < ctx->dirty_hash_capacity; i++) {
        uint32_t probe = (idx + i) % ctx->dirty_hash_capacity;
        if (ctx->dirty_hash_keys[probe] == entity_id) {
            /* 找到已有条目 */
            return &ctx->dirty_entities[ctx->dirty_hash_values[probe]];
        }
        if (ctx->dirty_hash_keys[probe] == 0 && ctx->dirty_count < CE_REPL_MAX_DIRTY_ENTITIES) {
            /* 空槽位，创建新条目 */
            uint32_t entry_idx = ctx->dirty_count++;
            ctx->dirty_hash_keys[probe] = entity_id;
            ctx->dirty_hash_values[probe] = entry_idx;

            CeReplDirtyEntry* entry = &ctx->dirty_entities[entry_idx];
            memset(entry, 0, sizeof(*entry));
            entry->entity_id = entity_id;
            entry->owner_client_id = ce_repl_find_owner(ctx, entity_id);
            return entry;
        }
    }

    /* 哈希表满或脏实体表满 */
    if (ctx->dirty_count >= CE_REPL_MAX_DIRTY_ENTITIES) {
        CE_LOG_ERROR("REPL", "dirty entity table full (%d entries). "
                     "Increase CE_REPL_MAX_DIRTY_ENTITIES.",
                     CE_REPL_MAX_DIRTY_ENTITIES);
    }
    return NULL;
}

/* ---- 辅助: 查找属主 ---- */

uint64_t ce_repl_find_owner(CeReplContext* ctx, uint64_t entity_id) {
    for (uint32_t i = 0; i < ctx->owner_count; i++) {
        if (ctx->owner_entity_ids[i] == entity_id) {
            return ctx->owner_client_ids[i];
        }
    }
    return 0;
}

/* ---- 生命周期 ---- */

CeReplContext* ce_repl_init(const CeReplConfig* config) {
    CeReplContext* ctx = (CeReplContext*)calloc(1, sizeof(CeReplContext));
    if (!ctx) {
        CE_LOG_ERROR("REPL", "failed to allocate context");
        return NULL;
    }

    /* 默认配置 */
    uint32_t max_dirty = config ? config->max_dirty_entities : CE_REPL_MAX_DIRTY_ENTITIES;
    if (max_dirty == 0) max_dirty = CE_REPL_MAX_DIRTY_ENTITIES;

    /* 初始化脏标哈希表 (容量为 2 倍以降低碰撞) */
    ctx->dirty_hash_capacity = max_dirty * 2;
    ctx->dirty_hash_keys = (uint64_t*)calloc(ctx->dirty_hash_capacity, sizeof(uint64_t));
    ctx->dirty_hash_values = (uint32_t*)calloc(ctx->dirty_hash_capacity, sizeof(uint32_t));

    /* 初始化属主映射 */
    ctx->owner_entity_ids = (uint64_t*)calloc(max_dirty, sizeof(uint64_t));
    ctx->owner_client_ids = (uint64_t*)calloc(max_dirty, sizeof(uint64_t));

    if (!ctx->dirty_hash_keys || !ctx->dirty_hash_values ||
        !ctx->owner_entity_ids || !ctx->owner_client_ids) {
        CE_LOG_ERROR("REPL", "failed to allocate internal tables");
        ce_repl_shutdown(ctx);
        return NULL;
    }

    CE_LOG_INFO("REPL", "initialized (max_dirty=%u, hash_cap=%u)",
                max_dirty, ctx->dirty_hash_capacity);

    /* 初始化 Mailbox 哈希表 (空槽位标记为 UINT64_MAX) */
    for (uint32_t i = 0; i < 4096; i++) {
        ctx->mailbox_keys[i] = UINT64_MAX;
    }

    return ctx;
}

void ce_repl_shutdown(CeReplContext* ctx) {
    if (!ctx) return;

    free(ctx->dirty_hash_keys);
    free(ctx->dirty_hash_values);
    free(ctx->owner_entity_ids);
    free(ctx->owner_client_ids);
    free(ctx);

    CE_LOG_INFO("REPL", "shutdown complete");
}

/* ---- 组件注册 ---- */

CeResult ce_repl_register_component(
    CeReplContext*      ctx,
    uint32_t            component_id,
    const char*         component_name,
    const CeReplField*  fields,
    const void*         default_template
) {
    if (!ctx || !fields || !component_name) return CE_ERR;

    if (ctx->component_count >= CE_REPL_MAX_COMPONENTS) {
        CE_LOG_ERROR("REPL", "max components reached (%d)", CE_REPL_MAX_COMPONENTS);
        return CE_ERR;
    }

    /* 检查重复注册 */
    for (uint32_t i = 0; i < ctx->component_count; i++) {
        if (ctx->components[i].component_id == component_id) {
            CE_LOG_ERROR("REPL", "component %u already registered", component_id);
            return CE_ERR;
        }
    }

    CeReplComponent* comp = &ctx->components[ctx->component_count];
    comp->component_id = component_id;
    strncpy(comp->name, component_name, CE_REPL_MAX_FIELD_NAME - 1);
    comp->name[CE_REPL_MAX_FIELD_NAME - 1] = '\0';
    comp->default_template = default_template;
    comp->field_count = 0;

    /* 复制字段描述符 */
    for (const CeReplField* f = fields; f->name != NULL; f++) {
        if (comp->field_count >= CE_REPL_MAX_FIELDS_PER_COMP) {
            CE_LOG_ERROR("REPL", "component '%s' exceeds max fields (%d)",
                         component_name, CE_REPL_MAX_FIELDS_PER_COMP);
            return CE_ERR;
        }
        memcpy(&comp->fields[comp->field_count], f, sizeof(CeReplField));
        comp->field_count++;
    }

    ctx->component_count++;

    CE_LOG_INFO("REPL", "registered component '%s' (id=%u, %u fields)",
                component_name, component_id, comp->field_count);

    return CE_OK;
}

/* ---- 启动时值域校验 ---- */

CeResult ce_repl_validate_initial_values(CeReplContext* ctx) {
    if (!ctx) return CE_ERR;

    for (uint32_t ci = 0; ci < ctx->component_count; ci++) {
        const CeReplComponent* comp = &ctx->components[ci];

        if (!comp->default_template) {
            CE_LOG_WARN("REPL", "component '%s' has no default_template, "
                        "skipping validation", comp->name);
            continue;
        }

        for (uint32_t fi = 0; fi < comp->field_count; fi++) {
            const CeReplField* field = &comp->fields[fi];
            const CeReplConstraint* c = &field->constraint;

            if (!c->has_min && !c->has_max) continue;

            /* 读取初始值 */
            int64_t value = ce_repl_read_field_value(field->type,
                (const uint8_t*)comp->default_template + field->offset);

            if (c->has_min && value < c->min_value) {
                CE_LOG_ERROR(
                    "Replication: VALIDATION FAILED — "
                    "component '%s' field '%s' initial value %ld < min %ld. "
                    "Server refusing to start.",
                    comp->name, field->name, (long)value, (long)c->min_value);
                return CE_ERR_VALIDATION;
            }
            if (c->has_max && value > c->max_value) {
                CE_LOG_ERROR(
                    "Replication: VALIDATION FAILED — "
                    "component '%s' field '%s' initial value %ld > max %ld. "
                    "Server refusing to start.",
                    comp->name, field->name, (long)value, (long)c->max_value);
                return CE_ERR_VALIDATION;
            }
        }
    }

    CE_LOG_INFO("REPL", "initial value validation passed (%u components)",
                ctx->component_count);
    return CE_OK;
}

/* ---- 脏标 ---- */

void ce_repl_mark_dirty(CeReplContext* ctx, uint64_t entity_id, uint32_t component_id) {
    if (!ctx) return;

    /* 查找组件 */
    int comp_idx = -1;
    for (uint32_t i = 0; i < ctx->component_count; i++) {
        if (ctx->components[i].component_id == component_id) {
            comp_idx = (int)i;
            break;
        }
    }
    if (comp_idx < 0) return;  /* 未注册的组件，忽略 */

    CeReplDirtyEntry* entry = ce_repl_find_or_create_dirty(ctx, entity_id);
    if (!entry) return;

    /* 标记该组件的所有字段为脏 */
    const CeReplComponent* comp = &ctx->components[comp_idx];
    for (uint32_t fi = 0; fi < comp->field_count; fi++) {
        uint32_t bit_idx = comp_idx * CE_REPL_MAX_FIELDS_PER_COMP + fi;
        uint32_t word = bit_idx / 64;
        uint32_t bit  = bit_idx % 64;
        if (word < 4) {
            entry->mask.bits[word] |= (1ULL << bit);
        }
    }
}

void ce_repl_mark_field_dirty(CeReplContext* ctx, uint64_t entity_id,
                              uint32_t component_id, uint32_t field_index) {
    if (!ctx) return;

    /* 查找组件 */
    int comp_idx = -1;
    for (uint32_t i = 0; i < ctx->component_count; i++) {
        if (ctx->components[i].component_id == component_id) {
            comp_idx = (int)i;
            break;
        }
    }
    if (comp_idx < 0) return;

    CeReplDirtyEntry* entry = ce_repl_find_or_create_dirty(ctx, entity_id);
    if (!entry) return;

    uint32_t bit_idx = (uint32_t)comp_idx * CE_REPL_MAX_FIELDS_PER_COMP + field_index;
    uint32_t word = bit_idx / 64;
    uint32_t bit  = bit_idx % 64;
    if (word < 4) {
        entry->mask.bits[word] |= (1ULL << bit);
    }
}

/* ---- 帧更新 ---- */

void ce_repl_tick(CeReplContext* ctx, float dt) {
    if (!ctx) return;
    (void)dt;  /* 预留: AOI 事件处理、超时检测等 */
}

void ce_repl_flush(CeReplContext* ctx) {
    if (!ctx) return;

    uint64_t t_start = ce_time_now_us();
    uint32_t synced_entities = 0;
    uint32_t synced_fields = 0;

    /* 遍历所有脏实体 */
    for (uint32_t di = 0; di < ctx->dirty_count; di++) {
        CeReplDirtyEntry* entry = &ctx->dirty_entities[di];
        bool entity_has_sync = false;

        /* 遍历所有已注册组件 */
        for (uint32_t ci = 0; ci < ctx->component_count; ci++) {
            const CeReplComponent* comp = &ctx->components[ci];

            /* 遍历该组件的所有字段 */
            for (uint32_t fi = 0; fi < comp->field_count; fi++) {
                uint32_t bit_idx = ci * CE_REPL_MAX_FIELDS_PER_COMP + fi;
                uint32_t word = bit_idx / 64;
                uint32_t bit  = bit_idx % 64;

                if (word >= 4) continue;
                if (!(entry->mask.bits[word] & (1ULL << bit))) continue;

                /* 该字段脏了 */
                const CeReplField* field = &comp->fields[fi];
                uint32_t flags = field->flags;

                /* SERVER_ONLY 字段跳过 */
                if (flags & CE_FLAG_SERVER_ONLY) continue;

                /* 按 flag 分流:
                 * - AOI_BROADCAST → 发送到 AOI 内所有客户端
                 * - OWNER_ONLY    → 发送到属主客户端
                 * - PERSIST       → 标记需要存档 (在 flush_to_dbproxy 中处理)
                 *
                 * 当前 MVP: 仅记录统计，实际发送在后续 Phase 实现
                 */
                synced_fields++;
                entity_has_sync = true;
            }
        }

        if (entity_has_sync) {
            synced_entities++;
        }
    }

    /* 清除所有脏标 */
    memset(ctx->dirty_hash_keys, 0, sizeof(uint64_t) * ctx->dirty_hash_capacity);
    memset(ctx->dirty_hash_values, 0, sizeof(uint32_t) * ctx->dirty_hash_capacity);
    ctx->dirty_count = 0;

    /* 更新统计 */
    uint64_t t_end = ce_time_now_us();
    ctx->stats.total_flushes++;
    ctx->stats.total_entities_synced += synced_entities;
    ctx->stats.total_fields_synced += synced_fields;
    ctx->stats.current_dirty_entities = 0;
    ctx->stats.current_dirty_fields = 0;
    ctx->stats.last_flush_time_us = t_end - t_start;
    ctx->frame_id++;
}

/* ---- 连接设置 ---- */

void ce_repl_set_gateway(CeReplContext* ctx, CeGatewayConn* gateway) {
    if (!ctx) return;
    ctx->gateway = gateway;
}

void ce_repl_set_dbproxy(CeReplContext* ctx, CeSyncContext* sync) {
    if (!ctx) return;
    ctx->sync = sync;
}

void ce_repl_set_aoi(CeReplContext* ctx, CeAoiContext* aoi) {
    if (!ctx) return;
    ctx->aoi = aoi;
}

void ce_repl_set_owner(CeReplContext* ctx, uint64_t entity_id, uint64_t client_id) {
    if (!ctx) return;

    /* 查找已有映射 */
    for (uint32_t i = 0; i < ctx->owner_count; i++) {
        if (ctx->owner_entity_ids[i] == entity_id) {
            if (client_id == 0) {
                /* 删除映射: 与最后一个交换后缩减 */
                ctx->owner_entity_ids[i] = ctx->owner_entity_ids[ctx->owner_count - 1];
                ctx->owner_client_ids[i] = ctx->owner_client_ids[ctx->owner_count - 1];
                ctx->owner_count--;
            } else {
                ctx->owner_client_ids[i] = client_id;
            }
            return;
        }
    }

    /* 新映射 */
    if (client_id != 0 && ctx->owner_count < CE_REPL_MAX_DIRTY_ENTITIES) {
        ctx->owner_entity_ids[ctx->owner_count] = entity_id;
        ctx->owner_client_ids[ctx->owner_count] = client_id;
        ctx->owner_count++;
    }
}

/* ---- 统计查询 ---- */

void ce_repl_get_stats(const CeReplContext* ctx, CeReplStats* stats) {
    if (!ctx || !stats) return;
    memcpy(stats, &ctx->stats, sizeof(CeReplStats));
    stats->current_dirty_entities = ctx->dirty_count;
}
