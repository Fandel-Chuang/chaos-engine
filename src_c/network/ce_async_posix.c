/*
 * ChaosEngine POSIX 异步 I/O fallback
 *
 * 当 io_uring 不可用时使用 select() + 非阻塞 socket。
 * 功能子集：仅支持 accept/recv/send。
 */

#ifndef CHAOS_HAS_IO_URING

#include "network/ce_async_io.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <errno.h>

#define CE_ASYNC_MAX_EVENTS 256
#define CE_ASYNC_MAX_FDS    1024

typedef enum CeAsyncOpType {
    CE_ASYNC_OP_ACCEPT = 0,
    CE_ASYNC_OP_RECV,
    CE_ASYNC_OP_SEND,
    CE_ASYNC_OP_READ,
    CE_ASYNC_OP_CLOSE,
} CeAsyncOpType;

typedef struct CeAsyncOp {
    CeAsyncOpType type;
    int           fd;
    void*         buf;
    int           size;
    off_t         offset;
    void*         user_data;
    CeBool        submitted;
} CeAsyncOp;

struct CeAsyncContext {
    CeAsyncOp  ops[CE_ASYNC_MAX_FDS];
    int        op_count;

    CeAsyncEvent events[CE_ASYNC_MAX_EVENTS];
    int          event_count;
};

CeAsyncContext* ce_async_init(int queue_depth) {
    (void)queue_depth;
    CeAsyncContext* ctx = (CeAsyncContext*)calloc(1, sizeof(CeAsyncContext));
    if (!ctx) return NULL;
    CE_LOG_INFO("ASYNC", "POSIX fallback initialized");
    return ctx;
}

void ce_async_shutdown(CeAsyncContext* ctx) {
    if (!ctx) return;
    free(ctx);
    CE_LOG_INFO("ASYNC", "POSIX fallback shut down");
}

/* ---- 操作提交（记录到 ops 数组） ---- */

static CeAsyncOp* add_op(CeAsyncContext* ctx, CeAsyncOpType type, int fd,
                          void* buf, int size, off_t offset, void* user_data) {
    if (ctx->op_count >= CE_ASYNC_MAX_FDS) return NULL;
    CeAsyncOp* op = &ctx->ops[ctx->op_count++];
    op->type      = type;
    op->fd        = fd;
    op->buf       = buf;
    op->size      = size;
    op->offset    = offset;
    op->user_data = user_data;
    op->submitted = CE_FALSE;
    return op;
}

void ce_async_accept(CeAsyncContext* ctx, int listen_fd, void* user_data) {
    add_op(ctx, CE_ASYNC_OP_ACCEPT, listen_fd, NULL, 0, 0, user_data);
}

void ce_async_recv(CeAsyncContext* ctx, int fd, void* buf, int size, void* user_data) {
    add_op(ctx, CE_ASYNC_OP_RECV, fd, buf, size, 0, user_data);
}

void ce_async_send(CeAsyncContext* ctx, int fd, const void* buf, int size, void* user_data) {
    add_op(ctx, CE_ASYNC_OP_SEND, fd, (void*)buf, size, 0, user_data);
}

void ce_async_read(CeAsyncContext* ctx, int fd, void* buf, int size, off_t offset, void* user_data) {
    add_op(ctx, CE_ASYNC_OP_READ, fd, buf, size, offset, user_data);
}

void ce_async_close(CeAsyncContext* ctx, int fd) {
    add_op(ctx, CE_ASYNC_OP_CLOSE, fd, NULL, 0, 0, NULL);
}

/* ---- 事件处理 ---- */

int ce_async_submit(CeAsyncContext* ctx) {
    /* POSIX fallback: submit 不做实际操作，wait 时一起处理 */
    for (int i = 0; i < ctx->op_count; i++) {
        ctx->ops[i].submitted = CE_TRUE;
    }
    return ctx->op_count;
}

