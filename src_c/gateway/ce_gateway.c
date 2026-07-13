/*
 * ChaosEngine Gateway - io_uring 事件驱动网络网关实现
 *
 * 完全使用 ce_async_* 系列 API (io_uring 后端) 驱动事件循环：
 *   - 原生 POSIX socket 创建监听 fd (不走 CeSocket 封装)
 *   - accept / recv / send / close 全部异步
 *   - user_data 指向 CeGatewayConn，recv 完成后通过 user_data 找回连接
 *   - accept 完成后立即再提交 accept (始终有一个 accept 在飞行)
 *   - 后端连接也用 io_uring 管理 (recv 后端响应)
 *
 * 纯 C99，不使用 epoll。
 */

/* POSIX 特性宏 */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include "ce_gateway.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>

/* ================================================================
 * 全局停止标志 (信号处理) + 活跃 Gateway 引用 (KCP output callback)
 * ================================================================ */

static volatile sig_atomic_t g_stop_flag = 0;
CeGateway* g_active_gw = NULL;  /* 事件循环期间的全局引用，供 KCP output callback 使用 */

static void gw_signal_handler(int sig) {
    (void)sig;
    g_stop_flag = 1;
}

/* ================================================================
 * 时间辅助
 * ================================================================ */

static uint64_t gw_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ================================================================
 * 大端序读写
 * ================================================================ */

void ce_gateway_write_u32(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

void ce_gateway_write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val);
}

uint32_t ce_gateway_read_u32(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
}

