/*
 * ChaosEngine 客户端网络模块
 *
 * 提供连接到 Gateway、加入游戏、收发实体位置更新的 API。
 *
 * 二进制协议格式（与 Gateway protocol.lua 兼容）：
 *   [2B msg_type][4B body_len][N body]
 *   所有整数均为大端序。
 *
 * 消息类型：
 *   0x8001 — MSG_JOIN          (client→server) body: entity_id (4B)
 *   0x8002 — MSG_POSITION      (client→server) body: entity_id(4B) + x(4B) + y(4B) + z(4B)
 *   0x8003 — MSG_ENTITY_UPDATE (server→client) body: count(2B) + [entity_id(4B)+x(4B)+y(4B)+z(4B)]*count
 *
 * 也支持简单的文本协议（与 Gateway server.lua TCP handler 兼容）：
 *   "JOIN:<entity_id>\n"
 *   "POS:<entity_id>:<x>:<y>:<z>\n"
 *   "UPDATE:<entity_id>:<x>:<y>:<z>\n"
 *
 * 客户端默认使用文本协议，可通过 ce_client_net_set_binary_mode() 切换。
 */

#ifndef CE_CLIENT_NETWORK_H
#define CE_CLIENT_NETWORK_H

#include "public_api/ce_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 常量
 * ================================================================ */

/** 最大实体数量 */
#define CE_CLIENT_MAX_ENTITIES 256

/** 接收缓冲区大小 */
#define CE_CLIENT_RECV_BUF_SIZE (64 * 1024)

/** 默认 Gateway 主机 */
#define CE_CLIENT_DEFAULT_HOST "127.0.0.1"

/** 默认 Gateway TCP 端口 */
#define CE_CLIENT_DEFAULT_PORT 9000

/** 位置发送间隔（帧数） */
#define CE_CLIENT_POSITION_INTERVAL 10

/* ================================================================
 * 二进制协议消息类型
 * ================================================================ */

#define CE_MSG_JOIN          0x8001
#define CE_MSG_POSITION      0x8002
#define CE_MSG_ENTITY_UPDATE 0x8003

/* ================================================================
 * 数据结构
 * ================================================================ */

/** 实体位置信息 */
typedef struct CeClientEntity {
    uint32_t entity_id;
    float    x, y, z;
} CeClientEntity;

/** 客户端网络上下文 */
typedef struct CeClientNet {
    int    sock_fd;
    int    connected;          /* 0=未连接, 1=已连接 */
    int    use_binary;         /* 0=文本协议, 1=二进制协议 */
    uint32_t entity_id;        /* 本客户端实体 ID */

    /* 接收缓冲区 */
    uint8_t recv_buf[CE_CLIENT_RECV_BUF_SIZE];
    int     recv_offset;

    /* 可见实体列表 */
    CeClientEntity entities[CE_CLIENT_MAX_ENTITIES];
    int            entity_count;

    /* 统计 */
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t msgs_sent;
    uint64_t msgs_recv;
} CeClientNet;

/* ================================================================
 * API 函数
 * ================================================================ */

/**
 * 连接到 Gateway
 *
 * @param host  Gateway 主机名或 IP
 * @param port  Gateway TCP 端口
 * @return      CeClientNet* 成功，NULL 失败（Gateway 未运行等）
 */
CeClientNet* ce_client_net_connect(const char* host, int port);

/**
 * 断开连接并释放资源
 *
 * @param ctx  客户端网络上下文
 */
void ce_client_net_disconnect(CeClientNet* ctx);

/**
 * 设置二进制协议模式
 *
 * @param ctx        客户端网络上下文
 * @param use_binary 1=二进制协议, 0=文本协议
 */
void ce_client_net_set_binary_mode(CeClientNet* ctx, int use_binary);

/**
 * 发送加入游戏请求
 *
 * @param ctx       客户端网络上下文
 * @param entity_id 实体 ID
 * @return          CE_OK 成功，CE_ERR 失败
 */
CeResult ce_client_net_send_join(CeClientNet* ctx, uint32_t entity_id);

/**
 * 发送位置更新
 *
 * @param ctx  客户端网络上下文
 * @param x, y, z  位置坐标
 * @return    CE_OK 成功，CE_ERR 失败
 */
CeResult ce_client_net_send_position(CeClientNet* ctx, float x, float y, float z);

/**
 * 轮询网络消息（非阻塞）
 *
 * 从 socket 读取数据，解析消息，更新实体列表。
 * 应在主循环中每帧调用。
 *
 * @param ctx  客户端网络上下文
 * @return     收到的消息数量，0 表示无消息，-1 表示错误
 */
int ce_client_net_poll(CeClientNet* ctx);

/**
 * 获取可见实体数量
 */
int ce_client_net_entity_count(const CeClientNet* ctx);

/**
 * 获取指定索引的实体
 *
 * @param ctx   客户端网络上下文
 * @param index 实体索引 (0-based)
 * @return      CeClientEntity* 或 NULL
 */
const CeClientEntity* ce_client_net_get_entity(const CeClientNet* ctx, int index);

/**
 * 检查是否已连接
 *
 * @return 1 已连接，0 未连接
 */
int ce_client_net_is_connected(const CeClientNet* ctx);

/**
 * 获取连接状态字符串（用于日志）
 */
const char* ce_client_net_status(const CeClientNet* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CE_CLIENT_NETWORK_H */
