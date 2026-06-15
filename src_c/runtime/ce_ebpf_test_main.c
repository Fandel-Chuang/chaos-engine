/*
 * ChaosEngine eBPF 可观测性测试
 */

#define _POSIX_C_SOURCE 200112L
#include "public_api/chaos_engine.h"
#include "network/ce_ebpf.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    CeEngineConfig config = {
        .app_name = "ChaosEngine-eBPF-Test",
        .window_width = 0, .window_height = 0,
        .fullscreen = CE_FALSE, .vsync = CE_FALSE,
        .log_level = CE_LOG_INFO
    };
    ce_init(&config);

    printf("========================================\n");
    printf("  ChaosEngine eBPF Observability Test\n");
    printf("  BTF available: %s\n", ce_ebpf_available() ? "YES" : "NO");
    printf("========================================\n\n");

    if (!ce_ebpf_available()) {
        printf("⚠️  BTF not available, skipping eBPF tests\n");
        ce_shutdown();
        return 0;
    }

    /* 初始化 eBPF */
    CeEbpfContext* ebpf = ce_ebpf_init();
    if (!ebpf) {
        printf("❌ Failed to init eBPF (may need root/CAP_BPF)\n");
        ce_shutdown();
        return 1;
    }
    printf("✅ eBPF context created\n");

    /* 测试 1: 函数追踪 */
    if (ce_ebpf_trace_function(ebpf, "ce_ecs_update") == CE_OK) {
        printf("✅ Function trace: ce_ecs_update\n");
    } else {
        printf("⚠️  Function trace failed (may need root)\n");
    }

    /* 测试 2: TCP 重传监控 */
    if (ce_ebpf_trace_tcp_retransmit(ebpf) == CE_OK) {
        printf("✅ TCP retransmit monitor\n");
    } else {
        printf("⚠️  TCP retransmit monitor failed (may need root)\n");
    }

    /* 测试 3: I/O 延迟 */
    if (ce_ebpf_trace_io_latency(ebpf) == CE_OK) {
        printf("✅ I/O latency trace\n");
    } else {
        printf("⚠️  I/O latency trace failed (may need root)\n");
    }

    /* 运行一会儿收集数据 */
    printf("\nRunning for 2 seconds to collect data...\n");
    for (int i = 0; i < 20; i++) {
        ce_update();
        usleep(100000);  /* 100ms */
    }

    /* 读取统计 */
    int retrans = ce_ebpf_get_retransmit_count(ebpf);
    printf("TCP retransmits: %d\n", retrans);

    int p50, p90, p99;
    ce_ebpf_get_io_latency_stats(ebpf, &p50, &p90, &p99);
    printf("I/O latency: P50=%dus P90=%dus P99=%dus\n", p50, p90, p99);

    /* 打印直方图 */
    ce_ebpf_dump_latency(ebpf, "ce_ecs_update");

    ce_ebpf_shutdown(ebpf);
    ce_shutdown();

    printf("\n🎉 eBPF observability test complete!\n");
    return 0;
}
