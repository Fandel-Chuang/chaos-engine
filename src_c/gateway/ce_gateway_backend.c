/*
 * ChaosEngine Gateway - 后端连接池
 *
 * 管理到后端服务器（如 chaos_server:7777）的 TCP 长连接。
 * 固定大小数组，支持健康检查和 PING 心跳。
 */

#define _POSIX_C_SOURCE 200112L

#include "gateway/ce_gateway.h"
#include "network/ce_net_base.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ================================================================
 * 后端连接结构
 * ================================================================ */

typedef enum CeBackendState {
    CE_BACKEND_DISCONNECTED = 0,
    CE_BACKEND_CONNECTING   = 1,
    CE_BACKEND_CONNECTED    = 2,
    CE_BACKEND_UNHEALTHY    = 3,
} CeBackendState;

typedef struct CeGatewayBackend {
    int             fd;                 /* 文件描述符，-1 表示未连接 */
    CeBackendState  state;              /* 健康状态 */
    char            host[256];          /* 后端地址 */
    uint16_t        port;               /* 后端端口 */
    time_t          last_heartbeat;     /* 最后心跳时间 */
    time_t          last_ping_sent;     /* 最后发送 PING 时间 */
    uint64_t        msgs_sent;          /* 已发送消息数 */
    uint64_t        msgs_recv;          /* 已接收消息数 */

    /* 接收缓冲区（后端响应） */
    uint8_t         recv_buf[CE_GATEWAY_RECV_BUF_SIZE];
    int             recv_len;
} CeGatewayBackend;

typedef struct CeGatewayBackendPool {
    CeGatewayBackend   backends[CE_GATEWAY_MAX_BACKENDS];
    int                count;           /* 已配置的后端数 */
    int                heartbeat_sec;   /* 心跳间隔（秒） */
} CeGatewayBackendPool;

/* ================================================================
 * 创建/销毁
 * ================================================================ */

CeGatewayBackendPool* ce_gateway_backend_pool_create(int heartbeat_sec)
{
    CeGatewayBackendPool* pool = (CeGatewayBackendPool*)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->count = 0;
    pool->heartbeat_sec = heartbeat_sec > 0 ? heartbeat_sec : CE_GATEWAY_DEFAULT_HEARTBEAT_SEC;

    /* 初始化所有后端槽位 */
    for (int i = 0; i < CE_GATEWAY_MAX_BACKENDS; i++) {
        pool->backends[i].fd = -1;
        pool->backends[i].state = CE_BACKEND_DISCONNECTED;
        pool->backends[i].recv_len = 0;
    }

    return pool;
}

void ce_gateway_backend_pool_destroy(CeGatewayBackendPool* pool)
{
    if (!pool) return;

    for (int i = 0; i < pool->count; i++) {
        if (pool->backends[i].fd >= 0) {
            close(pool->backends[i].fd);
            pool->backends[i].fd = -1;
        }
    }
    free(pool);
}

/* ================================================================
 * 添加后端
 * ================================================================ */

CeResult ce_gateway_backend_pool_add(CeGatewayBackendPool* pool,
                                    const char* host, uint16_t port)
{
    if (!pool || !host) return CE_ERR;
    if (pool->count >= CE_GATEWAY_MAX_BACKENDS) return CE_ERR;

    CeGatewayBackend* b = &pool->backends[pool->count];
    memset(b, 0, sizeof(*b));
    strncpy(b->host, host, sizeof(b->host) - 1);
    b->port = port;
    b->fd = -1;
    b->state = CE_BACKEND_DISCONNECTED;
    b->recv_len = 0;

    pool->count++;
    return CE_OK;
}

/* ================================================================
 * 连接后端（非阻塞 connect）
 * ================================================================ */

/**
 * 发起到后端的非阻塞连接。
 *
 * @param pool     后端池
 * @param index    后端索引
 * @return         CE_OK 连接已建立，CE_ERR 连接中或失败
 *                 （连接中时 fd 已设置，epoll 会通知 EPOLLOUT）
 */
CeResult ce_gateway_backend_connect(CeGatewayBackendPool* pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return CE_ERR;

    CeGatewayBackend* b = &pool->backends[index];
    if (b->fd >= 0) return CE_OK;  /* 已连接 */
    if (b->state == CE_BACKEND_CONNECTING) return CE_ERR;

    /* 创建 TCP socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return CE_ERR;

    /* 设非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* TCP_NODELAY */
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* 构建地址 */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(b->port);
    if (inet_pton(AF_INET, b->host, &addr.sin_addr) <= 0) {
        close(fd);
        return CE_ERR;
    }

    /* 非阻塞 connect */
    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == 0) {
        /* 立即连接成功 */
        b->fd = fd;
        b->state = CE_BACKEND_CONNECTED;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        b->last_heartbeat = ts.tv_sec;
        b->last_ping_sent = ts.tv_sec;

        return CE_OK;
    }

    if (errno == EINPROGRESS) {
        /* 连接进行中，等待 epoll 通知 EPOLLOUT */
        b->fd = fd;
        b->state = CE_BACKEND_CONNECTING;
        return CE_ERR;
    }

    /* 连接失败 */
    close(fd);
    return CE_ERR;
}

