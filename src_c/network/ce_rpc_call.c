/*
 * ChaosEngine RPC Call — 实现
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
 *   total_len = 6 + payload_len（大端序）
 *
 * 特性：
 *   - TCP 非阻塞连接
 *   - 自动重连（指数退避：1s, 2s, 4s, 最大 30s）
 *   - 心跳检测与自动维持
 *   - 服务注册/注销管理
 */

/* POSIX 特性宏：C99 下启用 clock_gettime, usleep, select 等 */
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "network/ce_rpc_call.h"
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

/* ---- 内部结构 ---- */

/** RPC 客户端上下文（不透明） */
struct CeRpcContext {
    /* 连接 */
    int             sock_fd;          /* 当前 TCP socket */
    CeBool          connected;

    /* 配置 */
    CeRpcConfig     config;
    char            router_host_buf[256];  /* Router 地址副本 */

    /* 端点描述 */
    char            endpoint_str[280];      /* "host:port" */

    /* 注册状态 */
    CeBool          registered;
    uint64_t        last_register_attempt_us;

    /* 心跳 */
    uint64_t        last_heartbeat_sent_us;
    uint64_t        last_heartbeat_recv_us;

    /* 重连 */
    uint64_t        last_reconnect_attempt_us;
    int             reconnect_backoff_ms;
    CeBool          reconnect_pending;

    /* 接收缓冲区 */
    uint8_t         recv_buf[CE_RPC_MAX_MSG_SIZE];
    int             recv_offset;
};

/* ---- 内部辅助：时间戳 ---- */

static uint64_t ce_rpc_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ---- 内部辅助：大端序读写 ---- */

static void ce_rpc_write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val);
}

static void ce_rpc_write_u32(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

static uint16_t ce_rpc_read_u16(const uint8_t* buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

static uint32_t ce_rpc_read_u32(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
}

/* ---- 内部辅助：网络 ---- */

/** 设置 socket 为非阻塞 */
static CeResult ce_rpc_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return CE_ERR;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return CE_ERR;
    return CE_OK;
}

/** 设置 TCP_NODELAY */
static CeResult ce_rpc_set_nodelay(int fd) {
    int opt = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        CE_LOG_WARN("RPC", "Failed to set TCP_NODELAY: %s", strerror(errno));
        return CE_ERR;
    }
    return CE_OK;
}

/** 连接到指定 host:port，返回 socket fd */
static int ce_rpc_connect_to(const char* host, int port, int timeout_ms) {
    if (!host || port <= 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CE_LOG_ERROR("RPC", "socket() failed: %s", strerror(errno));
        return -1;
    }

    /* 解析主机名 */
    struct hostent* he = gethostbyname(host);
    if (!he) {
        CE_LOG_ERROR("RPC", "gethostbyname(%s) failed: %s", host, strerror(h_errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* 非阻塞连接 */
    ce_rpc_set_nonblock(fd);

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        CE_LOG_ERROR("RPC", "connect(%s:%d) failed: %s", host, port, strerror(errno));
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
            CE_LOG_ERROR("RPC", "connect(%s:%d) timeout/error", host, port);
            close(fd);
            return -1;
        }

        /* 检查连接是否成功 */
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            CE_LOG_ERROR("RPC", "connect(%s:%d) SO_ERROR: %s",
                         host, port, so_error ? strerror(so_error) : "unknown");
            close(fd);
            return -1;
        }
    }

    ce_rpc_set_nodelay(fd);

    CE_LOG_INFO("RPC", "Connected to Router at %s:%d (fd=%d)", host, port, fd);
    return fd;
}

/** 关闭当前连接 */
static void ce_rpc_close_connection(CeRpcContext* ctx) {
    if (ctx->sock_fd >= 0) {
        CE_LOG_INFO("RPC", "Disconnecting fd=%d", ctx->sock_fd);
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }
    ctx->connected = CE_FALSE;
    ctx->recv_offset = 0;
}

/** 更新端点描述字符串 */
static void ce_rpc_update_endpoint(CeRpcContext* ctx) {
    snprintf(ctx->endpoint_str, sizeof(ctx->endpoint_str), "%s:%d",
             ctx->config.router_host ? ctx->config.router_host : "?",
             ctx->config.router_port);
}

