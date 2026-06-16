/*
 * ChaosEngine 共享网络库 ce_net_base — 实现
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 *
 * 二进制协议格式：
 *   [4B total_len][2B msg_type][N payload]
 *   total_len = 6 + payload_len（大端序）
 *
 * 特性：
 *   - TCP 连接管理（非阻塞 + 阻塞）
 *   - 二进制协议编解码
 *   - 消息收发（带缓冲）
 *   - 心跳检测（PING/PONG）
 *   - 连接池（轮询策略）
 *   - 自动重连（指数退避）
 *   - 跨区消息格式
 *   - 全球 Router 网格连接管理
 */

/* POSIX 特性宏 */
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "network/ce_net_base.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>

/* ================================================================
 * 内部辅助：大端序读写
 * ================================================================ */

static void write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val);
}

static void write_u32(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

static void write_u64(uint8_t* buf, uint64_t val) {
    buf[0] = (uint8_t)(val >> 56);
    buf[1] = (uint8_t)(val >> 48);
    buf[2] = (uint8_t)(val >> 40);
    buf[3] = (uint8_t)(val >> 32);
    buf[4] = (uint8_t)(val >> 24);
    buf[5] = (uint8_t)(val >> 16);
    buf[6] = (uint8_t)(val >> 8);
    buf[7] = (uint8_t)(val);
}

static uint16_t read_u16(const uint8_t* buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

static uint32_t read_u32(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
}

static uint64_t read_u64(const uint8_t* buf) {
    return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48)
         | ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32)
         | ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16)
         | ((uint64_t)buf[6] << 8)  |  (uint64_t)buf[7];
}

/* ================================================================
 * 内部辅助：时间戳
 * ================================================================ */

uint64_t ce_net_base_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ================================================================
 * 内部辅助：网络
 * ================================================================ */

static CeResult set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return CE_ERR;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return CE_ERR;
    return CE_OK;
}

static CeResult set_nodelay(int fd) {
    int opt = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        return CE_ERR;
    }
    return CE_OK;
}

