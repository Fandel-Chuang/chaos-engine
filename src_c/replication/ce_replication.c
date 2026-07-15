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

/* ---- 内部扩容函数 ---- */

/** 确保脏标表有足够容量, 不够则 realloc 翻倍 */
bool ce_repl_ensure_dirty_capacity(CeReplContext* ctx, uint32_t needed) {
    if (needed <= ctx->dirty_capacity) return true;

    /* 翻倍扩容直到满足需求 */
    uint32_t new_cap = ctx->dirty_capacity;
    while (new_cap < needed) {
        new_cap = (new_cap == 0) ? CE_REPL_INITIAL_DIRTY_CAPACITY : new_cap * 2;
    }

    CeReplDirtyEntry* new_entities = (CeReplDirtyEntry*)realloc(
        ctx->dirty_entities, new_cap * sizeof(CeReplDirtyEntry));
    if (!new_entities) {
        CE_LOG_ERROR("REPL", "dirty_entities realloc failed (%u -> %u)",
                     ctx->dirty_capacity, new_cap);
        return false;
    }
    /* 清零新分配的部分 */
    memset(new_entities + ctx->dirty_capacity, 0,
           (new_cap - ctx->dirty_capacity) * sizeof(CeReplDirtyEntry));
    ctx->dirty_entities = new_entities;

    /* 哈希表也需要扩容 (保持 2x 脏实体容量) */
    uint32_t new_hash_cap = new_cap * 2;
    uint64_t* new_hash_keys = (uint64_t*)calloc(new_hash_cap, sizeof(uint64_t));
    uint32_t* new_hash_values = (uint32_t*)calloc(new_hash_cap, sizeof(uint32_t));
    if (!new_hash_keys || !new_hash_values) {
        free(new_hash_keys);
        free(new_hash_values);
        CE_LOG_ERROR("REPL", "dirty hash table realloc failed (cap=%u)", new_hash_cap);
        return false;
    }

    /* 重新插入所有已有条目 (rehash) */
    for (uint32_t i = 0; i < ctx->dirty_count; i++) {
        uint64_t eid = ctx->dirty_entities[i].entity_id;
        uint32_t idx = hash_entity_id(eid, new_hash_cap);
        for (uint32_t j = 0; j < new_hash_cap; j++) {
            uint32_t probe = (idx + j) % new_hash_cap;
            if (new_hash_keys[probe] == 0) {
                new_hash_keys[probe] = eid;
                new_hash_values[probe] = i;
                break;
            }
        }
    }

    free(ctx->dirty_hash_keys);
    free(ctx->dirty_hash_values);
    ctx->dirty_hash_keys = new_hash_keys;
    ctx->dirty_hash_values = new_hash_values;
    ctx->dirty_hash_capacity = new_hash_cap;
    ctx->dirty_capacity = new_cap;

    CE_LOG_INFO("REPL", "dirty table expanded: entities=%u hash=%u",
                ctx->dirty_capacity, ctx->dirty_hash_capacity);
    return true;
}

/** 确保 Mailbox 哈希表有足够容量, 不够则 realloc 翻倍 */
bool ce_repl_ensure_mailbox_capacity(CeReplContext* ctx, uint32_t needed) {
    /* load factor: 保持 hash_capacity >= count * 2 */
    uint32_t needed_hash = needed * 2;
    if (needed_hash <= ctx->mailbox_capacity) return true;

    uint32_t new_cap = ctx->mailbox_capacity;
    while (new_cap < needed_hash) {
        new_cap = (new_cap == 0) ? CE_REPL_INITIAL_MAILBOX_CAPACITY : new_cap * 2;
    }

    uint64_t* new_keys = (uint64_t*)malloc(new_cap * sizeof(uint64_t));
    uint32_t* new_values = (uint32_t*)malloc(new_cap * sizeof(uint32_t));
    if (!new_keys || !new_values) {
        free(new_keys);
        free(new_values);
        CE_LOG_ERROR("REPL", "mailbox realloc failed (cap=%u)", new_cap);
        return false;
    }

    /* 初始化空槽标记 */
    for (uint32_t i = 0; i < new_cap; i++) {
        new_keys[i] = UINT64_MAX;
    }

    /* 重新插入所有已有条目 (rehash) */
    uint32_t reinserted = 0;
    for (uint32_t i = 0; i < ctx->mailbox_capacity && reinserted < ctx->mailbox_count; i++) {
        if (ctx->mailbox_keys[i] != UINT64_MAX) {
            uint64_t key = ctx->mailbox_keys[i];
            uint32_t val = ctx->mailbox_values[i];
            uint32_t idx = hash_entity_id(key, new_cap);
            for (uint32_t j = 0; j < new_cap; j++) {
                uint32_t probe = (idx + j) % new_cap;
                if (new_keys[probe] == UINT64_MAX) {
                    new_keys[probe] = key;
                    new_values[probe] = val;
                    reinserted++;
                    break;
                }
            }
        }
    }

    free(ctx->mailbox_keys);
    free(ctx->mailbox_values);
    ctx->mailbox_keys = new_keys;
    ctx->mailbox_values = new_values;
    ctx->mailbox_capacity = new_cap;

    CE_LOG_INFO("REPL", "mailbox hash table expanded: capacity=%u", ctx->mailbox_capacity);
    return true;
}

