/*
 * ChaosEngine 存档管理器 — 头文件
 *
 * 负责定时存档 + 手动存档 + 脏实体收集。
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 *
 * 功能：
 * - 定时自动存档（可配置间隔和全量频率）
 * - 手动触发存档（指定玩家 / 全部在线玩家）
 * - 事件触发存档（玩家下线等）
 * - FIFO 存档队列，避免并发写冲突
 * - 全量存档：忽略 dirty flag，收集所有实体
 * - 增量存档：仅收集脏实体
 */

#ifndef CE_SAVE_H
#define CE_SAVE_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

/** 不透明存档上下文句柄 */
typedef struct CeSaveContext CeSaveContext;

/** DBProxy 客户端上下文（来自 dbproxy/ce_dbproxy.h） */
typedef struct CeDbproxyContext CeDbproxyContext;

/** Admin IPC 上下文（来自 admin_ipc/ce_admin_ipc.h） */
typedef struct CeAdminIpc CeAdminIpc;

/* ---- 存档模式 ---- */

/** 存档模式枚举 */
typedef enum CeSaveMode {
    CE_SAVE_FULL        = 0,   /* 全量存档：忽略 dirty flag，收集所有实体 */
    CE_SAVE_INCREMENTAL = 1,   /* 增量存档：仅收集脏实体 */
} CeSaveMode;

/* ---- 存档事件类型 ---- */

/** 触发存档的事件类型 */
typedef enum CeSaveEvent {
    CE_SAVE_EVENT_PLAYER_LOGOUT  = 0,   /* 玩家下线 */
    CE_SAVE_EVENT_PLAYER_DEATH   = 1,   /* 玩家死亡 */
    CE_SAVE_EVENT_WORLD_CHECKPOINT = 2, /* 世界检查点 */
    CE_SAVE_EVENT_SERVER_SHUTDOWN  = 3, /* 服务器关闭 */
    CE_SAVE_EVENT_MANUAL         = 4,   /* 手动触发 */
    CE_SAVE_EVENT_TIMER          = 5,   /* 定时器触发 */
} CeSaveEvent;

/* ---- 常量 ---- */

/** 默认存档间隔（秒） */
#define CE_SAVE_DEFAULT_INTERVAL_SEC    300

/** 默认全量存档频率（每 N 次增量后一次全量） */
#define CE_SAVE_DEFAULT_FULL_EVERY_N    6

/** 存档队列最大深度 */
#define CE_SAVE_MAX_QUEUE_DEPTH         64

/** 单次存档最大实体数 */
#define CE_SAVE_MAX_ENTITIES_PER_SAVE   4096

/** 单个实体序列化缓冲区最大大小 */
#define CE_SAVE_MAX_ENTITY_BUF_SIZE     (64 * 1024)   /* 64 KiB */

/* ---- 存档配置 ---- */

/** 存档模块配置 */
typedef struct CeSaveConfig {
    int     save_interval_sec;    /* 定时存档间隔（秒），默认 300 */
    int     full_save_every_n;    /* 每 N 次增量后一次全量，默认 6 */
    CeBool  auto_save_enabled;    /* 是否启用自动存档，默认 CE_TRUE */
} CeSaveConfig;

/* ---- 存档统计 ---- */

/** 存档统计信息 */
typedef struct CeSaveStats {
    uint64_t    total_saves;        /* 总存档次数 */
    uint64_t    full_saves;         /* 全量存档次数 */
    uint64_t    incremental_saves;  /* 增量存档次数 */
    uint64_t    total_entities_saved; /* 累计存档实体数 */
    uint64_t    last_save_time_us;  /* 上次存档时间戳（微秒） */
    uint64_t    last_save_duration_us; /* 上次存档耗时（微秒） */
    int         queue_depth;        /* 当前队列深度 */
    int         dirty_entity_count; /* 当前脏实体数 */
} CeSaveStats;

/* ---- 生命周期 ---- */

/**
 * 初始化存档管理器
 *
 * 初始化定时器，注册 Admin IPC 命令 save.now / save.status。
 *
 * @param dbproxy  DBProxy 客户端上下文（必须已连接）
 * @param config   存档配置（NULL 则使用默认值）
 * @return         存档上下文，失败返回 NULL
 */
