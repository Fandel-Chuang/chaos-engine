/*
 * ChaosEngine Gateway - 连接管理
 *
 * 用 fd 做 index 的数组，O(1) 查找。
 * 支持半包/粘包：固定 64KB 接收缓冲区，记录已读偏移和已解析偏移。
 */

#define _POSIX_C_SOURCE 200112L

#include "gateway/ce_gateway.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ================================================================
 * 连接结构体
 * ================================================================ */

typedef enum CeGatewayConnType {
    CE_GW_CONN_FREE      = 0,
    CE_GW_CONN_CLIENT    = 1,
    CE_GW_CONN_BACKEND   = 2,
} CeGatewayConnType;

typedef struct CeGatewayConn {
    int             fd;             /* 文件描述符 */
    uint32_t        conn_id;        /* 连接 ID（自增） */
    CeGatewayConnType type;         /* 连接类型 */
    CeBool          active;         /* 是否活跃 */

    /* 客户端地址信息 */
    char            remote_host[64];
    uint16_t        remote_port;

    /* 时间戳（秒，单调时钟） */
    time_t          connect_time;
    time_t          last_active;

    /* 接收缓冲区（支持半包粘包） */
    uint8_t         recv_buf[CE_GATEWAY_RECV_BUF_SIZE];
    int             recv_len;       /* 已读入缓冲区的字节数 */

    /* 关联：客户端连接对应的后端 index，后端连接对应的客户端 fd */
    int             peer_fd;        /* 对端 fd（客户端->后端 或 后端->客户端） */
    int             backend_index;  /* 对应后端索引（-1 表示未绑定） */
} CeGatewayConn;

/* ================================================================
 * 连接表
 * ================================================================ */

typedef struct CeGatewayConnTable {
    CeGatewayConn*  slots;          /* fd 索引数组 [CE_GATEWAY_MAX_FD] */
    uint32_t        next_conn_id;   /* 下一个连接 ID */
    int             active_count;   /* 活跃连接数 */
} CeGatewayConnTable;

/* ================================================================
 * 创建/销毁连接表
 * ================================================================ */

CeGatewayConnTable* ce_gateway_conn_table_create(void)
{
    CeGatewayConnTable* table = (CeGatewayConnTable*)calloc(1, sizeof(*table));
    if (!table) return NULL;

    /* 用 fd 做 index，分配 65536 个槽位 */
    table->slots = (CeGatewayConn*)calloc(CE_GATEWAY_MAX_FD, sizeof(CeGatewayConn));
    if (!table->slots) {
        free(table);
        return NULL;
    }

    table->next_conn_id = 1;
    table->active_count = 0;
    return table;
}

void ce_gateway_conn_table_destroy(CeGatewayConnTable* table)
{
    if (!table) return;

    /* 关闭所有活跃连接 */
    if (table->slots) {
        for (int i = 0; i < CE_GATEWAY_MAX_FD; i++) {
            if (table->slots[i].active) {
                if (table->slots[i].fd >= 0) {
                    close(table->slots[i].fd);
                }
            }
        }
        free(table->slots);
    }
    free(table);
}

/* ================================================================
 * 连接操作
 * ================================================================ */

/**
 * 创建新连接（注册到 fd 索引槽位）
 */
CeGatewayConn* ce_gateway_conn_create(CeGatewayConnTable* table, int fd,
                                      CeGatewayConnType type,
                                      const struct sockaddr_in* addr)
{
    if (!table || fd < 0 || fd >= CE_GATEWAY_MAX_FD) return NULL;
    if (table->slots[fd].active) return NULL;  /* fd 已被占用 */

    CeGatewayConn* conn = &table->slots[fd];
    memset(conn, 0, sizeof(*conn));

    conn->fd       = fd;
    conn->conn_id  = table->next_conn_id++;
    conn->type     = type;
    conn->active   = CE_TRUE;
    conn->peer_fd  = -1;
    conn->backend_index = -1;
    conn->recv_len = 0;

    /* 记录地址 */
    if (addr) {
        inet_ntop(AF_INET, &addr->sin_addr, conn->remote_host, sizeof(conn->remote_host));
        conn->remote_port = ntohs(addr->sin_port);
    } else {
        strncpy(conn->remote_host, "unknown", sizeof(conn->remote_host) - 1);
        conn->remote_port = 0;
    }

    /* 记录时间 */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    conn->connect_time = ts.tv_sec;
    conn->last_active  = ts.tv_sec;

    table->active_count++;
    return conn;
}

