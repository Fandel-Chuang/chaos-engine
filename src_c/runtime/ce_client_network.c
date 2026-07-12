/*
 * ChaosEngine 客户端网络模块 — 实现
 *
 * 连接到 Gateway (TCP:9000)，发送加入/位置消息，
 * 接收实体更新。支持文本和二进制两种协议。
 *
 * 文本协议（默认，与 Gateway server.lua TCP handler 兼容）：
 *   "JOIN:<entity_id>\n"
 *   "POS:<entity_id>:<x>:<y>:<z>\n"
 *   "UPDATE:<entity_id>:<x>:<y>:<z>\n"
 *
 * 二进制协议（与 Game server 兼容）：
 *   [4B total_len][2B msg_type][N body]
 */

#define _POSIX_C_SOURCE 200112L
#include "runtime/ce_client_network.h"
#include "server/ce_game_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
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

static uint16_t read_u16(const uint8_t* buf) {
    return (uint16_t)(((uint16_t)buf[0] << 8) | (uint16_t)buf[1]);
}

static uint32_t read_u32(const uint8_t* buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
         | ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
}

static float read_float(const uint8_t* buf) {
    float val;
    memcpy(&val, buf, sizeof(val));
    return val;
}

/* ================================================================
 * 内部辅助：Socket
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

/**
 * 连接到指定主机和端口
 * 使用非阻塞连接 + select() 超时
 */
static int tcp_connect(const char* host, int port, int timeout_ms) {
    if (!host || port <= 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* 解析主机名 */
    struct hostent* he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "[ClientNet] gethostbyname(%s) failed: %s\n",
                host, strerror(errno));
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
        fprintf(stderr, "[ClientNet] connect(%s:%d) failed: %s\n",
                host, port, strerror(errno));
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
            fprintf(stderr, "[ClientNet] connect(%s:%d) timeout (%d ms)\n",
                    host, port, timeout_ms);
            close(fd);
            return -1;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
            fprintf(stderr, "[ClientNet] connect(%s:%d) error: %s\n",
                    host, port, so_error ? strerror(so_error) : "unknown");
            close(fd);
            return -1;
        }
    }

    set_nodelay(fd);
    printf("[ClientNet] Connected to %s:%d (fd=%d)\n", host, port, fd);
    return fd;
}

/* ================================================================
 * 内部辅助：生成随机 entity_id
 * ================================================================ */

static uint32_t generate_entity_id(void) {
    /* 用时间 + PID 生成一个简单的唯一 ID */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t id = (uint32_t)(ts.tv_nsec & 0xFFFF) |
                  ((uint32_t)(getpid() & 0xFF) << 16) |
                  ((uint32_t)(ts.tv_sec & 0xFF) << 24);
    return id;
}

/* ================================================================
 * 内部辅助：发送数据
 * ================================================================ */

static int send_all(int fd, const void* data, size_t size) {
    const uint8_t* ptr = (const uint8_t*)data;
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t n = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 非阻塞，稍后重试 */
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
                nanosleep(&ts, NULL);
                continue;
            }
            return -1;
        }
        ptr += n;
        remaining -= (size_t)n;
    }
    return (int)size;
}

/* ================================================================
 * 内部辅助：文本协议解析
 * ================================================================ */

/**
 * 解析 "UPDATE:<entity_id>:<x>:<y>:<z>\n" 格式的消息
 * 返回 0 成功，-1 解析失败
 */
static int parse_text_update(const char* line, CeClientEntity* out_entity) {
    uint32_t eid;
    float x, y, z;
    if (sscanf(line, "UPDATE:%u:%f:%f:%f", &eid, &x, &y, &z) == 4) {
        out_entity->entity_id = eid;
        out_entity->x = x;
        out_entity->y = y;
        out_entity->z = z;
        return 0;
    }
    return -1;
}

/* ================================================================
 * API 实现
 * ================================================================ */

