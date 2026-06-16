/*
 * ChaosEngine 共享网络库 ce_net_base
 *
 * 提供 TCP 连接管理、二进制协议编解码、消息收发、心跳检测、
 * 连接池、自动重连、跨区消息格式和全球 Router 网格连接管理。
 *
 * 二进制协议格式：
 *   [4B total_len][2B msg_type][N payload]
 *   total_len = 6 + payload_len（大端序，包含头部的 6 字节）
 *
 * 纯 C99，ce_ 前缀，复用 ce_async_io.h 异步 I/O 接口。
 */

#ifndef CE_NET_BASE_H
#define CE_NET_BASE_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 前向声明
 * ================================================================ */

typedef struct CeNetConnection  CeNetConnection;
typedef struct CeNetPool        CeNetPool;
typedef struct CeNetRouterMesh  CeNetRouterMesh;

/* ================================================================
 * 常量
 * ================================================================ */

/** 协议帧头大小：4B len + 2B type */
#define CE_NET_BASE_HEADER_SIZE         6

/** 默认最大消息大小 */
#define CE_NET_BASE_MAX_MSG_SIZE        (256 * 1024)   /* 256 KiB */

/** 默认接收缓冲区大小 */
#define CE_NET_BASE_RECV_BUF_SIZE       CE_NET_BASE_MAX_MSG_SIZE

/** 默认连接超时（毫秒） */
#define CE_NET_BASE_DEFAULT_TIMEOUT_MS  10000

/** 默认心跳间隔（毫秒） */
#define CE_NET_BASE_DEFAULT_HEARTBEAT_MS 5000

/** 默认心跳超时（毫秒，3 次心跳无响应判定超时） */
#define CE_NET_BASE_DEFAULT_HEARTBEAT_TIMEOUT_MS 15000

/** 重连参数：最小退避 */
#define CE_NET_BASE_RECONNECT_MIN_MS    1000

/** 重连参数：最大退避 */
#define CE_NET_BASE_RECONNECT_MAX_MS    30000

/** 连接池默认最大连接数 */
#define CE_NET_BASE_POOL_DEFAULT_MAX    16

/** Router 网格默认最大区域数 */
#define CE_NET_BASE_MESH_MAX_REGIONS    32

/* ================================================================
 * 消息类型枚举
 * ================================================================ */

/** 通用消息类型（0x0000-0x0FFF 保留给系统消息） */
typedef enum CeNetMsgType {
    CE_NET_MSG_PING         = 0x0001,   /* 心跳请求 */
    CE_NET_MSG_PONG         = 0x0002,   /* 心跳响应 */
    CE_NET_MSG_LOGIN        = 0x0010,   /* 登录请求 */
    CE_NET_MSG_LOGIN_RESP   = 0x0011,   /* 登录响应 */
    CE_NET_MSG_GAME_DATA    = 0x0100,   /* 游戏数据 */
    CE_NET_MSG_DISCONNECT   = 0xFFFF,   /* 优雅断开 */
    /* 跨区消息（0x1000-0x1FFF） */
    CE_NET_MSG_CROSS_REGION = 0x1000,   /* 跨区转发 */
    CE_NET_MSG_REGION_SYNC  = 0x1001,   /* 区域同步 */
    CE_NET_MSG_ROUTER_HELLO = 0x1002,   /* Router 发现 */
    CE_NET_MSG_ROUTER_BYE   = 0x1003,   /* Router 离开 */
    /* 用户自定义消息从 0x8000 开始 */
    CE_NET_MSG_USER_BASE    = 0x8000,
} CeNetMsgType;

/* ================================================================
 * 消息结构
 * ================================================================ */

/** 通用消息 */
typedef struct CeNetMessage {
    uint16_t        type;           /* 消息类型 */
    uint32_t        payload_len;    /* payload 长度 */
    const uint8_t*  payload;        /* payload 数据（调用者管理生命周期） */
} CeNetMessage;

