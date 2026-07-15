/*
 * RPC 模块单元测试
 * 测试: 协程创建/切换/销毁, 熔断器状态机, 限流器令牌桶
 */

#include "public_api/ce_types.h"
#include "rpc/ce_coroutine.h"
#include "rpc/ce_governance.h"
#include "rpc/ce_service_registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- 协程测试 ---- */

static int g_co_test_value = 0;

static void co_set_value(void* arg) {
    int* val = (int*)arg;
    *val = 42;
    g_co_test_value = 42;
}

static void test_coroutine_basic(void) {
    int val = 0;
    CeCoroutine* co = ce_co_create(co_set_value, &val, 0);
    assert(co != NULL);
    assert(ce_co_state(co) == CE_CO_READY);

    ce_co_resume(co);
    assert(val == 42);
    assert(ce_co_state(co) == CE_CO_DEAD);

    ce_co_destroy(co);
    printf("[OK] test_coroutine_basic\n");
}

static void test_scheduler(void) {
    g_co_test_value = 0;
    int val = 0;

    CeScheduler* sched = ce_sched_create();
    CeCoroutine* co = ce_co_create(co_set_value, &val, 0);
    ce_sched_add(sched, co);

    assert(ce_sched_active_count(sched) == 1);
    ce_sched_run(sched);
    assert(g_co_test_value == 42);

    ce_sched_destroy(sched);
    printf("[OK] test_scheduler\n");
}

/* ---- 熔断器测试 ---- */

static void test_circuit_breaker(void) {
    CeCircuitBreaker* cb = ce_cb_create(3, 100);  /* 3次失败熔断, 100ms 恢复 */

    /* 初始状态 CLOSED，允许请求 */
    assert(ce_cb_state(cb) == CE_CB_CLOSED);
    assert(ce_cb_allow_request(cb) == CE_TRUE);

    /* 记录2次失败，不应熔断 */
    ce_cb_record_failure(cb);
    ce_cb_record_failure(cb);
    assert(ce_cb_state(cb) == CE_CB_CLOSED);
    assert(ce_cb_allow_request(cb) == CE_TRUE);

    /* 第3次失败，熔断 */
    ce_cb_record_failure(cb);
    assert(ce_cb_state(cb) == CE_CB_OPEN);
    assert(ce_cb_allow_request(cb) == CE_FALSE);

    /* 等待恢复超时 */
    usleep(150000);  /* 150ms > 100ms */
    assert(ce_cb_allow_request(cb) == CE_TRUE);  /* 进入 HALF_OPEN */
    assert(ce_cb_state(cb) == CE_CB_HALF_OPEN);

    /* 半开状态成功，恢复关闭 */
    ce_cb_record_success(cb);
    assert(ce_cb_state(cb) == CE_CB_CLOSED);

    ce_cb_destroy(cb);
    printf("[OK] test_circuit_breaker\n");
}

/* ---- 限流器测试 ---- */

static void test_rate_limiter(void) {
    CeRateLimiter* rl = ce_rl_create(10);  /* 10 QPS */

    /* 初始满桶，应该能获取 10 个令牌 */
    int acquired = 0;
    for (int i = 0; i < 15; i++) {
        if (ce_rl_try_acquire(rl)) {
            acquired++;
        }
    }
    assert(acquired >= 10);  /* 至少 10 个 */
    assert(acquired <= 11);  /* 最多 11 个（可能有微小补充） */

    ce_rl_destroy(rl);
    printf("[OK] test_rate_limiter (acquired=%d)\n", acquired);
}

int main(void) {
    printf("=== test_rpc ===\n");

    test_coroutine_basic();
    test_scheduler();
    test_circuit_breaker();
    test_rate_limiter();

    printf("=== All tests passed ===\n");
    return 0;
}