CeClientNet* ce_client_net_connect(const char* host, int port) {
    if (!host) host = CE_CLIENT_DEFAULT_HOST;
    if (port <= 0) port = CE_CLIENT_DEFAULT_PORT;

    CeClientNet* ctx = (CeClientNet*)calloc(1, sizeof(CeClientNet));
    if (!ctx) {
        fprintf(stderr, "[ClientNet] Failed to allocate context\n");
        return NULL;
    }

    ctx->sock_fd = -1;
    ctx->connected = 0;
    ctx->use_binary = 0;   /* 默认文本协议，与 Gateway server.lua 兼容 */
    ctx->entity_id = 0;
    ctx->recv_offset = 0;
    ctx->entity_count = 0;

    /* 尝试连接 */
    int fd = tcp_connect(host, port, 3000);  /* 3 秒超时 */
    if (fd < 0) {
        printf("[ClientNet] Could not connect to Gateway at %s:%d "
               "(Gateway may not be running — continuing without network)\n",
               host, port);
        /* 不释放 ctx，让调用者可以检查连接状态 */
        return ctx;
    }

    ctx->sock_fd = fd;
    ctx->connected = 1;

    /* 生成实体 ID */
    ctx->entity_id = generate_entity_id();

    printf("[ClientNet] Client network initialized (entity_id=%u)\n", ctx->entity_id);
    return ctx;
}

void ce_client_net_disconnect(CeClientNet* ctx) {
    if (!ctx) return;

    if (ctx->sock_fd >= 0) {
        printf("[ClientNet] Disconnecting from Gateway (fd=%d)\n", ctx->sock_fd);
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
    }

    ctx->connected = 0;
    ctx->recv_offset = 0;
    ctx->entity_count = 0;

    printf("[ClientNet] Disconnected. "
           "Sent: %llu bytes, %llu msgs | Recv: %llu bytes, %llu msgs\n",
           (unsigned long long)ctx->bytes_sent,
           (unsigned long long)ctx->msgs_sent,
           (unsigned long long)ctx->bytes_recv,
           (unsigned long long)ctx->msgs_recv);

    free(ctx);
}

void ce_client_net_set_binary_mode(CeClientNet* ctx, int use_binary) {
    if (!ctx) return;
    ctx->use_binary = use_binary ? 1 : 0;
}

CeResult ce_client_net_send_join(CeClientNet* ctx, uint32_t entity_id) {
    if (!ctx || !ctx->connected || ctx->sock_fd < 0) return CE_ERR;

    ctx->entity_id = entity_id;

    if (ctx->use_binary) {
        /* Game server: JOIN_REQUEST only needs the header. */
        uint8_t buf[6];
        write_u32(buf, (uint32_t)sizeof(buf));
        write_u16(buf + 4, MSG_JOIN_REQUEST);
        if (send_all(ctx->sock_fd, buf, sizeof(buf)) < 0) {
            fprintf(stderr, "[ClientNet] Failed to send JOIN (binary)\n");
            return CE_ERR;
        }
        ctx->bytes_sent += sizeof(buf);
    } else {
        /* 文本协议 */
        char msg[64];
        int len = snprintf(msg, sizeof(msg), "JOIN:%u\n", entity_id);
        if (send_all(ctx->sock_fd, msg, (size_t)len) < 0) {
            fprintf(stderr, "[ClientNet] Failed to send JOIN\n");
            return CE_ERR;
        }
        ctx->bytes_sent += (uint64_t)len;
    }

    ctx->msgs_sent++;
    printf("[ClientNet] Sent JOIN (entity_id=%u)\n", entity_id);
    return CE_OK;
}

CeResult ce_client_net_send_position(CeClientNet* ctx, float x, float y, float z) {
    if (!ctx || !ctx->connected || ctx->sock_fd < 0) return CE_ERR;

    if (ctx->use_binary) {
        /* Game server only expects raw xyz floats in the body. */
        uint8_t buf[18];
        write_u32(buf, (uint32_t)sizeof(buf));
        write_u16(buf + 4, MSG_POSITION_UPDATE);
        memcpy(buf + 6, &x, sizeof(float));
        memcpy(buf + 10, &y, sizeof(float));
        memcpy(buf + 14, &z, sizeof(float));
        if (send_all(ctx->sock_fd, buf, sizeof(buf)) < 0) {
            return CE_ERR;
        }
        ctx->bytes_sent += sizeof(buf);
    } else {
        /* 文本协议 */
        char msg[128];
        int len = snprintf(msg, sizeof(msg), "POS:%u:%.3f:%.3f:%.3f\n",
                           ctx->entity_id, x, y, z);
        if (send_all(ctx->sock_fd, msg, (size_t)len) < 0) {
            return CE_ERR;
        }
        ctx->bytes_sent += (uint64_t)len;
    }

    ctx->msgs_sent++;
    return CE_OK;
}

