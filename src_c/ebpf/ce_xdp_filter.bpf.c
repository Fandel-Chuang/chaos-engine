/*
 * ChaosEngine XDP 包过滤 — BPF 内核态程序 (Phase 4.1)
 *
 * 编译: clang -O2 -target bpf -I src_c/ebpf -c ce_xdp_filter.bpf.c -o ce_xdp_filter.bpf.o
 *
 * 功能:
 * 1. IP 黑名单 — 直接丢弃黑名单 IP 的所有包
 * 2. SYN flood 检测 — 检测每 IP 的 SYN 速率，超过阈值则丢弃
 * 3. 速率限制 — per-IP token bucket，超过速率则丢弃
 *
 * 返回:
 *   XDP_DROP   — 丢弃恶意包
 *   XDP_PASS   — 放行正常包
 *   XDP_ABORTED — 解析错误
 */

#include "ce_xdp_common.h"

char LICENSE[] SEC("license") = "GPL";

/* ============================================================
 * BPF Maps
 * ============================================================ */

/* IP 黑名单 (IPv4 addr -> 1) */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10000);
    __type(key, __u32);    /* 源 IP (网络字节序) */
    __type(value, __u8);   /* 1 = 黑名单 */
} ip_blacklist SEC(".maps");

/* SYN flood 检测状态 (per-IP) */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 100000);
    __type(key, __u32);    /* 源 IP */
    __type(value, struct syn_flood_state);
} syn_flood_state SEC(".maps");

/* 速率限制状态 (per-IP token bucket) */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 100000);
    __type(key, __u32);    /* 源 IP */
    __type(value, struct rate_limit_state);
} rate_limit_state SEC(".maps");

/* 配置 map (运行时通过用户态更新) */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} xdp_filter_config SEC(".maps");

/*
 * 配置索引:
 *   [0] = syn_flood_threshold (SYN/窗口, 默认 100)
 *   [1] = syn_flood_window_ns (检测窗口, 默认 1秒 = 1000000000)
 *   [2] = rate_limit_tokens_per_sec (令牌补充速率, 默认 10000)
 *   [3] = rate_limit_bucket_size (令牌桶容量, 默认 10000)
 *   [4] = filter_enabled (0=全放行, 1=启用过滤)
 */

/* 统计 map */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} xdp_filter_stats SEC(".maps");

/*
 * 统计索引:
 *   [0] = total_packets
 *   [1] = passed_packets
 *   [2] = dropped_blacklist
 *   [3] = dropped_syn_flood
 *   [4] = dropped_rate_limit
 *   [5] = dropped_non_ip
 */

/* ---- 辅助函数：读取配置 ---- */
static __always_inline __u64 get_config(__u32 idx, __u64 default_val) {
    __u32 key = idx;
    __u64 *val = bpf_map_lookup_elem(&xdp_filter_config, &key);
    return val ? *val : default_val;
}

/* ---- 辅助函数：递增统计 ---- */
static __always_inline void inc_stat(__u32 idx) {
    __u32 key = idx;
    __u64 *val = bpf_map_lookup_elem(&xdp_filter_stats, &key);
    if (val)
        __sync_fetch_and_add(val, 1);
}

/* ============================================================
 * XDP 主程序
 * ============================================================ */

SEC("xdp")
int xdp_filter(struct xdp_md *ctx) {
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    struct ethhdr *eth;
    struct iphdr *ip;
    struct tcphdr *tcp;
    void *next_hdr;

    inc_stat(0);  /* total_packets */

    /* 检查过滤是否启用 */
    if (get_config(4, 1) == 0) {
        inc_stat(1);
        return XDP_PASS;
    }

    /* 解析以太网头 */
    eth = data;
    if ((void *)(eth + 1) > data_end)
        goto drop_non_ip;
    next_hdr = (void *)(eth + 1);

    /* 只处理 IPv4 */
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        goto pass;

    /* 解析 IPv4 头 */
    ip = next_hdr;
    if ((void *)(ip + 1) > data_end)
        goto drop_non_ip;
    next_hdr = (void *)ip + (ip->ihl * 4);
    if (next_hdr > data_end)
        goto drop_non_ip;

    __u32 src_ip = ip->saddr;

    /* ---- 1. IP 黑名单检查 ---- */
    {
        __u8 *blacklisted = bpf_map_lookup_elem(&ip_blacklist, &src_ip);
        if (blacklisted && *blacklisted) {
            inc_stat(2);  /* dropped_blacklist */
            return XDP_DROP;
        }
    }

    /* ---- 2. SYN flood 检测 (仅 TCP) ---- */
    if (ip->protocol == IPPROTO_TCP) {
        tcp = next_hdr;
        if ((void *)(tcp + 1) > data_end)
            goto drop_non_ip;

        /* 只检测 SYN (SYN=1, ACK=0) */
        if (tcp->syn && !tcp->ack) {
            __u64 now = bpf_ktime_get_ns();
            __u64 window_ns = get_config(1, 1000000000ULL);  /* 1 秒 */
            __u64 threshold = get_config(0, 100);            /* 100 SYN/秒 */

            struct syn_flood_state *state, new_state = {};
            state = bpf_map_lookup_elem(&syn_flood_state, &src_ip);

            if (!state) {
                /* 首次 SYN */
                new_state.last_window_ns = now;
                new_state.syn_count = 1;
                bpf_map_update_elem(&syn_flood_state, &src_ip, &new_state, BPF_ANY);
            } else {
                /* 检查是否在同一窗口内 */
                if (now - state->last_window_ns < window_ns) {
                    /* 同一窗口内，递增计数 */
                    __sync_fetch_and_add(&state->syn_count, 1);
                    /* 读取最新值 */
                    struct syn_flood_state *check = bpf_map_lookup_elem(&syn_flood_state, &src_ip);
                    if (check && check->syn_count > threshold) {
                        inc_stat(3);  /* dropped_syn_flood */
                        return XDP_DROP;
                    }
                } else {
                    /* 新窗口，重置 */
                    state->last_window_ns = now;
                    state->syn_count = 1;
                }
            }
        }
    }

    /* ---- 3. 速率限制 (per-IP token bucket) ---- */
    {
        __u64 now = bpf_ktime_get_ns();
        __u64 rate = get_config(2, 10000);   /* 令牌补充速率 (tokens/sec) */
        __u64 burst = get_config(3, 10000);  /* 桶容量 */

        struct rate_limit_state *state, new_state = {};
        state = bpf_map_lookup_elem(&rate_limit_state, &src_ip);

        if (!state) {
            /* 首次见到此 IP */
            new_state.last_ns = now;
            new_state.tokens = burst - 1;  /* 消耗 1 个令牌 */
            bpf_map_update_elem(&rate_limit_state, &src_ip, &new_state, BPF_ANY);
        } else {
            /* 计算补充的令牌数 */
            __u64 elapsed_ns = now - state->last_ns;
            __u64 new_tokens = state->tokens + (elapsed_ns * rate / 1000000000ULL);

            if (new_tokens > burst)
                new_tokens = burst;

            if (new_tokens == 0) {
                /* 无令牌可用，丢弃 */
                inc_stat(4);  /* dropped_rate_limit */
                return XDP_DROP;
            }

            /* 消耗 1 个令牌 */
            state->tokens = new_tokens - 1;
            state->last_ns = now;
        }
    }

pass:
    inc_stat(1);  /* passed_packets */
    return XDP_PASS;

drop_non_ip:
    inc_stat(5);  /* dropped_non_ip */
    return XDP_ABORTED;
}
