/*
 * ChaosEngine Replication Manager — 公共头文件
 *
 * 统一属性复制管线：
 *   - 字段级同步策略 (AOI_BROADCAST / OWNER_ONLY / SERVER_ONLY / PERSIST)
 *   - 脏标收集 + 帧末批量 flush
 *   - FlatBuffers 序列化
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 */

#ifndef CE_REPLICATION_H
#define CE_REPLICATION_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

/** 不透明复制管理器上下文 */
typedef struct CeReplContext CeReplContext;

/** Gateway 连接句柄 (不透明) */
typedef struct CeGatewayConn CeGatewayConn;

/** DBProxy 同步上下文 (来自 ce_sync.h) */
typedef struct CeSyncContext CeSyncContext;

/** 同步帧 (来自 ce_sync.h, 前向声明以避免循环依赖) */
struct CeSyncFrame;

/** AOI 上下文 (来自 ce_aoi.h) */
typedef struct CeAoiContext CeAoiContext;

/* ---- 复制 Flag ---- */

/** 属性复制策略标志 (位掩码，可组合) */
typedef enum CeReplFlag {
    CE_FLAG_AOI_BROADCAST  = 0x01,   /* 同步给 AOI 范围内所有客户端 */
    CE_FLAG_OWNER_ONLY     = 0x02,   /* 只同步给属主客户端 */
    CE_FLAG_SERVER_ONLY    = 0x04,   /* 服务器专属，不同步客户端 */
    CE_FLAG_PERSIST        = 0x08,   /* 标记脏时同步到 DBProxy 存档 */
} CeReplFlag;

/** 特殊组件 ID: 标记实体的所有已注册组件为脏 */
#define CE_REPL_ALL_COMPONENTS  ((uint32_t)-1)

/* ---- 字段类型 ---- */

/** 可复制字段的数据类型 */
typedef enum CeReplFieldType {
    CE_REPL_TYPE_I8,
    CE_REPL_TYPE_I16,
    CE_REPL_TYPE_I32,
    CE_REPL_TYPE_I64,
    CE_REPL_TYPE_U8,
    CE_REPL_TYPE_U16,
    CE_REPL_TYPE_U32,
    CE_REPL_TYPE_U64,
    CE_REPL_TYPE_F32,
    CE_REPL_TYPE_F64,
    CE_REPL_TYPE_BOOL,
    CE_REPL_TYPE_STRING,
    CE_REPL_TYPE_VEC2,      /* {float x, y} */
    CE_REPL_TYPE_VEC3,      /* {float x, y, z} */
    CE_REPL_TYPE_VEC4,      /* {float x, y, z, w} */
    CE_REPL_TYPE_QUAT,      /* {float x, y, z, w} */
    CE_REPL_TYPE_BLOB,      /* 变长二进制 */
} CeReplFieldType;

/* ---- 值域约束 ---- */

/** 字段值域约束 */
typedef struct CeReplConstraint {
    bool     has_min;         /* 是否有最小值约束 */
    bool     has_max;         /* 是否有最大值约束 */
    int64_t  min_value;       /* 最小值 (类型擦除，按 field->type 解释) */
    int64_t  max_value;       /* 最大值 */
} CeReplConstraint;

/* ---- 字段描述符 ---- */

/** 单个可复制字段的描述 */
typedef struct CeReplField {
    const char*        name;           /* 字段名 (与 FlatBuffer schema 一致) */
    CeReplFieldType    type;           /* 字段类型 */
    uint32_t           flags;          /* 位掩码: CE_FLAG_* */
    uint32_t           offset;         /* 字段在组件结构体中的偏移 (offsetof) */
    uint32_t           size;           /* 字段大小 (sizeof) */
    CeReplConstraint   constraint;     /* 值域约束 ({0} 表示无约束) */
} CeReplField;

/* ---- 配置 ---- */

/** 复制管理器配置 */
typedef struct CeReplConfig {
    uint32_t    max_components;     /* 最大注册组件数，默认 64 */
    uint32_t    max_fields_per_component; /* 每组件最大字段数，默认 32 */
    uint32_t    max_dirty_entities; /* 最大脏实体数，默认 4096 */
    uint32_t    max_rpc_handlers;   /* 最大 RPC handler 数，默认 128 */
} CeReplConfig;

/* ---- 编译期校验宏 ---- */

/**
 * 编译期断言：字段初始值必须在约束范围内
 *
 * 用法:
 *   #define PLAYER_INITIAL_HP 100
 *   CE_REPL_CHECK_INIT(hp, PLAYER_INITIAL_HP,
 *       ((CeReplConstraint){.has_min=true, .min_value=0,
 *                           .has_max=true, .max_value=999999}));
 *
 * 如果 PLAYER_INITIAL_HP 越界，编译失败并显示字段名。
 */
#define CE_REPL_CHECK_INIT(field_name, init_value, constraint)              \
    _Static_assert(                                                         \
        (!(constraint).has_min || ((init_value) >= (constraint).min_value)) \
        &&                                                                  \
        (!(constraint).has_max || ((init_value) <= (constraint).max_value)),\
        "CE_REPL: " #field_name " initial value out of constraint range"    \
    )

/* ---- 生命周期 ---- */

/**
 * 初始化复制管理器
 *
 * 分配上下文，初始化脏标表、字段注册表、RPC handler 表。
 * 初始化完成后自动调用 ce_repl_validate_initial_values()。
 * 校验失败返回 NULL，服务器应拒绝启动。
 *
 * @param config  配置 (NULL 则使用默认值)
 * @return        上下文，失败返回 NULL
 */
CeReplContext* ce_repl_init(const CeReplConfig* config);

