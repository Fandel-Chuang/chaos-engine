/*
 * ChaosEngine RPC Call — 头文件
 *
 * 封装 Game 进程与 Router 集群的通信：
 *   - 服务注册/注销
 *   - 消息发送（RPC 调用）
 *   - 心跳维持
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 *
 * 二进制协议（复用 ce_net_base）：
 *   [4B total_len][2B msg_type][N payload]
 */

#ifndef CE_RPC_CALL_H
#define CE_RPC_CALL_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

/** 不透明 RPC 客户端上下文句柄 */
typedef struct CeRpcContext CeRpcContext;

/* ---- 服务类型枚举（与 service_types.lua 同步） ---- */

typedef enum CeServiceType {
    CE_SERVICE_AUTH   = 0x01,   /* 认证 & 登录 */
    CE_SERVICE_FRIEND = 0x02,   /* 好友系统 */
    CE_SERVICE_CHAT   = 0x03,   /* 聊天 & 消息 */
    CE_SERVICE_MAIL   = 0x04,   /* 邮件 & 通知 */
    CE_SERVICE_GUILD  = 0x05,   /* 公会管理 */
    CE_SERVICE_MATCH  = 0x06,   /* 匹配 & 队列 */
    CE_SERVICE_SHOP   = 0x07,   /* 商店 & 经济 */
    CE_SERVICE_SCENE  = 0x08,   /* 场景/世界管理 */
} CeServiceType;

/* ---- 消息类型 ---- */

/** RPC 消息类型枚举 */
typedef enum CeRpcMsgType {
    CE_RPC_MSG_REGISTER        = 0x2000,   /* 服务注册 */
    CE_RPC_MSG_REGISTER_RESP   = 0x2001,   /* 注册响应 */
    CE_RPC_MSG_HEARTBEAT       = 0x2002,   /* 心跳请求 */
    CE_RPC_MSG_HEARTBEAT_RESP  = 0x2003,   /* 心跳响应 */
    CE_RPC_MSG_DEREGISTER      = 0x2004,   /* 服务注销 */
    CE_RPC_MSG_RPC_CALL        = 0x2100,   /* RPC 调用 */
    CE_RPC_MSG_RPC_RESP        = 0x2101,   /* RPC 响应 */
} CeRpcMsgType;

/* ---- 常量 ---- */

/** 默认 Router 端口 */
#define CE_RPC_DEFAULT_ROUTER_PORT      9000

/** 最大消息大小 */
#define CE_RPC_MAX_MSG_SIZE             (256 * 1024)   /* 256 KiB */

/** 默认连接超时（毫秒） */
#define CE_RPC_DEFAULT_TIMEOUT_MS       10000

/** 默认心跳间隔（毫秒） */
#define CE_RPC_DEFAULT_HEARTBEAT_MS     5000

/** 默认心跳超时（毫秒） */
#define CE_RPC_DEFAULT_HEARTBEAT_TIMEOUT_MS 15000

/** 重连参数 */
#define CE_RPC_RECONNECT_MIN_MS         1000
#define CE_RPC_RECONNECT_MAX_MS         30000

/** 帧头大小：4B len + 2B msg_type */
#define CE_RPC_HEADER_SIZE              6

/* ---- 配置 ---- */

/** RPC 客户端配置 */
typedef struct CeRpcConfig {
    const char* router_host;          /* Router 地址 */
    int         router_port;          /* Router 端口 */
    CeServiceType service_type;       /* 本服务类型 */
    const char* service_name;         /* 服务实例名称 */
    int         heartbeat_ms;         /* 心跳间隔（毫秒） */
    int         heartbeat_timeout_ms; /* 心跳超时（毫秒） */
    int         timeout_ms;           /* 连接超时（毫秒） */
} CeRpcConfig;

/* ---- 消息 ---- */

/** RPC 消息 */
typedef struct CeRpcMessage {
    uint16_t        type;           /* 消息类型 */
    uint32_t        payload_len;    /* payload 长度 */
    const uint8_t*  payload;        /* payload 数据 */
} CeRpcMessage;

/** RPC 响应 */
typedef struct CeRpcResponse {
    uint16_t        type;           /* 响应类型 */
    uint32_t        payload_len;    /* payload 长度 */
    uint8_t         payload[CE_RPC_MAX_MSG_SIZE]; /* 响应数据 */
} CeRpcResponse;

