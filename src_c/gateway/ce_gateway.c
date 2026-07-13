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
#include <ctype.h>

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
 * SHA1 + Base64 (纯 C99，不依赖 OpenSSL)
 * ================================================================ */

/* SHA1 上下文 */
typedef struct {
    uint32_t h[5];        /* 状态 */
    uint64_t total_bits;  /* 总比特数 */
    uint8_t  buf[64];     /* 输入缓冲区 */
    int      buf_len;     /* 缓冲区当前长度 */
} ce_sha1_ctx;

#define SHA1_ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_init(ce_sha1_ctx* c) {
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->total_bits = 0;
    c->buf_len = 0;
}

static void sha1_process_block(ce_sha1_ctx* c, const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24)
             | ((uint32_t)block[i * 4 + 1] << 16)
             | ((uint32_t)block[i * 4 + 2] << 8)
             |  (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = SHA1_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    uint32_t f, k;
    for (int i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & cc) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = SHA1_ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = SHA1_ROL(b, 30);
        b = a;
        a = temp;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_update(ce_sha1_ctx* c, const uint8_t* data, size_t len) {
    c->total_bits += (uint64_t)len * 8;
    while (len > 0) {
        int copy = 64 - c->buf_len;
        if ((size_t)copy > len) copy = (int)len;
        memcpy(c->buf + c->buf_len, data, (size_t)copy);
        c->buf_len += copy;
        data += copy;
        len -= copy;
        if (c->buf_len == 64) {
            sha1_process_block(c, c->buf);
            c->buf_len = 0;
        }
    }
}

static void sha1_final(ce_sha1_ctx* c, uint8_t out[20]) {
    /* 补 0x80 */
    c->buf[c->buf_len++] = 0x80;
    /* 补 0 到 len ≡ 56 (mod 64) */
    if (c->buf_len > 56) {
        while (c->buf_len < 64) c->buf[c->buf_len++] = 0;
        sha1_process_block(c, c->buf);
        c->buf_len = 0;
    }
    while (c->buf_len < 56) c->buf[c->buf_len++] = 0;
    /* 追加 8 字节大端序原始长度 */
    c->buf[56] = (uint8_t)(c->total_bits >> 56);
    c->buf[57] = (uint8_t)(c->total_bits >> 48);
    c->buf[58] = (uint8_t)(c->total_bits >> 40);
    c->buf[59] = (uint8_t)(c->total_bits >> 32);
    c->buf[60] = (uint8_t)(c->total_bits >> 24);
    c->buf[61] = (uint8_t)(c->total_bits >> 16);
    c->buf[62] = (uint8_t)(c->total_bits >> 8);
    c->buf[63] = (uint8_t)(c->total_bits);
    sha1_process_block(c, c->buf);

    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

/** 一次性 SHA1 计算 */
static void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    ce_sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

static const char BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Base64 编码 (out 需要至少 ceil(len/3)*4 + 1 字节) */
static void base64_encode(const uint8_t* data, size_t len, char* out) {
    int oi = 0;
    size_t i;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[oi++] = BASE64_TABLE[(v >> 18) & 0x3F];
        out[oi++] = BASE64_TABLE[(v >> 12) & 0x3F];
        out[oi++] = BASE64_TABLE[(v >> 6) & 0x3F];
        out[oi++] = BASE64_TABLE[v & 0x3F];
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        out[oi++] = BASE64_TABLE[(v >> 18) & 0x3F];
        out[oi++] = BASE64_TABLE[(v >> 12) & 0x3F];
        out[oi++] = (i + 1 < len) ? BASE64_TABLE[(v >> 6) & 0x3F] : '=';
        out[oi++] = '=';
    }
    out[oi] = '\0';
}

/* ================================================================
 * WebSocket 帧编解码
 * ================================================================ */

/* WebSocket opcodes (RFC 6455) */
#define WS_OPCODE_CONTINUATION  0x0
#define WS_OPCODE_TEXT          0x1
#define WS_OPCODE_BINARY        0x2
#define WS_OPCODE_CLOSE         0x8
#define WS_OPCODE_PING          0x9
#define WS_OPCODE_PONG          0xA

/**
 * 解析 WebSocket 帧
 * @param buf      接收缓冲区
 * @param len      缓冲区数据长度
 * @param fin      [out] FIN 标志
 * @param opcode   [out] 操作码
 * @param payload  [out] 指向 payload 的指针 (已 unmask)
 * @param payload_len [out] payload 长度
 * @return >0 成功(帧总长度), 0 数据不完整, <0 错误
 */
static int ws_parse_frame(const uint8_t* buf, int len,
                          int* fin, int* opcode,
                          const uint8_t** payload, int* payload_len) {
    if (len < 2) return 0;

    *fin = (buf[0] >> 7) & 1;
    *opcode = buf[0] & 0x0F;

    int masked = (buf[1] >> 7) & 1;
    int plen = buf[1] & 0x7F;

    int header_len = 2;

    if (plen == 126) {
        if (len < 4) return 0;
        plen = (buf[2] << 8) | buf[3];
        header_len = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        /* 只取低 32 位 (足够游戏网关场景) */
        plen = (buf[6] << 24) | (buf[7] << 16) | (buf[8] << 8) | buf[9];
        header_len = 10;
    }

    if (masked) header_len += 4;

    if (len < header_len + plen) return 0; /* 数据不完整 */

    /* unmask payload (原地修改, recv_buf 是可写的) */
    if (masked) {
        uint8_t mask[4];
        memcpy(mask, buf + header_len - 4, 4);
        uint8_t* p = (uint8_t*)buf + header_len;
        for (int i = 0; i < plen; i++) {
            p[i] ^= mask[i % 4];
        }
    }

    *payload = buf + header_len;
    *payload_len = plen;
    return header_len + plen;
}

/**
 * 封装 WebSocket 帧 (服务端发送, 不 mask)
 * @param payload     数据
 * @param payload_len 数据长度
 * @param opcode      操作码
 * @param out         输出缓冲区
 * @param out_max     输出缓冲区大小
 * @return >0 帧总长度, <0 错误
 */
static int ws_pack_frame(const uint8_t* payload, int payload_len,
                         int opcode, uint8_t* out, int out_max) {
    int header_len;
    if (payload_len < 126) {
        header_len = 2;
    } else if (payload_len <= 65535) {
        header_len = 4;
    } else {
        header_len = 10;
    }

    if (header_len + payload_len > out_max) return -1;

    out[0] = (uint8_t)(0x80 | opcode); /* FIN=1 */
    if (payload_len < 126) {
        out[1] = (uint8_t)payload_len;
    } else if (payload_len <= 65535) {
        out[1] = 126;
        out[2] = (uint8_t)(payload_len >> 8);
        out[3] = (uint8_t)(payload_len & 0xFF);
    } else {
        out[1] = 127;
        memset(out + 2, 0, 8);
        out[9] = (uint8_t)(payload_len & 0xFF);
        out[8] = (uint8_t)((payload_len >> 8) & 0xFF);
        out[7] = (uint8_t)((payload_len >> 16) & 0xFF);
        out[6] = (uint8_t)((payload_len >> 24) & 0xFF);
    }

    memcpy(out + header_len, payload, (size_t)payload_len);
    return header_len + payload_len;
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
    conn->ws_state = 0;  /* 0=等待握手判断 */

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
 * WebSocket 握手
 * ================================================================ */

/** 查找子字符串 (大小写不敏感) */
static char* ci_strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    size_t nl = strlen(needle);
    if (nl == 0) return (char*)haystack;
    const char* p = haystack;
    while (*p) {
        size_t i = 0;
        while (i < nl && p[i] && (p[i] == needle[i] || tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])))
            i++;
        if (i == nl) return (char*)p;
        p++;
    }
    return NULL;
}