/**
 * 关闭复制管理器
 *
 * 释放所有资源。应在服务器 shutdown 时调用。
 */
void ce_repl_shutdown(CeReplContext* ctx);

/* ---- 组件注册 ---- */

/**
 * 注册组件的可复制字段
 *
 * 每个需要属性复制的 ECS 组件在初始化时调用此函数。
 * 注册后，ECS setter 会自动标记脏。
 *
 * @param ctx               复制管理器上下文
 * @param component_id      组件类型 ID (与 ECS 一致)
 * @param component_name    组件名称 (用于日志)
 * @param fields            字段描述符数组 (以 {0} 终止)
 * @param default_template  组件的默认值模板 (用于启动时校验初始值)
 * @return                  CE_OK 成功
 */
CeResult ce_repl_register_component(
    CeReplContext*      ctx,
    uint32_t            component_id,
    const char*         component_name,
    const CeReplField*  fields,
    const void*         default_template
);

/**
 * 校验所有已注册字段的初始值是否在约束范围内
 *
 * 在 ce_repl_init 中自动调用。也可手动调用用于热更新场景。
 *
 * @param ctx  复制管理器上下文
 * @return     CE_OK 全部通过，CE_ERR_VALIDATION 有字段越界
 */
CeResult ce_repl_validate_initial_values(CeReplContext* ctx);

/* ---- 脏标 ---- */

/**
 * 标记实体的整个组件为脏 (ECS setter 自动调用)
 *
 * @param ctx          复制管理器上下文
 * @param entity_id    实体 ID
 * @param component_id 组件类型 ID
 */
void ce_repl_mark_dirty(CeReplContext* ctx, uint64_t entity_id, uint32_t component_id);

/**
 * 标记实体的单个字段为脏
 *
 * @param ctx          复制管理器上下文
 * @param entity_id    实体 ID
 * @param component_id 组件类型 ID
 * @param field_index  字段索引
 */
void ce_repl_mark_field_dirty(CeReplContext* ctx, uint64_t entity_id,
                              uint32_t component_id, uint32_t field_index);

/* ---- 帧更新 ---- */

/**
 * 每帧 tick (处理 AOI 事件、超时等)
 *
 * @param ctx  复制管理器上下文
 * @param dt   帧间隔 (秒)
 */
void ce_repl_tick(CeReplContext* ctx, float dt);

/**
 * 帧末 flush: 收集脏字段 → 按 flag 分流 → 发送
 *
 * 应在每帧末尾调用。流程:
 *   1. 遍历脏实体表
 *   2. 对每个脏字段按 flag 分流:
 *      - AOI_BROADCAST → 查 AOI → 打包 → 发送到 Gateway
 *      - OWNER_ONLY    → 查属主 → 打包 → 发送到 Gateway
 *      - PERSIST       → 打包 → 发送到 DBProxy
 *      - SERVER_ONLY   → 跳过
 *   3. 清除所有脏标
 *
 * @param ctx  复制管理器上下文
 */
void ce_repl_flush(CeReplContext* ctx);

/* ---- 连接设置 ---- */

/**
 * 设置 Gateway 连接 (用于发送到客户端)
 *
 * @param ctx      复制管理器上下文
 * @param gateway  Gateway 连接句柄
 */
void ce_repl_set_gateway(CeReplContext* ctx, CeGatewayConn* gateway);

/**
 * 设置 DBProxy 连接 (用于存档)
 *
 * @param ctx   复制管理器上下文
 * @param sync  DBProxy 同步上下文
 */
void ce_repl_set_dbproxy(CeReplContext* ctx, CeSyncContext* sync);

/**
 * 设置 DBProxy 同步发送回调函数
 *
 * 用于打破 engine_core → engine_sync 的循环依赖。
 * 在设置 sync 后调用此函数注册 ce_sync_send_frame 回调。
 *
 * @param ctx  复制管理器上下文
 * @param fn   发送回调 (签名: CeResult fn(CeSyncContext*, const CeSyncFrame*))
 */
void ce_repl_set_sync_send_fn(CeReplContext* ctx,
                               CeResult (*fn)(CeSyncContext*, const struct CeSyncFrame*));

/**
 * 设置 AOI 上下文 (用于 AOI 范围内广播)
 *
 * @param ctx  复制管理器上下文
 * @param aoi  AOI 上下文
 */
void ce_repl_set_aoi(CeReplContext* ctx, CeAoiContext* aoi);

/**
 * 设置实体属主映射 (entity_id → client_id)
 *
 * @param ctx       复制管理器上下文
 * @param entity_id 实体 ID
 * @param client_id 属主客户端 ID (0 表示无属主)
 */
void ce_repl_set_owner(CeReplContext* ctx, uint64_t entity_id, uint64_t client_id);

/* ---- 统计查询 ---- */

/** 复制统计信息 */
typedef struct CeReplStats {
    uint64_t    total_flushes;          /* 总 flush 次数 */
    uint64_t    total_entities_synced;  /* 累计同步实体数 */
    uint64_t    total_fields_synced;    /* 累计同步字段数 */
    uint64_t    total_bytes_sent;       /* 累计发送字节数 */
    uint32_t    current_dirty_entities; /* 当前脏实体数 */
    uint32_t    current_dirty_fields;   /* 当前脏字段数 */
    uint64_t    last_flush_time_us;     /* 上次 flush 耗时 (微秒) */
} CeReplStats;

/**
 * 获取复制统计信息
 */
void ce_repl_get_stats(const CeReplContext* ctx, CeReplStats* stats);

#ifdef __cplusplus
}
#endif

#endif /* CE_REPLICATION_H */