/** 尝试连接到 Router */
static int ce_rpc_try_connect(CeRpcContext* ctx) {
    if (!ctx->config.router_host || ctx->config.router_port <= 0) {
        CE_LOG_WARN("RPC", "No valid Router endpoint configured");
        return -1;
    }

    int fd = ce_rpc_connect_to(ctx->config.router_host,
                                ctx->config.router_port,
                                ctx->config.timeout_ms);
    if (fd >= 0) {
        ctx->sock_fd = fd;
        ctx->connected = CE_TRUE;
        ctx->reconnect_backoff_ms = CE_RPC_RECONNECT_MIN_MS;
        ctx->reconnect_pending = CE_FALSE;
        ce_rpc_update_endpoint(ctx);
        CE_LOG_INFO("RPC", "Connected to Router at %s", ctx->endpoint_str);
        return 0;
    }

    return -1;
}

/** 尝试重连（指数退避） */
static void ce_rpc_try_reconnect(CeRpcContext* ctx) {
    if (ctx->connected) return;

    uint64_t now = ce_rpc_now_us();
    uint64_t elapsed_ms = (now - ctx->last_reconnect_attempt_us) / 1000ULL;

    if (elapsed_ms < (uint64_t)ctx->reconnect_backoff_ms) {
        return; /* 还没到重试时间 */
    }

    ctx->last_reconnect_attempt_us = now;

    if (ce_rpc_try_connect(ctx) == 0) {
        return;
    }

    /* 指数退避 */
    ctx->reconnect_backoff_ms *= 2;
    if (ctx->reconnect_backoff_ms > CE_RPC_RECONNECT_MAX_MS) {
        ctx->reconnect_backoff_ms = CE_RPC_RECONNECT_MAX_MS;
    }

    CE_LOG_WARN("RPC", "Reconnect failed, next attempt in %d ms",
                ctx->reconnect_backoff_ms);
}

/* ---- 生命周期 ---- */

CeRpcContext* ce_rpc_init(const CeRpcConfig* config) {
    CeRpcContext* ctx = (CeRpcContext*)calloc(1, sizeof(CeRpcContext));
    if (!ctx) {
        CE_LOG_ERROR("RPC", "Failed to allocate CeRpcContext");
        return NULL;
    }

    ctx->sock_fd = -1;
    ctx->connected = CE_FALSE;
    ctx->registered = CE_FALSE;
    ctx->recv_offset = 0;
    ctx->last_heartbeat_sent_us = 0;
    ctx->last_heartbeat_recv_us = 0;
    ctx->last_reconnect_attempt_us = 0;
    ctx->reconnect_backoff_ms = CE_RPC_RECONNECT_MIN_MS;
    ctx->reconnect_pending = CE_FALSE;

    /* 默认配置 */
    if (config) {
        ctx->config = *config;
    } else {
        memset(&ctx->config, 0, sizeof(ctx->config));
        ctx->config.router_port = CE_RPC_DEFAULT_ROUTER_PORT;
        ctx->config.heartbeat_ms = CE_RPC_DEFAULT_HEARTBEAT_MS;
        ctx->config.heartbeat_timeout_ms = CE_RPC_DEFAULT_HEARTBEAT_TIMEOUT_MS;
        ctx->config.timeout_ms = CE_RPC_DEFAULT_TIMEOUT_MS;
    }

    /* 确保有效端口 */
    if (ctx->config.router_port <= 0) ctx->config.router_port = CE_RPC_DEFAULT_ROUTER_PORT;
    if (ctx->config.heartbeat_ms <= 0) ctx->config.heartbeat_ms = CE_RPC_DEFAULT_HEARTBEAT_MS;
    if (ctx->config.heartbeat_timeout_ms <= 0) ctx->config.heartbeat_timeout_ms = CE_RPC_DEFAULT_HEARTBEAT_TIMEOUT_MS;
    if (ctx->config.timeout_ms <= 0) ctx->config.timeout_ms = CE_RPC_DEFAULT_TIMEOUT_MS;

    /* 复制主机名字符串 */
    if (ctx->config.router_host) {
        strncpy(ctx->router_host_buf, ctx->config.router_host, sizeof(ctx->router_host_buf) - 1);
        ctx->router_host_buf[sizeof(ctx->router_host_buf) - 1] = '\0';
        ctx->config.router_host = ctx->router_host_buf;
    }

    /* 连接到 Router */
    if (ctx->config.router_host) {
        int fd = ce_rpc_connect_to(
            ctx->config.router_host,
            ctx->config.router_port,
            ctx->config.timeout_ms
        );
        if (fd >= 0) {
            ctx->sock_fd = fd;
            ctx->connected = CE_TRUE;
            ce_rpc_update_endpoint(ctx);
        } else {
            CE_LOG_WARN("RPC", "Failed to connect to Router %s:%d",
                        ctx->config.router_host, ctx->config.router_port);
            ctx->reconnect_pending = CE_TRUE;
            ctx->last_reconnect_attempt_us = ce_rpc_now_us();
        }
    } else {
        CE_LOG_WARN("RPC", "No router_host configured, RPC module started without connection");
    }

    CE_LOG_INFO("RPC", "RPC client initialized (service_type=0x%02X, heartbeat=%dms, timeout=%dms)",
                (unsigned)ctx->config.service_type,
                ctx->config.heartbeat_ms, ctx->config.timeout_ms);
    return ctx;
}