/** 接收到的消息（内部缓冲区） */
typedef struct CeNetRecvMessage {
    uint16_t        type;           /* 消息类型 */
    uint32_t        payload_len;    /* payload 长度 */
    uint8_t         payload[CE_NET_BASE_MAX_MSG_SIZE]; /* payload 数据 */
} CeNetRecvMessage;

/* ================================================================
 * 连接状态
 * ================================================================ */

typedef enum CeNetConnState {
    CE_NET_CONN_DISCONNECTED = 0,
    CE_NET_CONN_CONNECTING   = 1,
    CE_NET_CONN_CONNECTED    = 2,
    CE_NET_CONN_CLOSING      = 3,
} CeNetConnState;

/* ================================================================
 * 连接配置
 * ================================================================ */

typedef struct CeNetConnConfig {
    const char* host;               /* 目标主机 */
    int         port;               /* 目标端口 */
    int         timeout_ms;         /* 连接超时（毫秒） */
    int         heartbeat_ms;       /* 心跳间隔（毫秒），0 禁用 */
    int         heartbeat_timeout_ms; /* 心跳超时（毫秒） */
    CeBool      auto_reconnect;     /* 是否自动重连 */
    CeBool      nonblocking;        /* 是否非阻塞模式 */
} CeNetConnConfig;

/* ================================================================
 * 连接统计
 * ================================================================ */

typedef struct CeNetConnStats {
    uint64_t    bytes_sent;
    uint64_t    bytes_recv;
    uint64_t    msgs_sent;
    uint64_t    msgs_recv;
    uint64_t    connect_time_us;    /* 连接建立时间（单调时钟） */
    uint64_t    last_active_us;     /* 最后活跃时间 */
    uint64_t    last_heartbeat_us;  /* 最后心跳时间 */
    int         reconnect_count;    /* 重连次数 */
    CeNetConnState state;           /* 当前状态 */
} CeNetConnStats;

/* ================================================================
 * 1.1-1.3: TCP 连接管理 + 消息收发
 * ================================================================ */

/**
 * 创建 TCP 连接
 *
 * 根据配置创建连接，可选非阻塞模式和自动重连。
 *
 * @param config  连接配置（NULL 则使用默认值 localhost:0）
 * @return        连接句柄，失败返回 NULL
 */
CeNetConnection* ce_net_conn_create(const CeNetConnConfig* config);

/**
 * 连接到目标主机
 *
 * 如果 auto_reconnect 为 CE_TRUE，连接失败后会自动重试。
 *
 * @param conn  连接句柄
 * @return      CE_OK 成功，CE_ERR 失败
 */
CeResult ce_net_conn_connect(CeNetConnection* conn);

/**
 * 断开连接
 *
 * @param conn  连接句柄
 */
void ce_net_conn_disconnect(CeNetConnection* conn);

/**
 * 销毁连接并释放所有资源
 *
 * @param conn  连接句柄
 */
void ce_net_conn_destroy(CeNetConnection* conn);

/**
 * 获取连接状态
 */
CeNetConnState ce_net_conn_get_state(const CeNetConnection* conn);

/**
 * 获取连接的文件描述符（用于集成到事件循环）
 */
int ce_net_conn_get_fd(const CeNetConnection* conn);

/**
 * 获取连接统计信息
 */
void ce_net_conn_get_stats(const CeNetConnection* conn, CeNetConnStats* stats);

/* ---- 消息收发 ---- */

/**
 * 发送消息
 *
 * 自动打包为二进制协议格式：[4B total_len][2B msg_type][N payload]
 *
 * @param conn  连接句柄
 * @param msg   待发送的消息
 * @return      CE_OK 成功，CE_ERR 失败
 */
CeResult ce_net_conn_send(CeNetConnection* conn, const CeNetMessage* msg);

