/*
 * ChaosEngine Game↔DBProxy 同步模块 — 实现
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 *
 * 二进制协议：
 *   [4B frame_len][2B frame_seq][8B timestamp][2B entity_count][N entities...]
 *   每个 entity: [8B entity_id][2B component_type][4B data_len][N data]
 */

/* POSIX 特性宏：C99 下启用 clock_gettime, usleep, select 等 */
#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "sync/ce_sync.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"

#include <stdlib.h>
#include <string.h>
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

/** 环形缓冲区条目 */
#define CE_SYNC_RING_BUF_SIZE    CE_SYNC_RING_SIZE

/** 帧头固定大小（不含 entity 数据） */
#define CE_SYNC_FRAME_HEADER_SIZE   16   /* 4+2+8+2 */

/** 单个 entity 头大小 */
#define CE_SYNC_ENTITY_HEADER_SIZE  14   /* 8+2+4 */

/** 最大响应缓冲区 */
#define CE_SYNC_RECV_BUF_SIZE       CE_SYNC_MAX_FRAME_SIZE

/** 响应实体池大小 */
#define CE_SYNC_MAX_RESP_ENTITIES   64

/* ---- 内部结构 ---- */

/** 同步上下文（不透明） */
struct CeSyncContext {
    /* 连接 */
    int             sock_fd;          /* 当前 TCP socket */
    CeBool          connected;

    /* 配置 */
    CeSyncConfig    config;
    char            host_buf[256];    /* 主地址副本 */
    char            backup_buf[256];  /* 备地址副本 */
    CeBool          using_backup;     /* 是否正在使用备用 */

    /* 心跳 */
    uint64_t        last_heartbeat_us;
    uint16_t        next_frame_seq;

    /* 环形缓冲区（帧序号追踪） */
    uint16_t        ring_seq[CE_SYNC_RING_BUF_SIZE];
    int             ring_head;
    int             ring_count;

    /* 接收缓冲区 */
    uint8_t         recv_buf[CE_SYNC_RECV_BUF_SIZE];
    int             recv_offset;
    int             recv_expected;

    /* 响应实体池 */
    CeSyncEntity    resp_entities[CE_SYNC_MAX_RESP_ENTITIES];
    int             resp_entity_count;
};

/* ---- 内部辅助：时间戳 ---- */

static uint64_t ce_sync_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ---- 内部辅助：网络 ---- */

/** 设置 socket 为非阻塞 */
static CeResult ce_sync_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return CE_ERR;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return CE_ERR;
    return CE_OK;
}

/** 设置 TCP_NODELAY */
static CeResult ce_sync_set_nodelay(int fd) {
    int opt = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        CE_LOG_WARN("SYNC", "Failed to set TCP_NODELAY: %s", strerror(errno));
        return CE_ERR;
    }
    return CE_OK;
}

/** 连接到指定 host:port，返回 socket fd */
static int ce_sync_connect_to(const char* host, int port, int timeout_ms) {
    if (!host || port <= 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CE_LOG_ERROR("SYNC", "socket() failed: %s", strerror(errno));
        return -1;
    }

    /* 解析主机名 */
    struct hostent* he = gethostbyname(host);
    if (!he) {
        CE_LOG_ERROR("SYNC", "gethostbyname(%s) failed: %s", host, strerror(h_errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* 非阻塞连接 */
    ce_sync_set_nonblock(fd);

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        CE_LOG_ERROR("SYNC", "connect(%s:%d) failed: %s", host, port, strerror(errno));
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
            CE_LOG_ERROR("SYNC", "connect(%s:%d) timeout/error", host, port);
            close(fd);
            return -1;
        }

        /* 检查连接是否成功 */
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            CE_LOG_ERROR("SYNC", "connect(%s:%d) SO_ERROR: %s",
                         host, port, so_error ? strerror(so_error) : "unknown");
            close(fd);
            return -1;
        }
    }

    ce_sync_set_nodelay(fd);

    CE_LOG_INFO("SYNC", "Connected to %s:%d (fd=%d)", host, port, fd);
    return fd;
}