/**
 * 尝试 WebSocket 握手
 * @param conn  连接 (recv_buf 中有数据, recv_offset 是长度)
 * @param gw    Gateway 实例 (用于提交 async send)
 * @return 1=握手成功, 0=数据不完整(继续等), -1=不是WebSocket(当TCP处理)
 */
static int try_ws_handshake(CeGatewayConn* conn, CeGateway* gw) {
    if (conn->recv_offset < 4) return 0;

    /* 检查是否以 "GET " 开头 (HTTP 请求) */
    if (memcmp(conn->recv_buf, "GET ", 4) != 0) {
        conn->ws_state = -1; /* 不是 WebSocket */
        conn->protocol = CE_GW_PROTO_TCP;
        return -1;
    }

    /* 检查是否有完整的 HTTP 头 (\r\n\r\n) */
    char* end = NULL;
    for (int i = 0; i <= conn->recv_offset - 4; i++) {
        if (memcmp(conn->recv_buf + i, "\r\n\r\n", 4) == 0) {
            end = (char*)(conn->recv_buf + i);
            break;
        }
    }
    if (!end) return 0; /* 握手数据不完整 */

    /* 解析 Sec-WebSocket-Key (大小写不敏感) */
    char* key = ci_strstr((char*)conn->recv_buf, "Sec-WebSocket-Key:");
    if (!key) return -1; /* 不是合法的 WebSocket 请求 */
    key += strlen("Sec-WebSocket-Key:");
    /* 跳过空格 */
    while (*key == ' ' || *key == '\t') key++;

    char* key_end = strstr(key, "\r\n");
    if (!key_end) return -1;

    int key_len = (int)(key_end - key);
    char client_key[128];
    if (key_len >= (int)sizeof(client_key)) key_len = (int)sizeof(client_key) - 1;
    memcpy(client_key, key, (size_t)key_len);
    client_key[key_len] = '\0';

    /* 去除尾部空白 */
    while (key_len > 0 && (client_key[key_len - 1] == ' ' || client_key[key_len - 1] == '\r' || client_key[key_len - 1] == '\n')) {
        client_key[--key_len] = '\0';
    }

    /* 计算 Sec-WebSocket-Accept = base64(sha1(client_key + magic)) */
    const char* magic = CE_GW_WS_MAGIC;
    size_t magic_len = strlen(magic);
    size_t ck_len = strlen(client_key);
    size_t combined_len = ck_len + magic_len;

    /* 构造 combined 缓冲区 (栈上) */
    char combined[256];
    if (combined_len >= sizeof(combined)) {
        CE_LOG_WARN("GATEWAY", "WS handshake: key too long (%zu)", ck_len);
        return -1;
    }
    memcpy(combined, client_key, ck_len);
    memcpy(combined + ck_len, magic, magic_len);

    /* SHA1 */
    uint8_t sha1_out[20];
    sha1((const uint8_t*)combined, combined_len, sha1_out);

    /* Base64 */
    char accept_value[64];
    base64_encode(sha1_out, 20, accept_value);

    /* 构造 101 响应 (使用 send_buf 避免栈悬空) */
    char response[512];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept_value);

    if (resp_len < 0 || resp_len >= (int)sizeof(response)) {
        CE_LOG_ERROR("GATEWAY", "WS handshake response too long");
        return -1;
    }

    /* 将响应拷贝到 send_buf (io_uring 异步发送，不能使用栈缓冲区) */
    if (resp_len > CE_GW_SEND_BUF_SIZE) {
        CE_LOG_ERROR("GATEWAY", "WS handshake response exceeds send buffer");
        return -1;
    }
    memcpy(conn->send_buf, response, (size_t)resp_len);
    ce_async_send(gw->io, conn->fd, conn->send_buf, resp_len, conn);

    conn->ws_state = 1;   /* 握手完成 */
    conn->protocol = CE_GW_PROTO_WS;

    CE_LOG_INFO("GATEWAY", "WebSocket handshake completed (conn_id=%llu, fd=%d)",
                (unsigned long long)conn->conn_id, conn->fd);

    /* 清空握手缓冲区，后续数据走 WS 帧解析 */
    conn->recv_offset = 0;

    return 1;
}

