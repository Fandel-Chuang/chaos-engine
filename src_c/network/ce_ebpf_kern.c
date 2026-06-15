/*
 * ChaosEngine eBPF 可观测性 — BPF 内核态程序
 *
 * 编译: clang -O2 -target bpf -c ce_ebpf_kern.c -o ce_ebpf_kern.o
 *
 * 提供:
 * 1. kprobe 函数延迟追踪 (直方图)
 * 2. kprobe TCP 重传监控 (计数)
 * 3. tracepoint I/O 延迟 (直方图)
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

/* ---- License (required) ---- */
char LICENSE[] SEC("license") = "GPL";

/* ---- Map: 函数延迟直方图 ---- */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);    /* 延迟桶 (微秒) */
    __type(value, __u64);  /* 计数 */
} func_latency_hist SEC(".maps");

/* ---- Map: 函数进入时间戳 ---- */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u32);    /* tid */
    __type(value, __u64);  /* 进入时间戳 (ns) */
} func_entry_ts SEC(".maps");

/* ---- Map: TCP 重传计数 ---- */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);  /* 重传总数 */
} tcp_retransmit_count SEC(".maps");

/* ---- Map: I/O 延迟直方图 ---- */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 256);
    __type(key, __u64);    /* 延迟桶 (微秒) */
    __type(value, __u64);  /* 计数 */
} io_latency_hist SEC(".maps");

/* ---- Map: I/O 操作开始时间 ---- */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u32);    /* tid */
    __type(value, __u64);  /* 开始时间 (ns) */
} io_start_ts SEC(".maps");

/* ============================================================
 * 1. 函数延迟追踪 (kprobe)
 * ============================================================ */

/* kprobe: 函数入口 — 记录时间戳 */
SEC("kprobe/ce_ecs_update")
int kprobe_ecs_update_entry(struct pt_regs* ctx) {
    __u32 tid = bpf_get_current_pid_tgid();
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&func_entry_ts, &tid, &ts, BPF_ANY);
    return 0;
}

/* kretprobe: 函数返回 — 计算延迟并记录直方图 */
SEC("kretprobe/ce_ecs_update")
int kretprobe_ecs_update_exit(struct pt_regs* ctx) {
    __u32 tid = bpf_get_current_pid_tgid();
    __u64* entry_ts = bpf_map_lookup_elem(&func_entry_ts, &tid);
    if (!entry_ts) return 0;

    __u64 delta_ns = bpf_ktime_get_ns() - *entry_ts;
    __u64 delta_us = delta_ns / 1000;

    /* 放入合适的桶 (对数分桶: 1,2,4,8,16,32,64,128,256,512,1024,... us) */
    __u64 bucket = 1;
    while (bucket < delta_us && bucket < (1ULL << 20)) {
        bucket <<= 1;
    }

    __u64* count = bpf_map_lookup_elem(&func_latency_hist, &bucket);
    if (count) {
        __sync_fetch_and_add(count, 1);
    } else {
        __u64 one = 1;
        bpf_map_update_elem(&func_latency_hist, &bucket, &one, BPF_ANY);
    }

    bpf_map_delete_elem(&func_entry_ts, &tid);
    return 0;
}

/* ============================================================
 * 2. TCP 重传监控 (kprobe)
 * ============================================================ */

SEC("kprobe/tcp_retransmit_skb")
int kprobe_tcp_retransmit(struct pt_regs* ctx) {
    __u32 key = 0;
    __u64* count = bpf_map_lookup_elem(&tcp_retransmit_count, &key);
    if (count) {
        __sync_fetch_and_add(count, 1);
    }
    return 0;
}

/* ============================================================
 * 3. I/O 延迟追踪 (tracepoint: sys_enter_recvfrom / sys_exit_recvfrom)
 * ============================================================ */

SEC("tracepoint/syscalls/sys_enter_recvfrom")
int trace_recvfrom_entry(struct trace_event_raw_sys_enter* ctx) {
    __u32 tid = bpf_get_current_pid_tgid();
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&io_start_ts, &tid, &ts, BPF_ANY);
    return 0;
}

SEC("tracepoint/syscalls/sys_exit_recvfrom")
int trace_recvfrom_exit(struct trace_event_raw_sys_exit* ctx) {
    __u32 tid = bpf_get_current_pid_tgid();
    __u64* start_ts = bpf_map_lookup_elem(&io_start_ts, &tid);
    if (!start_ts) return 0;

    __u64 delta_ns = bpf_ktime_get_ns() - *start_ts;
    __u64 delta_us = delta_ns / 1000;

    /* 对数分桶 */
    __u64 bucket = 1;
    while (bucket < delta_us && bucket < (1ULL << 20)) {
        bucket <<= 1;
    }

    __u64* count = bpf_map_lookup_elem(&io_latency_hist, &bucket);
    if (count) {
        __sync_fetch_and_add(count, 1);
    } else {
        __u64 one = 1;
        bpf_map_update_elem(&io_latency_hist, &bucket, &one, BPF_ANY);
    }

    bpf_map_delete_elem(&io_start_ts, &tid);
    return 0;
}