/** 关闭当前连接 */
static void ce_sync_disconnect(CeSyncContext* ctx) {
    if (ctx->sock_fd >= 0) {
        CE_LOG_INFO("SYNC", "Disconnecting fd=%d", ctx->sock_fd);
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }
    ctx->connected = CE_FALSE;
    ctx->recv_offset = 0;
    ctx->recv_expected = 0;
}

/* ---- 内部辅助：序列化 ---- */

/** 写入大端 uint16_t */
static void ce_sync_write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val);
}

/** 写入大端 uint32_t */
static void ce_sync_write_u32(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

/** 写入大端 uint64_t */
static void ce_sync_write_u64(uint8_t* buf, uint64_t val) {
    buf[0] = (uint8_t)(val >> 56);
    buf[1] = (uint8_t)(val >> 48);
    buf[2] = (uint8_t)(val >> 40);
    buf[3] = (uint8_t)(val >> 32);
    buf[4] = (uint8_t)(val >> 24);
    buf[5] = (uint8_t)(val >> 16);
    buf[6] = (uint8_t)(val >> 8);
    buf[7] = (uint8_t)(val);
}

/** 读取大端 uint16_t */
static uint16_t ce_sync_read_u16(const uint8_t* buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

/** 读取大端 uint32_t */
static uint32_t ce_sync_read_u32(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
}

/** 读取大端 uint64_t */
static uint64_t ce_sync_read_u64(const uint8_t* buf) {
    return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48)
         | ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32)
         | ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16)
         | ((uint64_t)buf[6] << 8)  |  (uint64_t)buf[7];
}

/* ---- 内部辅助：环形缓冲区 ---- */

static void ce_sync_ring_push(CeSyncContext* ctx, uint16_t seq) {
    ctx->ring_seq[ctx->ring_head] = seq;
    ctx->ring_head = (ctx->ring_head + 1) % CE_SYNC_RING_BUF_SIZE;
    if (ctx->ring_count < CE_SYNC_RING_BUF_SIZE) {
        ctx->ring_count++;
    }
}

/* ---- 生命周期 ---- */

CeSyncContext* ce_sync_init(const CeSyncConfig* config) {
    CeSyncContext* ctx = (CeSyncContext*)calloc(1, sizeof(CeSyncContext));
    if (!ctx) {
        CE_LOG_ERROR("SYNC", "Failed to allocate CeSyncContext");
        return NULL;
    }

    ctx->sock_fd = -1;
    ctx->connected = CE_FALSE;
    ctx->using_backup = CE_FALSE;
    ctx->next_frame_seq = 0;
    ctx->ring_head = 0;
    ctx->ring_count = 0;
    ctx->recv_offset = 0;
    ctx->recv_expected = 0;
    ctx->last_heartbeat_us = ce_sync_now_us();

    /* 默认配置 */
    if (config) {
        ctx->config = *config;
    } else {
        memset(&ctx->config, 0, sizeof(ctx->config));
        ctx->config.dbproxy_port  = CE_SYNC_DEFAULT_PORT;
        ctx->config.backup_port   = CE_SYNC_DEFAULT_PORT;
        ctx->config.heartbeat_ms  = CE_SYNC_DEFAULT_HEARTBEAT_MS;
        ctx->config.timeout_ms    = CE_SYNC_DEFAULT_TIMEOUT_MS;
    }

    /* 确保有效端口 */
    if (ctx->config.dbproxy_port <= 0) ctx->config.dbproxy_port = CE_SYNC_DEFAULT_PORT;
    if (ctx->config.backup_port  <= 0) ctx->config.backup_port  = CE_SYNC_DEFAULT_PORT;
    if (ctx->config.heartbeat_ms <= 0) ctx->config.heartbeat_ms = CE_SYNC_DEFAULT_HEARTBEAT_MS;
    if (ctx->config.timeout_ms   <= 0) ctx->config.timeout_ms   = CE_SYNC_DEFAULT_TIMEOUT_MS;

    /* 复制主机名字符串 */
    if (ctx->config.dbproxy_host) {
        strncpy(ctx->host_buf, ctx->config.dbproxy_host, sizeof(ctx->host_buf) - 1);
        ctx->host_buf[sizeof(ctx->host_buf) - 1] = '\0';
        ctx->config.dbproxy_host = ctx->host_buf;
    }
    if (ctx->config.backup_host) {
        strncpy(ctx->backup_buf, ctx->config.backup_host, sizeof(ctx->backup_buf) - 1);
        ctx->backup_buf[sizeof(ctx->backup_buf) - 1] = '\0';
        ctx->config.backup_host = ctx->backup_buf;
    }

    /* 连接到主 DBProxy */
    if (ctx->config.dbproxy_host) {
        ctx->sock_fd = ce_sync_connect_to(
            ctx->config.dbproxy_host,
            ctx->config.dbproxy_port,
            ctx->config.timeout_ms
        );
        if (ctx->sock_fd >= 0) {
            ctx->connected = CE_TRUE;
        } else {
            CE_LOG_WARN("SYNC", "Failed to connect to primary DBProxy %s:%d",
                        ctx->config.dbproxy_host, ctx->config.dbproxy_port);
        }
    } else {
        CE_LOG_WARN("SYNC", "No dbproxy_host configured, sync module started without connection");
    }

    CE_LOG_INFO("SYNC", "Sync module initialized (heartbeat=%dms, timeout=%dms)",
                ctx->config.heartbeat_ms, ctx->config.timeout_ms);
    return ctx;
}