/**
 * 销毁连接（从表中移除，关闭 fd）
 */
void ce_gateway_conn_destroy(CeGatewayConnTable* table, int fd)
{
    if (!table || fd < 0 || fd >= CE_GATEWAY_MAX_FD) return;
    if (!table->slots[fd].active) return;

    CeGatewayConn* conn = &table->slots[fd];

    if (conn->fd >= 0) {
        close(conn->fd);
    }

    conn->active = CE_FALSE;
    conn->fd = -1;
    conn->recv_len = 0;

    table->active_count--;
}

/**
 * 通过 fd 查找连接（O(1)）
 */
CeGatewayConn* ce_gateway_conn_find(CeGatewayConnTable* table, int fd)
{
    if (!table || fd < 0 || fd >= CE_GATEWAY_MAX_FD) return NULL;
    if (!table->slots[fd].active) return NULL;
    return &table->slots[fd];
}

/**
 * 更新最后活跃时间
 */
void ce_gateway_conn_touch(CeGatewayConnTable* table, int fd)
{
    if (!table || fd < 0 || fd >= CE_GATEWAY_MAX_FD) return;
    if (!table->slots[fd].active) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    table->slots[fd].last_active = ts.tv_sec;
}

/**
 * 获取活跃连接数
 */
int ce_gateway_conn_active_count(const CeGatewayConnTable* table)
{
    return table ? table->active_count : 0;
}

/* ================================================================
 * 接收缓冲区操作（半包/粘包处理）
 * ================================================================ */

/**
 * 获取连接的接收缓冲区指针和当前长度
 */
uint8_t* ce_gateway_conn_recv_buf(CeGatewayConn* conn)
{
    return conn ? conn->recv_buf : NULL;
}

int ce_gateway_conn_recv_len(const CeGatewayConn* conn)
{
    return conn ? conn->recv_len : 0;
}

/**
 * 获取接收缓冲区剩余可用空间
 */
int ce_gateway_conn_recv_space(const CeGatewayConn* conn)
{
    if (!conn) return 0;
    return CE_GATEWAY_RECV_BUF_SIZE - conn->recv_len;
}

/**
 * 追加数据到接收缓冲区
 */
void ce_gateway_conn_recv_append(CeGatewayConn* conn, const uint8_t* data, int len)
{
    if (!conn || !data || len <= 0) return;
    int space = CE_GATEWAY_RECV_BUF_SIZE - conn->recv_len;
    if (len > space) len = space;  /* 防止溢出 */
    memcpy(conn->recv_buf + conn->recv_len, data, (size_t)len);
    conn->recv_len += len;
}

/**
 * 消费缓冲区中已处理的数据，将剩余数据前移
 */
void ce_gateway_conn_recv_consume(CeGatewayConn* conn, int consumed)
{
    if (!conn || consumed <= 0 || consumed > conn->recv_len) return;
    int remaining = conn->recv_len - consumed;
    if (remaining > 0) {
        memmove(conn->recv_buf, conn->recv_buf + consumed, (size_t)remaining);
    }
    conn->recv_len = remaining;
}

/**
 * 重置接收缓冲区
 */
void ce_gateway_conn_recv_reset(CeGatewayConn* conn)
{
    if (!conn) return;
    conn->recv_len = 0;
}
