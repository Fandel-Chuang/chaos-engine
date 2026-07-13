/*
 * ChaosEngine Gateway - io_uring 事件驱动网络网关
 *
 * 基于 ce_async_io 抽象层 (io_uring) 实现：
 *   - TCP 客户端接入 (默认端口 9000)
 *   - 二进制协议帧解析 [4B total_len][2B msg_type][N payload]
 *   - 消息路由到后端 Game 服务
 *   - 心跳检测 (PING/PONG) 与超时清理
 *   - 后端连接管理 (异步 recv 后端响应)
 *
 * 纯 C99，不使用 epoll，完全通过 ce_async_* 系列 API 驱动。
 */

#ifndef CE_GATEWAY_H
#define CE_GATEWAY_H

#include "public_api/ce_types.h"
#include "network/ce_async_io.h"
#include "network/ce_kcp.h"

#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 常量
 * ================================================================ */

/** 协议帧头大小：4B total_len + 2B msg_type */
#define CE_GW_HEADER_SIZE       6

/** 每连接接收缓冲区大小 (64 KiB) */
#define CE_GW_RECV_BUF_SIZE     (64 * 1024)

/** 每连接发送缓冲区大小 (64 KiB) */
#define CE_GW_SEND_BUF_SIZE     (64 * 1024)

/** 默认监听端口 */
#define CE_GW_DEFAULT_PORT      9000

/** 默认最大连接数 */
#define CE_GW_DEFAULT_MAX_CONNS 10000

/** io_uring 队列深度 */
#define CE_GW_QUEUE_DEPTH       16384

/** 心跳间隔 (毫秒) */
#define CE_GW_HEARTBEAT_INTERVAL_MS  30000

/** 心跳超时 (毫秒) */
#define CE_GW_HEARTBEAT_TIMEOUT_MS   90000

/** 事件循环等待超时 (毫秒) */
#define CE_GW_LOOP_TIMEOUT_MS        1000

/** 最大后端服务数 */
#define CE_GW_MAX_BACKENDS     16

/** 后端地址字符串长度 */
#define CE_GW_BACKEND_ADDR_LEN 256

/* ================================================================
 * 消息类型 (与 ce_net_base.h CeNetMsgType 对齐)
 * ================================================================ */

typedef enum CeGatewayMsgType {
    CE_GW_MSG_PING       = 0x0001,
    CE_GW_MSG_PONG       = 0x0002,
    CE_GW_MSG_LOGIN      = 0x0010,
    CE_GW_MSG_LOGIN_RESP = 0x0011,
    CE_GW_MSG_GAME_DATA  = 0x0100,
    CE_GW_MSG_DISCONNECT = 0xFFFF,
} CeGatewayMsgType;

/** KCP 接收缓冲区大小 */
#define CE_GW_KCP_RECV_BUF_SIZE  (1500)

/* ================================================================
 * 连接状态
 * ================================================================ */

typedef enum CeGatewayConnState {
    CE_GW_CONN_FREE      = 0,   /* 空闲槽位 */
    CE_GW_CONN_ACTIVE    = 1,   /* 活跃连接 */
    CE_GW_CONN_CLOSING   = 2,   /* 正在关闭 */
} CeGatewayConnState;

/** 协议类型：0=TCP, 1=KCP, 2=WebSocket */
#define CE_GW_PROTO_TCP  0
#define CE_GW_PROTO_KCP  1
#define CE_GW_PROTO_WS   2  /* WebSocket (复用 TCP socket) */

/* ---- WebSocket 相关常量 ---- */

/** WebSocket 握手缓冲区大小 */
#define CE_GW_WS_HANDSHAKE_BUF_SIZE  4096

/** WebSocket 握手 magic GUID (RFC 6455) */
#define CE_GW_WS_MAGIC  "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ================================================================
 * 客户端连接上下文
 * ================================================================ */

typedef struct CeGatewayConn {
    int                  fd;            /* 客户端 socket fd (KCP 连接复用 kcp_fd) */
    uint64_t             conn_id;       /* 唯一连接 ID */
    uint64_t             connect_time_us; /* 连接建立时间 */
    uint64_t             last_active_us;   /* 最后活跃时间 */
    CeGatewayConnState   state;         /* 连接状态 */
    uint8_t*             recv_buf;      /* 接收缓冲区 (CE_GW_RECV_BUF_SIZE) */
    int                  recv_offset;   /* 接收缓冲区当前偏移 */
    uint8_t*             send_buf;      /* 发送缓冲区 (CE_GW_SEND_BUF_SIZE) */
    int                  send_len;      /* 待发送数据长度 */
    CeBool               recv_pending;  /* 是否已有 recv 在飞行 */
    /* ---- KCP 专属字段 ---- */
    int                  protocol;      /* 0=TCP, 1=KCP, 2=WebSocket */
    struct sockaddr_in   peer_addr;     /* KCP: 对端地址 (用于 sendto) */
    CeKcpContext*        kcp_ctx;       /* KCP: 协议栈上下文 (TCP 连接为 NULL) */
    uint32_t             kcp_conv;      /* KCP: 会话 ID (conv) */
    char                 addr[64];      /* KCP: 对端地址字符串 "IP:Port" */
    /* ---- WebSocket 专属字段 ---- */
    int                  ws_state;      /* WS 状态：0=等待握手, 1=已升级, -1=不是WS */
    int                  slot;          /* 在 conns 数组中的索引，-1 表示未注册 */
} CeGatewayConn;

