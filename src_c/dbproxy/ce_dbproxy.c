/*
 * ChaosEngine DBProxy 客户端 — 实现
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 *
 * 二进制协议：
 *   [4B total_len][2B msg_type][N payload]
 *   total_len = 6 + payload_len（大端序）
 *
 * 特性：
 *   - TCP 非阻塞连接
 *   - 自动重连（指数退避：1s, 2s, 4s, 最大 30s）
 *   - 主备切换：主不可用时切到备用
 */

/* POSIX 特性宏：C99 下启用 clock_gettime, usleep, select 等 */
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "dbproxy/ce_dbproxy.h"
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

/* ---- 内部常量 ---- */

/** 帧头大小：4B len + 2B msg_type */
#define CE_DBPROXY_HEADER_SIZE          6

/** 接收缓冲区大小 */
#define CE_DBPROXY_RECV_BUF_SIZE        CE_DBPROXY_MAX_MSG_SIZE

/* ---- 内部结构 ---- */

/** DBProxy 客户端上下文（不透明） */
struct CeDbproxyContext {
    /* 连接 */
    int             sock_fd;          /* 当前 TCP socket */
    CeBool          connected;

    /* 配置 */
    CeDbproxyConfig config;
    char            primary_host_buf[256];  /* 主地址副本 */
    char            backup_host_buf[256];   /* 备地址副本 */
    CeBool          using_backup;           /* 是否正在使用备用 */

    /* 端点描述 */
    char            endpoint_str[280];      /* "host:port" */

    /* 心跳 */
    uint64_t        last_heartbeat_us;

    /* 重连 */
    uint64_t        last_reconnect_attempt_us;
    int             reconnect_backoff_ms;
    CeBool          reconnect_pending;

    /* 接收缓冲区 */
    uint8_t         recv_buf[CE_DBPROXY_RECV_BUF_SIZE];
    int             recv_offset;
};

/* ---- 内部辅助：时间戳 ---- */

static uint64_t ce_dbproxy_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ---- 内部辅助：大端序读写 ---- */

static void ce_dbproxy_write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val);
}

static void ce_dbproxy_write_u32(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

static uint16_t ce_dbproxy_read_u16(const uint8_t* buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

static uint32_t ce_dbproxy_read_u32(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
}

/* ---- 内部辅助：网络 ---- */

/** 设置 socket 为非阻塞 */
static CeResult ce_dbproxy_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return CE_ERR;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return CE_ERR;
    return CE_OK;
}

/** 设置 TCP_NODELAY */
static CeResult ce_dbproxy_set_nodelay(int fd) {
    int opt = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        CE_LOG_WARN("DBPROXY", "Failed to set TCP_NODELAY: %s", strerror(errno));
        return CE_ERR;
    }
    return CE_OK;
}

/** 连接到指定 host:port，返回 socket fd */
static int ce_dbproxy_connect_to(const char* host, int port, int timeout_ms) {
    if (!host || port <= 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CE_LOG_ERROR("DBPROXY", "socket() failed: %s", strerror(errno));
        return -1;
    }

    /* 解析主机名 */
    struct hostent* he = gethostbyname(host);
    if (!he) {
        CE_LOG_ERROR("DBPROXY", "gethostbyname(%s) failed: %s", host, strerror(h_errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* 非阻塞连接 */
    ce_dbproxy_set_nonblock(fd);

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        CE_LOG_ERROR("DBPROXY", "connect(%s:%d) failed: %s", host, port, strerror(errno));
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
            CE_LOG_ERROR("DBPROXY", "connect(%s:%d) timeout/error", host, port);
            close(fd);
            return -1;
        }

        /* 检查连接是否成功 */
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            CE_LOG_ERROR("DBPROXY", "connect(%s:%d) SO_ERROR: %s",
                         host, port, so_error ? strerror(so_error) : "unknown");
            close(fd);
            return -1;
        }
    }

    ce_dbproxy_set_nodelay(fd);

    CE_LOG_INFO("DBPROXY", "Connected to %s:%d (fd=%d)", host, port, fd);
    return fd;
}