/** 确保属主映射哈希表有足够容量, 不够则 realloc 翻倍 */
bool ce_repl_ensure_owner_capacity(CeReplContext* ctx, uint32_t needed) {
    uint32_t needed_hash = needed * 2;
    if (needed_hash <= ctx->owner_hash_capacity) return true;

    uint32_t new_hash_cap = ctx->owner_hash_capacity;
    while (new_hash_cap < needed_hash) {
        new_hash_cap = (new_hash_cap == 0) ? CE_REPL_INITIAL_OWNER_CAPACITY * 2 : new_hash_cap * 2;
    }
    uint32_t new_cap = new_hash_cap / 2;

    uint64_t* new_hash_keys = (uint64_t*)calloc(new_hash_cap, sizeof(uint64_t));
    uint64_t* new_hash_values = (uint64_t*)calloc(new_hash_cap, sizeof(uint64_t));
    if (!new_hash_keys || !new_hash_values) {
        free(new_hash_keys);
        free(new_hash_values);
        CE_LOG_ERROR("REPL", "owner hash table realloc failed (cap=%u)", new_hash_cap);
        return false;
    }

    /* 重新插入所有已有条目 (rehash) */
    for (uint32_t i = 0; i < ctx->owner_hash_capacity; i++) {
        if (ctx->owner_hash_keys[i] != 0) {
            uint64_t key = ctx->owner_hash_keys[i];
            uint64_t val = ctx->owner_hash_values[i];
            uint32_t idx = hash_entity_id(key, new_hash_cap);
            for (uint32_t j = 0; j < new_hash_cap; j++) {
                uint32_t probe = (idx + j) % new_hash_cap;
                if (new_hash_keys[probe] == 0) {
                    new_hash_keys[probe] = key;
                    new_hash_values[probe] = val;
                    break;
                }
            }
        }
    }

    free(ctx->owner_hash_keys);
    free(ctx->owner_hash_values);
    ctx->owner_hash_keys = new_hash_keys;
    ctx->owner_hash_values = new_hash_values;
    ctx->owner_hash_capacity = new_hash_cap;
    ctx->owner_capacity = new_cap;

    CE_LOG_INFO("REPL", "owner hash table expanded: capacity=%u hash=%u",
                ctx->owner_capacity, ctx->owner_hash_capacity);
    return true;
}

/** 确保 RPC pending 队列有足够容量, 不够则 realloc 翻倍 */
bool ce_repl_ensure_rpc_pending_capacity(CeReplContext* ctx, uint32_t needed) {
    if (needed <= ctx->rpc_pending_capacity) return true;

    uint32_t new_cap = ctx->rpc_pending_capacity;
    while (new_cap < needed) {
        new_cap = (new_cap == 0) ? CE_REPL_INITIAL_RPC_PENDING : new_cap * 2;
    }

    CeRpcPendingEntry* new_pending = (CeRpcPendingEntry*)realloc(
        ctx->rpc_pending, new_cap * sizeof(CeRpcPendingEntry));
    if (!new_pending) {
        CE_LOG_ERROR("RPC", "rpc_pending realloc failed (%u -> %u)",
                     ctx->rpc_pending_capacity, new_cap);
        return false;
    }
    /* 清零新分配部分 */
    memset(new_pending + ctx->rpc_pending_capacity, 0,
           (new_cap - ctx->rpc_pending_capacity) * sizeof(CeRpcPendingEntry));
    ctx->rpc_pending = new_pending;
    ctx->rpc_pending_capacity = new_cap;

    CE_LOG_INFO("RPC", "rpc_pending queue expanded: capacity=%u", ctx->rpc_pending_capacity);
    return true;
}

/* ---- 辅助: 在脏标表中查找或创建条目 ---- */