/* ================================================================
 * 后端服务
 * ================================================================ */

typedef struct CeGatewayBackend {
    char     host[CE_GW_BACKEND_ADDR_LEN]; /* 后端主机 */
    int      port;                         /* 后端端口 */
    int      fd;                           /* 后端连接 fd (-1 = 未连接) */
    uint8_t* recv_buf;                     /* 后端响应接收缓冲区 */
    int      recv_offset;                  /* 接收偏移 */
    CeBool   connected;                    /* 是否已连接 */
    CeBool   recv_pending;                 /* 是否已有 recv 在飞行 */
    int      fail_count;                   /* 连续失败次数 */
} CeGatewayBackend;

/* ================================================================
 * Gateway 主结构
 * ================================================================ */

typedef struct CeGateway {
    CeAsyncContext*      io;               /* io_uring 异步 I/O 上下文 */
    int                  listen_fd;        /* TCP 监听 socket fd */
    int                  port;             /* 监听端口 (TCP+KCP 共用) */
    int                  kcp_fd;           /* KCP/UDP socket fd (-1 = 未启用) */
    int                  kcp_enabled;      /* 是否启用 KCP */
    uint8_t*             kcp_recv_buf;     /* KCP UDP recv 缓冲区 */
    CeBool               kcp_recv_pending; /* 是否已有 KCP recv 在飞行 */

    CeGatewayConn**      conns;            /* 连接指针数组 (TCP+KCP 共用) */
    int                  conn_count;       /* 当前活跃连接数 */
    int                  max_conns;        /* 最大连接数 */
    int                  conn_capacity;    /* 数组容量 */
    uint64_t             next_conn_id;     /* 下一个连接 ID */

    CeGatewayBackend     backends[CE_GW_MAX_BACKENDS]; /* 后端列表 */
    int                  backend_count;    /* 后端数量 */

    int                  heartbeat_interval_ms; /* 心跳间隔 */
    int                  heartbeat_timeout_ms;  /* 心跳超时 */
    uint64_t             last_heartbeat_check_us; /* 上次心跳检查时间 */
    uint64_t             last_backend_reconnect_us; /* 上次后端重连时间 */

    CeBool               running;          /* 事件循环运行标志 */
    CeBool               ws_enabled;       /* 是否启用 WebSocket */

    /* 动态超时参数 */
    int                  wait_timeout_ms;  /* 当前 wait 超时 (动态调整) */
    CeBool               last_wait_timed_out; /* 上次 wait 是否超时 */
} CeGateway;

/* ================================================================
 * 配置
 * ================================================================ */

typedef struct CeGatewayConfig {
    int          port;                     /* 监听端口 */
    int          max_connections;          /* 最大连接数 */
    int          heartbeat_interval_ms;    /* 心跳间隔 */
    int          heartbeat_timeout_ms;     /* 心跳超时 */
    int          kcp_enabled;              /* 是否启用 KCP (1=启用, 0=禁用) */
    int          ws_enabled;               /* 是否启用 WebSocket (1=启用, 0=禁用) */
} CeGatewayConfig;

/* ================================================================
 * 生命周期
 * ================================================================ */

/** 创建 Gateway 实例 */
CeGateway* ce_gateway_create(const CeGatewayConfig* config);

/** 销毁 Gateway 实例 */
void ce_gateway_destroy(CeGateway* gw);

/** 添加后端服务 */
CeResult ce_gateway_add_backend(CeGateway* gw, const char* host, int port);

/** 运行事件循环 (阻塞直到停止) */
CeResult ce_gateway_run(CeGateway* gw);

/** 停止事件循环 */
void ce_gateway_stop(CeGateway* gw);

/* ================================================================
 * 协议编解码 (内部辅助，但导出供测试)
 * ================================================================ */

/** 大端序写入 32 位整数 */
void ce_gateway_write_u32(uint8_t* buf, uint32_t val);

/** 大端序写入 16 位整数 */
void ce_gateway_write_u16(uint8_t* buf, uint16_t val);

/** 大端序读取 32 位整数 */
uint32_t ce_gateway_read_u32(const uint8_t* buf);

/** 大端序读取 16 位整数 */
uint16_t ce_gateway_read_u16(const uint8_t* buf);

#ifdef __cplusplus
}
#endif

#endif /* CE_GATEWAY_H */
