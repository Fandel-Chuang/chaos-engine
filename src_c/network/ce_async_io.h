/*
 * ChaosEngine 异步 I/O 抽象层
 *
 * 统一异步 I/O 接口，支持多后端：
 *   Linux:   io_uring (liburing)
 *   Windows: IOCP (后续)
 *   Fallback: POSIX (epoll/select)
 *
 * 纯 C99，单线程事件循环模型
 */

#ifndef CE_ASYNC_IO_H
#define CE_ASYNC_IO_H

#include "public_api/ce_types.h"

#include <sys/types.h>  /* off_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明句柄 ---- */

typedef struct CeAsyncContext CeAsyncContext;

/* ---- 事件类型 ---- */

typedef enum CeAsyncEventType {
    CE_ASYNC_ACCEPT = 0,
    CE_ASYNC_RECV,
    CE_ASYNC_SEND,
    CE_ASYNC_READ,     /* 文件读取 */
    CE_ASYNC_WRITE,    /* 文件写入 */
    CE_ASYNC_CLOSE,
    CE_ASYNC_ERROR,
} CeAsyncEventType;

typedef struct CeAsyncEvent {
    CeAsyncEventType type;
    int              fd;           /* 关联的文件描述符 */
    int              client_fd;    /* accept 时的新连接 fd */
    void*            buf;          /* 数据缓冲区 */
    int              nbytes;       /* 实际传输字节数（正数=成功，0=EOF，负数=错误） */
    int              error;        /* 错误码（0 = 成功） */
    void*            user_data;    /* 用户自定义数据 */
} CeAsyncEvent;

/* ---- 生命周期 ---- */

/** 初始化异步 I/O 上下文
 *  @param queue_depth  队列深度（io_uring: SQ entries, POSIX: 忽略） */
CeAsyncContext* ce_async_init(int queue_depth);

/** 关闭异步 I/O 上下文 */
void ce_async_shutdown(CeAsyncContext* ctx);

/* ---- 操作提交（非阻塞，仅准备 SQE） ---- */

/** 提交 accept 请求 */
void ce_async_accept(CeAsyncContext* ctx, int listen_fd, void* user_data);

/** 提交 recv 请求 */
void ce_async_recv(CeAsyncContext* ctx, int fd, void* buf, int size, void* user_data);

/** 提交 send 请求 */
void ce_async_send(CeAsyncContext* ctx, int fd, const void* buf, int size, void* user_data);

/** 提交文件读取请求 */
void ce_async_read(CeAsyncContext* ctx, int fd, void* buf, int size, off_t offset, void* user_data);

/** 提交关闭请求 */
void ce_async_close(CeAsyncContext* ctx, int fd);

/* ---- 事件处理 ---- */

/** 提交所有待处理的请求到内核
 *  @return 提交的请求数量，-1 表示错误 */
int ce_async_submit(CeAsyncContext* ctx);

/** 等待完成事件
 *  @param min_events  最少等待的事件数
 *  @param timeout_ms  超时毫秒数（-1 = 无限等待，0 = 立即返回）
 *  @return 实际获取的事件数量，-1 表示错误 */
int ce_async_wait(CeAsyncContext* ctx, int min_events, int timeout_ms);

/** 获取第 N 个完成事件（索引从 0 开始） */
const CeAsyncEvent* ce_async_get_event(CeAsyncContext* ctx, int index);

/* ---- 高级特性（io_uring 专属，其他后端返回 CE_ERR） ---- */

/** 注册固定缓冲区（减少内存拷贝，提升性能） */
CeResult ce_async_register_buffers(CeAsyncContext* ctx, void* buf, int buf_size, int buf_count);

/** 是否支持零拷贝接收 */
CeBool ce_async_has_zcrx(void);

/** 获取后端名称（调试用） */
const char* ce_async_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_ASYNC_IO_H */