/* ---- 生命周期 ---- */

/**
 * 初始化 RPC 客户端并连接到 Router
 *
 * @param config  RPC 配置（NULL 则使用默认值）
 * @return        RPC 上下文，失败返回 NULL
 */
CeRpcContext* ce_rpc_init(const CeRpcConfig* config);

/**
 * 断开连接并释放资源
 *
 * @param ctx  RPC 上下文
 */
void ce_rpc_shutdown(CeRpcContext* ctx);

/* ---- 服务注册 ---- */

/**
 * 向 Router 注册本服务
 *
 * 发送 CE_RPC_MSG_REGISTER 消息，包含服务类型和名称。
 *
 * @param ctx  RPC 上下文
 * @return     CE_OK 成功，CE_ERR 失败
 */
CeResult ce_rpc_register(CeRpcContext* ctx);

/**
 * 向 Router 注销本服务
 *
 * @param ctx  RPC 上下文
 * @return     CE_OK 成功，CE_ERR 失败
 */
CeResult ce_rpc_deregister(CeRpcContext* ctx);

/* ---- 消息发送 ---- */

/**
 * 发送 RPC 消息到 Router
 *
 * 通用消息发送接口，自动打包为二进制协议格式。
 *
 * @param ctx          RPC 上下文
 * @param msg_type     消息类型
 * @param payload      payload 数据
 * @param payload_len  payload 长度
 * @return             CE_OK 成功，CE_ERR 失败
 */
CeResult ce_rpc_send(CeRpcContext* ctx, uint16_t msg_type,
                     const uint8_t* payload, uint32_t payload_len);

/**
 * 发送 RPC 调用到目标服务
 *
 * 封装 CE_RPC_MSG_RPC_CALL 消息，由 Router 转发到目标服务。
 *
 * @param ctx           RPC 上下文
 * @param target_service 目标服务类型
 * @param method        RPC 方法名
 * @param params        RPC 参数（JSON 字符串）
 * @param params_len    参数长度
 * @return              CE_OK 成功，CE_ERR 失败
 */
CeResult ce_rpc_call(CeRpcContext* ctx, CeServiceType target_service,
                     const char* method, const char* params, uint32_t params_len);

/* ---- 消息接收 ---- */

/**
 * 非阻塞接收 Router 响应
 *
 * @param ctx       RPC 上下文
 * @param response  输出参数，接收响应
 * @return          CE_OK 有响应，CE_ERR 无响应或出错
 */
CeResult ce_rpc_recv(CeRpcContext* ctx, CeRpcResponse* response);

/* ---- 心跳 ---- */

/**
 * 发送心跳到 Router
 *
 * @param ctx  RPC 上下文
 * @return     CE_OK 成功，CE_ERR 失败
 */
CeResult ce_rpc_send_heartbeat(CeRpcContext* ctx);

/**
 * 检查心跳是否超时
 *
 * @param ctx  RPC 上下文
 * @return     CE_TRUE 超时，CE_FALSE 正常
 */
CeBool ce_rpc_heartbeat_timeout(const CeRpcContext* ctx);

/**
 * 更新心跳时间（收到任何消息时调用）
 *
 * @param ctx  RPC 上下文
 */
void ce_rpc_heartbeat_touch(CeRpcContext* ctx);

/* ---- 状态查询 ---- */

/**
 * 获取当前连接是否活跃
 *
 * @param ctx  RPC 上下文
 * @return     CE_TRUE 已连接，CE_FALSE 未连接
 */
CeBool ce_rpc_is_connected(const CeRpcContext* ctx);

/**
 * 获取服务注册状态
 *
 * @param ctx  RPC 上下文
 * @return     CE_TRUE 已注册，CE_FALSE 未注册
 */
CeBool ce_rpc_is_registered(const CeRpcContext* ctx);

/**
 * 获取当前 Router 端点描述
 *
 * @param ctx  RPC 上下文
 * @return     端点字符串（如 "127.0.0.1:9000"），内部缓冲区
 */
const char* ce_rpc_current_endpoint(const CeRpcContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CE_RPC_CALL_H */
