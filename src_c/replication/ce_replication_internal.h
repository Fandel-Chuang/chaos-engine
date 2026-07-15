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
#define CE_REPL_MAX_RPC_HANDLERS        128
#define CE_REPL_MAX_FIELD_NAME          64
#define CE_RPC_MAX_METHOD_NAME          64
#define CE_RPC_MAX_PAYLOAD              4096

/* ---- 动态扩容初始容量 ---- */

#define CE_REPL_INITIAL_DIRTY_CAPACITY      4096    /* 脏标表初始容量 */
#define CE_REPL_INITIAL_MAILBOX_CAPACITY    4096    /* Mailbox 哈希表初始容量 */
#define CE_REPL_INITIAL_OWNER_CAPACITY      256     /* 属主映射哈希表初始容量 */
#define CE_REPL_INITIAL_RPC_PENDING         256     /* RPC pending 队列初始容量 */

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

/* ---- RPC pending 队列条目 ---- */

typedef struct CeRpcPendingEntry {
    uint32_t            call_id;
    uint64_t            entity_id;
    CeRpcTarget         target;
    CeRpcReliability    reliability;
    uint8_t*            payload;        /* 动态分配的 payload 缓冲区 */
    uint32_t            payload_len;
    float               timeout;        /* remaining time before retry */
} CeRpcPendingEntry;

/* ---- 复制管理器上下文 ---- */

struct CeReplContext {
    /* 组件注册表 */
    uint32_t        component_count;
    CeReplComponent components[CE_REPL_MAX_COMPONENTS];

    /* 脏标表 (动态扩容) */
    uint32_t        dirty_count;
    uint32_t        dirty_capacity;
    CeReplDirtyEntry* dirty_entities;   /* 动态分配的脏实体数组 */
    /* entity_id -> dirty index 哈希表 (简单线性探测, 动态扩容) */
    uint32_t        dirty_hash_capacity;
    uint64_t*       dirty_hash_keys;    /* entity_id, 0 = 空槽 */
    uint32_t*       dirty_hash_values;  /* index into dirty_entities */

    /* 属主映射 (entity_id -> client_id) 哈希表 O(1) 查找, 动态扩容 */
    uint32_t        owner_count;
    uint32_t        owner_capacity;
    uint32_t        owner_hash_capacity;    /* 哈希表槽位数 (>= owner_capacity * 2) */
    uint64_t*       owner_hash_keys;        /* entity_id, 0 = 空槽 */
    uint64_t*       owner_hash_values;      /* client_id */

    /* RPC handler 表 */
    uint32_t        rpc_handler_count;
    CeRpcHandlerEntry rpc_handlers[CE_REPL_MAX_RPC_HANDLERS];

    /* 外部连接 */
    CeGatewayConn*  gateway;
    CeSyncContext*  sync;
    CeAoiContext*   aoi;

    /* DBProxy 发送回调 (避免 engine_core -> engine_sync 循环依赖) */
    CeResult (*sync_send_fn)(CeSyncContext* ctx, const struct CeSyncFrame* frame);

    /* 统计 */
    CeReplStats     stats;

    /* 帧计数 */
    uint64_t        frame_id;

    /* Mailbox: entity_id -> server_id 哈希表 (线性探测, 动态扩容) */
    uint32_t        mailbox_count;
    uint32_t        mailbox_capacity;
    uint64_t*       mailbox_keys;      /* entity_id, UINT64_MAX = 空槽 */
    uint32_t*       mailbox_values;    /* server_id */

    /* RPC pending queue (动态扩容, for reliable RPC ack/timeout) */
    uint32_t        rpc_pending_count;
    uint32_t        rpc_pending_capacity;
    uint32_t        rpc_call_id_counter;
    CeRpcPendingEntry* rpc_pending;    /* 动态分配的 pending 数组 */
};

/* ---- 内部函数 ---- */

/** 从字段类型读取值 (用于校验) */
int64_t ce_repl_read_field_value(CeReplFieldType type, const uint8_t* src);

/** 在脏标表中查找或创建条目 (动态扩容) */
CeReplDirtyEntry* ce_repl_find_or_create_dirty(CeReplContext* ctx, uint64_t entity_id);

/** 查找属主客户端 ID (哈希表 O(1)) */
uint64_t ce_repl_find_owner(CeReplContext* ctx, uint64_t entity_id);

/* ---- 内部扩容函数 ---- */

/** 确保脏标表有足够容量, 不够则 realloc 翻倍 */
bool ce_repl_ensure_dirty_capacity(CeReplContext* ctx, uint32_t needed);

/** 确保 Mailbox 哈希表有足够容量, 不够则 realloc 翻倍 */
bool ce_repl_ensure_mailbox_capacity(CeReplContext* ctx, uint32_t needed);

/** 确保属主映射哈希表有足够容量, 不够则 realloc 翻倍 */
bool ce_repl_ensure_owner_capacity(CeReplContext* ctx, uint32_t needed);

/** 确保 RPC pending 队列有足够容量, 不够则 realloc 翻倍 */
bool ce_repl_ensure_rpc_pending_capacity(CeReplContext* ctx, uint32_t needed);

#endif /* CE_REPLICATION_INTERNAL_H */
