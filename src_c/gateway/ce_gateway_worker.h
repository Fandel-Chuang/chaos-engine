/*
 * ChaosEngine Gateway 线程池 + 编解码 - 头文件
 *
 * 主线程 io_uring 收发包，工作线程池处理压缩/解压/加密/解密。
 * 首包(LOGIN)协商编解码算法，后续包按协商结果处理。
 *
 * 纯 C99。
 */

#ifndef CE_GATEWAY_WORKER_H
#define CE_GATEWAY_WORKER_H

#include "gateway/ce_gateway.h"
#include "codec/ce_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 工作任务类型 ---- */

typedef enum {
    CE_WORKER_TASK_ENCODE = 1,  /* 编码: 压缩→加密 */
    CE_WORKER_TASK_DECODE = 2,  /* 解码: 解密→解压 */
} CeWorkerTaskType;

/* ---- 工作任务 ---- */

typedef struct CeWorkerTask {
    CeWorkerTaskType   type;
    uint64_t           conn_id;      /* 关联的连接 ID */
    CeCodecCtx*        codec;        /* 编解码上下文 */
    uint8_t*           input;        /* 输入数据 */
    uint32_t           input_len;    /* 输入长度 */
    uint8_t*           output;       /* 输出缓冲区 */
    uint32_t           output_cap;   /* 输出缓冲区容量 */
    uint32_t           output_len;   /* 输出实际长度 */
    CeResult           result;       /* 处理结果 */
    void*              user_data;    /* 回调数据 */
} CeWorkerTask;

/* ---- 线程池 ---- */

typedef struct CeWorkerPool CeWorkerPool;

/** 任务完成回调 */
typedef void (*CeWorkerCallback)(CeWorkerTask* task);

/**
 * 创建工作线程池
 *
 * @param num_threads  线程数（0 = CPU 核心数）
 */
CeWorkerPool* ce_worker_pool_create(int num_threads);

/**
 * 销毁线程池（等待所有任务完成）
 */
void ce_worker_pool_destroy(CeWorkerPool* pool);

/**
 * 提交任务到线程池
 *
 * @param pool      线程池
 * @param task      任务（调用方分配，完成后通过 callback 返回）
 * @param callback  完成回调（在主线程被调用）
 */
CeResult ce_worker_pool_submit(CeWorkerPool* pool,
                                 CeWorkerTask* task,
                                 CeWorkerCallback callback);

/**
 * 轮询已完成的任务（在主线程 io_uring 循环中调用）
 * 触发对应回调
 */
int ce_worker_pool_poll(CeWorkerPool* pool);

/**
 * 获取线程池线程数
 */
int ce_worker_pool_size(CeWorkerPool* pool);

/**
 * 获取待处理任务数
 */
int ce_worker_pool_pending(CeWorkerPool* pool);

#ifdef __cplusplus
}
#endif

#endif /* CE_GATEWAY_WORKER_H */
