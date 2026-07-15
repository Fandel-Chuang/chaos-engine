/*
 * ChaosEngine Gateway 线程池 - 实现
 *
 * 线程池模式:
 *   主线程提交任务 → 任务队列 → 工作线程处理 → 完成队列 → 主线程 poll 回调
 *
 * 纯 C99。
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200112L

#include "gateway/ce_gateway_worker.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ---- 内部结构 ---- */

/** 任务队列节点 */
typedef struct CeWorkerNode {
    CeWorkerTask      task;
    CeWorkerCallback  callback;
    struct CeWorkerNode* next;
} CeWorkerNode;

struct CeWorkerPool {
    pthread_t*        threads;
    int               num_threads;

    /* 任务队列（待处理） */
    pthread_mutex_t   queue_mutex;
    pthread_cond_t    queue_cond;
    CeWorkerNode*     queue_head;
    CeWorkerNode*     queue_tail;
    int               queue_count;
    CeBool            shutdown;

    /* 完成队列（已处理） */
    pthread_mutex_t   done_mutex;
    CeWorkerNode*     done_head;
    CeWorkerNode*     done_tail;
    int               done_count;
};

/* ---- 工作线程入口 ---- */

static void* worker_thread_fn(void* arg) {
    CeWorkerPool* pool = (CeWorkerPool*)arg;

    while (1) {
        /* 从任务队列取任务 */
        pthread_mutex_lock(&pool->queue_mutex);

        while (pool->queue_count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
        }

        if (pool->shutdown && pool->queue_count == 0) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }

        CeWorkerNode* node = pool->queue_head;
        if (node) {
            pool->queue_head = node->next;
            if (!pool->queue_head) pool->queue_tail = NULL;
            pool->queue_count--;
        }
        pthread_mutex_unlock(&pool->queue_mutex);

        if (!node) continue;

        CeWorkerTask* task = &node->task;

        /* 执行编解码 */
        task->output_len = task->output_cap;
        if (task->type == CE_WORKER_TASK_ENCODE) {
            task->result = ce_codec_encode(task->codec,
                                            task->input, task->input_len,
                                            task->output, &task->output_len);
        } else {
            task->result = ce_codec_decode(task->codec,
                                            task->input, task->input_len,
                                            task->output, &task->output_len);
        }

        /* 放入完成队列 */
        pthread_mutex_lock(&pool->done_mutex);
        node->next = NULL;
        if (pool->done_tail) {
            pool->done_tail->next = node;
        } else {
            pool->done_head = node;
        }
        pool->done_tail = node;
        pool->done_count++;
        pthread_mutex_unlock(&pool->done_mutex);
    }

    return NULL;
}

/* ---- 公开 API ---- */

CeWorkerPool* ce_worker_pool_create(int num_threads) {
    if (num_threads <= 0) {
        /* 默认 CPU 核心数 */
        long ncpu = 0;
#ifdef _SC_NPROCESSORS_ONLN
        ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#endif
        num_threads = (int)(ncpu > 0 ? ncpu : 4);
    }
    if (num_threads > 16) num_threads = 16;

    CeWorkerPool* pool = (CeWorkerPool*)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->num_threads = num_threads;
    pool->shutdown = CE_FALSE;
    pool->queue_head = pool->queue_tail = NULL;
    pool->done_head = pool->done_tail = NULL;
    pool->queue_count = 0;
    pool->done_count = 0;

    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->queue_cond, NULL);
    pthread_mutex_init(&pool->done_mutex, NULL);

    pool->threads = (pthread_t*)calloc(num_threads, sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread_fn, pool) != 0) {
            CE_LOG_ERROR("WORKER", "Failed to create worker thread %d", i);
            pool->num_threads = i;
            break;
        }
    }

    CE_LOG_INFO("WORKER", "Thread pool created: %d threads", pool->num_threads);
    return pool;
}

void ce_worker_pool_destroy(CeWorkerPool* pool) {
    if (!pool) return;

    /* 通知工作线程退出 */
    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = CE_TRUE;
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    /* 等待线程退出 */
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* 清理队列 */
    CeWorkerNode* node = pool->queue_head;
    while (node) {
        CeWorkerNode* next = node->next;
        free(node);
        node = next;
    }
    node = pool->done_head;
    while (node) {
        CeWorkerNode* next = node->next;
        free(node);
        node = next;
    }

    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);
    pthread_mutex_destroy(&pool->done_mutex);

    free(pool->threads);
    free(pool);
}

CeResult ce_worker_pool_submit(CeWorkerPool* pool,
                                 CeWorkerTask* task,
                                 CeWorkerCallback callback) {
    if (!pool || !task) return CE_ERR;

    CeWorkerNode* node = (CeWorkerNode*)calloc(1, sizeof(*node));
    if (!node) return CE_ERR;

    node->task = *task;
    node->callback = callback;
    node->next = NULL;

    pthread_mutex_lock(&pool->queue_mutex);
    if (pool->queue_tail) {
        pool->queue_tail->next = node;
    } else {
        pool->queue_head = node;
    }
    pool->queue_tail = node;
    pool->queue_count++;
    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    return CE_OK;
}

int ce_worker_pool_poll(CeWorkerPool* pool) {
    if (!pool) return 0;

    int processed = 0;

    pthread_mutex_lock(&pool->done_mutex);
    while (pool->done_head) {
        CeWorkerNode* node = pool->done_head;
        pool->done_head = node->next;
        if (!pool->done_head) pool->done_tail = NULL;
        pool->done_count--;

        pthread_mutex_unlock(&pool->done_mutex);

        /* 回调（在主线程执行） */
        if (node->callback) {
            node->callback(&node->task);
        }
        free(node);
        processed++;

        pthread_mutex_lock(&pool->done_mutex);
    }
    pthread_mutex_unlock(&pool->done_mutex);

    return processed;
}

int ce_worker_pool_size(CeWorkerPool* pool) {
    return pool ? pool->num_threads : 0;
}

int ce_worker_pool_pending(CeWorkerPool* pool) {
    if (!pool) return 0;
    int count;
    pthread_mutex_lock(&pool->queue_mutex);
    count = pool->queue_count;
    pthread_mutex_unlock(&pool->queue_mutex);
    return count;
}