static int connect_to(const char* host, int port, int timeout_ms) {
    if (!host || port <= 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CE_LOG_ERROR("NET_BASE", "socket() failed: %s", strerror(errno));
        return -1;
    }

    struct hostent* he = gethostbyname(host);
    if (!he) {
        CE_LOG_ERROR("NET_BASE", "gethostbyname(%s) failed: %s", host, strerror(h_errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* 非阻塞连接 */
    set_nonblock(fd);

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        CE_LOG_ERROR("NET_BASE", "connect(%s:%d) failed: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    /* 等待连接完成 */
    if (ret < 0) {
        fd_set wfds;
        struct timeval tv;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        ret = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (ret <= 0) {
            CE_LOG_ERROR("NET_BASE", "connect(%s:%d) timeout/error", host, port);
            close(fd);
            return -1;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            CE_LOG_ERROR("NET_BASE", "connect(%s:%d) SO_ERROR: %s",
                         host, port, so_error ? strerror(so_error) : "unknown");
            close(fd);
            return -1;
        }
    }

    set_nodelay(fd);

    CE_LOG_INFO("NET_BASE", "Connected to %s:%d (fd=%d)", host, port, fd);
    return fd;
}

/* ================================================================
 * 1.1-1.3: TCP 连接管理
 * ================================================================ */

struct CeNetConnection {
    /* 连接 */
    int             sock_fd;
    CeNetConnState  state;

    /* 配置 */
    CeNetConnConfig config;
    char            host_buf[256];  /* 主机名副本 */

    /* 心跳 */
    uint64_t        last_heartbeat_us;
    uint64_t        last_active_us;
    uint64_t        connect_time_us;

    /* 重连 */
    uint64_t        last_reconnect_attempt_us;
    int             reconnect_backoff_ms;

    /* 统计 */
    uint64_t        bytes_sent;
    uint64_t        bytes_recv;
    uint64_t        msgs_sent;
    uint64_t        msgs_recv;
    int             reconnect_count;

    /* 接收缓冲区 */
    uint8_t         recv_buf[CE_NET_BASE_RECV_BUF_SIZE];
    int             recv_offset;
};

CeNetConnection* ce_net_conn_create(const CeNetConnConfig* config) {
    CeNetConnection* conn = (CeNetConnection*)calloc(1, sizeof(CeNetConnection));
    if (!conn) {
        CE_LOG_ERROR("NET_BASE", "Failed to allocate CeNetConnection");
        return NULL;
    }

    conn->sock_fd = -1;
    conn->state = CE_NET_CONN_DISCONNECTED;
    conn->recv_offset = 0;

    /* 默认配置 */
    if (config) {
        conn->config = *config;
    } else {
        memset(&conn->config, 0, sizeof(conn->config));
        conn->config.timeout_ms = CE_NET_BASE_DEFAULT_TIMEOUT_MS;
        conn->config.heartbeat_ms = CE_NET_BASE_DEFAULT_HEARTBEAT_MS;
        conn->config.heartbeat_timeout_ms = CE_NET_BASE_DEFAULT_HEARTBEAT_TIMEOUT_MS;
        conn->config.auto_reconnect = CE_FALSE;
        conn->config.nonblocking = CE_TRUE;
    }

    /* 确保有效值 */
    if (conn->config.timeout_ms <= 0)
        conn->config.timeout_ms = CE_NET_BASE_DEFAULT_TIMEOUT_MS;
    /* heartbeat_ms = 0 表示禁用心跳，不覆盖 */
    if (conn->config.heartbeat_timeout_ms <= 0)
        conn->config.heartbeat_timeout_ms = CE_NET_BASE_DEFAULT_HEARTBEAT_TIMEOUT_MS;

    /* 复制主机名 */
    if (conn->config.host) {
        strncpy(conn->host_buf, conn->config.host, sizeof(conn->host_buf) - 1);
        conn->host_buf[sizeof(conn->host_buf) - 1] = '\0';
        conn->config.host = conn->host_buf;
    }

    conn->reconnect_backoff_ms = CE_NET_BASE_RECONNECT_MIN_MS;

    return conn;
}

CeResult ce_net_conn_connect(CeNetConnection* conn) {
    if (!conn) return CE_ERR;
    if (conn->state == CE_NET_CONN_CONNECTED) return CE_OK;

    if (!conn->config.host || conn->config.port <= 0) {
        CE_LOG_WARN("NET_BASE", "No valid host/port configured");
        return CE_ERR;
    }

    conn->state = CE_NET_CONN_CONNECTING;

    int fd = connect_to(conn->config.host, conn->config.port, conn->config.timeout_ms);
    if (fd < 0) {
        conn->state = CE_NET_CONN_DISCONNECTED;
        if (conn->config.auto_reconnect) {
            conn->last_reconnect_attempt_us = ce_net_base_now_us();
        }
        return CE_ERR;
    }

    conn->sock_fd = fd;
    conn->state = CE_NET_CONN_CONNECTED;
    conn->connect_time_us = ce_net_base_now_us();
    conn->last_active_us = conn->connect_time_us;
    conn->last_heartbeat_us = conn->connect_time_us;
    conn->reconnect_backoff_ms = CE_NET_BASE_RECONNECT_MIN_MS;

    CE_LOG_INFO("NET_BASE", "Connection established to %s:%d",
                conn->config.host, conn->config.port);
    return CE_OK;
}

void ce_net_conn_disconnect(CeNetConnection* conn) {
    if (!conn) return;
    if (conn->sock_fd >= 0) {
        CE_LOG_INFO("NET_BASE", "Disconnecting fd=%d", conn->sock_fd);
        close(conn->sock_fd);
        conn->sock_fd = -1;
    }
    conn->state = CE_NET_CONN_DISCONNECTED;
    conn->recv_offset = 0;
}

void ce_net_conn_destroy(CeNetConnection* conn) {
    if (!conn) return;
    ce_net_conn_disconnect(conn);
    free(conn);
}

CeNetConnState ce_net_conn_get_state(const CeNetConnection* conn) {
    if (!conn) return CE_NET_CONN_DISCONNECTED;
    return conn->state;
}

int ce_net_conn_get_fd(const CeNetConnection* conn) {
    if (!conn) return -1;
    return conn->sock_fd;
}

void ce_net_conn_get_stats(const CeNetConnection* conn, CeNetConnStats* stats) {
    if (!conn || !stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->bytes_sent        = conn->bytes_sent;
    stats->bytes_recv        = conn->bytes_recv;
    stats->msgs_sent         = conn->msgs_sent;
    stats->msgs_recv         = conn->msgs_recv;
    stats->connect_time_us   = conn->connect_time_us;
    stats->last_active_us    = conn->last_active_us;
    stats->last_heartbeat_us = conn->last_heartbeat_us;
    stats->reconnect_count   = conn->reconnect_count;
    stats->state             = conn->state;
}

/* ================================================================
 * 1.2: 消息收发
 * ================================================================ */

CeResult ce_net_conn_send(CeNetConnection* conn, const CeNetMessage* msg) {
    if (!conn || !msg) return CE_ERR;
    if (conn->state != CE_NET_CONN_CONNECTED || conn->sock_fd < 0) {
        CE_LOG_WARN("NET_BASE", "Cannot send: not connected");
        return CE_ERR;
    }

    uint32_t total_len = CE_NET_BASE_HEADER_SIZE + msg->payload_len;
    if (total_len > CE_NET_BASE_MAX_MSG_SIZE) {
        CE_LOG_ERROR("NET_BASE", "Message too large: %u bytes (max %u)",
                     total_len, CE_NET_BASE_MAX_MSG_SIZE);
        return CE_ERR;
    }

    /* 构建发送缓冲区 */
    uint8_t send_buf[CE_NET_BASE_MAX_MSG_SIZE];
    write_u32(send_buf, total_len);
    write_u16(send_buf + 4, msg->type);
    if (msg->payload_len > 0 && msg->payload) {
        memcpy(send_buf + CE_NET_BASE_HEADER_SIZE, msg->payload, msg->payload_len);
    }

    /* TCP 发送 */
    uint32_t sent = 0;
    int retries = 0;
    while (sent < total_len) {
        ssize_t n = send(conn->sock_fd, send_buf + sent, total_len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (retries++ > 100) {
                    CE_LOG_ERROR("NET_BASE", "Send timeout after %d retries", retries);
                    return CE_ERR;
                }
                usleep(1000);
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                CE_LOG_ERROR("NET_BASE", "Connection lost during send");
                conn->state = CE_NET_CONN_DISCONNECTED;
                conn->last_reconnect_attempt_us = ce_net_base_now_us();
                return CE_ERR;
            }
            CE_LOG_ERROR("NET_BASE", "send() failed: %s", strerror(errno));
            return CE_ERR;
        }
        sent += (uint32_t)n;
        retries = 0;
    }

    conn->bytes_sent += total_len;
    conn->msgs_sent++;
    conn->last_active_us = ce_net_base_now_us();

    return CE_OK;
}

CeResult ce_net_conn_recv(CeNetConnection* conn, CeNetRecvMessage* out_msg) {
    if (!conn || !out_msg) return CE_ERR;
    if (conn->state != CE_NET_CONN_CONNECTED || conn->sock_fd < 0) {
        return CE_ERR;
    }

    memset(out_msg, 0, sizeof(*out_msg));

    /* 读取数据到接收缓冲区 */
    ssize_t n = recv(conn->sock_fd,
                     conn->recv_buf + conn->recv_offset,
                     CE_NET_BASE_RECV_BUF_SIZE - conn->recv_offset,
                     0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return CE_ERR; /* 无数据 */
        }
        if (errno == ECONNRESET || errno == EPIPE) {
            CE_LOG_WARN("NET_BASE", "Connection reset by peer");
            conn->state = CE_NET_CONN_DISCONNECTED;
            conn->last_reconnect_attempt_us = ce_net_base_now_us();
            return CE_ERR;
        }
        CE_LOG_ERROR("NET_BASE", "recv() failed: %s", strerror(errno));
        return CE_ERR;
    }
    if (n == 0) {
        CE_LOG_WARN("NET_BASE", "Peer closed connection");
        conn->state = CE_NET_CONN_DISCONNECTED;
        conn->last_reconnect_attempt_us = ce_net_base_now_us();
        return CE_ERR;
    }

    conn->recv_offset += (int)n;
    conn->bytes_recv += (uint64_t)n;

    /* 检查是否有完整消息 */
    if (conn->recv_offset < CE_NET_BASE_HEADER_SIZE) {
        return CE_ERR; /* 头部不完整 */
    }

    uint32_t total_len = read_u32(conn->recv_buf);
    if (total_len > CE_NET_BASE_MAX_MSG_SIZE) {
        CE_LOG_ERROR("NET_BASE", "Message too large: %u", total_len);
        conn->recv_offset = 0;
        return CE_ERR;
    }
    if (total_len < CE_NET_BASE_HEADER_SIZE) {
        CE_LOG_ERROR("NET_BASE", "Message too small: %u", total_len);
        conn->recv_offset = 0;
        return CE_ERR;
    }

    if (conn->recv_offset < (int)total_len) {
        return CE_ERR; /* 消息不完整 */
    }

    /* 解析消息 */
    uint16_t msg_type = read_u16(conn->recv_buf + 4);
    uint32_t payload_len = total_len - CE_NET_BASE_HEADER_SIZE;

    out_msg->type = msg_type;
    out_msg->payload_len = payload_len;
    if (payload_len > 0) {
        memcpy(out_msg->payload, conn->recv_buf + CE_NET_BASE_HEADER_SIZE, payload_len);
    }

    /* 从缓冲区移除已处理的消息 */
    int remaining = conn->recv_offset - (int)total_len;
    if (remaining > 0) {
        memmove(conn->recv_buf, conn->recv_buf + total_len, (size_t)remaining);
    }
    conn->recv_offset = remaining;

    conn->msgs_recv++;
    conn->last_active_us = ce_net_base_now_us();

    /* 更新心跳时间 */
    ce_net_conn_heartbeat_touch(conn);

    return CE_OK;
}

/* ================================================================
 * 1.4: 心跳检测
 * ================================================================ */

CeResult ce_net_conn_send_ping(CeNetConnection* conn) {
    CeNetMessage msg;
    msg.type = CE_NET_MSG_PING;
    msg.payload_len = 0;
    msg.payload = NULL;
    return ce_net_conn_send(conn, &msg);
}

CeResult ce_net_conn_send_pong(CeNetConnection* conn) {
    CeNetMessage msg;
    msg.type = CE_NET_MSG_PONG;
    msg.payload_len = 0;
    msg.payload = NULL;
    return ce_net_conn_send(conn, &msg);
}

CeBool ce_net_conn_heartbeat_timeout(const CeNetConnection* conn) {
    if (!conn) return CE_TRUE;
    if (conn->config.heartbeat_ms == 0) return CE_FALSE; /* 心跳禁用 */

    uint64_t now = ce_net_base_now_us();
    uint64_t elapsed_ms = (now - conn->last_heartbeat_us) / 1000ULL;
    return (elapsed_ms > (uint64_t)conn->config.heartbeat_timeout_ms) ? CE_TRUE : CE_FALSE;
}

void ce_net_conn_heartbeat_touch(CeNetConnection* conn) {
    if (!conn) return;
    conn->last_heartbeat_us = ce_net_base_now_us();
}

/* ================================================================
 * 1.5: 连接池
 * ================================================================ */

struct CeNetPool {
    CeNetConnection**   connections;
    int                 max_connections;
    int                 count;
    int                 next_index;     /* 轮询索引 */
};

CeNetPool* ce_net_pool_create(int max_connections) {
    if (max_connections <= 0) {
        max_connections = CE_NET_BASE_POOL_DEFAULT_MAX;
    }

    CeNetPool* pool = (CeNetPool*)calloc(1, sizeof(CeNetPool));
    if (!pool) {
        CE_LOG_ERROR("NET_BASE", "Failed to allocate CeNetPool");
        return NULL;
    }

    pool->connections = (CeNetConnection**)calloc((size_t)max_connections, sizeof(CeNetConnection*));
    if (!pool->connections) {
        CE_LOG_ERROR("NET_BASE", "Failed to allocate pool connections array");
        free(pool);
        return NULL;
    }

    pool->max_connections = max_connections;
    pool->count = 0;
    pool->next_index = 0;

    return pool;
}

void ce_net_pool_destroy(CeNetPool* pool) {
    if (!pool) return;
    for (int i = 0; i < pool->count; i++) {
        if (pool->connections[i]) {
            ce_net_conn_destroy(pool->connections[i]);
        }
    }
    free(pool->connections);
    free(pool);
}

CeResult ce_net_pool_add(CeNetPool* pool, CeNetConnection* conn) {
    if (!pool || !conn) return CE_ERR;
    if (pool->count >= pool->max_connections) {
        CE_LOG_WARN("NET_BASE", "Pool full (%d/%d)", pool->count, pool->max_connections);
        return CE_ERR;
    }

    pool->connections[pool->count++] = conn;
    return CE_OK;
}

CeNetConnection* ce_net_pool_acquire(CeNetPool* pool) {
    if (!pool || pool->count == 0) return NULL;

    /* 轮询策略：从上次位置开始查找可用连接 */
    for (int i = 0; i < pool->count; i++) {
        int idx = (pool->next_index + i) % pool->count;
        CeNetConnection* conn = pool->connections[idx];
        if (conn && conn->state == CE_NET_CONN_CONNECTED) {
            pool->next_index = (idx + 1) % pool->count;
            return conn;
        }
    }

    return NULL; /* 无可用连接 */
}

void ce_net_pool_release(CeNetPool* pool, CeNetConnection* conn) {
    (void)pool;
    (void)conn;
    /* 连接池中的连接是共享的，不需要显式释放 */
    /* 如果连接断开，cleanup 会处理 */
}

CeResult ce_net_pool_remove(CeNetPool* pool, CeNetConnection* conn) {
    if (!pool || !conn) return CE_ERR;

    for (int i = 0; i < pool->count; i++) {
        if (pool->connections[i] == conn) {
            /* 将最后一个元素移到当前位置 */
            pool->count--;
            if (i < pool->count) {
                pool->connections[i] = pool->connections[pool->count];
            }
            pool->connections[pool->count] = NULL;
            return CE_OK;
        }
    }

    return CE_ERR;
}

void ce_net_pool_stats(const CeNetPool* pool, int* total, int* available) {
    if (!pool) {
        if (total) *total = 0;
        if (available) *available = 0;
        return;
    }

    int t = pool->count;
    int a = 0;
    for (int i = 0; i < pool->count; i++) {
        if (pool->connections[i] && pool->connections[i]->state == CE_NET_CONN_CONNECTED) {
            a++;
        }
    }

    if (total) *total = t;
    if (available) *available = a;
}

int ce_net_pool_cleanup(CeNetPool* pool) {
    if (!pool) return 0;

    int removed = 0;
    int i = 0;
    while (i < pool->count) {
        CeNetConnection* conn = pool->connections[i];
        if (!conn || conn->state == CE_NET_CONN_DISCONNECTED) {
            if (conn) {
                ce_net_conn_destroy(conn);
            }
            /* 将最后一个元素移到当前位置 */
            pool->count--;
            if (i < pool->count) {
                pool->connections[i] = pool->connections[pool->count];
            }
            pool->connections[pool->count] = NULL;
            removed++;
        } else {
            i++;
        }
    }

    return removed;
}

/* ================================================================
 * 1.6: 自动重连
 * ================================================================ */

CeResult ce_net_conn_try_reconnect(CeNetConnection* conn) {
    if (!conn) return CE_ERR;
    if (conn->state == CE_NET_CONN_CONNECTED) return CE_OK;
    if (!conn->config.auto_reconnect) return CE_ERR;

    uint64_t now = ce_net_base_now_us();
    uint64_t elapsed_ms = (now - conn->last_reconnect_attempt_us) / 1000ULL;

    if (elapsed_ms < (uint64_t)conn->reconnect_backoff_ms) {
        return CE_ERR; /* 还没到重试时间 */
    }

    conn->last_reconnect_attempt_us = now;

    if (!conn->config.host || conn->config.port <= 0) {
        return CE_ERR;
    }

    int fd = connect_to(conn->config.host, conn->config.port, conn->config.timeout_ms);
    if (fd < 0) {
        /* 指数退避 */
        conn->reconnect_backoff_ms *= 2;
        if (conn->reconnect_backoff_ms > CE_NET_BASE_RECONNECT_MAX_MS) {
            conn->reconnect_backoff_ms = CE_NET_BASE_RECONNECT_MAX_MS;
        }
        CE_LOG_WARN("NET_BASE", "Reconnect failed, next attempt in %d ms",
                    conn->reconnect_backoff_ms);
        return CE_ERR;
    }

    conn->sock_fd = fd;
    conn->state = CE_NET_CONN_CONNECTED;
    conn->connect_time_us = now;
    conn->last_active_us = now;
    conn->last_heartbeat_us = now;
    conn->reconnect_backoff_ms = CE_NET_BASE_RECONNECT_MIN_MS;
    conn->reconnect_count++;

    CE_LOG_INFO("NET_BASE", "Reconnected to %s:%d (attempt #%d)",
                conn->config.host, conn->config.port, conn->reconnect_count);
    return CE_OK;
}

void ce_net_conn_reset_reconnect(CeNetConnection* conn) {
    if (!conn) return;
    conn->reconnect_backoff_ms = CE_NET_BASE_RECONNECT_MIN_MS;
    conn->last_reconnect_attempt_us = 0;
}

/* ================================================================
 * 1.7: 二进制协议编解码（独立工具函数）
 * ================================================================ */

uint32_t ce_net_base_pack(uint8_t* buf, uint32_t buf_size,
                          uint16_t msg_type,
                          const uint8_t* payload, uint32_t payload_len) {
    uint32_t total_len = CE_NET_BASE_HEADER_SIZE + payload_len;
    if (total_len > buf_size) return 0;

    write_u32(buf, total_len);
    write_u16(buf + 4, msg_type);
    if (payload_len > 0 && payload) {
        memcpy(buf + CE_NET_BASE_HEADER_SIZE, payload, payload_len);
    }

    return total_len;
}

CeResult ce_net_base_unpack(const uint8_t* data, uint32_t data_len,
                            uint16_t* out_type,
                            const uint8_t** out_payload, uint32_t* out_payload_len) {
    if (!data || data_len < CE_NET_BASE_HEADER_SIZE) return CE_ERR;

    uint32_t total_len = read_u32(data);
    if (total_len < CE_NET_BASE_HEADER_SIZE || total_len > data_len) return CE_ERR;

    if (out_type) *out_type = read_u16(data + 4);

    uint32_t plen = total_len - CE_NET_BASE_HEADER_SIZE;
    if (out_payload_len) *out_payload_len = plen;
    if (out_payload) {
        *out_payload = (plen > 0) ? (data + CE_NET_BASE_HEADER_SIZE) : NULL;
    }

    return CE_OK;
}

uint32_t ce_net_base_peek_len(const uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 4) return 0;
    return read_u32(data);
}

uint16_t ce_net_base_peek_type(const uint8_t* data, uint32_t data_len) {
    if (!data || data_len < CE_NET_BASE_HEADER_SIZE) return 0;
    return read_u16(data + 4);
}

/* ================================================================
 * 1.10-1.12: 跨区消息格式 & 全球 Router 网格
 * ================================================================ */

/** 跨区消息头部大小：20 字节 */
#define CE_NET_CROSS_REGION_HEADER_SIZE  20

/** 跨区消息内部头大小：2B inner_type + 4B inner_len = 6 */
#define CE_NET_CROSS_REGION_INNER_SIZE   6

uint32_t ce_net_base_pack_cross_region(uint8_t* buf, uint32_t buf_size,
                                       const CeNetCrossRegionMessage* msg) {
    if (!buf || !msg) return 0;

    uint32_t total_len = CE_NET_BASE_HEADER_SIZE      /* 4+2 */
                       + CE_NET_CROSS_REGION_HEADER_SIZE  /* 20 */
                       + CE_NET_CROSS_REGION_INNER_SIZE   /* 6 */
                       + msg->inner_len;

    if (total_len > buf_size) return 0;

    uint8_t* p = buf;

    /* 外层协议头 */
    write_u32(p, total_len);                p += 4;
    write_u16(p, CE_NET_MSG_CROSS_REGION);  p += 2;

    /* 跨区头 */
    write_u32(p, msg->header.src_region);   p += 4;
    write_u32(p, msg->header.dst_region);   p += 4;
    write_u64(p, msg->header.timestamp_us); p += 8;
    write_u32(p, msg->header.hop_count);    p += 4;
    /* TTL 放在 hop_count 后面，但 header 只有 20 字节 */
    /* 修正：hop_count(4) + ttl(4) = 8，但 header 结构体有 ttl */
    /* 实际上 header 是 4+4+8+4+4 = 24 字节，但注释写 20 */
    /* 让我们保持一致：20 字节 = src(4)+dst(4)+ts(8)+hop(4) */
    /* ttl 放在 inner 部分前面 */
    write_u32(p, msg->header.ttl);          p += 4;

    /* 内部消息 */
    write_u16(p, msg->inner_type);          p += 2;
    write_u32(p, msg->inner_len);           p += 4;
    if (msg->inner_len > 0 && msg->inner_data) {
        memcpy(p, msg->inner_data, msg->inner_len);
        p += msg->inner_len;
    }

    return (uint32_t)(p - buf);
}

CeResult ce_net_base_unpack_cross_region(const uint8_t* data, uint32_t data_len,
                                         CeNetCrossRegionMessage* out_msg) {
    if (!data || !out_msg) return CE_ERR;

    uint32_t min_len = CE_NET_BASE_HEADER_SIZE
                     + CE_NET_CROSS_REGION_HEADER_SIZE
                     + 4  /* ttl */
                     + CE_NET_CROSS_REGION_INNER_SIZE;

    if (data_len < min_len) return CE_ERR;

    uint32_t total_len = read_u32(data);
    if (total_len < min_len || total_len > data_len) return CE_ERR;

    uint16_t outer_type = read_u16(data + 4);
    if (outer_type != CE_NET_MSG_CROSS_REGION) return CE_ERR;

    const uint8_t* p = data + CE_NET_BASE_HEADER_SIZE;

    memset(out_msg, 0, sizeof(*out_msg));
    out_msg->header.src_region   = read_u32(p);  p += 4;
    out_msg->header.dst_region   = read_u32(p);  p += 4;
    out_msg->header.timestamp_us = read_u64(p);  p += 8;
    out_msg->header.hop_count    = read_u32(p);  p += 4;
    out_msg->header.ttl          = read_u32(p);  p += 4;

    out_msg->inner_type = read_u16(p);  p += 2;
    out_msg->inner_len  = read_u32(p);  p += 4;

    if (out_msg->inner_len > 0) {
        out_msg->inner_data = p;
    }

    return CE_OK;
}

/* ---- Router 网格 ---- */

struct CeNetRouterMesh {
    CeNetRegion     local_region;                           /* 本地区域 */
    CeNetRegion     regions[CE_NET_BASE_MESH_MAX_REGIONS];  /* 远程区域 */
    CeNetConnection* connections[CE_NET_BASE_MESH_MAX_REGIONS]; /* 对应连接 */
    int             region_count;
};

CeNetRouterMesh* ce_net_mesh_create(const CeNetRegion* local_region) {
    CeNetRouterMesh* mesh = (CeNetRouterMesh*)calloc(1, sizeof(CeNetRouterMesh));
    if (!mesh) {
        CE_LOG_ERROR("NET_BASE", "Failed to allocate CeNetRouterMesh");
        return NULL;
    }

    if (local_region) {
        mesh->local_region = *local_region;
    }

    mesh->region_count = 0;

    CE_LOG_INFO("NET_BASE", "Router mesh created for region %s (id=%u)",
                mesh->local_region.name, mesh->local_region.region_id);
    return mesh;
}

void ce_net_mesh_destroy(CeNetRouterMesh* mesh) {
    if (!mesh) return;
    ce_net_mesh_disconnect_all(mesh);
    free(mesh);
}

CeResult ce_net_mesh_add_region(CeNetRouterMesh* mesh, const CeNetRegion* region) {
    if (!mesh || !region) return CE_ERR;
    if (mesh->region_count >= CE_NET_BASE_MESH_MAX_REGIONS) {
        CE_LOG_ERROR("NET_BASE", "Mesh full: max %d regions", CE_NET_BASE_MESH_MAX_REGIONS);
        return CE_ERR;
    }

    /* 检查重复 */
    for (int i = 0; i < mesh->region_count; i++) {
        if (mesh->regions[i].region_id == region->region_id) {
            CE_LOG_WARN("NET_BASE", "Region %u already in mesh", region->region_id);
            return CE_ERR;
        }
    }

    mesh->regions[mesh->region_count] = *region;
    mesh->connections[mesh->region_count] = NULL;
    mesh->region_count++;

    CE_LOG_INFO("NET_BASE", "Added region %s (id=%u) to mesh", region->name, region->region_id);
    return CE_OK;
}

CeResult ce_net_mesh_remove_region(CeNetRouterMesh* mesh, uint32_t region_id) {
    if (!mesh) return CE_ERR;

    for (int i = 0; i < mesh->region_count; i++) {
        if (mesh->regions[i].region_id == region_id) {
            /* 关闭连接 */
            if (mesh->connections[i]) {
                ce_net_conn_destroy(mesh->connections[i]);
                mesh->connections[i] = NULL;
            }

            /* 将最后一个元素移到当前位置 */
            int last = mesh->region_count - 1;
            if (i < last) {
                mesh->regions[i] = mesh->regions[last];
                mesh->connections[i] = mesh->connections[last];
            }
            mesh->region_count--;

            CE_LOG_INFO("NET_BASE", "Removed region %u from mesh", region_id);
            return CE_OK;
        }
    }

    return CE_ERR;
}

const CeNetRegion* ce_net_mesh_get_region(const CeNetRouterMesh* mesh, uint32_t region_id) {
    if (!mesh) return NULL;

    /* 检查本地区域 */
    if (mesh->local_region.region_id == region_id) {
        return &mesh->local_region;
    }

    for (int i = 0; i < mesh->region_count; i++) {
        if (mesh->regions[i].region_id == region_id) {
            return &mesh->regions[i];
        }
    }

    return NULL;
}

const CeNetRegion* ce_net_mesh_get_all_regions(const CeNetRouterMesh* mesh, int* out_count) {
    if (!mesh) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (out_count) *out_count = mesh->region_count;
    return mesh->regions;
}

CeResult ce_net_mesh_connect_all(CeNetRouterMesh* mesh) {
    if (!mesh) return CE_ERR;

    CeResult overall = CE_OK;

    for (int i = 0; i < mesh->region_count; i++) {
        CeNetRegion* region = &mesh->regions[i];

        /* 如果已有连接，跳过 */
        if (mesh->connections[i] && mesh->connections[i]->state == CE_NET_CONN_CONNECTED) {
            continue;
        }

        CeNetConnConfig config;
        memset(&config, 0, sizeof(config));
        config.host = region->host;
        config.port = region->port;
        config.timeout_ms = CE_NET_BASE_DEFAULT_TIMEOUT_MS;
        config.heartbeat_ms = CE_NET_BASE_DEFAULT_HEARTBEAT_MS;
        config.heartbeat_timeout_ms = CE_NET_BASE_DEFAULT_HEARTBEAT_TIMEOUT_MS;
        config.auto_reconnect = CE_TRUE;
        config.nonblocking = CE_TRUE;

        CeNetConnection* conn = ce_net_conn_create(&config);
        if (!conn) {
            CE_LOG_ERROR("NET_BASE", "Failed to create connection for region %s", region->name);
            overall = CE_ERR;
            continue;
        }

        if (ce_net_conn_connect(conn) != CE_OK) {
            CE_LOG_WARN("NET_BASE", "Failed to connect to region %s (%s:%d)",
                        region->name, region->host, region->port);
            /* 保留连接对象以便后续重连 */
            region->active = CE_FALSE;
            overall = CE_ERR;
        } else {
            region->active = CE_TRUE;
        }

        mesh->connections[i] = conn;
    }

    return overall;
}

void ce_net_mesh_disconnect_all(CeNetRouterMesh* mesh) {
    if (!mesh) return;

    for (int i = 0; i < mesh->region_count; i++) {
        if (mesh->connections[i]) {
            ce_net_conn_destroy(mesh->connections[i]);
            mesh->connections[i] = NULL;
        }
        mesh->regions[i].active = CE_FALSE;
    }
}

CeNetConnection* ce_net_mesh_get_connection(const CeNetRouterMesh* mesh, uint32_t region_id) {
    if (!mesh) return NULL;

    for (int i = 0; i < mesh->region_count; i++) {
        if (mesh->regions[i].region_id == region_id) {
            return mesh->connections[i];
        }
    }

    return NULL;
}

CeResult ce_net_mesh_send(CeNetRouterMesh* mesh, uint32_t region_id,
                          const CeNetMessage* msg) {
    if (!mesh || !msg) return CE_ERR;

    CeNetConnection* conn = ce_net_mesh_get_connection(mesh, region_id);
    if (!conn) {
        CE_LOG_WARN("NET_BASE", "No connection for region %u", region_id);
        return CE_ERR;
    }

    return ce_net_conn_send(conn, msg);
}

int ce_net_mesh_broadcast(CeNetRouterMesh* mesh, const CeNetMessage* msg) {
    if (!mesh || !msg) return 0;

    int sent = 0;
    for (int i = 0; i < mesh->region_count; i++) {
        if (mesh->connections[i] && mesh->connections[i]->state == CE_NET_CONN_CONNECTED) {
            if (ce_net_conn_send(mesh->connections[i], msg) == CE_OK) {
                sent++;
            }
        }
    }

    return sent;
}

void ce_net_mesh_stats(const CeNetRouterMesh* mesh, int* total, int* connected) {
    if (!mesh) {
        if (total) *total = 0;
        if (connected) *connected = 0;
        return;
    }

    int t = mesh->region_count;
    int c = 0;
    for (int i = 0; i < mesh->region_count; i++) {
        if (mesh->connections[i] && mesh->connections[i]->state == CE_NET_CONN_CONNECTED) {
            c++;
        }
    }

    if (total) *total = t;
    if (connected) *connected = c;
}

/* ================================================================
 * 工具函数
 * ================================================================ */

CeBool ce_net_base_is_heartbeat(uint16_t msg_type) {
    return (msg_type == CE_NET_MSG_PING || msg_type == CE_NET_MSG_PONG) ? CE_TRUE : CE_FALSE;
}