uint16_t ce_gateway_read_u16(const uint8_t* buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

/* ================================================================
 * Socket 辅助 (原生 POSIX，不走 CeSocket)
 * ================================================================ */

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_nodelay(int fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

static int set_reuseaddr(int fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

/** 创建监听 socket (原生 POSIX)，返回 fd 或 -1 */
static int create_listen_fd(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CE_LOG_ERROR("GATEWAY", "socket() failed: %s", strerror(errno));
        return -1;
    }

    set_reuseaddr(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CE_LOG_ERROR("GATEWAY", "bind(port=%d) failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 512) < 0) {
        CE_LOG_ERROR("GATEWAY", "listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    CE_LOG_INFO("GATEWAY", "TCP listening on 0.0.0.0:%d (fd=%d)", port, fd);
    return fd;
}

/** 创建 UDP socket 并 bind 同一个端口 (与 TCP 共用端口 9000)
 *  内核端口空间按协议 (TCP/UDP) 隔离，互不冲突 */
static int create_udp_fd(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        CE_LOG_ERROR("GATEWAY", "UDP socket() failed: %s", strerror(errno));
        return -1;
    }

    set_reuseaddr(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CE_LOG_ERROR("GATEWAY", "UDP bind(port=%d) failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    CE_LOG_INFO("GATEWAY", "KCP/UDP listening on 0.0.0.0:%d (fd=%d)", port, fd);
    return fd;
}

/** 连接到后端 (原生 POSIX 非阻塞连接 + 等待完成) */
static int connect_to_backend(const char* host, int port) {
    struct hostent* he = gethostbyname(host);
    if (!he) {
        CE_LOG_ERROR("GATEWAY", "gethostbyname(%s) failed", host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CE_LOG_ERROR("GATEWAY", "backend socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* 阻塞连接后端 (启动时) */
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CE_LOG_ERROR("GATEWAY", "connect(%s:%d) failed: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    set_nonblock(fd);
    set_nodelay(fd);
    CE_LOG_INFO("GATEWAY", "Connected to backend %s:%d (fd=%d)", host, port, fd);
    return fd;
}

/* ================================================================
 * 连接管理
 * ================================================================ */

static CeGatewayConn* conn_create(int fd, uint64_t conn_id) {
    CeGatewayConn* conn = (CeGatewayConn*)calloc(1, sizeof(CeGatewayConn));
    if (!conn) {
        CE_LOG_ERROR("GATEWAY", "Failed to allocate CeGatewayConn");
        return NULL;
    }

    conn->recv_buf = (uint8_t*)malloc(CE_GW_RECV_BUF_SIZE);
    conn->send_buf = (uint8_t*)malloc(CE_GW_SEND_BUF_SIZE);
    if (!conn->recv_buf || !conn->send_buf) {
        free(conn->recv_buf);
        free(conn->send_buf);
        free(conn);
        CE_LOG_ERROR("GATEWAY", "Failed to allocate connection buffers");
        return NULL;
    }

    conn->fd = fd;
    conn->conn_id = conn_id;
    conn->state = CE_GW_CONN_ACTIVE;
    conn->connect_time_us = gw_now_us();
    conn->last_active_us = conn->connect_time_us;
    conn->recv_offset = 0;
    conn->send_len = 0;
    conn->recv_pending = CE_FALSE;
    conn->protocol = CE_GW_PROTO_TCP;  /* 默认 TCP */
    conn->kcp_ctx = NULL;

    return conn;
}

static void conn_destroy(CeGatewayConn* conn) {
    if (!conn) return;
    /* KCP 连接需要销毁 KCP 上下文 */
    if (conn->kcp_ctx) {
        ce_kcp_destroy(conn->kcp_ctx);
        conn->kcp_ctx = NULL;
    }
    free(conn->recv_buf);
    free(conn->send_buf);
    free(conn);
}

/** 在 Gateway 的连接数组中找到空闲槽位，返回索引或 -1 */
static int find_free_slot(CeGateway* gw) {
    for (int i = 0; i < gw->conn_capacity; i++) {
        if (gw->conns[i] == NULL) return i;
    }
    return -1;
}

/** 扩容连接数组 */
static CeResult expand_conn_array(CeGateway* gw) {
    int new_cap = gw->conn_capacity * 2;
    if (new_cap > gw->max_conns) new_cap = gw->max_conns;
    if (new_cap <= gw->conn_capacity) return CE_ERR; /* 已达上限 */

    CeGatewayConn** new_arr = (CeGatewayConn**)realloc(gw->conns,
                                    (size_t)new_cap * sizeof(CeGatewayConn*));
    if (!new_arr) {
        CE_LOG_ERROR("GATEWAY", "Failed to expand connection array to %d", new_cap);
        return CE_ERR;
    }
    memset(new_arr + gw->conn_capacity, 0,
           (size_t)(new_cap - gw->conn_capacity) * sizeof(CeGatewayConn*));
    gw->conns = new_arr;
    gw->conn_capacity = new_cap;
    return CE_OK;
}

/** 注册新连接到数组，返回槽位索引或 -1 */
static int register_conn(CeGateway* gw, CeGatewayConn* conn) {
    if (gw->conn_count >= gw->max_conns) {
        CE_LOG_WARN("GATEWAY", "Max connections (%d) reached, rejecting fd=%d",
                    gw->max_conns, conn->fd);
        return -1;
    }

    int slot = find_free_slot(gw);
    if (slot < 0) {
        if (expand_conn_array(gw) != CE_OK) return -1;
        slot = find_free_slot(gw);
        if (slot < 0) return -1;
    }

    gw->conns[slot] = conn;
    gw->conn_count++;
    return slot;
}

/** 注销连接 (从数组移除，不释放) */
static void unregister_conn(CeGateway* gw, int slot) {
    if (slot < 0 || slot >= gw->conn_capacity) return;
    if (gw->conns[slot]) {
        gw->conns[slot] = NULL;
        gw->conn_count--;
    }
}

/* ================================================================
 * KCP 连接管理
 * ================================================================ */

/* g_active_gw 在文件顶部定义，供 KCP output callback 使用 */

/** KCP 输出回调：将 KCP 协议栈产生的数据通过 sendto 发回对端 UDP socket */
static int kcp_output_cb(const char* buf, int len, void* user_data) {
    CeGatewayConn* conn = (CeGatewayConn*)user_data;
    if (!conn || !g_active_gw || g_active_gw->kcp_fd < 0) return -1;

    int n = sendto(g_active_gw->kcp_fd, buf, len, 0,
                   (struct sockaddr*)&conn->peer_addr,
                   sizeof(conn->peer_addr));
    if (n < 0) {
        CE_LOG_TRACE("GATEWAY", "KCP sendto failed: %s", strerror(errno));
        return -1;
    }
    return n;
}

/** 从 KCP UDP 数据包前 4 字节解析 conv ID */
static uint32_t kcp_parse_conv(const uint8_t* data, int len) {
    if (len < 4) return 0;
    /* KCP conv 是小端序 */
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/** 查找已存在的 KCP 连接 (按 conv ID 匹配) */
static CeGatewayConn* kcp_find_conn(CeGateway* gw, uint32_t conv) {
    for (int i = 0; i < gw->conn_capacity; i++) {
        CeGatewayConn* conn = gw->conns[i];
        if (conn && conn->protocol == CE_GW_PROTO_KCP && conn->kcp_ctx) {
            if (conn->kcp_conv == conv) {
                return conn;
            }
        }
    }
    return NULL;
}

/** 创建新的 KCP 连接 (复用 CeGatewayConn 结构体) */
static CeGatewayConn* kcp_create_conn(CeGateway* gw, uint32_t conv,
                                      const struct sockaddr_in* peer_addr) {
    CeGatewayConn* conn = conn_create(gw->kcp_fd, gw->next_conn_id++);
    if (!conn) return NULL;

    conn->protocol = CE_GW_PROTO_KCP;
    conn->fd = gw->kcp_fd;  /* 所有 KCP 连接共用同一个 UDP fd */
    conn->peer_addr = *peer_addr;
    conn->kcp_conv = conv;

    /* 格式化地址字符串 */
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr->sin_addr, ip, sizeof(ip));
    snprintf(conn->addr, sizeof(conn->addr), "%s:%d",
             ip, ntohs(peer_addr->sin_port));

    /* 创建 KCP 上下文 */
    conn->kcp_ctx = ce_kcp_create(conv, conn);
    if (!conn->kcp_ctx) {
        CE_LOG_ERROR("GATEWAY", "Failed to create KCP context for conv=%u", conv);
        conn_destroy(conn);
        return NULL;
    }

    /* 配置 KCP: 快速模式 */
    ce_kcp_set_config(conn->kcp_ctx, 1, 10, 2, 1);
    ce_kcp_set_wndsize(conn->kcp_ctx, 128, 128);
    ce_kcp_set_output_callback(conn->kcp_ctx, kcp_output_cb);

    /* 注册到连接数组 */
    int slot = register_conn(gw, conn);
    if (slot < 0) {
        conn_destroy(conn);
        return NULL;
    }

    CE_LOG_INFO("GATEWAY", "[+] KCP client connected (conv=%u, addr=%s, conn_id=%llu, slot=%d)",
                conv, conn->addr, (unsigned long long)conn->conn_id, slot);

    return conn;
}

/* 前向声明：process_recv_data 定义在后面 */
static void process_recv_data(CeGateway* gw, CeGatewayConn* conn, int nbytes);

/** 处理收到的 KCP UDP 数据包 */
static void handle_kcp_recv(CeGateway* gw, const uint8_t* data, int len,
                            const struct sockaddr_in* peer_addr) {
    /* 解析 KCP conv ID */
    uint32_t conv = kcp_parse_conv(data, len);
    if (conv == 0) {
        CE_LOG_WARN("GATEWAY", "KCP: invalid conv=0, dropping %d bytes", len);
        return;
    }

    /* 查找或创建 KCP 连接 */
    CeGatewayConn* conn = kcp_find_conn(gw, conv);
    if (!conn) {
        conn = kcp_create_conn(gw, conv, peer_addr);
        if (!conn) return;
    }

    /* 更新活跃时间和对端地址 (地址可能变化, UDP 无连接) */
    conn->last_active_us = gw_now_us();
    conn->peer_addr = *peer_addr;

    /* 喂给 KCP 协议栈 */
    int ret = ce_kcp_input(conn->kcp_ctx, data, len);
    if (ret < 0) {
        CE_LOG_TRACE("GATEWAY", "KCP input error conv=%u ret=%d", conv, ret);
        return;
    }

    /* 驱动 KCP 状态机 */
    uint32_t now_ms = (uint32_t)(gw_now_us() / 1000ULL);
    ce_kcp_update(conn->kcp_ctx, now_ms);

    /* 读取可靠数据 (可能有多个消息) */
    uint8_t kcp_data[CE_GW_RECV_BUF_SIZE];
    int kcp_len;
    while ((kcp_len = ce_kcp_recv(conn->kcp_ctx, kcp_data, sizeof(kcp_data))) > 0) {
        /* 将 KCP 可靠数据当作 TCP recv 数据处理 */
        memcpy(conn->recv_buf, kcp_data, kcp_len);
        conn->recv_offset = kcp_len;
        process_recv_data(gw, conn, kcp_len);
        conn->recv_offset = 0;  /* KCP 连接每次 recv 都是完整消息，重置 */
    }
}

/** 每次事件循环迭代时驱动所有 KCP 连接的状态机 */
static void kcp_update_all(CeGateway* gw) {
    uint32_t now_ms = (uint32_t)(gw_now_us() / 1000ULL);
    for (int i = 0; i < gw->conn_capacity; i++) {
        CeGatewayConn* conn = gw->conns[i];
        if (!conn || conn->protocol != CE_GW_PROTO_KCP || !conn->kcp_ctx) continue;

        ce_kcp_update(conn->kcp_ctx, now_ms);

        /* 检查是否有可靠数据到达 */
        uint8_t kcp_data[CE_GW_RECV_BUF_SIZE];
        int kcp_len;
        while ((kcp_len = ce_kcp_recv(conn->kcp_ctx, kcp_data, sizeof(kcp_data))) > 0) {
            memcpy(conn->recv_buf, kcp_data, kcp_len);
            conn->recv_offset = kcp_len;
            process_recv_data(gw, conn, kcp_len);
            conn->recv_offset = 0;
        }
    }
}

/* ================================================================
 * 后端管理
 * ================================================================ */

CeResult ce_gateway_add_backend(CeGateway* gw, const char* host, int port) {
    if (!gw || !host || gw->backend_count >= CE_GW_MAX_BACKENDS) return CE_ERR;

    CeGatewayBackend* be = &gw->backends[gw->backend_count];
    memset(be, 0, sizeof(*be));
    strncpy(be->host, host, CE_GW_BACKEND_ADDR_LEN - 1);
    be->port = port;
    be->fd = -1;
    be->connected = CE_FALSE;
    be->recv_pending = CE_FALSE;

    /* 分配后端接收缓冲区 */
    be->recv_buf = (uint8_t*)malloc(CE_GW_RECV_BUF_SIZE);
    if (!be->recv_buf) {
        CE_LOG_ERROR("GATEWAY", "Failed to allocate backend recv buffer");
        return CE_ERR;
    }
    be->recv_offset = 0;

    gw->backend_count++;
    CE_LOG_INFO("GATEWAY", "Backend added: %s:%d (index=%d)", host, port, gw->backend_count - 1);
    return CE_OK;
}

/** 连接所有后端并提交初始 recv */
static void connect_backends(CeGateway* gw) {
    for (int i = 0; i < gw->backend_count; i++) {
        CeGatewayBackend* be = &gw->backends[i];
        if (be->connected) continue;

        int fd = connect_to_backend(be->host, be->port);
        if (fd < 0) {
            be->fail_count++;
            CE_LOG_WARN("GATEWAY", "Backend %s:%d connect failed (fails=%d)",
                        be->host, be->port, be->fail_count);
            continue;
        }

        be->fd = fd;
        be->connected = CE_TRUE;
        be->recv_offset = 0;
        be->recv_pending = CE_FALSE;
        be->fail_count = 0;

        /* 提交后端 recv (user_data = 后端索引的编码指针) */
        ce_async_recv(gw->io, fd, be->recv_buf, CE_GW_RECV_BUF_SIZE,
                      (void*)(intptr_t)(i + 1)); /* +1 避免 NULL */
        be->recv_pending = CE_TRUE;
    }
}

/* ================================================================
 * 消息处理
 * ================================================================ */

/** 向客户端发送数据 (自动区分 TCP/KCP) */
static void gw_send_to_conn(CeGateway* gw, CeGatewayConn* conn,
                            const uint8_t* data, int len) {
    if (conn->protocol == CE_GW_PROTO_KCP && conn->kcp_ctx) {
        /* KCP 连接：通过 ce_kcp_send 发送可靠数据 */
        int ret = ce_kcp_send(conn->kcp_ctx, data, len);
        if (ret < 0) {
            CE_LOG_WARN("GATEWAY", "KCP send failed for conn=%llu",
                        (unsigned long long)conn->conn_id);
        }
        /* 驱动 KCP 状态机以尽快发送 */
        uint32_t now_ms = (uint32_t)(gw_now_us() / 1000ULL);
        ce_kcp_update(conn->kcp_ctx, now_ms);
    } else {
        /* TCP 连接：通过 io_uring 异步发送 */
        ce_async_send(gw->io, conn->fd, data, len, conn);
    }
}

/** 向客户端发送 PONG 消息 */
static void send_pong(CeGateway* gw, CeGatewayConn* conn) {
    /* 构造 PONG 消息帧: [4B total_len=6][2B msg_type=PONG]
     * 注意: ce_async_send 将 buf 指针传给 io_uring_prep_send，内核异步读取。
     * 不能使用栈缓冲区（函数返回后栈帧失效），必须使用连接的 send_buf。 */
    if (CE_GW_HEADER_SIZE > CE_GW_SEND_BUF_SIZE) {
        CE_LOG_ERROR("GATEWAY", "Header size exceeds send buffer");
        return;
    }
    ce_gateway_write_u32(conn->send_buf, (uint32_t)CE_GW_HEADER_SIZE);
    ce_gateway_write_u16(conn->send_buf + 4, CE_GW_MSG_PONG);

    gw_send_to_conn(gw, conn, conn->send_buf, CE_GW_HEADER_SIZE);
}

/** 处理收到的完整消息 (解析协议帧 -> 路由) */
static void handle_message(CeGateway* gw, CeGatewayConn* conn,
                           const uint8_t* data, int len) {
    if (len < CE_GW_HEADER_SIZE) return;

    uint32_t total_len = ce_gateway_read_u32(data);
    uint16_t msg_type = ce_gateway_read_u16(data + 4);

    if (total_len > (uint32_t)len) return; /* 不完整 */

    /* payload 指针和长度（当前 switch 分支未直接使用，但保留供未来扩展） */
    const uint8_t* payload = data + CE_GW_HEADER_SIZE;
    int payload_len = (int)total_len - CE_GW_HEADER_SIZE;
    (void)payload;
    (void)payload_len;

    switch (msg_type) {
    case CE_GW_MSG_PING:
        /* 回复 PONG */
        send_pong(gw, conn);
        CE_LOG_DEBUG("GATEWAY", "PING from conn=%llu, sent PONG",
                     (unsigned long long)conn->conn_id);
        break;

    case CE_GW_MSG_PONG:
        /* 心跳响应，更新活跃时间已在调用处处理 */
        CE_LOG_TRACE("GATEWAY", "PONG from conn=%llu", (unsigned long long)conn->conn_id);
        break;

    case CE_GW_MSG_DISCONNECT:
        CE_LOG_INFO("GATEWAY", "Client %llu requested disconnect",
                    (unsigned long long)conn->conn_id);
        conn->state = CE_GW_CONN_CLOSING;
        break;

    case CE_GW_MSG_GAME_DATA:
    case CE_GW_MSG_LOGIN:
        /* 转发到后端 (如果已连接) */
        if (gw->backend_count > 0 && gw->backends[0].connected) {
            CeGatewayBackend* be = &gw->backends[0];
            ce_async_send(gw->io, be->fd, data, (int)total_len, conn);
            CE_LOG_DEBUG("GATEWAY", "Forwarded msg_type=0x%04X (%d bytes) to backend %s:%d",
                         msg_type, (int)total_len, be->host, be->port);
        } else {
            CE_LOG_WARN("GATEWAY", "No backend available for msg_type=0x%04X", msg_type);
            /* 发送错误响应: 使用 conn->send_buf 避免栈缓冲区悬空 */
            ce_gateway_write_u32(conn->send_buf, (uint32_t)CE_GW_HEADER_SIZE);
            ce_gateway_write_u16(conn->send_buf + 4, CE_GW_MSG_LOGIN_RESP);
            gw_send_to_conn(gw, conn, conn->send_buf, CE_GW_HEADER_SIZE);
        }
        break;

    default:
        CE_LOG_WARN("GATEWAY", "Unknown msg_type=0x%04X from conn=%llu, dropping",
                    msg_type, (unsigned long long)conn->conn_id);
        break;
    }
}

/** 处理 recv 完成后的数据：解析所有完整消息帧 */
static void process_recv_data(CeGateway* gw, CeGatewayConn* conn, int nbytes) {
    conn->recv_offset += nbytes;

    /* 循环解析所有完整消息 */
    while (conn->recv_offset >= CE_GW_HEADER_SIZE) {
        uint32_t total_len = ce_gateway_read_u32(conn->recv_buf);

        /* 校验消息长度 */
        if (total_len < CE_GW_HEADER_SIZE) {
            CE_LOG_WARN("GATEWAY", "Invalid msg len %u from conn=%llu, resetting buffer",
                        total_len, (unsigned long long)conn->conn_id);
            conn->recv_offset = 0;
            break;
        }
        if (total_len > CE_GW_RECV_BUF_SIZE) {
            CE_LOG_ERROR("GATEWAY", "Msg too large %u (max %d) from conn=%llu",
                         total_len, CE_GW_RECV_BUF_SIZE, (unsigned long long)conn->conn_id);
            conn->recv_offset = 0;
            conn->state = CE_GW_CONN_CLOSING;
            break;
        }

        /* 消息不完整，等待更多数据 */
        if ((uint32_t)conn->recv_offset < total_len) break;

        /* 处理完整消息 */
        handle_message(gw, conn, conn->recv_buf, (int)total_len);

        /* 从缓冲区移除已处理的消息 */
        int remaining = conn->recv_offset - (int)total_len;
        if (remaining > 0) {
            memmove(conn->recv_buf, conn->recv_buf + total_len, (size_t)remaining);
        }
        conn->recv_offset = remaining;
    }
}

/** 处理后端 recv 完成 (后端响应转发给客户端) */
static void handle_backend_recv(CeGateway* gw, int backend_idx, int nbytes) {
    if (backend_idx < 0 || backend_idx >= gw->backend_count) return;
    CeGatewayBackend* be = &gw->backends[backend_idx];
    be->recv_pending = CE_FALSE;

    if (nbytes <= 0) {
        /* 后端断开 */
        CE_LOG_ERROR("GATEWAY", "Backend %s:%d disconnected (fd=%d)",
                     be->host, be->port, be->fd);
        be->connected = CE_FALSE;
        be->fail_count++;
        if (be->fd >= 0) {
            close(be->fd);
            be->fd = -1;
        }
        return;
    }

    be->recv_offset += nbytes;

    /* 尝试解析后端响应帧，转发给所有客户端 (广播) */
    /* 实际实现中应只转发给原始请求的客户端，
       但简化版本中广播到所有活跃连接 */
    while (be->recv_offset >= CE_GW_HEADER_SIZE) {
        uint32_t total_len = ce_gateway_read_u32(be->recv_buf);
        if (total_len < CE_GW_HEADER_SIZE || total_len > CE_GW_RECV_BUF_SIZE) {
            be->recv_offset = 0;
            break;
        }
        if ((uint32_t)be->recv_offset < total_len) break;

        /* 转发响应给所有活跃连接 */
        for (int i = 0; i < gw->conn_capacity; i++) {
            CeGatewayConn* conn = gw->conns[i];
            if (conn && conn->state == CE_GW_CONN_ACTIVE) {
                gw_send_to_conn(gw, conn, be->recv_buf, (int)total_len);
            }
        }

        int remaining = be->recv_offset - (int)total_len;
        if (remaining > 0) {
            memmove(be->recv_buf, be->recv_buf + total_len, (size_t)remaining);
        }
        be->recv_offset = remaining;
    }

    /* 继续提交后端 recv */
    if (be->connected) {
        ce_async_recv(gw->io, be->fd,
                      be->recv_buf + be->recv_offset,
                      CE_GW_RECV_BUF_SIZE - be->recv_offset,
                      (void*)(intptr_t)(backend_idx + 1));
        be->recv_pending = CE_TRUE;
    }
}

/* ================================================================
 * 心跳检查
 * ================================================================ */

static void check_heartbeats(CeGateway* gw) {
    uint64_t now = gw_now_us();
    uint64_t elapsed_ms = (now - gw->last_heartbeat_check_us) / 1000ULL;

    if (elapsed_ms < (uint64_t)gw->heartbeat_interval_ms) return;

    gw->last_heartbeat_check_us = now;

    for (int i = 0; i < gw->conn_capacity; i++) {
        CeGatewayConn* conn = gw->conns[i];
        if (!conn || conn->state != CE_GW_CONN_ACTIVE) continue;

        uint64_t idle_ms = (now - conn->last_active_us) / 1000ULL;
        if (idle_ms > (uint64_t)gw->heartbeat_timeout_ms) {
            CE_LOG_WARN("GATEWAY", "Conn %llu heartbeat timeout (idle=%llums), closing",
                        (unsigned long long)conn->conn_id,
                        (unsigned long long)idle_ms);
            conn->state = CE_GW_CONN_CLOSING;
        }
    }
}

/* ================================================================
 * 连接关闭
 * ================================================================ */

static void close_conn(CeGateway* gw, int slot) {
    if (slot < 0 || slot >= gw->conn_capacity) return;
    CeGatewayConn* conn = gw->conns[slot];
    if (!conn) return;

    if (conn->protocol == CE_GW_PROTO_KCP) {
        CE_LOG_INFO("GATEWAY", "Closing KCP conn %llu (conv=%u, addr=%s)",
                    (unsigned long long)conn->conn_id, conn->kcp_conv, conn->addr);
        /* KCP 连接共用 kcp_fd，不能关闭它，只销毁 KCP 上下文和连接对象 */
    } else {
        CE_LOG_INFO("GATEWAY", "Closing TCP conn %llu (fd=%d)",
                    (unsigned long long)conn->conn_id, conn->fd);

        /* 异步关闭 fd (仅 TCP 连接) */
        if (conn->fd >= 0) {
            ce_async_close(gw->io, conn->fd);
            conn->fd = -1;
        }
    }

    unregister_conn(gw, slot);
    conn_destroy(conn);
}

/* ================================================================
 * 生命周期
 * ================================================================ */

CeGateway* ce_gateway_create(const CeGatewayConfig* config) {
    CeGateway* gw = (CeGateway*)calloc(1, sizeof(CeGateway));
    if (!gw) {
        CE_LOG_ERROR("GATEWAY", "Failed to allocate CeGateway");
        return NULL;
    }

    /* 应用配置 */
    gw->port = config ? config->port : CE_GW_DEFAULT_PORT;
    gw->max_conns = config ? config->max_connections : CE_GW_DEFAULT_MAX_CONNS;
    gw->heartbeat_interval_ms = config ? config->heartbeat_interval_ms : CE_GW_HEARTBEAT_INTERVAL_MS;
    gw->heartbeat_timeout_ms = config ? config->heartbeat_timeout_ms : CE_GW_HEARTBEAT_TIMEOUT_MS;
    gw->kcp_enabled = config ? config->kcp_enabled : 1;  /* 默认启用 KCP */

    if (gw->max_conns <= 0) gw->max_conns = CE_GW_DEFAULT_MAX_CONNS;
    if (gw->heartbeat_interval_ms <= 0) gw->heartbeat_interval_ms = CE_GW_HEARTBEAT_INTERVAL_MS;
    if (gw->heartbeat_timeout_ms <= 0) gw->heartbeat_timeout_ms = CE_GW_HEARTBEAT_TIMEOUT_MS;

    /* 初始连接数组容量 (较小初始值，按需扩容) */
    gw->conn_capacity = 256;
    if (gw->conn_capacity > gw->max_conns) gw->conn_capacity = gw->max_conns;
    gw->conns = (CeGatewayConn**)calloc((size_t)gw->conn_capacity, sizeof(CeGatewayConn*));
    if (!gw->conns) {
        CE_LOG_ERROR("GATEWAY", "Failed to allocate connection array");
        free(gw);
        return NULL;
    }

    gw->conn_count = 0;
    gw->next_conn_id = 1;
    gw->backend_count = 0;
    gw->running = CE_FALSE;
    gw->last_heartbeat_check_us = gw_now_us();
    gw->last_backend_reconnect_us = gw_now_us();

    /* 创建监听 socket (原生 POSIX) */
    gw->listen_fd = create_listen_fd(gw->port);
    if (gw->listen_fd < 0) {
        free(gw->conns);
        free(gw);
        return NULL;
    }

    /* 初始化 io_uring 异步 I/O 上下文 */
    gw->io = ce_async_init(CE_GW_QUEUE_DEPTH);
    if (!gw->io) {
        CE_LOG_ERROR("GATEWAY", "Failed to initialize async I/O (backend=%s)",
                     ce_async_backend_name());
        close(gw->listen_fd);
        free(gw->conns);
        free(gw);
        return NULL;
    }

    /* KCP 初始化: 创建 UDP socket 共用同一端口 */
    gw->kcp_fd = -1;
    gw->kcp_recv_buf = NULL;
    gw->kcp_recv_pending = CE_FALSE;
    if (gw->kcp_enabled) {
        gw->kcp_fd = create_udp_fd(gw->port);
        if (gw->kcp_fd < 0) {
            CE_LOG_WARN("GATEWAY", "KCP disabled (UDP socket creation failed)");
            gw->kcp_enabled = 0;
        } else {
            gw->kcp_recv_buf = (uint8_t*)malloc(CE_GW_KCP_RECV_BUF_SIZE);
            if (!gw->kcp_recv_buf) {
                CE_LOG_WARN("GATEWAY", "KCP disabled (recv buf alloc failed)");
                close(gw->kcp_fd);
                gw->kcp_fd = -1;
                gw->kcp_enabled = 0;
            }
        }
    }

    CE_LOG_INFO("GATEWAY", "Gateway created: port=%d, max_conns=%d, kcp=%s, backend=%s, queue_depth=%d",
                gw->port, gw->max_conns, gw->kcp_enabled ? "on" : "off",
                ce_async_backend_name(), CE_GW_QUEUE_DEPTH);

    return gw;
}

void ce_gateway_destroy(CeGateway* gw) {
    if (!gw) return;

    /* 关闭所有连接 (KCP 连接不关闭共享的 kcp_fd) */
    for (int i = 0; i < gw->conn_capacity; i++) {
        if (gw->conns[i]) {
            if (gw->conns[i]->protocol == CE_GW_PROTO_TCP && gw->conns[i]->fd >= 0) {
                close(gw->conns[i]->fd);
            }
            conn_destroy(gw->conns[i]);
            gw->conns[i] = NULL;
        }
    }
    free(gw->conns);

    /* 关闭后端连接 */
    for (int i = 0; i < gw->backend_count; i++) {
        CeGatewayBackend* be = &gw->backends[i];
        if (be->fd >= 0) close(be->fd);
        free(be->recv_buf);
    }

    /* 关闭 KCP UDP socket 和接收缓冲区 */
    if (gw->kcp_recv_buf) {
        free(gw->kcp_recv_buf);
        gw->kcp_recv_buf = NULL;
    }
    if (gw->kcp_fd >= 0) {
        close(gw->kcp_fd);
        gw->kcp_fd = -1;
    }

    /* 关闭异步 I/O */
    if (gw->io) ce_async_shutdown(gw->io);

    /* 关闭监听 socket */
    if (gw->listen_fd >= 0) close(gw->listen_fd);

    free(gw);
    CE_LOG_INFO("GATEWAY", "Gateway destroyed");
}

void ce_gateway_stop(CeGateway* gw) {
    if (!gw) return;
    gw->running = CE_FALSE;
    CE_LOG_INFO("GATEWAY", "Stop requested");
}

CeResult ce_gateway_run(CeGateway* gw) {
    if (!gw || !gw->io || gw->listen_fd < 0) {
        CE_LOG_ERROR("GATEWAY", "Gateway not properly initialized");
        return CE_ERR;
    }

    /* 注册信号处理 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gw_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* 忽略 SIGPIPE (避免 write 到已关闭连接导致进程退出) */
    signal(SIGPIPE, SIG_IGN);

    gw->running = CE_TRUE;
    g_stop_flag = 0;
    g_active_gw = gw;  /* 供 KCP output callback 使用 */

    /* 连接后端 */
    connect_backends(gw);

    /* 提交初始 accept (始终有一个 accept 在飞行) */
    ce_async_accept(gw->io, gw->listen_fd, NULL);

    /* 提交初始 KCP UDP recv (如果启用) */
    if (gw->kcp_enabled && gw->kcp_fd >= 0 && gw->kcp_recv_buf) {
        ce_async_recv(gw->io, gw->kcp_fd, gw->kcp_recv_buf,
                      CE_GW_KCP_RECV_BUF_SIZE, NULL);  /* user_data=NULL 标识 KCP recv */
        gw->kcp_recv_pending = CE_TRUE;
    }

    CE_LOG_INFO("GATEWAY", "Gateway event loop started (backend=%s, kcp=%s)",
                ce_async_backend_name(), gw->kcp_enabled ? "on" : "off");

    /* ==================== 主事件循环 ==================== */
    while (gw->running && !g_stop_flag) {
        /* 提交所有待处理请求到内核 */
        ce_async_submit(gw->io);

        /* 等待完成事件 (1s 超时做心跳检查) */
        int n = ce_async_wait(gw->io, 1, CE_GW_LOOP_TIMEOUT_MS);
        if (n < 0) {
            /* 信号中断 (EINTR) 时不退出，检查停止标志 */
            if (g_stop_flag) break;
            CE_LOG_ERROR("GATEWAY", "ce_async_wait failed");
            continue;
        }

        /* 处理完成事件 */
        for (int i = 0; i < n; i++) {
            const CeAsyncEvent* ev = ce_async_get_event(gw->io, i);
            if (!ev) continue;

            switch (ev->type) {
            /* ---- 新连接 ---- */
            case CE_ASYNC_ACCEPT: {
                if (ev->client_fd < 0) {
                    /* accept 失败，重新提交 */
                    CE_LOG_WARN("GATEWAY", "Accept failed (error=%d)", ev->error);
                    ce_async_accept(gw->io, gw->listen_fd, NULL);
                    break;
                }

                /* 设非阻塞 + nodelay */
                set_nonblock(ev->client_fd);
                set_nodelay(ev->client_fd);

                /* 创建连接对象 */
                CeGatewayConn* conn = conn_create(ev->client_fd, gw->next_conn_id++);
                if (!conn) {
                    close(ev->client_fd);
                    ce_async_accept(gw->io, gw->listen_fd, NULL);
                    break;
                }

                /* 注册到连接数组 */
                int slot = register_conn(gw, conn);
                if (slot < 0) {
                    close(ev->client_fd);
                    conn_destroy(conn);
                    ce_async_accept(gw->io, gw->listen_fd, NULL);
                    break;
                }

                CE_LOG_INFO("GATEWAY", "[+] Client connected (fd=%d, conn_id=%llu, slot=%d)",
                            ev->client_fd, (unsigned long long)conn->conn_id, slot);

                /* 立即提交 recv (user_data 指向连接) */
                ce_async_recv(gw->io, conn->fd, conn->recv_buf,
                              CE_GW_RECV_BUF_SIZE, conn);
                conn->recv_pending = CE_TRUE;

                /* 继续提交 accept (保持始终有一个 accept 在飞行) */
                ce_async_accept(gw->io, gw->listen_fd, NULL);
                break;
            }

            /* ---- 客户端数据到达 ---- */
            case CE_ASYNC_RECV: {
                /* 检查是否是 KCP UDP recv 事件 (user_data=NULL, fd=kcp_fd) */
                if (gw->kcp_enabled && gw->kcp_fd >= 0 && ev->fd == gw->kcp_fd) {
                    gw->kcp_recv_pending = CE_FALSE;
                    if (ev->nbytes > 0) {
                        /* 使用 recvfrom 获取对端地址 */
                        struct sockaddr_in peer_addr;
                        socklen_t addr_len = sizeof(peer_addr);
                        int n = recvfrom(gw->kcp_fd, gw->kcp_recv_buf,
                                         CE_GW_KCP_RECV_BUF_SIZE, 0,
                                         (struct sockaddr*)&peer_addr, &addr_len);
                        if (n > 0) {
                            handle_kcp_recv(gw, gw->kcp_recv_buf, n, &peer_addr);
                        }
                    }
                    /* 重新提交 KCP recv */
                    ce_async_recv(gw->io, gw->kcp_fd, gw->kcp_recv_buf,
                                  CE_GW_KCP_RECV_BUF_SIZE, NULL);
                    gw->kcp_recv_pending = CE_TRUE;
                    break;
                }

                CeGatewayConn* conn = (CeGatewayConn*)ev->user_data;
                if (!conn) break;

                conn->recv_pending = CE_FALSE;

                if (ev->nbytes <= 0) {
                    /* 连接关闭或错误 */
                    CE_LOG_INFO("GATEWAY", "[-] Client disconnected (fd=%d, conn_id=%llu, nbytes=%d)",
                                conn->fd, (unsigned long long)conn->conn_id, ev->nbytes);

                    /* 找到 slot 并关闭 */
                    for (int s = 0; s < gw->conn_capacity; s++) {
                        if (gw->conns[s] == conn) {
                            close_conn(gw, s);
                            break;
                        }
                    }
                    break;
                }

                /* 更新活跃时间 */
                conn->last_active_us = gw_now_us();

                /* 处理收到的数据 */
                process_recv_data(gw, conn, ev->nbytes);

                /* 如果连接正在关闭，不再提交 recv */
                if (conn->state == CE_GW_CONN_CLOSING) {
                    for (int s = 0; s < gw->conn_capacity; s++) {
                        if (gw->conns[s] == conn) {
                            close_conn(gw, s);
                            break;
                        }
                    }
                    break;
                }

                /* 继续提交 recv (追加到缓冲区剩余空间) */
                int remain = CE_GW_RECV_BUF_SIZE - conn->recv_offset;
                if (remain > 0) {
                    ce_async_recv(gw->io, conn->fd,
                                  conn->recv_buf + conn->recv_offset,
                                  remain, conn);
                    conn->recv_pending = CE_TRUE;
                } else {
                    /* 缓冲区满，重置 (异常情况) */
                    CE_LOG_WARN("GATEWAY", "Recv buffer full for conn=%llu, resetting",
                                (unsigned long long)conn->conn_id);
                    conn->recv_offset = 0;
                    ce_async_recv(gw->io, conn->fd, conn->recv_buf,
                                  CE_GW_RECV_BUF_SIZE, conn);
                    conn->recv_pending = CE_TRUE;
                }
                break;
            }

            /* ---- 发送完成 ---- */
            case CE_ASYNC_SEND: {
                /* 发送完成，可以释放发送缓冲区 (当前实现中发送缓冲区是栈分配的) */
                CeGatewayConn* conn = (CeGatewayConn*)ev->user_data;
                if (conn && ev->nbytes < 0) {
                    CE_LOG_WARN("GATEWAY", "Send error on conn=%llu (errno=%d)",
                                (unsigned long long)conn->conn_id, ev->error);
                    /* 标记关闭 */
                    conn->state = CE_GW_CONN_CLOSING;
                }
                break;
            }

            /* ---- 关闭完成 ---- */
            case CE_ASYNC_CLOSE:
                /* fd 已被内核关闭，无需额外操作 */
                break;

            /* ---- 后端 recv (user_data 编码了后端索引) ---- */
            case CE_ASYNC_READ:
                /* 不使用，忽略 */
                break;

            default:
                /* 检查是否是后端 recv (user_data 是后端索引编码) */
                if (ev->user_data) {
                    intptr_t idx = (intptr_t)ev->user_data;
                    if (idx >= 1 && idx <= gw->backend_count) {
                        handle_backend_recv(gw, (int)(idx - 1), ev->nbytes);
                    }
                }
                break;
            }
        }

        /* 处理需要关闭的连接 (CE_GW_CONN_CLOSING 状态) */
        for (int s = 0; s < gw->conn_capacity; s++) {
            CeGatewayConn* conn = gw->conns[s];
            if (conn && conn->state == CE_GW_CONN_CLOSING) {
                close_conn(gw, s);
            }
        }

        /* KCP 状态机定时驱动 (利用事件循环 1s timeout 间隙) */
        if (gw->kcp_enabled) {
            kcp_update_all(gw);
        }

        /* 心跳检查 (利用 timeout 间隙) */
        check_heartbeats(gw);

        /* 尝试重连断开的后端 (每 ~5 秒) */
        {
            uint64_t now = gw_now_us();
            uint64_t elapsed = (now - gw->last_backend_reconnect_us) / 1000000ULL;
            if (elapsed >= 5) {
                gw->last_backend_reconnect_us = now;
                for (int bi = 0; bi < gw->backend_count; bi++) {
                    CeGatewayBackend* be = &gw->backends[bi];
                    if (!be->connected) {
                        int fd = connect_to_backend(be->host, be->port);
                        if (fd >= 0) {
                            be->fd = fd;
                            be->connected = CE_TRUE;
                            be->recv_offset = 0;
                            be->fail_count = 0;
                            ce_async_recv(gw->io, fd, be->recv_buf,
                                          CE_GW_RECV_BUF_SIZE,
                                          (void*)(intptr_t)(bi + 1));
                            be->recv_pending = CE_TRUE;
                        }
                    }
                }
            }
        }
    }

    CE_LOG_INFO("GATEWAY", "Gateway event loop stopped");

    /* 优雅关闭：关闭所有客户端连接 (TCP 关闭 fd, KCP 只销毁上下文) */
    for (int i = 0; i < gw->conn_capacity; i++) {
        if (gw->conns[i]) {
            if (gw->conns[i]->protocol == CE_GW_PROTO_TCP && gw->conns[i]->fd >= 0) {
                ce_async_close(gw->io, gw->conns[i]->fd);
                gw->conns[i]->fd = -1;
            }
            conn_destroy(gw->conns[i]);
            gw->conns[i] = NULL;
        }
    }
    gw->conn_count = 0;

    /* 清除全局引用 */
    g_active_gw = NULL;

    /* 提交最后的 close 请求 */
    ce_async_submit(gw->io);
    ce_async_wait(gw->io, 1, 100);

    return CE_OK;
}
