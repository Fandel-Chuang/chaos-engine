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

/* ---- XDP 包过滤 (Phase 4.1) ---- */

/** 加载并 attach XDP 包过滤程序到指定网络接口 */
CeResult ce_ebpf_xdp_filter_attach(CeEbpfContext* ctx, const char* ifname);

/** 添加 IP 到黑名单 */
CeResult ce_ebpf_xdp_filter_blacklist_add(CeEbpfContext* ctx, uint32_t ip_addr);

/** 从黑名单移除 IP */
CeResult ce_ebpf_xdp_filter_blacklist_del(CeEbpfContext* ctx, uint32_t ip_addr);

/** 设置 SYN flood 检测阈值 (SYN/秒, 0=禁用) */
CeResult ce_ebpf_xdp_filter_set_syn_threshold(CeEbpfContext* ctx, uint32_t threshold);

/** 设置速率限制 (tokens/秒, 0=禁用) */
CeResult ce_ebpf_xdp_filter_set_rate_limit(CeEbpfContext* ctx, uint32_t tokens_per_sec);

/** 启用/禁用过滤 */
CeResult ce_ebpf_xdp_filter_set_enabled(CeEbpfContext* ctx, CeBool enabled);

/** 获取 XDP 过滤统计 */
void ce_ebpf_xdp_filter_get_stats(CeEbpfContext* ctx,
                                   uint64_t* total, uint64_t* passed,
                                   uint64_t* dropped_blacklist,
                                   uint64_t* dropped_syn_flood,
                                   uint64_t* dropped_rate_limit);

/* ---- XDP 连接分发 (Phase 4.2) ---- */

/** 加载并 attach XDP 连接分发程序到指定网络接口 */
CeResult ce_ebpf_xdp_dispatch_attach(CeEbpfContext* ctx, const char* ifname);

/** 设置分发目标数量 (CPU 数) */
CeResult ce_ebpf_xdp_dispatch_set_num_cpus(CeEbpfContext* ctx, uint32_t num_cpus);

/** 设置分发模式 (0=5-tuple hash, 1=player_id hash) */
CeResult ce_ebpf_xdp_dispatch_set_mode(CeEbpfContext* ctx, uint32_t mode);

/** 设置 player_id 在 UDP payload 中的偏移量 */
CeResult ce_ebpf_xdp_dispatch_set_player_id_offset(CeEbpfContext* ctx, uint32_t offset);

/** 添加 AF_XDP socket 到 XSK map */
CeResult ce_ebpf_xdp_dispatch_add_xsk(CeEbpfContext* ctx, uint32_t queue_id, int xsk_fd);

/** 获取 XDP 分发统计 */
void ce_ebpf_xdp_dispatch_get_stats(CeEbpfContext* ctx,
                                     uint64_t* total, uint64_t* tcp,
                                     uint64_t* udp, uint64_t* redirected,
                                     uint64_t* passed, uint64_t* dropped);

/* ---- 查询 ---- */

/** eBPF 是否可用 */
CeBool ce_ebpf_available(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_EBPF_H */