void ce_sync_shutdown(CeSyncContext* ctx) {
    if (!ctx) return;
    ce_sync_disconnect(ctx);
    free(ctx);
    CE_LOG_INFO("SYNC", "Sync module shut down");
}

/* ---- 帧发送 ---- */

CeResult ce_sync_send_frame(CeSyncContext* ctx, const CeSyncFrame* frame) {
    if (!ctx || !frame) return CE_ERR;
    if (!ctx->connected || ctx->sock_fd < 0) {
        CE_LOG_WARN("SYNC", "Cannot send frame: not connected");
        return CE_ERR;
    }

    /* 计算总大小 */
    uint32_t total_data_len = 0;
    uint16_t i;
    for (i = 0; i < frame->entity_count; i++) {
        total_data_len += CE_SYNC_ENTITY_HEADER_SIZE + frame->entities[i].data_len;
    }

    uint32_t frame_len = CE_SYNC_FRAME_HEADER_SIZE + total_data_len;

    if (frame_len > CE_SYNC_MAX_FRAME_SIZE) {
        CE_LOG_ERROR("SYNC", "Frame too large: %u bytes (max %u)", frame_len, CE_SYNC_MAX_FRAME_SIZE);
        return CE_ERR;
    }

    /* 分配发送缓冲区 */
    uint8_t* send_buf = (uint8_t*)malloc(frame_len);
    if (!send_buf) {
        CE_LOG_ERROR("SYNC", "Failed to allocate send buffer (%u bytes)", frame_len);
        return CE_ERR;
    }

    /* 序列化帧头 */
    uint8_t* p = send_buf;
    ce_sync_write_u32(p, frame_len);          p += 4;
    ce_sync_write_u16(p, frame->frame_seq);   p += 2;
    ce_sync_write_u64(p, frame->timestamp);   p += 8;
    ce_sync_write_u16(p, frame->entity_count); p += 2;

    /* 序列化实体 */
    for (i = 0; i < frame->entity_count; i++) {
        const CeSyncEntity* ent = &frame->entities[i];
        ce_sync_write_u64(p, ent->entity_id);        p += 8;
        ce_sync_write_u16(p, ent->component_type);   p += 2;
        ce_sync_write_u32(p, ent->data_len);         p += 4;
        if (ent->data_len > 0 && ent->data) {
            memcpy(p, ent->data, ent->data_len);
            p += ent->data_len;
        }
    }

    /* TCP 发送（非阻塞，循环发送直到全部发送完毕） */
    uint32_t sent = 0;
    int retries = 0;
    while (sent < frame_len) {
        ssize_t n = send(ctx->sock_fd, send_buf + sent, frame_len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 非阻塞模式下缓冲区满，短暂等待后重试 */
                if (retries++ > 100) {
                    CE_LOG_ERROR("SYNC", "Send timeout after %d retries", retries);
                    free(send_buf);
                    return CE_ERR;
                }
                usleep(1000); /* 1ms */
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                CE_LOG_ERROR("SYNC", "Connection lost during send");
                ctx->connected = CE_FALSE;
                free(send_buf);
                return CE_ERR;
            }
            CE_LOG_ERROR("SYNC", "send() failed: %s", strerror(errno));
            free(send_buf);
            return CE_ERR;
        }
        sent += (uint32_t)n;
        retries = 0;
    }

    free(send_buf);

    /* 记录到环形缓冲区 */
    ce_sync_ring_push(ctx, frame->frame_seq);

    CE_LOG_TRACE("SYNC", "Sent frame seq=%u, entities=%u, size=%u",
                 frame->frame_seq, frame->entity_count, frame_len);
    return CE_OK;
}

