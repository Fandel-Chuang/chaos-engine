/*
 * ChaosEngine Replication Manager — 内部头文件
 *
 * 不对外暴露的数据结构定义。
 */

#ifndef CE_REPLICATION_INTERNAL_H
#define CE_REPLICATION_INTERNAL_H

#include "replication/ce_replication.h"
#include "replication/ce_rpc_channel.h"
#include <stdint.h>
#include <stdbool.h>

#define CE_REPL_LOG(level, fmt, ...) \
    ce_log_write(level, "REPL", __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define CE_REPL_MAX_COMPONENTS          64
#define CE_REPL_MAX_FIELDS_PER_COMP     32
#define CE_REPL_MAX_DIRTY_ENTITIES      4096
#define CE_REPL_MAX_RPC_HANDLERS        128
#define CE_REPL_MAX_FIELD_NAME          64
#define CE_RPC_MAX_PENDING              256
#define CE_RPC_MAX_METHOD_NAME          64
#define CE_RPC_MAX_PAYLOAD              4096

/* ---- 已注册组件 ---- */

typedef struct CeReplComponent {
    uint32_t        component_id;
    char            name[CE_REPL_MAX_FIELD_NAME];
    uint32_t        field_count;
    CeReplField     fields[CE_REPL_MAX_FIELDS_PER_COMP];
    const void*     default_template;   /* 默认值模板指针 */
    uint32_t        template_size;      /* 模板大小 */
} CeReplComponent;

/* ---- 脏标位图 ---- */

typedef struct CeReplDirtyMask {
    uint64_t    bits[4];    /* 最多 256 个字段 (4 × 64) */
} CeReplDirtyMask;

/* ---- 脏实体条目 ---- */

typedef struct CeReplDirtyEntry {
    uint64_t            entity_id;
    CeReplDirtyMask     mask;           /* 哪些字段脏了 (按 component 分组) */
    uint64_t            owner_client_id; /* 属主客户端 ID (0 = 无属主) */
} CeReplDirtyEntry;

/* ---- RPC Handler ---- */

typedef void (*CeRpcHandlerFn)(uint64_t source_entity,
                               const uint8_t* params, uint32_t params_len,
                               void* user_data);

typedef struct CeRpcHandlerEntry {
    char            method[CE_REPL_MAX_FIELD_NAME];
    CeRpcHandlerFn  handler;
    void*           user_data;
} CeRpcHandlerEntry;

/* ---- 复制管理器上下文 ---- */

struct CeReplContext {
    /* 组件注册表 */
    uint32_t        component_count;
    CeReplComponent components[CE_REPL_MAX_COMPONENTS];

    /* 脏标表 */
    uint32_t        dirty_count;
    CeReplDirtyEntry dirty_entities[CE_REPL_MAX_DIRTY_ENTITIES];
    /* entity_id → dirty index 哈希表 (简单线性探测) */
    uint32_t        dirty_hash_capacity;
    uint64_t*       dirty_hash_keys;    /* entity_id */
    uint32_t*       dirty_hash_values;  /* index into dirty_entities */

    /* 属主映射 (entity_id → client_id) */
    uint32_t        owner_count;
    uint64_t*       owner_entity_ids;
    uint64_t*       owner_client_ids;

    /* RPC handler 表 */
    uint32_t        rpc_handler_count;
    CeRpcHandlerEntry rpc_handlers[CE_REPL_MAX_RPC_HANDLERS];

    /* 外部连接 */
    CeGatewayConn*  gateway;
    CeSyncContext*  sync;
    CeAoiContext*   aoi;

    /* 统计 */
    CeReplStats     stats;

    /* 帧计数 */
    uint64_t        frame_id;

    /* Mailbox: entity_id → server_id 哈希表 (线性探测, 容量 4096) */
    uint32_t        mailbox_count;
    uint64_t        mailbox_keys[4096];
    uint32_t        mailbox_values[4096];

    /* RPC pending queue (for reliable RPC ack/timeout) */
    uint32_t        rpc_pending_count;
    uint32_t        rpc_call_id_counter;
    /* Simple ring buffer of pending RPCs */
    struct {
        uint32_t    call_id;
        uint64_t    entity_id;
        CeRpcTarget target;
        CeRpcReliability reliability;
        uint8_t     payload[CE_RPC_MAX_PAYLOAD];
        uint32_t    payload_len;
        float       timeout;    /* remaining time before retry */
    } rpc_pending[CE_RPC_MAX_PENDING];
};

/* ---- 内部函数 ---- */

/** 从字段类型读取值 (用于校验) */
int64_t ce_repl_read_field_value(CeReplFieldType type, const uint8_t* src);

/** 在脏标表中查找或创建条目 */
CeReplDirtyEntry* ce_repl_find_or_create_dirty(CeReplContext* ctx, uint64_t entity_id);

/** 查找属主客户端 ID */
uint64_t ce_repl_find_owner(CeReplContext* ctx, uint64_t entity_id);

#endif /* CE_REPLICATION_INTERNAL_H */
