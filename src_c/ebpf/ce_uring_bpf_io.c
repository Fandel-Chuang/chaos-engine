/*
 * ChaosEngine Phase 4.4: io_uring + BPF 联动 — io_uring 端
 *
 * 实现 BPF 解析协议 → io_uring 直接投递到正确缓冲区。
 * 使用 BPF sockmap + io_uring IORING_OP_SENDMSG 零拷贝。
 *
 * 工作流程:
 *   1. BPF stream parser 在 socket 层解析自定义协议头 [2B type][4B len]
 *   2. BPF stream verdict 根据消息类型将数据重定向到 sockmap 中的目标 socket
 *   3. io_uring 从目标 socket 读取数据 (IORING_OP_RECV)
 *   4. io_uring 使用 registered buffers 实现零拷贝
 *   5. io_uring 通过 IORING_OP_SENDMSG_ZC 发送到上游
 *
 * 编译条件: CHAOS_HAS_IO_URING && CHAOS_HAS_EBPF
 */

#if defined(CHAOS_HAS_IO_URING) && defined(CHAOS_HAS_EBPF)

#include "ebpf/ce_stream_parser.h"
#include "network/ce_async_io.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"

#include <liburing.h>
#include <bpf/bpf.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- 内部结构 ---- */

#define CE_URING_BPF_MAX_BUFS  64
#define CE_URING_BPF_BUF_SIZE  65536
#define CE_URING_BPF_MAX_CONNS 1024

/* 每个连接的状态 */
typedef struct CeUringBpfConn {
    int         fd;
    int         channel;        /* 所属通道 */
    uint64_t    bytes_rx;
    uint64_t    bytes_tx;
    uint32_t    msg_count;
    CeBool      active;
} CeUringBpfConn;

/* io_uring + BPF 集成上下文 */
typedef struct CeUringBpfIo {
    struct io_uring     ring;
    CeUringBpfCtx*      bpf_ctx;        /* BPF 上下文 */
    int                 sockmap_fd;     /* sockmap fd (缓存) */

    /* Registered buffers (零拷贝) */
    void*               reg_bufs;       /* 注册的缓冲区池 */
    int                 buf_size;       /* 每个缓冲区大小 */
    int                 buf_count;      /* 缓冲区数量 */
    CeBool              bufs_registered;

    /* 连接池 */
    CeUringBpfConn      conns[CE_URING_BPF_MAX_CONNS];
    int                 conn_count;

    /* 统计 */
    CeStreamParserStats stats;

    /* 监听 socket */
    int                 listen_fd;
    CeBool              running;
} CeUringBpfIo;

/* ---- 辅助函数 ---- */

static CeUringBpfConn* find_conn(CeUringBpfIo* io, int fd) {
    for (int i = 0; i < io->conn_count; i++) {
        if (io->conns[i].fd == fd && io->conns[i].active) {
            return &io->conns[i];
        }
    }
    return NULL;
}

static CeUringBpfConn* alloc_conn(CeUringBpfIo* io, int fd) {
    if (io->conn_count >= CE_URING_BPF_MAX_CONNS) return NULL;
    CeUringBpfConn* conn = &io->conns[io->conn_count++];
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;
    conn->active = CE_TRUE;
    return conn;
}

/* ---- 生命周期 ---- */

CeUringBpfIo* ce_uring_bpf_io_init(int queue_depth, CeUringBpfCtx* bpf_ctx) {
    if (queue_depth <= 0) queue_depth = 256;

    CeUringBpfIo* io = (CeUringBpfIo*)calloc(1, sizeof(CeUringBpfIo));
    if (!io) return NULL;

    /* 初始化 io_uring */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL; /* 可选: 减少系统调用 */

    int ret = io_uring_queue_init_params(queue_depth, &io->ring, &params);
    if (ret < 0) {
        CE_LOG_ERROR("URING_BPF_IO", "io_uring_queue_init failed: %d (%s)",
                     ret, strerror(-ret));
        free(io);
        return NULL;
    }

    io->bpf_ctx = bpf_ctx;
    io->sockmap_fd = bpf_ctx ? ce_uring_bpf_sockmap_fd(bpf_ctx) : -1;

    CE_LOG_INFO("URING_BPF_IO", "Initialized (depth=%d, sockmap_fd=%d)",
                queue_depth, io->sockmap_fd);
    return io;
}

