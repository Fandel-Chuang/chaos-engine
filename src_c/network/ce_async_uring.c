/*
 * ChaosEngine io_uring 异步 I/O 后端
 *
 * 基于 liburing 2.x，提供：
 * - 异步 accept/recv/send
 * - 异步文件 read/write
 * - Registered Buffers（可选）
 * - 单线程事件循环
 *
 * 编译条件: CMake 仅在 CHAOS_HAS_IO_URING 时编译此文件
 */

#include "network/ce_async_io.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"

#include <liburing.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ---- 内部结构 ---- */

#define CE_ASYNC_MAX_EVENTS 16384

/* io_uring user_data 编码方案：
 *   低 48 位存 user_data 指针（应用层传入）
 *   高 16 位存 CeAsyncEventType
 * 这样不再需要 op_ctx_pool，彻底解决飞行中 op_ctx 被覆盖的问题。
 */
#define CE_UD_TYPE_SHIFT  48
#define CE_UD_TYPE_MASK   0xFFFFULL
#define CE_UD_PTR_MASK    0xFFFFFFFFFFFFULL

static inline uint64_t ce_ud_encode(CeAsyncEventType type, void* user_data) {
    return ((uint64_t)type << CE_UD_TYPE_SHIFT) | ((uint64_t)(uintptr_t)user_data & CE_UD_PTR_MASK);
}

static inline CeAsyncEventType ce_ud_type(uint64_t ud) {
    return (CeAsyncEventType)((ud >> CE_UD_TYPE_SHIFT) & CE_UD_TYPE_MASK);
}

static inline void* ce_ud_ptr(uint64_t ud) {
    return (void*)(uintptr_t)(ud & CE_UD_PTR_MASK);
}

struct CeAsyncContext {
    struct io_uring  ring;
    CeAsyncEvent     events[CE_ASYNC_MAX_EVENTS];
    int              event_count;
    int              pending_count;
    CeBool           buffers_registered;
};

/* ---- 操作上下文管理 ---- */
/* op_ctx_pool 已移除，改用 user_data 编码方案 */

/* ---- 生命周期 ---- */

CeAsyncContext* ce_async_init(int queue_depth) {
    if (queue_depth <= 0) queue_depth = 256;
    if (queue_depth > 32768) queue_depth = 32768;

    CeAsyncContext* ctx = (CeAsyncContext*)calloc(1, sizeof(CeAsyncContext));
    if (!ctx) return NULL;

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    int ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);
    if (ret < 0) {
        CE_LOG_ERROR("ASYNC", "io_uring_queue_init failed: %d (%s)", ret, strerror(-ret));
        free(ctx);
        return NULL;
    }

    CE_LOG_INFO("ASYNC", "io_uring initialized (depth=%d, features=0x%x)",
                queue_depth, params.features);
    return ctx;
}

void ce_async_shutdown(CeAsyncContext* ctx) {
    if (!ctx) return;
    io_uring_queue_exit(&ctx->ring);
    free(ctx);
    CE_LOG_INFO("ASYNC", "io_uring shut down");
}

/* ---- 操作提交 ---- */

void ce_async_accept(CeAsyncContext* ctx, int listen_fd, void* user_data) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) { CE_LOG_WARN("ASYNC", "SQ full"); return; }

    io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, ce_ud_encode(CE_ASYNC_ACCEPT, user_data));
    ctx->pending_count++;
}

void ce_async_recv(CeAsyncContext* ctx, int fd, void* buf, int size, void* user_data) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) { CE_LOG_WARN("ASYNC", "SQ full"); return; }

    io_uring_prep_recv(sqe, fd, buf, size, 0);
    io_uring_sqe_set_data64(sqe, ce_ud_encode(CE_ASYNC_RECV, user_data));
    ctx->pending_count++;
}

void ce_async_send(CeAsyncContext* ctx, int fd, const void* buf, int size, void* user_data) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) { CE_LOG_WARN("ASYNC", "SQ full"); return; }

    io_uring_prep_send(sqe, fd, buf, size, 0);
    io_uring_sqe_set_data64(sqe, ce_ud_encode(CE_ASYNC_SEND, user_data));
    ctx->pending_count++;
}

void ce_async_read(CeAsyncContext* ctx, int fd, void* buf, int size, off_t offset, void* user_data) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) { CE_LOG_WARN("ASYNC", "SQ full"); return; }

    io_uring_prep_read(sqe, fd, buf, size, offset);
    io_uring_sqe_set_data64(sqe, ce_ud_encode(CE_ASYNC_READ, user_data));
    ctx->pending_count++;
}

void ce_async_close(CeAsyncContext* ctx, int fd) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) { close(fd); return; }

    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data64(sqe, ce_ud_encode(CE_ASYNC_CLOSE, NULL));
    ctx->pending_count++;
}

/* ---- 事件处理 ---- */

