/*
 * ChaosEngine 协程调度器 - 头文件
 *
 * 基于 POSIX ucontext 实现的 C 协程库。
 * 支持创建、挂起(yield)、恢复(resume)协程。
 * 集成调度器(Scheduler)管理就绪队列。
 *
 * 纯 C99，ce_ 前缀。
 */

#ifndef CE_COROUTINE_H
#define CE_COROUTINE_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 协程 ---- */

/** 不透明协程句柄 */
typedef struct CeCoroutine CeCoroutine;

/** 协程入口函数 */
typedef void (*CeCoEntryFn)(void* arg);

/** 默认协程栈大小 */
#define CE_CO_DEFAULT_STACK  (128 * 1024)  /* 128 KB */

/** 协程状态 */
typedef enum {
    CE_CO_READY,      /* 就绪（可运行） */
    CE_CO_RUNNING,    /* 正在运行 */
    CE_CO_SUSPENDED,  /* 已挂起（等待 I/O） */
    CE_CO_DEAD        /* 已结束 */
} CeCoState;

/**
 * 创建协程
 *
 * @param fn         入口函数
 * @param arg        传给入口函数的参数
 * @param stack_size 栈大小（0 = 默认 128KB）
 * @return           协程句柄，NULL 失败
 */
CeCoroutine* ce_co_create(CeCoEntryFn fn, void* arg, size_t stack_size);

/**
 * 销毁协程（释放栈内存）
 */
void ce_co_destroy(CeCoroutine* co);

/**
 * 恢复协程执行（从 yield 处继续）
 * 调用此函数的协程会挂起，直到目标协程 yield 或结束
 */
CeResult ce_co_resume(CeCoroutine* co);

/**
 * 让出 CPU，回到调度器或调用者
 * 只能在协程内部调用
 */
CeResult ce_co_yield(void);

/**
 * 获取协程 ID
 */
uint64_t ce_co_id(const CeCoroutine* co);

/**
 * 获取协程状态
 */
CeCoState ce_co_state(const CeCoroutine* co);

/**
 * 获取当前正在运行的协程（NULL = 主线程/调度器上下文）
 */
CeCoroutine* ce_co_current(void);

/* ---- 调度器 ---- */

/** 不透明调度器句柄 */
typedef struct CeScheduler CeScheduler;

/**
 * 创建调度器
 */
CeScheduler* ce_sched_create(void);

/**
 * 销毁调度器
 */
void ce_sched_destroy(CeScheduler* sched);

/**
 * 添加协程到调度器就绪队列
 */
CeResult ce_sched_add(CeScheduler* sched, CeCoroutine* co);

/**
 * 运行调度器（阻塞，直到所有协程完成）
 */
CeResult ce_sched_run(CeScheduler* sched);

/**
 * 获取调度器中活跃协程数
 */
int ce_sched_active_count(CeScheduler* sched);

#ifdef __cplusplus
}
#endif

#endif /* CE_COROUTINE_H */