/**
 * 非阻塞接收消息
 *
 * 从 socket 读取数据到内部缓冲区，尝试解析完整消息。
 *
 * @param conn      连接句柄
 * @param out_msg   输出参数，接收解析后的消息（内部缓冲区，下次调用前有效）
 * @return          CE_OK 有完整消息，CE_ERR 无消息或出错
 */
CeResult ce_net_conn_recv(CeNetConnection* conn, CeNetRecvMessage* out_msg);

/* ================================================================
 * 1.4: 心跳检测
 * ================================================================ */

/**
 * 发送心跳 PING
 *
 * @param conn  连接句柄
 * @return      CE_OK 成功，CE_ERR 失败
 */
CeResult ce_net_conn_send_ping(CeNetConnection* conn);

/**
 * 发送心跳 PONG
 *
 * @param conn  连接句柄
 * @return      CE_OK 成功，CE_ERR 失败
 */
CeResult ce_net_conn_send_pong(CeNetConnection* conn);

/**
 * 检查心跳是否超时
 *
 * @param conn  连接句柄
 * @return      CE_TRUE 心跳超时，CE_FALSE 正常
 */
CeBool ce_net_conn_heartbeat_timeout(const CeNetConnection* conn);

/**
 * 更新心跳时间（收到任何消息时调用）
 *
 * @param conn  连接句柄
 */
void ce_net_conn_heartbeat_touch(CeNetConnection* conn);

/* ================================================================
 * 1.5: 连接池
 * ================================================================ */

/**
 * 创建连接池
 *
 * @param max_connections  最大连接数
 * @return                 连接池句柄，失败返回 NULL
 */
CeNetPool* ce_net_pool_create(int max_connections);

/**
 * 销毁连接池（关闭所有连接）
 *
 * @param pool  连接池句柄
 */
void ce_net_pool_destroy(CeNetPool* pool);

/**
 * 向连接池添加连接
 *
 * @param pool  连接池句柄
 * @param conn  连接句柄（所有权转移给连接池）
 * @return      CE_OK 成功，CE_ERR 失败（池已满）
 */
CeResult ce_net_pool_add(CeNetPool* pool, CeNetConnection* conn);

/**
 * 从连接池获取一个可用连接（轮询策略）
 *
 * @param pool  连接池句柄
 * @return      可用连接，无可用连接返回 NULL
 */
CeNetConnection* ce_net_pool_acquire(CeNetPool* pool);

/**
 * 释放连接回连接池
 *
 * @param pool  连接池句柄
 * @param conn  连接句柄
 */
void ce_net_pool_release(CeNetPool* pool, CeNetConnection* conn);

/**
 * 从连接池移除连接
 *
 * @param pool  连接池句柄
 * @param conn  连接句柄（所有权转回调用者）
 * @return      CE_OK 成功，CE_ERR 未找到
 */
CeResult ce_net_pool_remove(CeNetPool* pool, CeNetConnection* conn);

/**
 * 获取连接池统计
 *
 * @param pool       连接池句柄
 * @param total      输出：总连接数
 * @param available  输出：可用连接数
 */
void ce_net_pool_stats(const CeNetPool* pool, int* total, int* available);

/**
 * 关闭连接池中所有断开的连接
 *
 * @param pool  连接池句柄
 * @return      清理的连接数
 */
int ce_net_pool_cleanup(CeNetPool* pool);

/* ================================================================
 * 1.6: 自动重连
 * ================================================================ */

/**
 * 触发连接重连（指数退避）
 *
 * 如果连接断开且 auto_reconnect 为 CE_TRUE，按指数退避策略重试。
 * 应在事件循环中周期性调用。
 *
 * @param conn  连接句柄
 * @return      CE_OK 已连接或重连成功，CE_ERR 仍在等待重试
 */
CeResult ce_net_conn_try_reconnect(CeNetConnection* conn);

/**
 * 重置重连退避计时器
 *
 * @param conn  连接句柄
 */
void ce_net_conn_reset_reconnect(CeNetConnection* conn);