void ce_rpc_shutdown(CeRpcContext* ctx) {
    if (!ctx) return;

    /* 尝试发送注销消息 */
    if (ctx->connected && ctx->registered) {
        ce_rpc_deregister(ctx);
    }

    ce_rpc_close_connection(ctx);
    free(ctx);
    CE_LOG_INFO("RPC", "RPC client shut down");
}

/* ---- 服务注册 ---- */

CeResult ce_rpc_register(CeRpcContext* ctx) {
    if (!ctx) return CE_ERR;

    /* 尝试重连 */
    if (!ctx->connected) {
        ce_rpc_try_reconnect(ctx);
        if (!ctx->connected) {
            CE_LOG_WARN("RPC", "Cannot register: not connected to Router");
            return CE_ERR;
        }
    }

    /* 构建注册消息 payload：
     *   [1B service_type][1B name_len][N service_name]
     */
    uint8_t payload[512];
    uint32_t payload_len = 0;

    payload[payload_len++] = (uint8_t)ctx->config.service_type;

    const char* name = ctx->config.service_name ? ctx->config.service_name : "unknown";
    size_t raw_name_len = strlen(name);
    uint8_t name_len = (raw_name_len > 255) ? (uint8_t)255 : (uint8_t)raw_name_len;

    payload[payload_len++] = name_len;
    memcpy(payload + payload_len, name, name_len);
    payload_len += name_len;

    CeResult r = ce_rpc_send(ctx, CE_RPC_MSG_REGISTER, payload, payload_len);
    if (r == CE_OK) {
        ctx->registered = CE_TRUE;
        ctx->last_register_attempt_us = ce_rpc_now_us();
        CE_LOG_INFO("RPC", "Registered service 0x%02X (%s) with Router",
                    (unsigned)ctx->config.service_type, name);
    }

    return r;
}

CeResult ce_rpc_deregister(CeRpcContext* ctx) {
    if (!ctx) return CE_ERR;

    if (!ctx->connected) {
        ctx->registered = CE_FALSE;
        return CE_OK; /* 未连接，无需注销 */
    }

    /* 构建注销消息 payload */
    uint8_t payload[2];
    payload[0] = (uint8_t)ctx->config.service_type;
    payload[1] = 0; /* 无额外数据 */

    CeResult r = ce_rpc_send(ctx, CE_RPC_MSG_DEREGISTER, payload, 2);
    if (r == CE_OK) {
        ctx->registered = CE_FALSE;
        CE_LOG_INFO("RPC", "Deregistered service 0x%02X from Router",
                    (unsigned)ctx->config.service_type);
    }

    return r;
}

/* ---- 消息发送 ---- */