CeSaveContext* ce_save_init(CeDbproxyContext* dbproxy, const CeSaveConfig* config);

/**
 * 关闭存档管理器
 *
 * 等待队列中所有存档完成，释放所有资源。
 *
 * @param ctx  存档上下文
 */
void ce_save_shutdown(CeSaveContext* ctx);

/* ---- 帧更新 ---- */

/**
 * 每帧调用，检查定时器是否到期
 *
 * 如果定时器到期且队列未满，自动触发一次增量存档。
 * 每 full_save_every_n 次增量后触发一次全量存档。
 *
 * @param ctx  存档上下文
 */
void ce_save_tick(CeSaveContext* ctx);

/* ---- 手动存档 ---- */

/**
 * 立即存档指定玩家（手动触发）
 *
 * 收集该玩家的所有组件数据，序列化后发送到 DBProxy。
 * 如果队列已满，返回 CE_ERR。
 *
 * @param ctx       存档上下文
 * @param entity_id 玩家实体 ID
 * @param mode      存档模式（全量/增量）
 * @return          CE_OK 成功入队，CE_ERR 失败
 */
CeResult ce_save_now(CeSaveContext* ctx, uint64_t entity_id, CeSaveMode mode);

/**
 * 立即存档所有在线玩家
 *
 * 遍历所有在线玩家实体，为每个玩家触发一次存档。
 *
 * @param ctx   存档上下文
 * @param mode  存档模式（全量/增量）
 * @return      CE_OK 成功，CE_ERR 失败
 */
CeResult ce_save_all(CeSaveContext* ctx, CeSaveMode mode);

/* ---- 事件触发存档 ---- */

/**
 * 事件触发存档（玩家下线等）
 *
 * 根据事件类型决定存档模式：
 * - PLAYER_LOGOUT / PLAYER_DEATH: 增量存档该玩家
 * - WORLD_CHECKPOINT / SERVER_SHUTDOWN: 全量存档所有玩家
 *
 * @param ctx       存档上下文
 * @param event     事件类型
 * @param entity_id 关联实体 ID（CE_ENTITY_NULL 表示不关联特定实体）
 * @return          CE_OK 成功，CE_ERR 失败
 */
CeResult ce_save_on_event(CeSaveContext* ctx, CeSaveEvent event, uint64_t entity_id);

/* ---- 脏标记 ---- */

/**
 * 标记实体为脏（组件被修改时调用）
 *
 * 由 ECS 系统在组件写入后调用，标记该实体需要增量存档。
 *
 * @param ctx       存档上下文
 * @param entity_id 实体 ID
 */
void ce_save_mark_dirty(CeSaveContext* ctx, uint64_t entity_id);

/**
 * 清除实体的脏标记（存档完成后调用）
 *
 * @param ctx       存档上下文
 * @param entity_id 实体 ID
 */
void ce_save_clear_dirty(CeSaveContext* ctx, uint64_t entity_id);

/* ---- 统计查询 ---- */

/**
 * 获取存档统计信息
 *
 * @param ctx   存档上下文
 * @param stats 输出参数，接收统计信息
 */
void ce_save_get_stats(const CeSaveContext* ctx, CeSaveStats* stats);

/**
 * 获取 Admin IPC 状态 JSON（供 save.status 命令使用）
 *
 * @param ctx       存档上下文
 * @param buf       输出缓冲区
 * @param max_len   缓冲区大小
 * @return          写入的字节数，-1 表示缓冲区不足
 */
int ce_save_get_status_json(const CeSaveContext* ctx, char* buf, int max_len);

/**
 * 注册存档模块的 Admin IPC 命令
 *
 * 应在 ce_admin_ipc_start 之后调用。
 * 注册 save.now 和 save.status 两个命令。
 *
 * @param ctx       存档上下文
 * @param admin_ipc Admin IPC 句柄
 * @return          CE_OK 成功，CE_ERR 失败
 */
CeResult ce_save_register_admin_commands(CeSaveContext* ctx, CeAdminIpc* admin_ipc);

#ifdef __cplusplus
}
#endif

#endif /* CE_SAVE_H */