/* ================================================================
 * 1.7: 二进制协议编解码（独立工具函数）
 * ================================================================ */

/**
 * 打包消息为二进制格式
 *
 * 格式：[4B total_len][2B msg_type][N payload]
 * total_len = 6 + payload_len（大端序）
 *
 * @param buf           输出缓冲区
 * @param buf_size      缓冲区大小
 * @param msg_type      消息类型
 * @param payload       payload 数据
 * @param payload_len   payload 长度
 * @return              打包后的总长度，0 表示缓冲区不足
 */
uint32_t ce_net_base_pack(uint8_t* buf, uint32_t buf_size,
                          uint16_t msg_type,
                          const uint8_t* payload, uint32_t payload_len);

/**
 * 解包二进制消息
 *
 * @param data          输入数据
 * @param data_len      输入数据长度
 * @param out_type      输出：消息类型
 * @param out_payload   输出：payload 指针（指向 data 内部）
 * @param out_payload_len 输出：payload 长度
 * @return              CE_OK 成功，CE_ERR 数据不完整或格式错误
 */
CeResult ce_net_base_unpack(const uint8_t* data, uint32_t data_len,
                            uint16_t* out_type,
                            const uint8_t** out_payload, uint32_t* out_payload_len);

/**
 * 获取消息总长度（从头部读取，不解包）
 *
 * @param data      输入数据
 * @param data_len  输入数据长度
 * @return          消息总长度，0 表示头部不完整
 */
uint32_t ce_net_base_peek_len(const uint8_t* data, uint32_t data_len);

/**
 * 获取消息类型（从头部读取，不解包）
 *
 * @param data      输入数据
 * @param data_len  输入数据长度
 * @return          消息类型，0 表示头部不完整
 */
uint16_t ce_net_base_peek_type(const uint8_t* data, uint32_t data_len);

/* ================================================================
 * 1.10-1.12: 跨区消息格式 & 全球 Router 网格
 * ================================================================ */

/** 区域标识 */
typedef struct CeNetRegion {
    char        name[32];       /* 区域名称（如 "us-east", "eu-west"） */
    char        host[256];      /* Router 地址 */
    uint16_t    port;           /* Router 端口 */
    uint32_t    region_id;      /* 区域 ID（全局唯一） */
    CeBool      active;         /* 是否活跃 */
} CeNetRegion;

/** 跨区消息扩展头 */
typedef struct CeNetCrossRegionHeader {
    uint32_t    src_region;     /* 源区域 ID */
    uint32_t    dst_region;     /* 目标区域 ID */
    uint64_t    timestamp_us;   /* 发送时间戳（微秒） */
    uint32_t    hop_count;      /* 跳数 */
    uint32_t    ttl;            /* 生存时间 */
} CeNetCrossRegionHeader;

/** 跨区消息 */
typedef struct CeNetCrossRegionMessage {
    CeNetCrossRegionHeader  header;         /* 跨区头 */
    uint16_t                inner_type;     /* 内部消息类型 */
    uint32_t                inner_len;      /* 内部消息长度 */
    const uint8_t*          inner_data;     /* 内部消息数据 */
} CeNetCrossRegionMessage;

/**
 * 打包跨区消息
 *
 * 格式：[4B total_len][2B type=0x1000][20B cross_header][2B inner_type][4B inner_len][N inner_data]
 *
 * @param buf       输出缓冲区
 * @param buf_size  缓冲区大小
 * @param msg       跨区消息
 * @return          打包后总长度，0 表示缓冲区不足
 */
uint32_t ce_net_base_pack_cross_region(uint8_t* buf, uint32_t buf_size,
                                       const CeNetCrossRegionMessage* msg);

/**
 * 解包跨区消息
 *
 * @param data      输入数据
 * @param data_len  输入数据长度
 * @param out_msg   输出：跨区消息（指针指向 data 内部）
 * @return          CE_OK 成功，CE_ERR 失败
 */