/** 关闭当前连接 */
static void ce_dbproxy_close_connection(CeDbproxyContext* ctx) {
    if (ctx->sock_fd >= 0) {
        CE_LOG_INFO("DBPROXY", "Disconnecting fd=%d", ctx->sock_fd);
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }
    ctx->connected = CE_FALSE;
    ctx->recv_offset = 0;
}

/** 更新端点描述字符串 */
static void ce_dbproxy_update_endpoint(CeDbproxyContext* ctx) {
    const char* host = ctx->using_backup ? ctx->config.backup_host : ctx->config.primary_host;
    int port = ctx->using_backup ? ctx->config.backup_port : ctx->config.primary_port;
    snprintf(ctx->endpoint_str, sizeof(ctx->endpoint_str), "%s:%d",
             host ? host : "?", port);
}

/** 尝试连接到当前目标（主或备） */
static int ce_dbproxy_try_connect(CeDbproxyContext* ctx) {
    const char* host;
    int port;

    if (ctx->using_backup) {
        host = ctx->config.backup_host;
        port = ctx->config.backup_port;
    } else {
        host = ctx->config.primary_host;
        port = ctx->config.primary_port;
    }

    if (!host || port <= 0) {
        CE_LOG_WARN("DBPROXY", "No valid endpoint configured (using_backup=%d)", ctx->using_backup);
        return -1;
    }

    int fd = ce_dbproxy_connect_to(host, port, ctx->config.timeout_ms);
    if (fd >= 0) {
        ctx->sock_fd = fd;
        ctx->connected = CE_TRUE;
        ctx->reconnect_backoff_ms = CE_DBPROXY_RECONNECT_MIN_MS;
        ctx->reconnect_pending = CE_FALSE;
        ce_dbproxy_update_endpoint(ctx);
        CE_LOG_INFO("DBPROXY", "Connected to %s (%s)",
                    ctx->endpoint_str,
                    ctx->using_backup ? "backup" : "primary");
        return 0;
    }

    return -1;
}

/** 切换到备用 */
static void ce_dbproxy_switch_to_backup(CeDbproxyContext* ctx) {
    if (ctx->using_backup) {
        /* 已经在备用上，不再切换 */
        return;
    }
    if (!ctx->config.backup_host || ctx->config.backup_port <= 0) {
        CE_LOG_WARN("DBPROXY", "No backup configured, cannot switch");
        return;
    }

    CE_LOG_WARN("DBPROXY", "Switching from primary to backup DBProxy");
    ce_dbproxy_close_connection(ctx);
    ctx->using_backup = CE_TRUE;
    ctx->reconnect_backoff_ms = CE_DBPROXY_RECONNECT_MIN_MS;
    ctx->reconnect_pending = CE_TRUE;

    /* 立即尝试连接备用 */
    ce_dbproxy_try_connect(ctx);
}

/** 尝试重连（指数退避） */
static void ce_dbproxy_try_reconnect(CeDbproxyContext* ctx) {
    if (ctx->connected) return;

    uint64_t now = ce_dbproxy_now_us();
    uint64_t elapsed_ms = (now - ctx->last_reconnect_attempt_us) / 1000ULL;

    if (elapsed_ms < (uint64_t)ctx->reconnect_backoff_ms) {
        return; /* 还没到重试时间 */
    }

    ctx->last_reconnect_attempt_us = now;

    /* 先尝试当前端点 */
    if (ce_dbproxy_try_connect(ctx) == 0) {
        return;
    }

    /* 当前端点失败，尝试切换到备用 */
    if (!ctx->using_backup && ctx->config.backup_host && ctx->config.backup_port > 0) {
        CE_LOG_WARN("DBPROXY", "Primary unreachable, switching to backup");
        ctx->using_backup = CE_TRUE;
        if (ce_dbproxy_try_connect(ctx) == 0) {
            return;
        }
    }

    /* 备用也失败，指数退避 */
    ctx->reconnect_backoff_ms *= 2;
    if (ctx->reconnect_backoff_ms > CE_DBPROXY_RECONNECT_MAX_MS) {
        ctx->reconnect_backoff_ms = CE_DBPROXY_RECONNECT_MAX_MS;
    }

    CE_LOG_WARN("DBPROXY", "Reconnect failed, next attempt in %d ms (using_backup=%d)",
                ctx->reconnect_backoff_ms, ctx->using_backup);
}