CeResult ce_rpc_send(CeRpcContext* ctx, uint16_t msg_type,
                     const uint8_t* payload, uint32_t payload_len) {
    if (!ctx) return CE_ERR;

    /* 尝试重连 */
    if (!ctx->connected) {
        ce_rpc_try_reconnect(ctx);
        if (!ctx->connected) {
            CE_LOG_WARN("RPC", "Cannot send message: not connected");
            return CE_ERR;
        }
    }

    /* 计算总大小 */
    uint32_t total_len = CE_RPC_HEADER_SIZE + payload_len;

    if (total_len > CE_RPC_MAX_MSG_SIZE) {
        CE_LOG_ERROR("RPC", "Message too large: %u bytes (max %u)",
                     total_len, CE_RPC_MAX_MSG_SIZE);
        return CE_ERR;
    }

    /* 构建发送缓冲区 */
    uint8_t send_buf[CE_RPC_MAX_MSG_SIZE];
    ce_rpc_write_u32(send_buf, total_len);
    ce_rpc_write_u16(send_buf + 4, msg_type);
    if (payload_len > 0 && payload) {
        memcpy(send_buf + CE_RPC_HEADER_SIZE, payload, payload_len);
    }

    /* TCP 发送（非阻塞，循环发送直到全部发送完毕） */
    uint32_t sent = 0;
    int retries = 0;
    while (sent < total_len) {
        ssize_t n = send(ctx->sock_fd, send_buf + sent, total_len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (retries++ > 100) {
                    CE_LOG_ERROR("RPC", "Send timeout after %d retries", retries);
                    return CE_ERR;
                }
                usleep(1000); /* 1ms */
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                CE_LOG_ERROR("RPC", "Connection lost during send");
                ctx->connected = CE_FALSE;
                ctx->reconnect_pending = CE_TRUE;
                ctx->last_reconnect_attempt_us = ce_rpc_now_us();
                return CE_ERR;
            }
            CE_LOG_ERROR("RPC", "send() failed: %s", strerror(errno));
            return CE_ERR;
        }
        sent += (uint32_t)n;
        retries = 0;
    }

    CE_LOG_TRACE("RPC", "Sent msg type=0x%04X, payload=%u, total=%u",
                 (unsigned)msg_type, payload_len, total_len);
    return CE_OK;
}

CeResult ce_rpc_call(CeRpcContext* ctx, CeServiceType target_service,
                     const char* method, const char* params, uint32_t params_len) {
    if (!ctx || !method) return CE_ERR;

    /* 构建 RPC 调用 payload：
     *   [1B target_service][1B method_len][N method][4B params_len][N params]
     */
    uint8_t payload[CE_RPC_MAX_MSG_SIZE];
    uint32_t payload_len = 0;

    /* target_service */
    payload[payload_len++] = (uint8_t)target_service;

    /* method */
    uint32_t method_len = (uint32_t)strlen(method);
    if (method_len > 255) method_len = 255;
    payload[payload_len++] = (uint8_t)method_len;
    memcpy(payload + payload_len, method, method_len);
    payload_len += method_len;

    /* params */
    if (params && params_len > 0) {
        ce_rpc_write_u32(payload + payload_len, params_len);
        payload_len += 4;
        memcpy(payload + payload_len, params, params_len);
        payload_len += params_len;
    } else {
        ce_rpc_write_u32(payload + payload_len, 0);
        payload_len += 4;
    }

    CE_LOG_DEBUG("RPC", "RPC call: target=0x%02X, method=%s, params_len=%u",
                 (unsigned)target_service, method, params_len);

    return ce_rpc_send(ctx, CE_RPC_MSG_RPC_CALL, payload, payload_len);
}

/* ---- 消息接收 ---- */