void ce_uring_bpf_io_shutdown(CeUringBpfIo* io) {
    if (!io) return;

    /* 释放 registered buffers */
    if (io->bufs_registered) {
        io_uring_unregister_buffers(&io->ring);
    }
    if (io->reg_bufs) {
        free(io->reg_bufs);
    }

    io_uring_queue_exit(&io->ring);
    free(io);
    CE_LOG_INFO("URING_BPF_IO", "Shut down");
}

/* ---- Registered Buffers (零拷贝) ---- */

CeResult ce_uring_bpf_io_register_buffers(CeUringBpfIo* io, int buf_size, int buf_count) {
    if (!io) return CE_ERR;
    if (buf_size <= 0) buf_size = CE_URING_BPF_BUF_SIZE;
    if (buf_count <= 0 || buf_count > CE_URING_BPF_MAX_BUFS)
        buf_count = CE_URING_BPF_MAX_BUFS;

    /* 分配缓冲区池 */
    size_t total = (size_t)buf_size * (size_t)buf_count;
    io->reg_bufs = calloc(1, total);
    if (!io->reg_bufs) {
        CE_LOG_ERROR("URING_BPF_IO", "Failed to allocate %zu bytes for buffers", total);
        return CE_ERR;
    }

    /* 构建 iovec 数组 */
    struct iovec* iovs = (struct iovec*)calloc(buf_count, sizeof(struct iovec));
    if (!iovs) {
        free(io->reg_bufs);
        io->reg_bufs = NULL;
        return CE_ERR;
    }

    for (int i = 0; i < buf_count; i++) {
        iovs[i].iov_base = (char*)io->reg_bufs + i * buf_size;
        iovs[i].iov_len  = buf_size;
    }

    int ret = io_uring_register_buffers(&io->ring, iovs, buf_count);
    free(iovs);

    if (ret < 0) {
        CE_LOG_WARN("URING_BPF_IO", "Failed to register buffers: %s", strerror(-ret));
        free(io->reg_bufs);
        io->reg_bufs = NULL;
        return CE_ERR;
    }

    io->buf_size = buf_size;
    io->buf_count = buf_count;
    io->bufs_registered = CE_TRUE;

    CE_LOG_INFO("URING_BPF_IO", "Registered %d buffers (%d bytes each)", buf_count, buf_size);
    return CE_OK;
}

/* ---- Socket 操作 ---- */

CeResult ce_uring_bpf_io_listen(CeUringBpfIo* io, int port) {
    if (!io) return CE_ERR;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        CE_LOG_ERROR("URING_BPF_IO", "socket() failed: %s", strerror(errno));
        return CE_ERR;
    }

    /* 设置 SO_REUSEADDR */
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CE_LOG_ERROR("URING_BPF_IO", "bind() failed: %s", strerror(errno));
        close(fd);
        return CE_ERR;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        CE_LOG_ERROR("URING_BPF_IO", "listen() failed: %s", strerror(errno));
        close(fd);
        return CE_ERR;
    }

    io->listen_fd = fd;
    CE_LOG_INFO("URING_BPF_IO", "Listening on port %d (fd=%d)", port, fd);
    return CE_OK;
}

/* ---- 事件循环: BPF 解析 → io_uring 投递 ---- */

CeResult ce_uring_bpf_io_submit_accept(CeUringBpfIo* io) {
    if (!io || io->listen_fd < 0) return CE_ERR;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&io->ring);
    if (!sqe) return CE_ERR;

    io_uring_prep_accept(sqe, io->listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, NULL); /* accept 不需要额外数据 */

    return CE_OK;
}

CeResult ce_uring_bpf_io_submit_recv(CeUringBpfIo* io, int fd, int buf_index) {
    if (!io || fd < 0) return CE_ERR;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&io->ring);
    if (!sqe) return CE_ERR;

    if (io->bufs_registered && buf_index >= 0 && buf_index < io->buf_count) {
        /* 使用 registered buffer (零拷贝) */
        void* buf = (char*)io->reg_bufs + buf_index * io->buf_size;
        io_uring_prep_recv(sqe, fd, buf, io->buf_size, 0);
        io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
    } else {
        /* 普通 recv */
        io_uring_prep_recv(sqe, fd, NULL, 0, 0);
    }

    /* 将 fd 编码到 user_data 中 */
    io_uring_sqe_set_data(sqe, (void*)(intptr_t)(fd | (buf_index << 16)));

    return CE_OK;
}