/* ---- 生命周期 ---- */

CeDbproxyContext* ce_dbproxy_connect(const CeDbproxyConfig* config) {
    CeDbproxyContext* ctx = (CeDbproxyContext*)calloc(1, sizeof(CeDbproxyContext));
    if (!ctx) {
        CE_LOG_ERROR("DBPROXY", "Failed to allocate CeDbproxyContext");
        return NULL;
    }

    ctx->sock_fd = -1;
    ctx->connected = CE_FALSE;
    ctx->using_backup = CE_FALSE;
    ctx->recv_offset = 0;
    ctx->last_heartbeat_us = ce_dbproxy_now_us();
    ctx->last_reconnect_attempt_us = 0;
    ctx->reconnect_backoff_ms = CE_DBPROXY_RECONNECT_MIN_MS;
    ctx->reconnect_pending = CE_FALSE;

    /* 默认配置 */
    if (config) {
        ctx->config = *config;
    } else {
        memset(&ctx->config, 0, sizeof(ctx->config));
        ctx->config.primary_port = CE_DBPROXY_DEFAULT_PORT;
        ctx->config.backup_port  = CE_DBPROXY_DEFAULT_PORT;
        ctx->config.heartbeat_ms = CE_DBPROXY_DEFAULT_HEARTBEAT_MS;
        ctx->config.timeout_ms   = CE_DBPROXY_DEFAULT_TIMEOUT_MS;
    }

    /* 确保有效端口 */
    if (ctx->config.primary_port <= 0) ctx->config.primary_port = CE_DBPROXY_DEFAULT_PORT;
    if (ctx->config.backup_port  <= 0) ctx->config.backup_port  = CE_DBPROXY_DEFAULT_PORT;
    if (ctx->config.heartbeat_ms <= 0) ctx->config.heartbeat_ms = CE_DBPROXY_DEFAULT_HEARTBEAT_MS;
    if (ctx->config.timeout_ms   <= 0) ctx->config.timeout_ms   = CE_DBPROXY_DEFAULT_TIMEOUT_MS;

    /* 复制主机名字符串 */
    if (ctx->config.primary_host) {
        strncpy(ctx->primary_host_buf, ctx->config.primary_host, sizeof(ctx->primary_host_buf) - 1);
        ctx->primary_host_buf[sizeof(ctx->primary_host_buf) - 1] = '\0';
        ctx->config.primary_host = ctx->primary_host_buf;
    }
    if (ctx->config.backup_host) {
        strncpy(ctx->backup_host_buf, ctx->config.backup_host, sizeof(ctx->backup_host_buf) - 1);
        ctx->backup_host_buf[sizeof(ctx->backup_host_buf) - 1] = '\0';
        ctx->config.backup_host = ctx->backup_host_buf;
    }

    /* 连接到主 DBProxy */
    if (ctx->config.primary_host) {
        int fd = ce_dbproxy_connect_to(
            ctx->config.primary_host,
            ctx->config.primary_port,
            ctx->config.timeout_ms
        );
        if (fd >= 0) {
            ctx->sock_fd = fd;
            ctx->connected = CE_TRUE;
            ce_dbproxy_update_endpoint(ctx);
        } else {
            CE_LOG_WARN("DBPROXY", "Failed to connect to primary DBProxy %s:%d",
                        ctx->config.primary_host, ctx->config.primary_port);
            ctx->reconnect_pending = CE_TRUE;
            ctx->last_reconnect_attempt_us = ce_dbproxy_now_us();
        }
    } else {
        CE_LOG_WARN("DBPROXY", "No primary_host configured, dbproxy module started without connection");
    }

    CE_LOG_INFO("DBPROXY", "DBProxy client initialized (heartbeat=%dms, timeout=%dms)",
                ctx->config.heartbeat_ms, ctx->config.timeout_ms);
    return ctx;
}