int ce_async_wait(CeAsyncContext* ctx, int min_events, int timeout_ms) {
    (void)min_events;

    fd_set read_fds, write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    int max_fd = -1;

    /* 构建 fd_set */
    for (int i = 0; i < ctx->op_count; i++) {
        CeAsyncOp* op = &ctx->ops[i];
        if (!op->submitted) continue;

        switch (op->type) {
        case CE_ASYNC_OP_ACCEPT:
        case CE_ASYNC_OP_RECV:
        case CE_ASYNC_OP_READ:
            FD_SET(op->fd, &read_fds);
            if (op->fd > max_fd) max_fd = op->fd;
            break;
        case CE_ASYNC_OP_SEND:
            FD_SET(op->fd, &write_fds);
            if (op->fd > max_fd) max_fd = op->fd;
            break;
        default:
            break;
        }
    }

    if (max_fd < 0) {
        ctx->event_count = 0;
        return 0;
    }

    /* select 等待 */
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    struct timeval* ptv = (timeout_ms >= 0) ? &tv : NULL;

    int ret = select(max_fd + 1, &read_fds, &write_fds, NULL, ptv);
    if (ret < 0) {
        if (errno == EINTR) { ctx->event_count = 0; return 0; }
        return -1;
    }

    /* 处理就绪的操作 */
    int count = 0;
    for (int i = 0; i < ctx->op_count && count < CE_ASYNC_MAX_EVENTS; i++) {
        CeAsyncOp* op = &ctx->ops[i];
        if (!op->submitted) continue;

        CeAsyncEvent* ev = &ctx->events[count];
        memset(ev, 0, sizeof(*ev));
        ev->fd        = op->fd;
        ev->user_data = op->user_data;

        switch (op->type) {
        case CE_ASYNC_OP_ACCEPT:
            if (FD_ISSET(op->fd, &read_fds)) {
                ev->type      = CE_ASYNC_ACCEPT;
                ev->client_fd = accept(op->fd, NULL, NULL);
                ev->error     = (ev->client_fd < 0) ? errno : 0;
                count++;
            }
            break;

        case CE_ASYNC_OP_RECV:
            if (FD_ISSET(op->fd, &read_fds)) {
                ev->type   = CE_ASYNC_RECV;
                ev->buf    = op->buf;
                ev->nbytes = recv(op->fd, op->buf, op->size, 0);
                ev->error  = (ev->nbytes < 0) ? errno : 0;
                count++;
            }
            break;

        case CE_ASYNC_OP_SEND:
            if (FD_ISSET(op->fd, &write_fds)) {
                ev->type   = CE_ASYNC_SEND;
                ev->nbytes = send(op->fd, op->buf, op->size, 0);
                ev->error  = (ev->nbytes < 0) ? errno : 0;
                count++;
            }
            break;

        case CE_ASYNC_OP_READ:
            if (FD_ISSET(op->fd, &read_fds)) {
                ev->type   = CE_ASYNC_READ;
                ev->buf    = op->buf;
                ev->nbytes = pread(op->fd, op->buf, op->size, op->offset);
                ev->error  = (ev->nbytes < 0) ? errno : 0;
                count++;
            }
            break;

        case CE_ASYNC_OP_CLOSE:
            ev->type = CE_ASYNC_CLOSE;
            close(op->fd);
            count++;
            break;
        }
    }

    /* 清除已处理的 ops */
    ctx->op_count = 0;
    ctx->event_count = count;
    return count;
}

const CeAsyncEvent* ce_async_get_event(CeAsyncContext* ctx, int index) {
    if (index < 0 || index >= ctx->event_count) return NULL;
    return &ctx->events[index];
}

CeResult ce_async_register_buffers(CeAsyncContext* ctx, void* buf, int buf_size, int buf_count) {
    (void)ctx; (void)buf; (void)buf_size; (void)buf_count;
    return CE_ERR;  /* POSIX fallback 不支持 */
}

CeBool ce_async_has_zcrx(void) {
    return CE_FALSE;
}

const char* ce_async_backend_name(void) {
    return "posix";
}

#endif /* !CHAOS_HAS_IO_URING */

/* 当 io_uring 启用时，提供一个空符号避免空编译单元警告 */
#ifdef CHAOS_HAS_IO_URING
static int __ce_async_posix_unused = 0;
#endif