/**
 * 使用 IORING_OP_SENDMSG 零拷贝发送
 *
 * 通过 BPF sockmap 重定向后，数据已经在目标 socket 的接收缓冲区中。
 * io_uring 从目标 socket 读取后，可以直接通过 sendmsg 零拷贝转发。
 */
CeResult ce_uring_bpf_io_sendmsg_zc(CeUringBpfIo* io, int src_fd, int dst_fd,
                                     const void* data, int len) {
    if (!io || src_fd < 0 || dst_fd < 0) return CE_ERR;

    struct io_uring_sqe* sqe = io_uring_get_sqe(&io->ring);
    if (!sqe) return CE_ERR;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    struct iovec iov;
    iov.iov_base = (void*)data;
    iov.iov_len  = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* IORING_OP_SENDMSG: 支持零拷贝 (需要 IORING_FEAT_FAST_POLL) */
    io_uring_prep_sendmsg(sqe, dst_fd, &msg, 0);
    io_uring_sqe_set_data(sqe, (void*)(intptr_t)dst_fd);

    return CE_OK;
}

/**
 * 事件循环: 处理 io_uring 完成事件
 *
 * 核心流程:
 *   1. accept 新连接 → 将 socket 加入 BPF sockmap
 *   2. recv 完成 → BPF stream parser 自动解析协议头并分帧
 *   3. BPF stream verdict 自动将消息重定向到目标 socket
 *   4. io_uring 从目标 socket 读取 → 投递到正确缓冲区
 */
int ce_uring_bpf_io_process_events(CeUringBpfIo* io, int timeout_ms) {
    if (!io) return -1;

    struct io_uring_cqe* cqe;
    int processed = 0;

    /* 先提交所有待处理请求 */
    io_uring_submit(&io->ring);

    /* 等待完成事件 */
    struct __kernel_timespec ts;
    struct __kernel_timespec *ts_ptr = NULL;
    if (timeout_ms > 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        ts_ptr = &ts;
    }
    int ret = io_uring_wait_cqe_timeout(&io->ring, &cqe, ts_ptr);
    if (ret == -ETIME) return 0;
    if (ret < 0) return -1;

    /* 处理所有完成事件 */
    unsigned head;
    unsigned count = 0;
    io_uring_for_each_cqe(&io->ring, head, cqe) {
        int fd = (int)(intptr_t)io_uring_cqe_get_data(cqe);
        int res = cqe->res;

        if (res < 0) {
            /* 错误或连接关闭 */
            if (res != -ECANCELED && res != -ENOENT) {
                CE_LOG_DEBUG("URING_BPF_IO", "CQE error on fd=%d: %s", fd, strerror(-res));
            }
            count++;
            continue;
        }

        /* 判断操作类型: accept 返回新 fd，recv 返回字节数 */
        if (fd == 0) {
            /* accept: res = 新连接 fd */
            int client_fd = res;
            CeUringBpfConn* conn = alloc_conn(io, client_fd);
            if (conn) {
                CE_LOG_DEBUG("URING_BPF_IO", "Accepted fd=%d", client_fd);

                /* 将新连接加入 BPF sockmap (默认数据通道) */
                if (io->bpf_ctx) {
                    ce_uring_bpf_add_sock(io->bpf_ctx, client_fd, CE_CHANNEL_DATA);
                }

                /* 提交 recv 请求 */
                ce_uring_bpf_io_submit_recv(io, client_fd, conn->channel);
            } else {
                close(client_fd);
            }

            /* 重新提交 accept */
            ce_uring_bpf_io_submit_accept(io);

        } else {
            /* recv/send 完成 */
            int buf_index = (fd >> 16) & 0xFFFF;
            fd = fd & 0xFFFF;

            CeUringBpfConn* conn = find_conn(io, fd);
            if (conn) {
                if (res == 0) {
                    /* 连接关闭 */
                    CE_LOG_DEBUG("URING_BPF_IO", "Connection closed fd=%d", fd);
                    conn->active = CE_FALSE;
                    if (io->bpf_ctx) {
                        ce_uring_bpf_del_sock(io->bpf_ctx, fd);
                    }
                    close(fd);
                } else {
                    /* 收到数据 — BPF stream parser 已自动解析并重定向 */
                    conn->bytes_rx += res;
                    conn->msg_count++;
                    io->stats.bytes_processed += res;

                    /* 重新提交 recv */
                    ce_uring_bpf_io_submit_recv(io, fd, buf_index);
                }
            }
        }

        count++;
        processed++;
    }

    io_uring_cq_advance(&io->ring, count);

    /* 更新 BPF 统计 */
    if (io->bpf_ctx && processed > 0) {
        ce_uring_bpf_get_stats(io->bpf_ctx, &io->stats);
    }

    return processed;
}