int ce_async_submit(CeAsyncContext* ctx) {
    if (ctx->pending_count == 0) return 0;
    int ret = io_uring_submit(&ctx->ring);
    if (ret < 0) { CE_LOG_ERROR("ASYNC", "io_uring_submit failed: %d", ret); return -1; }
    ctx->pending_count = 0;
    return ret;
}

int ce_async_wait(CeAsyncContext* ctx, int min_events, int timeout_ms) {
    struct io_uring_cqe* cqe;
    int count = 0;

    /* 非阻塞获取 */
    while (count < CE_ASYNC_MAX_EVENTS) {
        int ret = io_uring_peek_cqe(&ctx->ring, &cqe);
        if (ret == -EAGAIN) break;
        if (ret < 0) { CE_LOG_ERROR("ASYNC", "peek_cqe: %d", ret); return -1; }

        uint64_t ud = io_uring_cqe_get_data64(cqe);
        CeAsyncEvent* ev = &ctx->events[count];
        ev->type      = ce_ud_type(ud);
        ev->user_data = ce_ud_ptr(ud);
        ev->error     = cqe->res < 0 ? -cqe->res : 0;
        ev->client_fd = (ev->type == CE_ASYNC_ACCEPT && cqe->res >= 0) ? cqe->res : -1;
        ev->nbytes    = (ev->type != CE_ASYNC_ACCEPT && cqe->res >= 0) ? cqe->res : 0;
        ev->fd        = -1; /* fd 不再编码在 user_data 中，由调用方通过 user_data 找回 */

        io_uring_cqe_seen(&ctx->ring, cqe);
        count++;
        if (count >= min_events) break;
    }

    /* 等待更多 */
    if (count < min_events && timeout_ms != 0) {
        struct __kernel_timespec ts = {
            .tv_sec  = timeout_ms > 0 ? timeout_ms / 1000 : 0,
            .tv_nsec = timeout_ms > 0 ? (timeout_ms % 1000) * 1000000L : 0
        };
        int ret = io_uring_wait_cqes(&ctx->ring, &cqe, min_events - count,
                                      timeout_ms > 0 ? &ts : NULL, NULL);
        if (ret < 0 && ret != -ETIME && ret != -EINTR) {
            CE_LOG_ERROR("ASYNC", "wait_cqes: %d", ret);
            return -1;
        }

        while (count < CE_ASYNC_MAX_EVENTS) {
            ret = io_uring_peek_cqe(&ctx->ring, &cqe);
            if (ret == -EAGAIN) break;
            if (ret < 0) break;

            uint64_t ud = io_uring_cqe_get_data64(cqe);
            CeAsyncEvent* ev = &ctx->events[count];
            ev->type      = ce_ud_type(ud);
            ev->user_data = ce_ud_ptr(ud);
            ev->error     = cqe->res < 0 ? -cqe->res : 0;
            ev->client_fd = (ev->type == CE_ASYNC_ACCEPT && cqe->res >= 0) ? cqe->res : -1;
            ev->nbytes    = (ev->type != CE_ASYNC_ACCEPT && cqe->res >= 0) ? cqe->res : 0;
            ev->fd        = -1;

            io_uring_cqe_seen(&ctx->ring, cqe);
            count++;
        }
    }

    ctx->event_count = count;
    return count;
}

const CeAsyncEvent* ce_async_get_event(CeAsyncContext* ctx, int index) {
    if (index < 0 || index >= ctx->event_count) return NULL;
    return &ctx->events[index];
}

/* ---- 高级特性 ---- */

CeResult ce_async_register_buffers(CeAsyncContext* ctx, void* buf, int buf_size, int buf_count) {
    struct iovec* iovs = (struct iovec*)calloc(buf_count, sizeof(struct iovec));
    if (!iovs) return CE_ERR;
    for (int i = 0; i < buf_count; i++) {
        iovs[i].iov_base = (char*)buf + i * buf_size;
        iovs[i].iov_len  = buf_size;
    }
    int ret = io_uring_register_buffers(&ctx->ring, iovs, buf_count);
    free(iovs);
    if (ret < 0) { CE_LOG_WARN("ASYNC", "register_buffers failed: %d", ret); return CE_ERR; }
    ctx->buffers_registered = CE_TRUE;
    CE_LOG_INFO("ASYNC", "Registered %d buffers (%d bytes each)", buf_count, buf_size);
    return CE_OK;
}

CeBool ce_async_has_zcrx(void) {
    struct io_uring ring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    if (io_uring_queue_init_params(1, &ring, &params) < 0) return CE_FALSE;
#ifdef IORING_FEAT_RECVSEND_BUNDLE
    CeBool has = (params.features & IORING_FEAT_RECVSEND_BUNDLE) ? CE_TRUE : CE_FALSE;
#else
    CeBool has = CE_FALSE;
#endif
    io_uring_queue_exit(&ring);
    return has;
}

const char* ce_async_backend_name(void) {
    return "io_uring";
}