CeResult ce_rpc_recv(CeRpcContext* ctx, CeRpcResponse* response) {
    if (!ctx || !response) return CE_ERR;

    /* 尝试重连 */
    if (!ctx->connected) {
        ce_rpc_try_reconnect(ctx);
        if (!ctx->connected) {
            return CE_ERR;
        }
    }

    memset(response, 0, sizeof(CeRpcResponse));

    /* 读取数据到接收缓冲区 */
    ssize_t n = recv(ctx->sock_fd,
                     ctx->recv_buf + ctx->recv_offset,
                     CE_RPC_MAX_MSG_SIZE - ctx->recv_offset,
                     0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return CE_ERR; /* 无数据 */
        }
        if (errno == ECONNRESET || errno == EPIPE) {
            CE_LOG_WARN("RPC", "Connection reset by Router");
            ctx->connected = CE_FALSE;
            ctx->reconnect_pending = CE_TRUE;
            ctx->last_reconnect_attempt_us = ce_rpc_now_us();
            return CE_ERR;
        }
        CE_LOG_ERROR("RPC", "recv() failed: %s", strerror(errno));
        return CE_ERR;
    }
    if (n == 0) {
        CE_LOG_WARN("RPC", "Router closed connection");
        ctx->connected = CE_FALSE;
        ctx->reconnect_pending = CE_TRUE;
        ctx->last_reconnect_attempt_us = ce_rpc_now_us();
        return CE_ERR;
    }

    ctx->recv_offset += (int)n;

    /* 尝试解析完整消息 */
    if (ctx->recv_offset < CE_RPC_HEADER_SIZE) {
        return CE_ERR; /* 头部不完整 */
    }

    uint32_t total_len = ce_rpc_read_u32(ctx->recv_buf);
    if (total_len > CE_RPC_MAX_MSG_SIZE) {
        CE_LOG_ERROR("RPC", "Message too large: %u bytes, discarding buffer", total_len);
        ctx->recv_offset = 0;
        return CE_ERR;
    }

    if ((int)total_len > ctx->recv_offset) {
        return CE_ERR; /* 消息不完整 */
    }

    /* 解析消息 */
    uint16_t msg_type = ce_rpc_read_u16(ctx->recv_buf + 4);
    uint32_t payload_len = total_len - CE_RPC_HEADER_SIZE;

    response->type = msg_type;
    response->payload_len = payload_len;
    if (payload_len > 0) {
        memcpy(response->payload, ctx->recv_buf + CE_RPC_HEADER_SIZE, payload_len);
    }

    /* 从缓冲区移除已处理的消息 */
    int remaining = ctx->recv_offset - (int)total_len;
    if (remaining > 0) {
        memmove(ctx->recv_buf, ctx->recv_buf + total_len, (size_t)remaining);
    }
    ctx->recv_offset = remaining;

    /* 更新心跳时间 */
    ce_rpc_heartbeat_touch(ctx);

    /* 处理注册响应 */
    if (msg_type == CE_RPC_MSG_REGISTER_RESP) {
        if (payload_len > 0 && response->payload[0] == 0) {
            ctx->registered = CE_TRUE;
            CE_LOG_INFO("RPC", "Registration confirmed by Router");
        } else {
            ctx->registered = CE_FALSE;
            CE_LOG_WARN("RPC", "Registration rejected by Router");
        }
    }

    /* 处理心跳响应 */
    if (msg_type == CE_RPC_MSG_HEARTBEAT_RESP) {
        CE_LOG_TRACE("RPC", "Heartbeat response received");
    }

    CE_LOG_TRACE("RPC", "Received msg type=0x%04X, payload=%u",
                 (unsigned)msg_type, payload_len);
    return CE_OK;
}

/* ---- 心跳 ---- */

CeResult ce_rpc_send_heartbeat(CeRpcContext* ctx) {
    if (!ctx) return CE_ERR;

    /* 发送心跳 payload：[1B service_type] */
    uint8_t payload[1];
    payload[0] = (uint8_t)ctx->config.service_type;

    CeResult r = ce_rpc_send(ctx, CE_RPC_MSG_HEARTBEAT, payload, 1);
    if (r == CE_OK) {
        ctx->last_heartbeat_sent_us = ce_rpc_now_us();
    }
    return r;
}

CeBool ce_rpc_heartbeat_timeout(const CeRpcContext* ctx) {
    if (!ctx || !ctx->connected) return CE_TRUE;

    uint64_t now = ce_rpc_now_us();
    uint64_t elapsed_ms = (now - ctx->last_heartbeat_recv_us) / 1000ULL;

    return (elapsed_ms > (uint64_t)ctx->config.heartbeat_timeout_ms) ? CE_TRUE : CE_FALSE;
}

void ce_rpc_heartbeat_touch(CeRpcContext* ctx) {
    if (!ctx) return;
    ctx->last_heartbeat_recv_us = ce_rpc_now_us();
}

/* ---- 状态查询 ---- */

CeBool ce_rpc_is_connected(const CeRpcContext* ctx) {
    if (!ctx) return CE_FALSE;
    return ctx->connected;
}

CeBool ce_rpc_is_registered(const CeRpcContext* ctx) {
    if (!ctx) return CE_FALSE;
    return ctx->registered;
}

const char* ce_rpc_current_endpoint(const CeRpcContext* ctx) {
    if (!ctx) return "?";
    return ctx->endpoint_str;
}