CeReplDirtyEntry* ce_repl_find_or_create_dirty(CeReplContext* ctx, uint64_t entity_id) {
    if (ctx->dirty_hash_capacity == 0) return NULL;

    uint32_t idx = hash_entity_id(entity_id, ctx->dirty_hash_capacity);

    /* 线性探测查找 */
    for (uint32_t i = 0; i < ctx->dirty_hash_capacity; i++) {
        uint32_t probe = (idx + i) % ctx->dirty_hash_capacity;
        if (ctx->dirty_hash_keys[probe] == entity_id) {
            /* 找到已有条目 */
            return &ctx->dirty_entities[ctx->dirty_hash_values[probe]];
        }
        if (ctx->dirty_hash_keys[probe] == 0) {
            /* 空槽位: 需要创建新条目. 先检查容量, 不够则扩容后重试 */
            if (ctx->dirty_count >= ctx->dirty_capacity) {
                if (!ce_repl_ensure_dirty_capacity(ctx, ctx->dirty_count + 1)) {
                    CE_LOG_ERROR("REPL", "failed to expand dirty table (count=%u)",
                                 ctx->dirty_count);
                    return NULL;
                }
                /* 扩容后哈希表已 rehash, 重新探测 */
                return ce_repl_find_or_create_dirty(ctx, entity_id);
            }

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

    /* 哈希表满 (load factor 过高), 扩容后重试 */
    if (!ce_repl_ensure_dirty_capacity(ctx, ctx->dirty_count + 1)) {
        CE_LOG_ERROR("REPL", "dirty hash table full and expand failed (count=%u)",
                     ctx->dirty_count);
        return NULL;
    }
    return ce_repl_find_or_create_dirty(ctx, entity_id);
}

/* ---- 辅助: 查找属主 (哈希表 O(1)) ---- */

uint64_t ce_repl_find_owner(CeReplContext* ctx, uint64_t entity_id) {
    if (ctx->owner_hash_capacity == 0) return 0;

    uint32_t idx = hash_entity_id(entity_id, ctx->owner_hash_capacity);

    for (uint32_t i = 0; i < ctx->owner_hash_capacity; i++) {
        uint32_t probe = (idx + i) % ctx->owner_hash_capacity;
        if (ctx->owner_hash_keys[probe] == entity_id) {
            return ctx->owner_hash_values[probe];
        }
        if (ctx->owner_hash_keys[probe] == 0) {
            /* 空槽 = 不存在 */
            return 0;
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

    /* 初始容量 (后续可动态扩容) */
    uint32_t init_dirty = CE_REPL_INITIAL_DIRTY_CAPACITY;
    uint32_t init_owner = CE_REPL_INITIAL_OWNER_CAPACITY;
    uint32_t init_mailbox = CE_REPL_INITIAL_MAILBOX_CAPACITY;
    uint32_t init_rpc_pending = CE_REPL_INITIAL_RPC_PENDING;

    (void)config;  /* 配置中的 max_dirty 现在只是建议值, 初始用默认值, 动态扩容 */

    /* 初始化脏标表 (动态分配) */
    ctx->dirty_capacity = init_dirty;
    ctx->dirty_count = 0;
    ctx->dirty_entities = (CeReplDirtyEntry*)calloc(init_dirty, sizeof(CeReplDirtyEntry));
    ctx->dirty_hash_capacity = init_dirty * 2;
    ctx->dirty_hash_keys = (uint64_t*)calloc(ctx->dirty_hash_capacity, sizeof(uint64_t));
    ctx->dirty_hash_values = (uint32_t*)calloc(ctx->dirty_hash_capacity, sizeof(uint32_t));

    /* 初始化属主映射哈希表 (动态分配, O(1) 查找) */
    ctx->owner_count = 0;
    ctx->owner_capacity = init_owner;
    ctx->owner_hash_capacity = init_owner * 2;
    ctx->owner_hash_keys = (uint64_t*)calloc(ctx->owner_hash_capacity, sizeof(uint64_t));
    ctx->owner_hash_values = (uint64_t*)calloc(ctx->owner_hash_capacity, sizeof(uint64_t));

    /* 初始化 Mailbox 哈希表 (动态分配) */
    ctx->mailbox_count = 0;
    ctx->mailbox_capacity = init_mailbox;
    ctx->mailbox_keys = (uint64_t*)malloc(init_mailbox * sizeof(uint64_t));
    ctx->mailbox_values = (uint32_t*)malloc(init_mailbox * sizeof(uint32_t));

    /* 初始化 RPC pending 队列 (动态分配) */
    ctx->rpc_pending_count = 0;
    ctx->rpc_pending_capacity = init_rpc_pending;
    ctx->rpc_call_id_counter = 0;
    ctx->rpc_pending = (CeRpcPendingEntry*)calloc(init_rpc_pending, sizeof(CeRpcPendingEntry));

    /* 检查所有分配 */
    if (!ctx->dirty_entities || !ctx->dirty_hash_keys || !ctx->dirty_hash_values ||
        !ctx->owner_hash_keys || !ctx->owner_hash_values ||
        !ctx->mailbox_keys || !ctx->mailbox_values ||
        !ctx->rpc_pending) {
        CE_LOG_ERROR("REPL", "failed to allocate internal tables");
        ce_repl_shutdown(ctx);
        return NULL;
    }

    /* 初始化 Mailbox 空槽标记为 UINT64_MAX */
    for (uint32_t i = 0; i < init_mailbox; i++) {
        ctx->mailbox_keys[i] = UINT64_MAX;
    }

    CE_LOG_INFO("REPL", "initialized (dirty_cap=%u, owner_cap=%u, mailbox_cap=%u, rpc_pending_cap=%u)",
                ctx->dirty_capacity, ctx->owner_capacity,
                ctx->mailbox_capacity, ctx->rpc_pending_capacity);

    return ctx;
}

void ce_repl_shutdown(CeReplContext* ctx) {
    if (!ctx) return;

    /* 释放 RPC pending 队列中的 payload 缓冲区 */
    if (ctx->rpc_pending) {
        for (uint32_t i = 0; i < ctx->rpc_pending_count; i++) {
            free(ctx->rpc_pending[i].payload);
        }
        free(ctx->rpc_pending);
    }

    free(ctx->dirty_entities);
    free(ctx->dirty_hash_keys);
    free(ctx->dirty_hash_values);
    free(ctx->owner_hash_keys);
    free(ctx->owner_hash_values);
    free(ctx->mailbox_keys);
    free(ctx->mailbox_values);
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

    CeReplDirtyEntry* entry = ce_repl_find_or_create_dirty(ctx, entity_id);
    if (!entry) return;

    if (component_id == CE_REPL_ALL_COMPONENTS) {
        /* 标记所有已注册组件的所有字段为脏 */
        for (uint32_t ci = 0; ci < ctx->component_count; ci++) {
            const CeReplComponent* comp = &ctx->components[ci];
            for (uint32_t fi = 0; fi < comp->field_count; fi++) {
                uint32_t bit_idx = ci * CE_REPL_MAX_FIELDS_PER_COMP + fi;
                uint32_t word = bit_idx / 64;
                uint32_t bit  = bit_idx % 64;
                if (word < 4) {
                    entry->mask.bits[word] |= (1ULL << bit);
                }
            }
        }
        return;
    }

    /* 查找组件 */
    int comp_idx = -1;
    for (uint32_t i = 0; i < ctx->component_count; i++) {
        if (ctx->components[i].component_id == component_id) {
            comp_idx = (int)i;
            break;
        }
    }
    if (comp_idx < 0) return;  /* 未注册的组件，忽略 */

    /* 标记该组件的所有字段为脏 */
    const CeReplComponent* comp = &ctx->components[comp_idx];
    for (uint32_t fi = 0; fi < comp->field_count; fi++) {
        uint32_t bit_idx = (uint32_t)comp_idx * CE_REPL_MAX_FIELDS_PER_COMP + fi;
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

/* ---- DBProxy PERSIST 字段过滤与发送 ---- */

/**
 * 收集所有标记了 CE_FLAG_PERSIST 的脏字段，
 * 通过 sync_send_fn 回调发送到 DBProxy。
 *
 * 仅在 ctx->sync 非 NULL 且 sync_send_fn 已设置时执行。
 * 当前 MVP: 统计并记录 PERSIST 字段，实际帧构建在后续 Phase 实现。
 */
static void ce_repl_flush_to_dbproxy(CeReplContext* ctx) {
    if (!ctx->sync || !ctx->sync_send_fn) return;

    /* 统计 PERSIST 字段数量 */
    uint32_t persist_count = 0;

    for (uint32_t di = 0; di < ctx->dirty_count; di++) {
        CeReplDirtyEntry* entry = &ctx->dirty_entities[di];

        for (uint32_t ci = 0; ci < ctx->component_count; ci++) {
            const CeReplComponent* comp = &ctx->components[ci];

            for (uint32_t fi = 0; fi < comp->field_count; fi++) {
                uint32_t bit_idx = ci * CE_REPL_MAX_FIELDS_PER_COMP + fi;
                uint32_t word = bit_idx / 64;
                uint32_t bit  = bit_idx % 64;

                if (word >= 4) continue;
                if (!(entry->mask.bits[word] & (1ULL << bit))) continue;

                const CeReplField* field = &comp->fields[fi];
                if (field->flags & CE_FLAG_PERSIST) {
                    persist_count++;
                }
            }
        }
    }

    if (persist_count == 0) return;

    /* MVP: 记录统计，实际帧构建和发送在后续 Phase 实现 */
    CE_LOG_INFO("REPL", "DBProxy flush: %u persist fields across %u entities (MVP: frame send deferred)",
                persist_count, ctx->dirty_count);
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

    /* ---- DBProxy PERSIST 字段发送 ---- */
    ce_repl_flush_to_dbproxy(ctx);

    /* 清除所有脏标 */
    memset(ctx->dirty_hash_keys, 0, sizeof(uint64_t) * ctx->dirty_hash_capacity);
    memset(ctx->dirty_hash_values, 0, sizeof(uint32_t) * ctx->dirty_hash_capacity);
    ctx->dirty_count = 0;
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

/**
 * 设置 DBProxy 同步发送回调函数
 *
 * 用于打破 engine_core → engine_sync 的循环依赖。
 * 调用者 (如 engine_sync 初始化代码) 负责在设置 sync 后注册此回调。
 *
 * @param ctx  复制管理器上下文
 * @param fn   发送回调 (签名: CeResult fn(CeSyncContext*, const CeSyncFrame*))
 */
void ce_repl_set_sync_send_fn(CeReplContext* ctx,
                               CeResult (*fn)(CeSyncContext*, const struct CeSyncFrame*)) {
    if (!ctx) return;
    ctx->sync_send_fn = fn;
}

void ce_repl_set_aoi(CeReplContext* ctx, CeAoiContext* aoi) {
    if (!ctx) return;
    ctx->aoi = aoi;
}

void ce_repl_set_owner(CeReplContext* ctx, uint64_t entity_id, uint64_t client_id) {
    if (!ctx) return;

    /* 哈希表查找已有映射 */
    uint32_t idx = hash_entity_id(entity_id, ctx->owner_hash_capacity);
    for (uint32_t i = 0; i < ctx->owner_hash_capacity; i++) {
        uint32_t probe = (idx + i) % ctx->owner_hash_capacity;
        if (ctx->owner_hash_keys[probe] == entity_id) {
            /* 找到已有映射 */
            if (client_id == 0) {
                /* 删除映射: 用 0 标记为空槽 (不缩容, 下次 rehash 时自然回收) */
                ctx->owner_hash_keys[probe] = 0;
                ctx->owner_hash_values[probe] = 0;
                ctx->owner_count--;
            } else {
                ctx->owner_hash_values[probe] = client_id;
            }
            return;
        }
        if (ctx->owner_hash_keys[probe] == 0) {
            /* 空槽 = 不存在该映射 */
            if (client_id != 0) {
                /* 新映射: 检查容量, 不够则扩容 */
                if (ctx->owner_count >= ctx->owner_capacity) {
                    if (!ce_repl_ensure_owner_capacity(ctx, ctx->owner_count + 1)) {
                        CE_LOG_ERROR("REPL", "failed to expand owner table (count=%u)",
                                     ctx->owner_count);
                        return;
                    }
                    /* 扩容后重新插入 */
                    ce_repl_set_owner(ctx, entity_id, client_id);
                    return;
                }
                ctx->owner_hash_keys[probe] = entity_id;
                ctx->owner_hash_values[probe] = client_id;
                ctx->owner_count++;
            }
            return;
        }
    }

    /* 哈希表满, 扩容后重试 */
    if (client_id != 0) {
        if (!ce_repl_ensure_owner_capacity(ctx, ctx->owner_count + 1)) {
            CE_LOG_ERROR("REPL", "owner hash table full and expand failed (count=%u)",
                         ctx->owner_count);
            return;
        }
        ce_repl_set_owner(ctx, entity_id, client_id);
    }
}

/* ---- 统计查询 ---- */

void ce_repl_get_stats(const CeReplContext* ctx, CeReplStats* stats) {
    if (!ctx || !stats) return;
    memcpy(stats, &ctx->stats, sizeof(CeReplStats));
    stats->current_dirty_entities = ctx->dirty_count;
}