void ce_dbproxy_disconnect(CeDbproxyContext* ctx) {
    if (!ctx) return;
    ce_dbproxy_close_connection(ctx);
    free(ctx);
    CE_LOG_INFO("DBPROXY", "DBProxy client shut down");
}

/* ---- 消息发送 ---- */

CeResult ce_dbproxy_send(CeDbproxyContext* ctx, const CeDbproxyMessage* msg) {
    if (!ctx || !msg) return CE_ERR;

    /* 尝试重连 */
    if (!ctx->connected) {
        ce_dbproxy_try_reconnect(ctx);
        if (!ctx->connected) {
            CE_LOG_WARN("DBPROXY", "Cannot send message: not connected");
            return CE_ERR;
        }
    }

    /* 计算总大小 */
    uint32_t total_len = CE_DBPROXY_HEADER_SIZE + msg->payload_len;

    if (total_len > CE_DBPROXY_MAX_MSG_SIZE) {
        CE_LOG_ERROR("DBPROXY", "Message too large: %u bytes (max %u)",
                     total_len, CE_DBPROXY_MAX_MSG_SIZE);
        return CE_ERR;
    }

    /* 构建发送缓冲区 */
    uint8_t send_buf[CE_DBPROXY_MAX_MSG_SIZE];
    ce_dbproxy_write_u32(send_buf, total_len);
    ce_dbproxy_write_u16(send_buf + 4, (uint16_t)msg->type);
    if (msg->payload_len > 0 && msg->payload) {
        memcpy(send_buf + CE_DBPROXY_HEADER_SIZE, msg->payload, msg->payload_len);
    }

    /* TCP 发送（非阻塞，循环发送直到全部发送完毕） */
    uint32_t sent = 0;
    int retries = 0;
    while (sent < total_len) {
        ssize_t n = send(ctx->sock_fd, send_buf + sent, total_len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 非阻塞模式下缓冲区满，短暂等待后重试 */
                if (retries++ > 100) {
                    CE_LOG_ERROR("DBPROXY", "Send timeout after %d retries", retries);
                    return CE_ERR;
                }
                usleep(1000); /* 1ms */
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                CE_LOG_ERROR("DBPROXY", "Connection lost during send");
                ctx->connected = CE_FALSE;
                ctx->reconnect_pending = CE_TRUE;
                ctx->last_reconnect_attempt_us = ce_dbproxy_now_us();
                /* 主不可用时尝试切到备用 */
                if (!ctx->using_backup) {
                    ce_dbproxy_switch_to_backup(ctx);
                }
                return CE_ERR;
            }
            CE_LOG_ERROR("DBPROXY", "send() failed: %s", strerror(errno));
            return CE_ERR;
        }
        sent += (uint32_t)n;
        retries = 0;
    }

    CE_LOG_TRACE("DBPROXY", "Sent msg type=0x%02X, payload=%u, total=%u",
                 (unsigned)msg->type, msg->payload_len, total_len);
    return CE_OK;
}

/* ---- 消息接收 ---- */

