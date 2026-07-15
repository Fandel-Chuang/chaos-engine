/*
 * ChaosEngine 服务治理 - 实现
 * 熔断器 + 限流器
 *
 * 纯 C99。
 */

#define _POSIX_C_SOURCE 200112L

#include "rpc/ce_governance.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <time.h>
#include <math.h>

/* ---- 熔断器 ---- */

struct CeCircuitBreaker {
    CeCbState  state;
    int        fail_threshold;     /* 连续失败阈值 */
    int        recovery_timeout_ms;/* 恢复超时 */
    int        consecutive_fails;  /* 当前连续失败次数 */
    int64_t    last_fail_time_ms;  /* 最后一次失败的时间(ms) */
};

/** 获取当前时间(ms) */
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

CeCircuitBreaker* ce_cb_create(int fail_threshold, int recovery_timeout_ms) {
    if (fail_threshold <= 0) fail_threshold = 5;
    if (recovery_timeout_ms <= 0) recovery_timeout_ms = 10000;

    CeCircuitBreaker* cb = (CeCircuitBreaker*)calloc(1, sizeof(*cb));
    if (!cb) return NULL;

    cb->state = CE_CB_CLOSED;
    cb->fail_threshold = fail_threshold;
    cb->recovery_timeout_ms = recovery_timeout_ms;
    cb->consecutive_fails = 0;
    cb->last_fail_time_ms = 0;

    return cb;
}

void ce_cb_destroy(CeCircuitBreaker* cb) {
    if (cb) free(cb);
}

CeBool ce_cb_allow_request(CeCircuitBreaker* cb) {
    if (!cb) return CE_TRUE;  /* 无熔断器，默认放行 */

    switch (cb->state) {
    case CE_CB_CLOSED:
        return CE_TRUE;

    case CE_CB_OPEN:
        /* 检查是否超时，超时则进入半开 */
        if (now_ms() - cb->last_fail_time_ms >= cb->recovery_timeout_ms) {
            cb->state = CE_CB_HALF_OPEN;
            CE_LOG_INFO("RPC", "CircuitBreaker: OPEN -> HALF_OPEN");
            return CE_TRUE;  /* 半开状态允许一个探测请求 */
        }
        return CE_FALSE;  /* 仍在熔断中 */

    case CE_CB_HALF_OPEN:
        return CE_TRUE;  /* 半开状态放行 */

    default:
        return CE_TRUE;
    }
}

void ce_cb_record_success(CeCircuitBreaker* cb) {
    if (!cb) return;

    if (cb->state == CE_CB_HALF_OPEN) {
        /* 半开状态成功，恢复到关闭 */
        cb->state = CE_CB_CLOSED;
        cb->consecutive_fails = 0;
        CE_LOG_INFO("RPC", "CircuitBreaker: HALF_OPEN -> CLOSED (recovered)");
    } else if (cb->state == CE_CB_CLOSED) {
        cb->consecutive_fails = 0;
    }
}

void ce_cb_record_failure(CeCircuitBreaker* cb) {
    if (!cb) return;

    cb->consecutive_fails++;
    cb->last_fail_time_ms = now_ms();

    if (cb->state == CE_CB_HALF_OPEN) {
        /* 半开状态失败，重新熔断 */
        cb->state = CE_CB_OPEN;
        CE_LOG_WARN("RPC", "CircuitBreaker: HALF_OPEN -> OPEN (probe failed)");
    } else if (cb->state == CE_CB_CLOSED) {
        if (cb->consecutive_fails >= cb->fail_threshold) {
            cb->state = CE_CB_OPEN;
            CE_LOG_WARN("RPC", "CircuitBreaker: CLOSED -> OPEN (fails=%d >= %d)",
                        cb->consecutive_fails, cb->fail_threshold);
        }
    }
}

CeCbState ce_cb_state(CeCircuitBreaker* cb) {
    return cb ? cb->state : CE_CB_CLOSED;
}

/* ---- 限流器（令牌桶） ---- */

struct CeRateLimiter {
    double   max_tokens;      /* 最大令牌数 (= max_qps) */
    double   tokens;          /* 当前令牌数 */
    double   refill_rate;     /* 每秒补充令牌数 (= max_qps) */
    int64_t  last_refill_ms;  /* 上次补充时间(ms) */
};

CeRateLimiter* ce_rl_create(int max_qps) {
    if (max_qps <= 0) max_qps = 1000;

    CeRateLimiter* rl = (CeRateLimiter*)calloc(1, sizeof(*rl));
    if (!rl) return NULL;

    rl->max_tokens = (double)max_qps;
    rl->tokens = (double)max_qps;  /* 初始满桶 */
    rl->refill_rate = (double)max_qps;
    rl->last_refill_ms = now_ms();

    return rl;
}

void ce_rl_destroy(CeRateLimiter* rl) {
    if (rl) free(rl);
}

CeBool ce_rl_try_acquire(CeRateLimiter* rl) {
    if (!rl) return CE_TRUE;

    /* 补充令牌 */
    int64_t current = now_ms();
    double elapsed_sec = (double)(current - rl->last_refill_ms) / 1000.0;
    rl->tokens += elapsed_sec * rl->refill_rate;
    if (rl->tokens > rl->max_tokens) {
        rl->tokens = rl->max_tokens;  /* 不超过桶容量 */
    }
    rl->last_refill_ms = current;

    /* 尝试消耗一个令牌 */
    if (rl->tokens >= 1.0) {
        rl->tokens -= 1.0;
        return CE_TRUE;
    }

    return CE_FALSE;  /* 令牌不足 */
}

double ce_rl_available_tokens(CeRateLimiter* rl) {
    if (!rl) return 0.0;
    return rl->tokens;
}
