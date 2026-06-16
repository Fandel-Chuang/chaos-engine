/*
 * ChaosEngine XDP 连接分发 — BPF 内核态程序 (Phase 4.2)
 *
 * 编译: clang -O2 -target bpf -I src_c/ebpf -c ce_xdp_dispatch.bpf.c -o ce_xdp_dispatch.bpf.o
 *
 * 功能:
 * 1. 根据协议类型 (TCP/UDP) 分发到不同队列
 * 2. 根据玩家 ID hash 分发到不同服务线程 (CPU)
 * 3. 支持基于源 IP/端口的 consistent hash
 *
 * 分发策略:
 *   - 默认: 基于 5-tuple hash (src_ip, dst_ip, src_port, dst_port, protocol)
 *   - 玩家模式: 基于 payload 中的 player_id 字段 hash
 *
 * 返回:
 *   XDP_PASS        — 继续内核协议栈处理
 *   XDP_REDIRECT    — 重定向到指定 CPU 的 AF_XDP socket
 *   XDP_TX          — 从同一网卡发回
 */

#include "ce_xdp_common.h"

char LICENSE[] SEC("license") = "GPL";

/* ============================================================
 * BPF Maps
 * ============================================================ */

/* 分发目标表: hash_bucket -> CPU */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 256);
    __type(key, __u32);              /* hash bucket */
    __type(value, struct dispatch_target);
} dispatch_targets SEC(".maps");

/* AF_XDP socket map (每个 CPU 一个 XSK) */
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 256);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

/* 玩家 ID -> CPU 映射 (用于玩家粘性路由) */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, __u64);    /* player_id */
    __type(value, __u32);  /* target CPU */
} player_cpu_map SEC(".maps");

/* 配置 map */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} xdp_dispatch_config SEC(".maps");

/*
 * 配置索引:
 *   [0] = num_cpus (分发目标数量)
 *   [1] = dispatch_mode (0=5-tuple hash, 1=player_id hash, 2=round-robin)
 *   [2] = player_id_offset (payload 中 player_id 的偏移量, 用于 UDP 游戏协议)
 *   [3] = redirect_enabled (0=PASS, 1=REDIRECT to XSK)
 */

/* 统计 map */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u64);
} xdp_dispatch_stats SEC(".maps");

/*
 * 统计索引:
 *   [0] = total_packets
 *   [1] = tcp_packets
 *   [2] = udp_packets
 *   [3] = redirected
 *   [4] = passed
 *   [5] = dropped
 */

/* ---- 辅助函数 ---- */

static __always_inline __u64 get_dispatch_config(__u32 idx, __u64 default_val) {
    __u32 key = idx;
    __u64 *val = bpf_map_lookup_elem(&xdp_dispatch_config, &key);
    return val ? *val : default_val;
}

static __always_inline void inc_dispatch_stat(__u32 idx) {
    __u32 key = idx;
    __u64 *val = bpf_map_lookup_elem(&xdp_dispatch_stats, &key);
    if (val)
        __sync_fetch_and_add(val, 1);
}