/**
 * 检查非阻塞 connect 是否完成。
 * 在 epoll 通知 EPOLLOUT 时调用。
 */
CeResult ce_gateway_backend_finish_connect(CeGatewayBackendPool* pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return CE_ERR;

    CeGatewayBackend* b = &pool->backends[index];
    if (b->state != CE_BACKEND_CONNECTING) return CE_ERR;

    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(b->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
        /* 连接失败 */
        close(b->fd);
        b->fd = -1;
        b->state = CE_BACKEND_DISCONNECTED;
        return CE_ERR;
    }

    b->state = CE_BACKEND_CONNECTED;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    b->last_heartbeat = ts.tv_sec;
    b->last_ping_sent = ts.tv_sec;

    return CE_OK;
}

/* ================================================================
 * 收发数据
 * ================================================================ */

int ce_gateway_backend_send(CeGatewayBackendPool* pool, int index,
                           const void* data, int len)
{
    if (!pool || index < 0 || index >= pool->count) return -1;

    CeGatewayBackend* b = &pool->backends[index];
    if (b->fd < 0 || b->state != CE_BACKEND_CONNECTED) return -1;

    int n = (int)send(b->fd, data, (size_t)len, MSG_NOSIGNAL);
    if (n > 0) {
        b->msgs_sent++;
    }
    return n;
}

int ce_gateway_backend_recv(CeGatewayBackendPool* pool, int index,
                           void* buf, int size)
{
    if (!pool || index < 0 || index >= pool->count) return -1;

    CeGatewayBackend* b = &pool->backends[index];
    if (b->fd < 0 || b->state != CE_BACKEND_CONNECTED) return -1;

    int n = (int)recv(b->fd, buf, (size_t)size, 0);
    if (n > 0) {
        b->msgs_recv++;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        b->last_heartbeat = ts.tv_sec;
    }
    return n;
}

/* ================================================================
 * 查询/状态
 * ================================================================ */

int ce_gateway_backend_fd(const CeGatewayBackendPool* pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return -1;
    return pool->backends[index].fd;
}

CeBool ce_gateway_backend_healthy(const CeGatewayBackendPool* pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return CE_FALSE;
    return pool->backends[index].state == CE_BACKEND_CONNECTED ? CE_TRUE : CE_FALSE;
}

int ce_gateway_backend_count(const CeGatewayBackendPool* pool)
{
    return pool ? pool->count : 0;
}

/**
 * 获取后端接收缓冲区指针和长度
 */
uint8_t* ce_gateway_backend_recv_buf(CeGatewayBackendPool* pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return NULL;
    return pool->backends[index].recv_buf;
}

int* ce_gateway_backend_recv_len_ptr(CeGatewayBackendPool* pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return NULL;
    return &pool->backends[index].recv_len;
}

/**
 * 标记后端断开
 */
void ce_gateway_backend_mark_disconnected(CeGatewayBackendPool* pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return;
    CeGatewayBackend* b = &pool->backends[index];
    if (b->fd >= 0) {
        close(b->fd);
        b->fd = -1;
    }
    b->state = CE_BACKEND_DISCONNECTED;
    b->recv_len = 0;
}

/**
 * 健康检查：发送 PING 到所有已连接后端
 * 返回发送了 PING 的后端数量
 */
int ce_gateway_backend_health_check(CeGatewayBackendPool* pool)
{
    if (!pool) return 0;

    int sent_count = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    for (int i = 0; i < pool->count; i++) {
        CeGatewayBackend* b = &pool->backends[i];
        if (b->state != CE_BACKEND_CONNECTED) continue;

        /* 检查心跳超时 */
        if (pool->heartbeat_sec > 0 &&
            (ts.tv_sec - b->last_heartbeat) > (time_t)(pool->heartbeat_sec * 3)) {
            b->state = CE_BACKEND_UNHEALTHY;
            continue;
        }

        /* 定时发 PING */
        if ((ts.tv_sec - b->last_ping_sent) >= pool->heartbeat_sec) {
            uint8_t ping_buf[CE_GATEWAY_HEADER_SIZE];
            uint32_t written = ce_net_base_pack(ping_buf, sizeof(ping_buf),
                                                CE_NET_MSG_PING, NULL, 0);
            if (written > 0) {
                int n = (int)send(b->fd, ping_buf, written, MSG_NOSIGNAL);
                if (n > 0) {
                    b->last_ping_sent = ts.tv_sec;
                    sent_count++;
                }
            }
        }
    }

    return sent_count;
}

/**
 * 尝试重连所有断开的后端
 */
int ce_gateway_backend_try_reconnect(CeGatewayBackendPool* pool)
{
    if (!pool) return 0;

    int reconnected = 0;
    for (int i = 0; i < pool->count; i++) {
        CeGatewayBackend* b = &pool->backends[i];
        if (b->state == CE_BACKEND_DISCONNECTED || b->state == CE_BACKEND_UNHEALTHY) {
            if (b->fd >= 0) {
                close(b->fd);
                b->fd = -1;
            }
            if (ce_gateway_backend_connect(pool, i) == CE_OK) {
                reconnected++;
            }
        }
    }
    return reconnected;
}