/* ---- 响应轮询 ---- */

CeResult ce_sync_poll(CeSyncContext* ctx, CeSyncResponse* response) {
    if (!ctx || !response) return CE_ERR;
    if (!ctx->connected || ctx->sock_fd < 0) return CE_ERR;

    memset(response, 0, sizeof(CeSyncResponse));

    /* 读取数据到接收缓冲区 */
    ssize_t n = recv(ctx->sock_fd,
                     ctx->recv_buf + ctx->recv_offset,
                     CE_SYNC_RECV_BUF_SIZE - ctx->recv_offset,
                     0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return CE_ERR; /* 无数据 */
        }
        if (errno == ECONNRESET || errno == EPIPE) {
            CE_LOG_WARN("SYNC", "Connection reset by DBProxy");
            ctx->connected = CE_FALSE;
            return CE_ERR;
        }
        CE_LOG_ERROR("SYNC", "recv() failed: %s", strerror(errno));
        return CE_ERR;
    }
    if (n == 0) {
        CE_LOG_WARN("SYNC", "DBProxy closed connection");
        ctx->connected = CE_FALSE;
        return CE_ERR;
    }

    ctx->recv_offset += (int)n;

    /* 检查是否有完整帧 */
    if (ctx->recv_offset < 4) {
        return CE_ERR; /* 还没收到帧长度 */
    }

    uint32_t frame_len = ce_sync_read_u32(ctx->recv_buf);
    if (frame_len > CE_SYNC_RECV_BUF_SIZE) {
        CE_LOG_ERROR("SYNC", "Response frame too large: %u", frame_len);
        ctx->recv_offset = 0;
        return CE_ERR;
    }
    if (frame_len < CE_SYNC_FRAME_HEADER_SIZE) {
        CE_LOG_ERROR("SYNC", "Response frame too small: %u", frame_len);
        ctx->recv_offset = 0;
        return CE_ERR;
    }

    if (ctx->recv_offset < (int)frame_len) {
        return CE_ERR; /* 帧不完整，等待更多数据 */
    }

    /* 解析响应帧 */
    const uint8_t* p = ctx->recv_buf + 4; /* 跳过 frame_len */

    response->frame_seq  = ce_sync_read_u16(p);  p += 2;
    response->timestamp  = ce_sync_read_u64(p);  p += 8;
    response->entity_count = ce_sync_read_u16(p); p += 2;

    /* 判断响应类型 */
    if (response->frame_seq == CE_SYNC_HEARTBEAT_SEQ) {
        response->type = CE_SYNC_RESP_HEARTBEAT;
    } else if (response->entity_count > 0) {
        response->type = CE_SYNC_RESP_DATA;
    } else {
        response->type = CE_SYNC_RESP_OK;
    }

    /* 解析实体 */
    if (response->entity_count > 0) {
        uint16_t ec = response->entity_count;
        if (ec > CE_SYNC_MAX_RESP_ENTITIES) {
            CE_LOG_WARN("SYNC", "Response entity count %u exceeds max %u, truncating",
                        ec, CE_SYNC_MAX_RESP_ENTITIES);
            ec = CE_SYNC_MAX_RESP_ENTITIES;
        }

        uint16_t i;
        for (i = 0; i < ec; i++) {
            CeSyncEntity* ent = &ctx->resp_entities[i];

            /* 检查边界 */
            if ((size_t)(p - ctx->recv_buf) + CE_SYNC_ENTITY_HEADER_SIZE > (size_t)frame_len) {
                CE_LOG_ERROR("SYNC", "Response frame truncated at entity %u", i);
                break;
            }

            ent->entity_id      = ce_sync_read_u64(p);  p += 8;
            ent->component_type = ce_sync_read_u16(p);  p += 2;
            ent->data_len       = ce_sync_read_u32(p);  p += 4;

            if (ent->data_len > 0) {
                if ((size_t)(p - ctx->recv_buf) + ent->data_len > (size_t)frame_len) {
                    CE_LOG_ERROR("SYNC", "Entity %u data exceeds frame boundary", i);
                    ent->data_len = 0;
                    ent->data = NULL;
                    break;
                }
                ent->data = (uint8_t*)p; /* 指向接收缓冲区 */
                p += ent->data_len;
            } else {
                ent->data = NULL;
            }
        }
        ctx->resp_entity_count = (int)ec;
        response->entities = ctx->resp_entities;
    } else {
        response->entities = NULL;
        ctx->resp_entity_count = 0;
    }

    /* 处理剩余数据（如果有粘包） */
    int consumed = (int)frame_len;
    if (ctx->recv_offset > consumed) {
        memmove(ctx->recv_buf, ctx->recv_buf + consumed,
                (size_t)(ctx->recv_offset - consumed));
    }
    ctx->recv_offset -= consumed;

    CE_LOG_TRACE("SYNC", "Received response seq=%u, type=%d, entities=%u",
                 response->frame_seq, response->type, response->entity_count);
    return CE_OK;
}