CeResult ce_net_base_unpack_cross_region(const uint8_t* data, uint32_t data_len,
                                         CeNetCrossRegionMessage* out_msg);

/* ---- Router 网格 ---- */

/**
 * 创建全球 Router 网格管理器
 *
 * @param local_region  本地区域配置
 * @return              网格管理器句柄，失败返回 NULL
 */
CeNetRouterMesh* ce_net_mesh_create(const CeNetRegion* local_region);

/**
 * 销毁 Router 网格
 *
 * @param mesh  网格管理器句柄
 */
void ce_net_mesh_destroy(CeNetRouterMesh* mesh);

/**
 * 添加远程区域到网格
 *
 * @param mesh    网格管理器句柄
 * @param region  区域配置
 * @return        CE_OK 成功，CE_ERR 失败（已达上限或重复）
 */
CeResult ce_net_mesh_add_region(CeNetRouterMesh* mesh, const CeNetRegion* region);

/**
 * 移除远程区域
 *
 * @param mesh       网格管理器句柄
 * @param region_id  区域 ID
 * @return           CE_OK 成功，CE_ERR 未找到
 */
CeResult ce_net_mesh_remove_region(CeNetRouterMesh* mesh, uint32_t region_id);

/**
 * 获取区域信息
 *
 * @param mesh       网格管理器句柄
 * @param region_id  区域 ID
 * @return           区域信息，未找到返回 NULL
 */
const CeNetRegion* ce_net_mesh_get_region(const CeNetRouterMesh* mesh, uint32_t region_id);

/**
 * 获取所有区域
 *
 * @param mesh       网格管理器句柄
 * @param out_count  输出：区域数量
 * @return           区域数组（内部数据，下次调用前有效）
 */
const CeNetRegion* ce_net_mesh_get_all_regions(const CeNetRouterMesh* mesh, int* out_count);

/**
 * 连接到网格中所有远程区域
 *
 * @param mesh  网格管理器句柄
 * @return      CE_OK 全部连接成功，CE_ERR 部分或全部失败
 */
CeResult ce_net_mesh_connect_all(CeNetRouterMesh* mesh);

/**
 * 断开网格中所有连接
 *
 * @param mesh  网格管理器句柄
 */
void ce_net_mesh_disconnect_all(CeNetRouterMesh* mesh);

/**
 * 获取网格中指定区域的连接
 *
 * @param mesh       网格管理器句柄
 * @param region_id  区域 ID
 * @return           连接句柄，未找到返回 NULL
 */
CeNetConnection* ce_net_mesh_get_connection(const CeNetRouterMesh* mesh, uint32_t region_id);

/**
 * 向网格中指定区域发送消息
 *
 * @param mesh       网格管理器句柄
 * @param region_id  目标区域 ID
 * @param msg        待发送的消息
 * @return           CE_OK 成功，CE_ERR 失败
 */
CeResult ce_net_mesh_send(CeNetRouterMesh* mesh, uint32_t region_id,
                          const CeNetMessage* msg);

/**
 * 向网格中所有区域广播消息
 *
 * @param mesh  网格管理器句柄
 * @param msg   待发送的消息
 * @return      成功发送的区域数
 */
int ce_net_mesh_broadcast(CeNetRouterMesh* mesh, const CeNetMessage* msg);

/**
 * 获取网格统计
 *
 * @param mesh        网格管理器句柄
 * @param total       输出：总区域数
 * @param connected   输出：已连接区域数
 */
void ce_net_mesh_stats(const CeNetRouterMesh* mesh, int* total, int* connected);

/* ================================================================
 * 工具函数
 * ================================================================ */

/**
 * 获取当前单调时间（微秒）
 */
uint64_t ce_net_base_now_us(void);

/**
 * 检查消息类型是否为心跳类型
 */
CeBool ce_net_base_is_heartbeat(uint16_t msg_type);

#ifdef __cplusplus
}
#endif

#endif /* CE_NET_BASE_H */
