/*
 * ChaosEngine DBProxy 客户端 — 头文件
 *
 * 负责 Game 进程与 DBProxy 之间的 TCP 通信。
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 *
 * 二进制协议：
 *   [4B total_len][2B msg_type][N payload]
 */

#ifndef CE_DBPROXY_H
#define CE_DBPROXY_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

/** 不透明 DBProxy 客户端上下文句柄 */
typedef struct CeDbproxyContext CeDbproxyContext;

/* ---- 消息类型 ---- */

/** DBProxy 消息类型枚举 */
typedef enum CeDbproxyMsgType {
    DB_SAVE_PLAYER  = 0x01,   /* 保存玩家数据 */
    DB_LOAD_PLAYER  = 0x02,   /* 加载玩家数据 */
    DB_SAVE_WORLD   = 0x03,   /* 保存世界数据 */
    DB_LOAD_WORLD   = 0x04,   /* 加载世界数据 */
    DB_HEARTBEAT    = 0x05,   /* 心跳 */
    DB_ERROR        = 0xFF,   /* 错误响应 */
} CeDbproxyMsgType;

/* ---- 常量 ---- */

/** 默认 DBProxy 端口 */
#define CE_DBPROXY_DEFAULT_PORT         9700

/** 最大消息大小 */
#define CE_DBPROXY_MAX_MSG_SIZE         (256 * 1024)   /* 256 KiB */

/** 默认连接超时（毫秒） */
#define CE_DBPROXY_DEFAULT_TIMEOUT_MS   10000

/** 默认心跳间隔（毫秒） */
#define CE_DBPROXY_DEFAULT_HEARTBEAT_MS 5000

/** 重连参数 */
#define CE_DBPROXY_RECONNECT_MIN_MS     1000    /* 初始退避 1s */
#define CE_DBPROXY_RECONNECT_MAX_MS     30000   /* 最大退避 30s */

/* ---- 配置 ---- */

/** DBProxy 客户端配置 */
typedef struct CeDbproxyConfig {
    const char* primary_host;     /* 主 DBProxy 地址 */
    int         primary_port;     /* 主 DBProxy 端口 */
    const char* backup_host;      /* 备用 DBProxy 地址 */
    int         backup_port;      /* 备用 DBProxy 端口 */
    int         heartbeat_ms;     /* 心跳间隔（毫秒） */
    int         timeout_ms;       /* 连接超时（毫秒） */
} CeDbproxyConfig;

/* ---- 消息 ---- */

/** DBProxy 消息 */
typedef struct CeDbproxyMessage {
    CeDbproxyMsgType  type;       /* 消息类型 */
    uint32_t          payload_len;/* payload 长度 */
    const uint8_t*    payload;    /* payload 数据（调用者管理生命周期） */
} CeDbproxyMessage;

/** DBProxy 响应 */
typedef struct CeDbproxyResponse {
    CeDbproxyMsgType  type;       /* 响应类型 */
    uint32_t          payload_len;/* payload 长度 */
    uint8_t           payload[CE_DBPROXY_MAX_MSG_SIZE]; /* 响应数据（内部缓冲区） */
} CeDbproxyResponse;

/* ---- 生命周期 ---- */

/**
 * 初始化 DBProxy 客户端
 *
 * 分配上下文，连接到主 DBProxy（非阻塞模式）。
 *
 * @param config  客户端配置（NULL 则使用默认值）
 * @return        客户端上下文，失败返回 NULL
 */
CeDbproxyContext* ce_dbproxy_connect(const CeDbproxyConfig* config);

/**
 * 断开连接并释放资源
 *
 * @param ctx  客户端上下文
 */
void ce_dbproxy_disconnect(CeDbproxyContext* ctx);

/* ---- 消息收发 ---- */

/**
 * 发送消息到 DBProxy
 *
 * 二进制协议打包：[4B total_len][2B msg_type][N payload]
 * total_len = 6 + payload_len（大端序）
 *
 * @param ctx  客户端上下文
 * @param msg  待发送的消息
 * @return     CE_OK 成功，CE_ERR 失败
 */
CeResult ce_dbproxy_send(CeDbproxyContext* ctx, const CeDbproxyMessage* msg);

/**
 * 非阻塞接收 DBProxy 响应
 *
 * @param ctx      客户端上下文
 * @param response 输出参数，接收响应（内部缓冲区，下次 recv 前有效）
 * @return         CE_OK 有响应，CE_ERR 无响应或出错
 */
CeResult ce_dbproxy_recv(CeDbproxyContext* ctx, CeDbproxyResponse* response);

/* ---- 主备切换 ---- */

/**
 * 切换主备地址
 *
 * 更新配置中的主备地址，但不立即重连。
 * 下次自动重连时将使用新地址。
 *
 * @param ctx          客户端上下文
 * @param primary_host 新主地址（NULL 保持不变）
 * @param primary_port 新主端口（<=0 保持不变）
 * @param backup_host  新备地址（NULL 保持不变）
 * @param backup_port  新备端口（<=0 保持不变）
 */
void ce_dbproxy_set_master(CeDbproxyContext* ctx,
                           const char* primary_host, int primary_port,
                           const char* backup_host,  int backup_port);

/* ---- 状态查询 ---- */

/**
 * 获取当前连接是否活跃
 *
 * @param ctx  客户端上下文
 * @return     CE_TRUE 已连接，CE_FALSE 未连接
 */
CeBool ce_dbproxy_is_connected(const CeDbproxyContext* ctx);

/**
 * 获取当前使用的端点描述（用于日志）
 *
 * @param ctx  客户端上下文
 * @return     端点字符串（如 "127.0.0.1:9700"），内部缓冲区
 */
const char* ce_dbproxy_current_endpoint(const CeDbproxyContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CE_DBPROXY_H */
