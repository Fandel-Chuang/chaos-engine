/*
 * ChaosEngine eBPF 可观测性层
 *
 * Linux 专属，编译时可选（CHAOS_HAS_EBPF）。
 * 提供函数延迟追踪、TCP 重传监控、I/O 延迟直方图。
 */

#ifndef CE_EBPF_H
#define CE_EBPF_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明句柄 ---- */

typedef struct CeEbpfContext CeEbpfContext;

/* ---- 生命周期 ---- */

/** 初始化 eBPF 可观测性（加载 BPF 程序到内核） */
CeEbpfContext* ce_ebpf_init(void);

/** 关闭 eBPF 可观测性（卸载 BPF 程序） */
void ce_ebpf_shutdown(CeEbpfContext* ctx);

/* ---- 函数追踪 ---- */

/** 开始追踪指定函数的延迟（kprobe） */
CeResult ce_ebpf_trace_function(CeEbpfContext* ctx, const char* func_name);

/** 读取函数延迟直方图（输出到日志，返回采样数） */
int ce_ebpf_dump_latency(CeEbpfContext* ctx, const char* func_name);

/* ---- 网络观测 ---- */

/** 开始记录 TCP 重传事件 */
CeResult ce_ebpf_trace_tcp_retransmit(CeEbpfContext* ctx);

/** 获取 TCP 重传统计 */
int ce_ebpf_get_retransmit_count(CeEbpfContext* ctx);

/* ---- I/O 观测 ---- */

/** 开始记录 I/O 延迟 */
CeResult ce_ebpf_trace_io_latency(CeEbpfContext* ctx);

/** 获取 I/O 延迟统计（P50/P90/P99，微秒） */
void ce_ebpf_get_io_latency_stats(CeEbpfContext* ctx, int* p50, int* p90, int* p99);

/* ---- 查询 ---- */

/** eBPF 是否可用 */
CeBool ce_ebpf_available(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_EBPF_H */