/* ---- 心跳 ---- */

CeResult ce_sync_heartbeat(CeSyncContext* ctx) {
    if (!ctx) return CE_ERR;
    if (!ctx->connected || ctx->sock_fd < 0) return CE_ERR;

    /* 构建心跳帧 */
    CeSyncFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.frame_seq    = CE_SYNC_HEARTBEAT_SEQ;
    frame.timestamp    = ce_sync_now_us();
    frame.entity_count = 0;
    frame.entities     = NULL;

    CeResult ret = ce_sync_send_frame(ctx, &frame);
    if (ret == CE_OK) {
        ctx->last_heartbeat_us = frame.timestamp;
        CE_LOG_TRACE("SYNC", "Heartbeat sent");
    }
    return ret;
}

/* ---- 故障切换 ---- */

CeResult ce_sync_switch_dbproxy(CeSyncContext* ctx) {
    if (!ctx) return CE_ERR;

    /* 断开当前连接 */
    ce_sync_disconnect(ctx);

    /* 确定目标 */
    const char* target_host;
    int         target_port;

    if (ctx->using_backup) {
        /* 切回主 */
        target_host = ctx->host_buf;
        target_port = ctx->config.dbproxy_port;
    } else {
        /* 切到备 */
        target_host = ctx->backup_buf;
        target_port = ctx->config.backup_port;
    }

    if (!target_host || target_host[0] == '\0') {
        CE_LOG_ERROR("SYNC", "No %s DBProxy configured",
                     ctx->using_backup ? "primary" : "backup");
        return CE_ERR;
    }

    CE_LOG_INFO("SYNC", "Switching to %s DBProxy %s:%d",
                ctx->using_backup ? "primary" : "backup",
                target_host, target_port);

    int fd = ce_sync_connect_to(target_host, target_port, ctx->config.timeout_ms);
    if (fd < 0) {
        CE_LOG_ERROR("SYNC", "Failed to connect to %s DBProxy %s:%d",
                     ctx->using_backup ? "primary" : "backup",
                     target_host, target_port);
        return CE_ERR;
    }

    ctx->sock_fd = fd;
    ctx->connected = CE_TRUE;
    ctx->using_backup = ctx->using_backup ? CE_FALSE : CE_TRUE;
    ctx->recv_offset = 0;
    ctx->recv_expected = 0;

    CE_LOG_INFO("SYNC", "Switched to %s DBProxy successfully",
                ctx->using_backup ? "backup" : "primary");
    return CE_OK;
}

/* ---- 状态查询 ---- */

CeBool ce_sync_is_connected(const CeSyncContext* ctx) {
    if (!ctx) return CE_FALSE;
    return ctx->connected;
}

const char* ce_sync_current_endpoint(const CeSyncContext* ctx) {
    if (!ctx) return "none";
    if (!ctx->connected) return "disconnected";
    return ctx->using_backup ? ctx->backup_buf : ctx->host_buf;
}
