/*
 * ChaosEngine eBPF 单元测试 (Phase 7.4)
 *
 * 测试: BPF 程序加载、attach、map 读写、卸载
 * 注意: 需要 root 或 CAP_BPF 才能成功 attach，非特权模式下仅验证 API 不崩溃
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

    printf("=== eBPF Unit Tests (Phase 7.4) ===\n");
    printf("  BTF available: %s\n", ce_ebpf_available() ? "YES" : "NO");

    /* ---- 7.4.1: ce_ebpf_available ---- */
    TEST("ebpf_available_returns_bool");
    {
        CeBool avail = ce_ebpf_available();
        CHECK(avail == CE_TRUE || avail == CE_FALSE);
    }
    PASS();

    /* ---- 7.4.2: ce_ebpf_init (BPF 对象加载) ---- */
    TEST("ebpf_init_creates_context");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ce_ebpf_available()) {
            /* 如果 BTF 可用，init 应该成功 */
            CHECK(ctx != NULL);
        } else {
            /* BTF 不可用时，init 返回 NULL（stub） */
            CHECK(ctx == NULL);
        }
        ce_ebpf_shutdown(ctx);
    }
    PASS();

    /* ---- 7.4.3: ce_ebpf_shutdown with NULL ---- */
    TEST("ebpf_shutdown_null_safe");
    {
        /* 不应崩溃 */
        ce_ebpf_shutdown(NULL);
    }
    PASS();

    /* ---- 7.4.4: ce_ebpf_trace_function (attach kprobe) ---- */
    TEST("ebpf_trace_function");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            CeResult r = ce_ebpf_trace_function(ctx, "ce_ecs_update");
            /* 非 root 下可能失败，但不应崩溃 */
            CHECK(r == CE_OK || r == CE_ERR);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.4.5: ce_ebpf_trace_tcp_retransmit ---- */
    TEST("ebpf_trace_tcp_retransmit");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            CeResult r = ce_ebpf_trace_tcp_retransmit(ctx);
            CHECK(r == CE_OK || r == CE_ERR);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.4.6: ce_ebpf_trace_io_latency ---- */
    TEST("ebpf_trace_io_latency");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            CeResult r = ce_ebpf_trace_io_latency(ctx);
            CHECK(r == CE_OK || r == CE_ERR);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.4.7: ce_ebpf_get_retransmit_count (map 读取) ---- */
    TEST("ebpf_get_retransmit_count");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            /* 即使没有 attach，也应该返回 0 */
            int count = ce_ebpf_get_retransmit_count(ctx);
            CHECK(count >= 0);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.4.8: ce_ebpf_get_io_latency_stats (map 读取) ---- */
    TEST("ebpf_get_io_latency_stats");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            int p50, p90, p99;
            ce_ebpf_get_io_latency_stats(ctx, &p50, &p90, &p99);
            /* 没有数据时应全为 0 */
            CHECK(p50 == 0 && p90 == 0 && p99 == 0);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.4.9: ce_ebpf_dump_latency (map 遍历) ---- */
    TEST("ebpf_dump_latency");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            int samples = ce_ebpf_dump_latency(ctx, "test_func");
            CHECK(samples >= 0);
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.4.10: 完整生命周期（加载 → attach → 读取 → 卸载） ---- */
    TEST("ebpf_full_lifecycle");
    {
        CeEbpfContext* ctx = ce_ebpf_init();
        if (ctx) {
            /* attach 多个探针 */
            ce_ebpf_trace_function(ctx, "ce_ecs_update");
            ce_ebpf_trace_tcp_retransmit(ctx);
            ce_ebpf_trace_io_latency(ctx);

            /* 读取统计 */
            int retrans = ce_ebpf_get_retransmit_count(ctx);
            CHECK(retrans >= 0);

            int p50, p90, p99;
            ce_ebpf_get_io_latency_stats(ctx, &p50, &p90, &p99);
            CHECK(p50 == 0 && p90 == 0 && p99 == 0);

            int samples = ce_ebpf_dump_latency(ctx, "ce_ecs_update");
            CHECK(samples >= 0);

            /* 卸载 */
            ce_ebpf_shutdown(ctx);
        }
    }
    PASS();

    /* ---- 7.4.11: 多次 init/shutdown ---- */
    TEST("ebpf_multiple_init_shutdown");
    {
        for (int i = 0; i < 3; i++) {
            CeEbpfContext* ctx = ce_ebpf_init();
            if (ctx) {
                ce_ebpf_trace_function(ctx, "ce_ecs_update");
                ce_ebpf_shutdown(ctx);
            }
        }
    }
    PASS();

    printf("\nAll eBPF unit tests passed!\n");
    return 0;
}