/**
 * 处理接收缓冲区中的完整消息（文本或二进制）
 * 返回处理的消息数量
 */
static int process_recv_buffer(CeClientNet* ctx) {
    if (!ctx || ctx->recv_offset <= 0) return 0;

    int messages_received = 0;

    if (ctx->use_binary) {
        /* 二进制协议解析 */
        while (1) {
            /* 需要至少 6 字节头部 */
            if (ctx->recv_offset < 6) break;

            uint32_t total_len = read_u32(ctx->recv_buf);
            uint16_t msg_type = read_u16(ctx->recv_buf + 4);
            if (total_len < 6) {
                fprintf(stderr, "[ClientNet] Binary message too short: %u bytes\n", total_len);
                ctx->recv_offset = 0;
                break;
            }
            uint32_t body_len = total_len - 6;

            if (total_len > CE_CLIENT_RECV_BUF_SIZE) {
                fprintf(stderr, "[ClientNet] Binary message too large: %u bytes\n", total_len);
                ctx->recv_offset = 0;
                break;
            }

            if (ctx->recv_offset < (int)total_len) break;  /* 不完整 */

            /* 处理消息 */
            if (msg_type == MSG_ENTITY_STATE && body_len >= 2) {
                uint16_t count = (uint16_t)(((uint16_t)ctx->recv_buf[6] << 8) | ctx->recv_buf[7]);
                int entities_in_msg = 0;

                for (int i = 0; i < (int)count && i < CE_CLIENT_MAX_ENTITIES; i++) {
                    int off = 8 + i * 16;
                    if (off + 16 > (int)body_len) break;

                    const uint8_t* ent = ctx->recv_buf + 6 + off;
                    uint32_t eid = read_u32(ent);
                    float ex, ey, ez;
                    memcpy(&ex, ent + 4, sizeof(float));
                    memcpy(&ey, ent + 8, sizeof(float));
                    memcpy(&ez, ent + 12, sizeof(float));

                    /* 更新或添加实体 */
                    int found = 0;
                    for (int j = 0; j < ctx->entity_count; j++) {
                        if (ctx->entities[j].entity_id == eid) {
                            ctx->entities[j].x = ex;
                            ctx->entities[j].y = ey;
                            ctx->entities[j].z = ez;
                            found = 1;
                            break;
                        }
                    }
                    if (!found && ctx->entity_count < CE_CLIENT_MAX_ENTITIES) {
                        ctx->entities[ctx->entity_count].entity_id = eid;
                        ctx->entities[ctx->entity_count].x = ex;
                        ctx->entities[ctx->entity_count].y = ey;
                        ctx->entities[ctx->entity_count].z = ez;
                        ctx->entity_count++;
                    }
                    entities_in_msg++;
                }

                messages_received++;
                ctx->msgs_recv++;

                printf("[ClientNet] Received ENTITY_UPDATE: %d entities (total visible: %d)\n",
                       entities_in_msg, ctx->entity_count);
            } else if (msg_type == MSG_JOIN_RESPONSE) {
                /* 服务端分配了实体 ID，更新本地 entity_id */
                if (body_len >= 8) {
                    /* body 格式: [4B result][4B assigned_entity_id] (大端) */
                    uint32_t assigned_id = read_u32(ctx->recv_buf + 6 + 4);
                    ctx->entity_id = assigned_id;
                    printf("[ClientNet] Join confirmed, assigned entity_id=%u (result=%d, body_len=%u)\n",
                           assigned_id, (int)read_u32(ctx->recv_buf + 6), body_len);
                }
                messages_received++;
                ctx->msgs_recv++;
            }

            /* 移除已处理的消息 */
            int remaining = ctx->recv_offset - (int)total_len;
            if (remaining > 0) {
                memmove(ctx->recv_buf, ctx->recv_buf + total_len, (size_t)remaining);
            }
            ctx->recv_offset = remaining;
        }
    } else {
        /* 文本协议解析 */
        while (1) {
            /* 查找换行符 */
            int nl_pos = -1;
            for (int i = 0; i < ctx->recv_offset; i++) {
                if (ctx->recv_buf[i] == '\n') {
                    nl_pos = i;
                    break;
                }
            }
            if (nl_pos < 0) break;  /* 没有完整行 */

            /* 提取一行 */
            ctx->recv_buf[nl_pos] = '\0';  /* 临时终止 */
            const char* line = (const char*)ctx->recv_buf;

            /* 解析 UPDATE 消息 */
            if (strncmp(line, "UPDATE:", 7) == 0) {
                CeClientEntity entity;
                if (parse_text_update(line, &entity) == 0) {
                    /* 更新或添加实体 */
                    int found = 0;
                    for (int j = 0; j < ctx->entity_count; j++) {
                        if (ctx->entities[j].entity_id == entity.entity_id) {
                            ctx->entities[j] = entity;
                            found = 1;
                            break;
                        }
                    }
                    if (!found && ctx->entity_count < CE_CLIENT_MAX_ENTITIES) {
                        ctx->entities[ctx->entity_count++] = entity;
                    }

                    messages_received++;
                    ctx->msgs_recv++;

                    printf("[ClientNet] Entity %u: (%.2f, %.2f, %.2f)\n",
                           entity.entity_id, entity.x, entity.y, entity.z);
                }
            } else if (strncmp(line, "JOINED:", 7) == 0) {
                /* 加入确认 */
                uint32_t assigned_id;
                if (sscanf(line, "JOINED:%u", &assigned_id) == 1) {
                    ctx->entity_id = assigned_id;
                    printf("[ClientNet] Join confirmed, assigned entity_id=%u\n", assigned_id);
                }
                messages_received++;
                ctx->msgs_recv++;
            }

            /* 移除已处理的行 */
            int remaining = ctx->recv_offset - (nl_pos + 1);
            if (remaining > 0) {
                memmove(ctx->recv_buf, ctx->recv_buf + nl_pos + 1, (size_t)remaining);
            }
            ctx->recv_offset = remaining;
        }
    }

    return messages_received;
}

