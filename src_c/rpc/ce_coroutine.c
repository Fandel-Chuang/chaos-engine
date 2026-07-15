/*
 * ChaosEngine 协程调度器 - 实现
 *
 * 基于 POSIX ucontext_t (makecontext/swapcontext) 实现。
 * 纯 C99。
 */

#define _XOPEN_SOURCE 700  /* ucontext 需要 */

#include "rpc/ce_coroutine.h"
#include "public_api/ce_log.h"

#include <ucontext.h>
#include <stdlib.h>
#include <string.h>

/* ---- 协程结构 ---- */

struct CeCoroutine {
    uint64_t        id;         /* 协程 ID */
    CeCoState       state;      /* 当前状态 */
    ucontext_t      ctx;        /* 协程上下文 */
    void*           stack;      /* 栈内存 */
    size_t          stack_size; /* 栈大小 */
    CeCoEntryFn     fn;         /* 入口函数 */
    void*           arg;        /* 入口参数 */
    CeCoroutine*    caller;     /* 调用者协程（resume 时保存） */
};

/* 全局当前协程指针（用于 yield 时找到当前协程） */
static CeCoroutine* g_current_co = NULL;

/* 全局协程 ID 计数器 */
static uint64_t g_next_co_id = 1;

/* ---- 协程入口 trampoline ---- */

static void co_entry_trampoline(unsigned int hi, unsigned int lo) {
    /* 从 hi/lo 恢复指针（32 位兼容） */
    uintptr_t ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    CeCoroutine* co = (CeCoroutine*)ptr;

    /* 执行用户函数 */
    if (co && co->fn) {
        co->fn(co->arg);
    }

    /* 函数返回，协程结束 */
    co->state = CE_CO_DEAD;

    /* 回到调用者 */
    if (co->caller) {
        swapcontext(&co->ctx, &co->caller->ctx);
    }
}

/* ---- 协程 API ---- */

CeCoroutine* ce_co_create(CeCoEntryFn fn, void* arg, size_t stack_size) {
    if (!fn) return NULL;
    if (stack_size == 0) stack_size = CE_CO_DEFAULT_STACK;

    CeCoroutine* co = (CeCoroutine*)calloc(1, sizeof(*co));
    if (!co) return NULL;

    co->stack = malloc(stack_size);
    if (!co->stack) {
        free(co);
        return NULL;
    }

    co->id = g_next_co_id++;
    co->fn = fn;
    co->arg = arg;
    co->stack_size = stack_size;
    co->state = CE_CO_READY;
    co->caller = NULL;

    /* 初始化 ucontext */
    getcontext(&co->ctx);
    co->ctx.uc_stack.ss_sp = co->stack;
    co->ctx.uc_stack.ss_size = stack_size;
    co->ctx.uc_link = NULL;  /* 我们自己管理返回 */

    /* makecontext 需要整数参数，把指针拆成两个 unsigned int */
    uintptr_t ptr = (uintptr_t)co;
    unsigned int hi = (unsigned int)(ptr >> 32);
    unsigned int lo = (unsigned int)(ptr & 0xFFFFFFFF);
    makecontext(&co->ctx, (void(*)(void))co_entry_trampoline, 2, hi, lo);

    return co;
}

void ce_co_destroy(CeCoroutine* co) {
    if (!co) return;
    if (co->stack) free(co->stack);
    free(co);
}

CeResult ce_co_resume(CeCoroutine* co) {
    if (!co) return CE_ERR;
    if (co->state == CE_CO_DEAD) return CE_ERR;

    CeCoroutine* prev = g_current_co;
    co->caller = prev;
    co->state = CE_CO_RUNNING;
    g_current_co = co;

    swapcontext(prev ? &prev->ctx : NULL, &co->ctx);

    g_current_co = prev;

    /* 如果协程已结束，状态已经是 DEAD */
    return CE_OK;
}

CeResult ce_co_yield(void) {
    CeCoroutine* co = g_current_co;
    if (!co || !co->caller) {
        /* 不在协程中或没有调用者 */
        return CE_ERR;
    }

    co->state = CE_CO_SUSPENDED;
    swapcontext(&co->ctx, &co->caller->ctx);
    co->state = CE_CO_RUNNING;

    return CE_OK;
}

uint64_t ce_co_id(const CeCoroutine* co) {
    return co ? co->id : 0;
}

CeCoState ce_co_state(const CeCoroutine* co) {
    return co ? co->state : CE_CO_DEAD;
}

CeCoroutine* ce_co_current(void) {
    return g_current_co;
}

/* ---- 调度器 ---- */

/** 就绪队列节点 */
typedef struct CoQueueNode {
    CeCoroutine* co;
    struct CoQueueNode* next;
} CoQueueNode;

struct CeScheduler {
    CoQueueNode*  ready_head;   /* 就绪队列头 */
    CoQueueNode*  ready_tail;   /* 就绪队列尾 */
    int           active_count; /* 活跃协程数 */
    ucontext_t    main_ctx;     /* 主上下文 */
};

CeScheduler* ce_sched_create(void) {
    CeScheduler* sched = (CeScheduler*)calloc(1, sizeof(*sched));
    if (!sched) return NULL;
    sched->ready_head = NULL;
    sched->ready_tail = NULL;
    sched->active_count = 0;
    return sched;
}

void ce_sched_destroy(CeScheduler* sched) {
    if (!sched) return;

    /* 释放队列中未完成的协程 */
    CoQueueNode* node = sched->ready_head;
    while (node) {
        CoQueueNode* next = node->next;
        if (node->co) {
            ce_co_destroy(node->co);
        }
        free(node);
        node = next;
    }
    free(sched);
}

CeResult ce_sched_add(CeScheduler* sched, CeCoroutine* co) {
    if (!sched || !co) return CE_ERR;

    CoQueueNode* node = (CoQueueNode*)calloc(1, sizeof(*node));
    if (!node) return CE_ERR;

    node->co = co;
    node->next = NULL;

    if (sched->ready_tail) {
        sched->ready_tail->next = node;
    } else {
        sched->ready_head = node;
    }
    sched->ready_tail = node;
    sched->active_count++;

    return CE_OK;
}

CeResult ce_sched_run(CeScheduler* sched) {
    if (!sched) return CE_ERR;

    while (sched->ready_head) {
        /* 取出队首协程 */
        CoQueueNode* node = sched->ready_head;
        sched->ready_head = node->next;
        if (!sched->ready_head) {
            sched->ready_tail = NULL;
        }

        CeCoroutine* co = node->co;
        free(node);

        if (co->state == CE_CO_DEAD) {
            ce_co_destroy(co);
            sched->active_count--;
            continue;
        }

        /* 执行协程 */
        co->caller = NULL;  /* 用调度器主上下文 */
        co->state = CE_CO_RUNNING;
        g_current_co = co;

        swapcontext(&sched->main_ctx, &co->ctx);

        g_current_co = NULL;

        if (co->state == CE_CO_DEAD) {
            ce_co_destroy(co);
            sched->active_count--;
        } else if (co->state == CE_CO_SUSPENDED) {
            /* 协程 yield，重新加入队列 */
            ce_sched_add(sched, co);
            sched->active_count--;  /* add 会加回来 */
        }
    }

    return CE_OK;
}

int ce_sched_active_count(CeScheduler* sched) {
    return sched ? sched->active_count : 0;
}