/**
 * 发送 WebSocket 帧 (自动封装帧头)
 * @param gw    Gateway 实例
 * @param conn  连接
 * @param data  payload 数据 (协议帧 [4B total_len][2B msg_type][N payload])
 * @param len   payload 长度
 * @param opcode WS 操作码 (0x1=文本, 0x2=二进制)
 */
static void ws_send_frame(CeGateway* gw, CeGatewayConn* conn,
                          const uint8_t* data, int len, int opcode) {
    /* 使用 send_buf 封装 WS 帧 (帧头最大 10 字节) */
    int frame_len = ws_pack_frame(data, len, opcode, conn->send_buf, CE_GW_SEND_BUF_SIZE);
    if (frame_len < 0) {
        CE_LOG_ERROR("GATEWAY", "WS pack frame failed (len=%d, buf=%d)",
                     len, CE_GW_SEND_BUF_SIZE);
        return;
    }
    ce_async_send(gw->io, conn->fd, conn->send_buf, frame_len, conn);
}

/* ================================================================
 * 消息处理
 * ================================================================ */

/** 向客户端发送数据 (自动区分 TCP/KCP/WS) */
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
    } else if (conn->protocol == CE_GW_PROTO_WS && conn->ws_state == 1) {
        /* WebSocket：封装成 WS 帧再发送 (二进制帧) */
        ws_send_frame(gw, conn, data, len, WS_OPCODE_BINARY);
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

    /* ---- 新连接：判断是否为 WebSocket ---- */
    /* ws_state==0 表示尚未判断协议，protocol==TCP 表示新 TCP 连接 (排除 KCP) */
    if (conn->ws_state == 0 && conn->protocol == CE_GW_PROTO_TCP && gw->ws_enabled) {
        int ret = try_ws_handshake(conn, gw);
        if (ret == 0) {
            /* 握手数据不完整，继续等 recv (不处理消息) */
            return;
        }
        if (ret > 0) {
            /* WebSocket 握手成功，后续走 WS 帧解析
             * recv_offset 已被清零，重新提交 recv 由事件循环处理 */
            return;
        }
        /* ret < 0: 不是 WebSocket，当纯 TCP 处理 (已设 protocol=TCP, ws_state=-1)
         * 继续走下面的 TCP 协议帧解析 (recv_buf 中有已接收数据) */
    }

    /* ---- WebSocket 帧解析 (握手完成后) ---- */
    if (conn->protocol == CE_GW_PROTO_WS && conn->ws_state == 1) {
        /* 循环解析所有完整 WS 帧 */
        while (conn->recv_offset >= 2) {
            int fin, opcode;
            const uint8_t* payload;
            int payload_len;
            int consumed = ws_parse_frame(conn->recv_buf, conn->recv_offset,
                                          &fin, &opcode, &payload, &payload_len);
            if (consumed == 0) break; /* 数据不完整 */

            /* 处理 WS 帧 */
            switch (opcode) {
            case WS_OPCODE_TEXT:
            case WS_OPCODE_BINARY:
                /* payload 就是协议帧 [4B total_len][2B msg_type][N payload] */
                if (payload_len > 0) {
                    handle_message(gw, conn, payload, payload_len);
                }
                break;
            case WS_OPCODE_CLOSE:
                CE_LOG_INFO("GATEWAY", "WS close frame from conn=%llu",
                            (unsigned long long)conn->conn_id);
                conn->state = CE_GW_CONN_CLOSING;
                break;
            case WS_OPCODE_PING:
                /* 回 PONG (使用相同 payload) */
                ws_send_frame(gw, conn, payload, payload_len, WS_OPCODE_PONG);
                CE_LOG_DEBUG("GATEWAY", "WS PING from conn=%llu, sent PONG",
                             (unsigned long long)conn->conn_id);
                break;
            case WS_OPCODE_PONG:
                /* 心跳响应，活跃时间已在调用处更新 */
                CE_LOG_TRACE("GATEWAY", "WS PONG from conn=%llu",
                             (unsigned long long)conn->conn_id);
                break;
            default:
                CE_LOG_WARN("GATEWAY", "WS unknown opcode=0x%X from conn=%llu",
                            opcode, (unsigned long long)conn->conn_id);
                break;
            }

            /* 移动剩余数据 */
            int remaining = conn->recv_offset - consumed;
            if (remaining > 0) {
                memmove(conn->recv_buf, conn->recv_buf + consumed, (size_t)remaining);
            }
            conn->recv_offset = remaining;

            if (conn->state == CE_GW_CONN_CLOSING) break;
        }
        return;
    }

    /* ---- TCP/KCP 协议帧解析 (原有逻辑) ---- */
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
        /* TCP 或 WebSocket 连接，各有独立 fd */
        const char* proto_name = (conn->protocol == CE_GW_PROTO_WS) ? "WS" : "TCP";
        CE_LOG_INFO("GATEWAY", "Closing %s conn %llu (fd=%d)",
                    proto_name, (unsigned long long)conn->conn_id, conn->fd);

        /* 同步关闭 fd（避免 io_uring 异步关闭导致 fd 复用竞争） */
        if (conn->fd >= 0) {
            close(conn->fd);
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
    gw->ws_enabled = config ? config->ws_enabled : 1;   /* 默认启用 WebSocket */

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

    CE_LOG_INFO("GATEWAY", "Gateway created: port=%d, max_conns=%d, kcp=%s, ws=%s, backend=%s, queue_depth=%d",
                gw->port, gw->max_conns, gw->kcp_enabled ? "on" : "off",
                gw->ws_enabled ? "on" : "off",
                ce_async_backend_name(), CE_GW_QUEUE_DEPTH);

    return gw;
}

void ce_gateway_destroy(CeGateway* gw) {
    if (!gw) return;

    /* 关闭所有连接 (KCP 连接不关闭共享的 kcp_fd; TCP/WS 连接关闭各自的 fd) */
    for (int i = 0; i < gw->conn_capacity; i++) {
        if (gw->conns[i]) {
            if ((gw->conns[i]->protocol == CE_GW_PROTO_TCP ||
                 gw->conns[i]->protocol == CE_GW_PROTO_WS) &&
                gw->conns[i]->fd >= 0) {
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

    CE_LOG_INFO("GATEWAY", "Gateway event loop started (backend=%s, kcp=%s, ws=%s)",
                ce_async_backend_name(), gw->kcp_enabled ? "on" : "off",
                gw->ws_enabled ? "on" : "off");

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

    /* 优雅关闭：关闭所有客户端连接 (TCP/WS 关闭 fd, KCP 只销毁上下文) */
    for (int i = 0; i < gw->conn_capacity; i++) {
        if (gw->conns[i]) {
            if ((gw->conns[i]->protocol == CE_GW_PROTO_TCP ||
                 gw->conns[i]->protocol == CE_GW_PROTO_WS) &&
                gw->conns[i]->fd >= 0) {
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
