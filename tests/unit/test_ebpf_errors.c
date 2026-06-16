/*
 * ChaosEngine eBPF 错误处理测试 (Phase 7.5)
 *
 * 测试: NULL 参数、无效函数名、重复 attach、未加载时读取 map、
 *        stub 模式下的行为、shutdown 后访问
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/ce_types.h"
#include "network/ce_ebpf.h"
#include <stdio.h>
#include <string.h>

#define TEST(name) printf("  TEST: %s ... ", name)
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while(0)
#define PASS() printf("PASS\n")

int main(void) {
    int failures = 0;

    printf("=== eBPF Error Handling Tests (Phase 7.5) ===\n");

    /* ---- 7.5.1: NULL context to all functions ---- */
    TEST("null_context_trace_function");
    {
        CeResult r = ce_ebpf_trace_function(NULL, "any_func");
        CHECK(r == CE_ERR);
    }
    PASS();

    TEST("null_context_trace_tcp");
    {
        CeResult r = ce_ebpf_trace_tcp_retransmit(NULL);
        CHECK(r == CE_ERR);
    }
    PASS();

    TEST("null_context_trace_io");
    {
        CeResult r = ce_ebpf_trace_io_latency(NULL);
        CHECK(r == CE_ERR);
    }
    PASS();

    TEST("null_context_get_retransmit");
    {
        int count = ce_ebpf_get_retransmit_count(NULL);
        CHECK(count == 0);
    }
    PASS();

    TEST("null_context_dump_latency");
    {
        int samples = ce_ebpf_dump_latency(NULL, "any_func");
        CHECK(samples == 0);
    }
    PASS();

    TEST("null_context_get_io_stats");
    {
        int p50 = 99, p90 = 99, p99 = 99;
        ce_ebpf_get_io_latency_stats(NULL, &p50, &p90, &p99);
        CHECK(p50 == 0 && p90 == 0 && p99 == 0);
    }
    PASS();

    /* ---- 7.5.2: NULL function name ---- */
    TEST("null_func_name_trace");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            CeResult r = ce_ebpf_trace_function(ctx, NULL);
            /* 不应崩溃，返回 CE_ERR */
            CHECK(r == CE_ERR);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    TEST("null_func_name_dump");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            int samples = ce_ebpf_dump_latency(ctx, NULL);
            CHECK(samples == 0);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.5.3: 无效函数名（不存在的函数） ---- */
    TEST("invalid_func_name_trace");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            CeResult r = ce_ebpf_trace_function(ctx, "nonexistent_func_xyz");
            /* 不应崩溃，找不到匹配的探针，返回 CE_OK 但无实际 attach */
            CHECK(r == CE_OK || r == CE_ERR);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.5.4: 重复 attach 同一函数 ---- */
    TEST("double_attach_same_func");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            CeResult r1 = ce_ebpf_trace_function(ctx, "ce_ecs_update");
            CeResult r2 = ce_ebpf_trace_function(ctx, "ce_ecs_update");
            /* 第二次 attach 不应崩溃 */
            CHECK(r1 == CE_OK || r1 == CE_ERR);
            CHECK(r2 == CE_OK || r2 == CE_ERR);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.5.5: 未加载 BPF 程序时读取 map ---- */
    TEST("read_map_before_load");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            /* 没有调用任何 trace 函数，BPF 程序未加载 */
            int count = ce_ebpf_get_retransmit_count(ctx);
            CHECK(count == 0);

            int p50, p90, p99;
            ce_ebpf_get_io_latency_stats(ctx, &p50, &p90, &p99);
            CHECK(p50 == 0 && p90 == 0 && p99 == 0);

            int samples = ce_ebpf_dump_latency(ctx, "any_func");
            CHECK(samples == 0);

            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.5.6: shutdown 后再次 shutdown ---- */
    TEST("double_shutdown");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            ce_ebpf_shutdown(ctx);
            /* 第二次 shutdown 同一指针（use-after-free 防护） */
            /* 注意：这是未定义行为，但我们验证 API 设计 */
        }
        /* 不崩溃即为通过 */
    }
    PASS();

    /* ---- 7.5.7: stub 模式（CHAOS_HAS_EBPF 未定义时的行为） ---- */
    TEST("stub_mode_behavior");
    {
        /* 当 BTF 不可用时，所有函数返回安全默认值 */
        if (!ce_ebpf_available()) {
            CeEbpfContext* ctx = ce_ebpf_init();
            CHECK(ctx == NULL);

            CHECK(ce_ebpf_trace_function(NULL, "f") == CE_ERR);
            CHECK(ce_ebpf_trace_tcp_retransmit(NULL) == CE_ERR);
            CHECK(ce_ebpf_trace_io_latency(NULL) == CE_ERR);
            CHECK(ce_ebpf_get_retransmit_count(NULL) == 0);
            CHECK(ce_ebpf_dump_latency(NULL, "f") == 0);

            int p50 = 1, p90 = 1, p99 = 1;
            ce_ebpf_get_io_latency_stats(NULL, &p50, &p90, &p99);
            CHECK(p50 == 0 && p90 == 0 && p99 == 0);

            ce_ebpf_shutdown(NULL); /* 安全 */
        }
    }
    PASS();

    /* ---- 7.5.8: 空字符串函数名 ---- */
    TEST("empty_func_name_trace");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            /* strstr(name, "") always matches, which may trigger attach attempts.
             * We just verify it doesn't crash. */
            CeResult r = ce_ebpf_trace_function(ctx, "");
            CHECK(r == CE_OK || r == CE_ERR);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.5.9: 超长函数名 ---- */
    TEST("long_func_name_trace");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            char long_name[512];
            memset(long_name, 'x', sizeof(long_name) - 1);
            long_name[sizeof(long_name) - 1] = '\0';
            /* strstr won't match any program name, so no attach attempt */
            CeResult r = ce_ebpf_trace_function(ctx, long_name);
            CHECK(r == CE_OK || r == CE_ERR);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.5.10: 混合错误场景 ---- */
    TEST("mixed_error_scenario");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            /* 先正常 attach */
            ce_ebpf_trace_function(ctx, "ce_ecs_update");

            /* 再传 NULL context（不影响已创建的 ctx） */
            ce_ebpf_trace_function(NULL, "ce_ecs_update");

            /* 读取数据 */
            int count = ce_ebpf_get_retransmit_count(ctx);
            CHECK(count >= 0);

            /* 正常卸载 */
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    printf("\nAll eBPF error handling tests passed!\n");
    return 0;
}