int ce_client_net_poll(CeClientNet* ctx) {
    if (!ctx || !ctx->connected) return 0;

    int messages_received = 0;

    /* ---- 先处理缓冲区中已有的数据 ---- */
    messages_received += process_recv_buffer(ctx);

    /* ---- 从 socket 读取更多数据 ---- */
    if (ctx->sock_fd < 0) {
        return messages_received;
    }

    int space = CE_CLIENT_RECV_BUF_SIZE - ctx->recv_offset;
    if (space <= 0) {
        /* 缓冲区满，丢弃 */
        ctx->recv_offset = 0;
        space = CE_CLIENT_RECV_BUF_SIZE;
    }

    ssize_t n = recv(ctx->sock_fd,
                     ctx->recv_buf + ctx->recv_offset,
                     (size_t)space,
                     MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return messages_received;  /* 无新数据 */
        }
        if (errno == ECONNRESET || errno == EPIPE) {
            printf("[ClientNet] Connection lost (fd=%d): %s\n",
                   ctx->sock_fd, strerror(errno));
            ctx->connected = 0;
            close(ctx->sock_fd);
            ctx->sock_fd = -1;
            return -1;
        }
        /* 其他错误 */
        return messages_received;
    }
    if (n == 0) {
        printf("[ClientNet] Gateway closed connection\n");
        ctx->connected = 0;
        close(ctx->sock_fd);
        ctx->sock_fd = -1;
        return -1;
    }

    ctx->recv_offset += (int)n;
    ctx->bytes_recv += (uint64_t)n;

    /* ---- 解析新收到的数据 ---- */
    messages_received += process_recv_buffer(ctx);

    return messages_received;
}

int ce_client_net_entity_count(const CeClientNet* ctx) {
    if (!ctx) return 0;
    return ctx->entity_count;
}

const CeClientEntity* ce_client_net_get_entity(const CeClientNet* ctx, int index) {
    if (!ctx || index < 0 || index >= ctx->entity_count) return NULL;
    return &ctx->entities[index];
}

int ce_client_net_is_connected(const CeClientNet* ctx) {
    return ctx && ctx->connected ? 1 : 0;
}

const char* ce_client_net_status(const CeClientNet* ctx) {
    if (!ctx) return "NULL";
    if (!ctx->connected) return "DISCONNECTED";
    return "CONNECTED";
}