CeResult ce_dbproxy_recv(CeDbproxyContext* ctx, CeDbproxyResponse* response) {
    if (!ctx || !response) return CE_ERR;

    /* 尝试重连 */
    if (!ctx->connected) {
        ce_dbproxy_try_reconnect(ctx);
        if (!ctx->connected) {
            return CE_ERR;
        }
    }

    memset(response, 0, sizeof(CeDbproxyResponse));

    /* 读取数据到接收缓冲区 */
    ssize_t n = recv(ctx->sock_fd,
                     ctx->recv_buf + ctx->recv_offset,
                     CE_DBPROXY_RECV_BUF_SIZE - ctx->recv_offset,
                     0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return CE_ERR; /* 无数据 */
        }
        if (errno == ECONNRESET || errno == EPIPE) {
            CE_LOG_WARN("DBPROXY", "Connection reset by DBProxy");
            ctx->connected = CE_FALSE;
            ctx->reconnect_pending = CE_TRUE;
            ctx->last_reconnect_attempt_us = ce_dbproxy_now_us();
            if (!ctx->using_backup) {
                ce_dbproxy_switch_to_backup(ctx);
            }
            return CE_ERR;
        }
        CE_LOG_ERROR("DBPROXY", "recv() failed: %s", strerror(errno));
        return CE_ERR;
    }
    if (n == 0) {
        CE_LOG_WARN("DBPROXY", "DBProxy closed connection");
        ctx->connected = CE_FALSE;
        ctx->reconnect_pending = CE_TRUE;
        ctx->last_reconnect_attempt_us = ce_dbproxy_now_us();
        if (!ctx->using_backup) {
            ce_dbproxy_switch_to_backup(ctx);
        }
        return CE_ERR;
    }

    ctx->recv_offset += (int)n;

    /* 检查是否有完整消息 */
    if (ctx->recv_offset < CE_DBPROXY_HEADER_SIZE) {
        return CE_ERR; /* 还没收到消息头 */
    }

    uint32_t total_len = ce_dbproxy_read_u32(ctx->recv_buf);
    if (total_len > CE_DBPROXY_RECV_BUF_SIZE) {
        CE_LOG_ERROR("DBPROXY", "Response message too large: %u", total_len);
        ctx->recv_offset = 0;
        return CE_ERR;
    }
    if (total_len < CE_DBPROXY_HEADER_SIZE) {
        CE_LOG_ERROR("DBPROXY", "Response message too small: %u", total_len);
        ctx->recv_offset = 0;
        return CE_ERR;
    }

    if (ctx->recv_offset < (int)total_len) {
        return CE_ERR; /* 消息不完整，等待更多数据 */
    }

    /* 解析响应 */
    response->type = (CeDbproxyMsgType)ce_dbproxy_read_u16(ctx->recv_buf + 4);
    response->payload_len = total_len - CE_DBPROXY_HEADER_SIZE;

    if (response->payload_len > 0) {
        memcpy(response->payload,
               ctx->recv_buf + CE_DBPROXY_HEADER_SIZE,
               response->payload_len);
    }

    /* 从缓冲区移除已处理的消息 */
    int remaining = ctx->recv_offset - (int)total_len;
    if (remaining > 0) {
        memmove(ctx->recv_buf, ctx->recv_buf + total_len, (size_t)remaining);
    }
    ctx->recv_offset = remaining;

    CE_LOG_TRACE("DBPROXY", "Recv msg type=0x%02X, payload=%u",
                 (unsigned)response->type, response->payload_len);
    return CE_OK;
}

/* ---- 主备切换 ---- */

void ce_dbproxy_set_master(CeDbproxyContext* ctx,
                           const char* primary_host, int primary_port,
                           const char* backup_host,  int backup_port) {
    if (!ctx) return;

    if (primary_host) {
        strncpy(ctx->primary_host_buf, primary_host, sizeof(ctx->primary_host_buf) - 1);
        ctx->primary_host_buf[sizeof(ctx->primary_host_buf) - 1] = '\0';
        ctx->config.primary_host = ctx->primary_host_buf;
    }
    if (primary_port > 0) {
        ctx->config.primary_port = primary_port;
    }
    if (backup_host) {
        strncpy(ctx->backup_host_buf, backup_host, sizeof(ctx->backup_host_buf) - 1);
        ctx->backup_host_buf[sizeof(ctx->backup_host_buf) - 1] = '\0';
        ctx->config.backup_host = ctx->backup_host_buf;
    }
    if (backup_port > 0) {
        ctx->config.backup_port = backup_port;
    }

    CE_LOG_INFO("DBPROXY", "Master config updated: primary=%s:%d, backup=%s:%d",
                ctx->config.primary_host ? ctx->config.primary_host : "(null)",
                ctx->config.primary_port,
                ctx->config.backup_host ? ctx->config.backup_host : "(null)",
                ctx->config.backup_port);
}

/* ---- 状态查询 ---- */

CeBool ce_dbproxy_is_connected(const CeDbproxyContext* ctx) {
    if (!ctx) return CE_FALSE;
    return ctx->connected;
}

const char* ce_dbproxy_current_endpoint(const CeDbproxyContext* ctx) {
    if (!ctx) return "?";
    return ctx->endpoint_str;
}
