/*
 * ChaosEngine 服务治理 - 头文件
 *
 * 熔断器 (Circuit Breaker) + 限流器 (Rate Limiter) + 负载均衡策略
 *
 * 纯 C99，ce_ 前缀。
 */

#ifndef CE_GOVERNANCE_H
#define CE_GOVERNANCE_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 负载均衡策略 ---- */

typedef enum {
    CE_LB_ROUND_ROBIN = 0,      /* 轮询 */
    CE_LB_RANDOM = 1,           /* 随机 */
    CE_LB_LEAST_CONN = 2,       /* 最少连接 */
    CE_LB_WEIGHTED = 3,         /* 加权 */
    CE_LB_CONSISTENT_HASH = 4,  /* 一致性哈希 */
} CeLbStrategy;

/* ---- 熔断器 ---- */

/** 熔断器状态 */
typedef enum {
    CE_CB_CLOSED = 0,    /* 关闭（正常放行） */
    CE_CB_OPEN = 1,      /* 打开（熔断，拒绝请求） */
    CE_CB_HALF_OPEN = 2, /* 半开（允许探测请求） */
} CeCbState;

/** 不透明熔断器 */
typedef struct CeCircuitBreaker CeCircuitBreaker;

/**
 * 创建熔断器
 *
 * @param fail_threshold    连续失败次数阈值（达到后熔断）
 * @param recovery_timeout  熔断后恢复时间（毫秒，超时后进入半开）
 */
CeCircuitBreaker* ce_cb_create(int fail_threshold, int recovery_timeout_ms);

/** 销毁熔断器 */
void ce_cb_destroy(CeCircuitBreaker* cb);

/** 是否允许请求（CLOSED/HALF_OPEN 放行，OPEN 拒绝） */
CeBool ce_cb_allow_request(CeCircuitBreaker* cb);

/** 记录成功 */
void ce_cb_record_success(CeCircuitBreaker* cb);

/** 记录失败 */
void ce_cb_record_failure(CeCircuitBreaker* cb);

/** 获取当前状态 */
CeCbState ce_cb_state(CeCircuitBreaker* cb);

/* ---- 限流器（令牌桶） ---- */

/** 不透明限流器 */
typedef struct CeRateLimiter CeRateLimiter;

/**
 * 创建限流器
 *
 * @param max_qps   每秒最大请求数
 */
CeRateLimiter* ce_rl_create(int max_qps);

/** 销毁限流器 */
void ce_rl_destroy(CeRateLimiter* rl);

/**
 * 尝试获取令牌
 *
 * @return CE_TRUE 获取成功（放行），CE_FALSE 被限流
 */
CeBool ce_rl_try_acquire(CeRateLimiter* rl);

/** 获取当前可用令牌数（浮点） */
double ce_rl_available_tokens(CeRateLimiter* rl);

#ifdef __cplusplus
}
#endif

#endif /* CE_GOVERNANCE_H */