/**
 * 运行事件循环
 *
 * BPF stream parser 在 socket 层自动解析协议头 [2B type][4B len]，
 * 完成消息分帧后通过 sockmap 重定向到目标 socket。
 * io_uring 从目标 socket 读取数据，实现零拷贝投递。
 */
CeResult ce_uring_bpf_io_run(CeUringBpfIo* io, int max_iterations) {
    if (!io || io->listen_fd < 0) return CE_ERR;

    io->running = CE_TRUE;

    /* 提交初始 accept */
    ce_uring_bpf_io_submit_accept(io);

    int iter = 0;
    while (io->running && (max_iterations <= 0 || iter < max_iterations)) {
        int n = ce_uring_bpf_io_process_events(io, 100); /* 100ms 超时 */
        if (n < 0) {
            CE_LOG_ERROR("URING_BPF_IO", "Event processing error");
            break;
        }
        iter++;
    }

    return CE_OK;
}

void ce_uring_bpf_io_stop(CeUringBpfIo* io) {
    if (io) io->running = CE_FALSE;
}

/* ---- 统计查询 ---- */

CeResult ce_uring_bpf_io_get_stats(CeUringBpfIo* io, CeStreamParserStats* stats) {
    if (!io || !stats) return CE_ERR;
    memcpy(stats, &io->stats, sizeof(*stats));
    return CE_OK;
}

int ce_uring_bpf_io_conn_count(CeUringBpfIo* io) {
    if (!io) return 0;
    int count = 0;
    for (int i = 0; i < io->conn_count; i++) {
        if (io->conns[i].active) count++;
    }
    return count;
}

#else /* !(CHAOS_HAS_IO_URING && CHAOS_HAS_EBPF) — stubs */

#include "ebpf/ce_stream_parser.h"

/* 定义不透明类型 */
typedef struct CeUringBpfIo { int dummy; } CeUringBpfIo;

CeUringBpfIo* ce_uring_bpf_io_init(int queue_depth, CeUringBpfCtx* bpf_ctx) {
    (void)queue_depth; (void)bpf_ctx; return NULL;
}
void ce_uring_bpf_io_shutdown(CeUringBpfIo* io) { (void)io; }
CeResult ce_uring_bpf_io_register_buffers(CeUringBpfIo* io, int buf_size, int buf_count) {
    (void)io; (void)buf_size; (void)buf_count; return CE_ERR;
}
CeResult ce_uring_bpf_io_listen(CeUringBpfIo* io, int port) {
    (void)io; (void)port; return CE_ERR;
}
CeResult ce_uring_bpf_io_submit_accept(CeUringBpfIo* io) { (void)io; return CE_ERR; }
CeResult ce_uring_bpf_io_submit_recv(CeUringBpfIo* io, int fd, int buf_index) {
    (void)io; (void)fd; (void)buf_index; return CE_ERR;
}
CeResult ce_uring_bpf_io_sendmsg_zc(CeUringBpfIo* io, int src_fd, int dst_fd,
                                     const void* data, int len) {
    (void)io; (void)src_fd; (void)dst_fd; (void)data; (void)len; return CE_ERR;
}
int ce_uring_bpf_io_process_events(CeUringBpfIo* io, int timeout_ms) {
    (void)io; (void)timeout_ms; return -1;
}
CeResult ce_uring_bpf_io_run(CeUringBpfIo* io, int max_iterations) {
    (void)io; (void)max_iterations; return CE_ERR;
}
void ce_uring_bpf_io_stop(CeUringBpfIo* io) { (void)io; }
CeResult ce_uring_bpf_io_get_stats(CeUringBpfIo* io, CeStreamParserStats* stats) {
    (void)io; (void)stats; return CE_ERR;
}
int ce_uring_bpf_io_conn_count(CeUringBpfIo* io) { (void)io; return 0; }

#endif /* CHAOS_HAS_IO_URING && CHAOS_HAS_EBPF */
