/*
 * ChaosEngine Game↔DBProxy 同步模块
 *
 * 提供游戏服务器与 DBProxy 之间的帧同步通信：
 * - TCP 长连接到 DBProxy（主/备）
 * - 二进制帧协议序列化/反序列化
 * - 非阻塞轮询 + 心跳保活
 * - 主备自动切换
 *
 * 纯 C99，ce_ 前缀
 */

#ifndef CE_SYNC_H
#define CE_SYNC_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

/** 不透明同步上下文句柄 */
typedef struct CeSyncContext CeSyncContext;

/* ---- 常量 ---- */

/** 最大帧大小（含所有 entity 数据） */
#define CE_SYNC_MAX_FRAME_SIZE    (64 * 1024)     /* 64 KiB */

/** 环形缓冲区大小 */
#define CE_SYNC_RING_SIZE         256

/** 默认心跳间隔（毫秒） */
#define CE_SYNC_DEFAULT_HEARTBEAT_MS  5000

/** 默认超时（毫秒） */
#define CE_SYNC_DEFAULT_TIMEOUT_MS    10000

/** 默认 DBProxy 端口 */
#define CE_SYNC_DEFAULT_PORT          9700

/** 心跳帧特殊标记：frame_seq = 0xFFFF */
#define CE_SYNC_HEARTBEAT_SEQ         ((uint16_t)0xFFFF)

/* ---- 同步实体 ---- */

/** 单个同步实体（序列化单元） */
typedef struct CeSyncEntity {
    uint64_t    entity_id;        /* 实体 ID（8 字节） */
    uint16_t    component_type;   /* 组件类型 */
    uint32_t    data_len;         /* 数据长度 */
    uint8_t*    data;             /* 组件数据 */
} CeSyncEntity;

/* ---- 同步帧 ---- */

/** 同步帧（Game → DBProxy 或 DBProxy → Game） */
typedef struct CeSyncFrame {
    uint16_t    frame_seq;        /* 帧序号（心跳帧 = 0xFFFF） */
    uint64_t    timestamp;        /* 时间戳（微秒） */
    uint16_t    entity_count;     /* 实体数量 */
    CeSyncEntity* entities;       /* 实体数组（调用者管理生命周期） */
} CeSyncFrame;

/* ---- 同步配置 ---- */

/** 同步模块配置 */
typedef struct CeSyncConfig {
    const char* dbproxy_host;     /* 主 DBProxy 地址 */
    int         dbproxy_port;     /* 主 DBProxy 端口 */
    const char* backup_host;      /* 备用 DBProxy 地址 */
    int         backup_port;      /* 备用 DBProxy 端口 */
    int         heartbeat_ms;     /* 心跳间隔（毫秒） */
    int         timeout_ms;       /* 连接超时（毫秒） */
} CeSyncConfig;

/* ---- 响应类型 ---- */

/** DBProxy 响应帧类型 */
typedef enum CeSyncResponseType {
    CE_SYNC_RESP_OK       = 0,   /* 同步成功 */
    CE_SYNC_RESP_ERROR    = 1,   /* 同步失败 */
    CE_SYNC_RESP_HEARTBEAT = 2,  /* 心跳响应 */
    CE_SYNC_RESP_DATA     = 3,   /* 数据响应（含实体数据） */
} CeSyncResponseType;

/** DBProxy 响应 */
typedef struct CeSyncResponse {
    CeSyncResponseType  type;
    uint16_t            frame_seq;      /* 对应的帧序号 */
    uint64_t            timestamp;      /* 服务端时间戳 */
    uint16_t            entity_count;
    CeSyncEntity*       entities;       /* 响应实体（内部缓冲区，仅 poll 返回时有效） */
    int                 error_code;
} CeSyncResponse;

/* ---- 生命周期 ---- */

/**
 * 初始化同步模块
 *
 * 连接到主 DBProxy，初始化环形缓冲区。
 *
 * @param config  同步配置（NULL 则使用默认值）
 * @return        同步上下文，失败返回 NULL
 */
CeSyncContext* ce_sync_init(const CeSyncConfig* config);

/**
 * 关闭同步模块
 *
 * 断开连接，释放所有资源。
 */
void ce_sync_shutdown(CeSyncContext* ctx);

/* ---- 帧操作 ---- */

/**
 * 发送同步帧到 DBProxy
 *
 * 将 CeSyncFrame 序列化为二进制协议并通过 TCP 发送。
 * 协议格式：
 *   [4B frame_len][2B frame_seq][8B timestamp][2B entity_count][N entities...]
 *   每个 entity: [8B entity_id][2B component_type][4B data_len][N data]
 *
 * @param ctx   同步上下文
 * @param frame 待发送的帧
 * @return      CE_OK 成功，CE_ERR 失败
 */
CeResult ce_sync_send_frame(CeSyncContext* ctx, const CeSyncFrame* frame);

/**
 * 非阻塞轮询 DBProxy 响应
 *
 * @param ctx      同步上下文
 * @param response 输出参数，接收响应（内部缓冲区，下次 poll 前有效）
 * @return         CE_OK 有响应，CE_ERR 无响应或出错
 */
CeResult ce_sync_poll(CeSyncContext* ctx, CeSyncResponse* response);

/* ---- 心跳 ---- */

/**
 * 发送心跳帧
 *
 * 心跳帧使用 frame_seq = 0xFFFF 作为特殊标记。
 *
 * @param ctx  同步上下文
 * @return     CE_OK 成功，CE_ERR 失败
 */
CeResult ce_sync_heartbeat(CeSyncContext* ctx);

/* ---- 故障切换 ---- */

/**
 * 切换到备用 DBProxy
 *
 * 断开当前连接，连接到备用 DBProxy。
 * 如果备用地址为 NULL 或连接失败，返回 CE_ERR。
 *
 * @param ctx  同步上下文
 * @return     CE_OK 成功，CE_ERR 失败
 */
CeResult ce_sync_switch_dbproxy(CeSyncContext* ctx);

/* ---- 状态查询 ---- */

/**
 * 获取当前连接是否活跃
 */
CeBool ce_sync_is_connected(const CeSyncContext* ctx);

/**
 * 获取当前使用的 DBProxy 地址描述（用于日志）
 */
const char* ce_sync_current_endpoint(const CeSyncContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CE_SYNC_H */