/* ---- Jenkins hash (快速、低碰撞) ---- */
static __always_inline __u32 jenkins_hash(const __u8 *data, __u32 len, __u32 seed) {
    __u32 hash = seed;
    for (__u32 i = 0; i < len; i++) {
        hash += data[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

/* ---- 5-tuple hash ---- */
static __always_inline __u32 hash_5tuple(__be32 src_ip, __be32 dst_ip,
                                          __be16 src_port, __be16 dst_port,
                                          __u8 protocol) {
    __u8 buf[13];  /* 4+4+2+2+1 */
    *(__be32 *)(buf + 0) = src_ip;
    *(__be32 *)(buf + 4) = dst_ip;
    *(__be16 *)(buf + 8) = src_port;
    *(__be16 *)(buf + 10) = dst_port;
    buf[12] = protocol;
    return jenkins_hash(buf, sizeof(buf), 0xDEADBEEF);
}

/* ---- 提取 player_id (从 UDP payload) ---- */
static __always_inline int extract_player_id(void *payload_start,
                                              void *data_end,
                                              __u32 offset,
                                              __u64 *player_id) {
    /* 检查偏移量是否在包范围内 */
    if ((__u8 *)payload_start + offset + 8 > (__u8 *)data_end)
        return -1;

    *player_id = *(__u64 *)((__u8 *)payload_start + offset);
    return 0;
}

/* ============================================================
 * XDP 主程序
 * ============================================================ */

SEC("xdp")
int xdp_dispatch(struct xdp_md *ctx) {
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    struct ethhdr *eth;
    struct iphdr *ip;
    struct tcphdr *tcp;
    struct udphdr *udp;
    void *next_hdr;

    inc_dispatch_stat(0);  /* total_packets */

    /* 解析以太网头 */
    eth = data;
    if ((void *)(eth + 1) > data_end)
        goto drop;
    next_hdr = (void *)(eth + 1);

    /* 只处理 IPv4 */
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        goto pass;

    /* 解析 IPv4 头 */
    ip = next_hdr;
    if ((void *)(ip + 1) > data_end)
        goto drop;
    next_hdr = (void *)ip + (ip->ihl * 4);
    if (next_hdr > data_end)
        goto drop;

    __u32 src_ip = ip->saddr;
    __u32 dst_ip = ip->daddr;
    __u8 protocol = ip->protocol;
    __u16 src_port = 0;
    __u16 dst_port = 0;
    __u32 hash_bucket;
    __u64 dispatch_mode = get_dispatch_config(1, 0);

    /* 解析传输层头 */
    if (protocol == IPPROTO_TCP) {
        inc_dispatch_stat(1);  /* tcp_packets */

        tcp = next_hdr;
        if ((void *)(tcp + 1) > data_end)
            goto drop;
        next_hdr = (void *)tcp + (tcp->doff * 4);
        if (next_hdr > data_end)
            goto drop;

        src_port = bpf_ntohs(tcp->source);
        dst_port = bpf_ntohs(tcp->dest);

        /* TCP: 使用 5-tuple hash */
        hash_bucket = hash_5tuple(ip->saddr, ip->daddr,
                                   tcp->source, tcp->dest, protocol);

    } else if (protocol == IPPROTO_UDP) {
        inc_dispatch_stat(2);  /* udp_packets */

        udp = next_hdr;
        if ((void *)(udp + 1) > data_end)
            goto drop;
        next_hdr = (void *)(udp + 1);
        if (next_hdr > data_end)
            goto drop;

        src_port = bpf_ntohs(udp->source);
        dst_port = bpf_ntohs(udp->dest);

        /* 检查是否为玩家模式 */
        if (dispatch_mode == 1) {
            /* 尝试从 payload 提取 player_id */
            __u64 player_id = 0;
            __u32 offset = (__u32)get_dispatch_config(2, 0);

            if (extract_player_id(next_hdr, data_end, offset, &player_id) == 0) {
                /* 查找或分配 player -> CPU */
                __u32 *cpu = bpf_map_lookup_elem(&player_cpu_map, &player_id);
                if (cpu) {
                    hash_bucket = *cpu;
                } else {
                    /* 新玩家: 基于 player_id hash 分配 */
                    hash_bucket = jenkins_hash((__u8 *)&player_id, 8, 0xCAFE);
                    __u32 num_cpus = (__u32)get_dispatch_config(0, 1);
                    if (num_cpus > 0)
                        hash_bucket = hash_bucket % num_cpus;

                    /* 记录映射 */
                    bpf_map_update_elem(&player_cpu_map, &player_id, &hash_bucket, BPF_ANY);
                }
            } else {
                /* 无法提取 player_id，回退到 5-tuple hash */
                hash_bucket = hash_5tuple(ip->saddr, ip->daddr,
                                           udp->source, udp->dest, protocol);
            }
        } else {
            /* 默认: 5-tuple hash */
            hash_bucket = hash_5tuple(ip->saddr, ip->daddr,
                                       udp->source, udp->dest, protocol);
        }
    } else {
        /* 非 TCP/UDP，直接放行 */
        goto pass;
    }

    /* 取模到目标数量 */
    {
        __u32 num_cpus = (__u32)get_dispatch_config(0, 1);
        if (num_cpus > 0)
            hash_bucket = hash_bucket % num_cpus;
    }

    /* 检查是否启用 XSK redirect */
    if (get_dispatch_config(3, 0)) {
        /* 重定向到 AF_XDP socket */
        int ret = bpf_redirect_map(&xsks_map, hash_bucket, 0);
        if (ret == XDP_REDIRECT) {
            inc_dispatch_stat(3);  /* redirected */
            return ret;
        }
        /* redirect 失败，回退到 PASS */
    }

pass:
    inc_dispatch_stat(4);  /* passed */
    return XDP_PASS;

drop:
    inc_dispatch_stat(5);  /* dropped */
    return XDP_DROP;
}
