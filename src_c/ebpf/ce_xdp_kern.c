/*
 * ChaosEngine XDP 性能基准测试 — BPF 内核态程序
 *
 * 编译: clang -O2 -g -target bpf -c ce_xdp_kern.c -o ce_xdp_kern.o
 *
 * 功能:
 * 1. XDP_DROP: 在网卡驱动层丢弃数据包（用于 DDoS 防护基准）
 * 2. XDP_PASS: 将数据包传递给内核网络栈（用于对比测试）
 * 3. XDP_TX:  将数据包从同一网卡发回（用于 echo 基准）
 * 4. 数据包计数统计
 *
 * 加载: ip link set dev <iface> xdpgeneric obj ce_xdp_kern.o sec xdp
 *       或: ip link set dev <iface> xdpdrv obj ce_xdp_kern.o sec xdp
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* ---- License (required) ---- */
char LICENSE[] SEC("license") = "GPL";

/* ---- 配置 Map: 运行模式 ---- */
/* key=0, value: 0=PASS, 1=DROP, 2=TX (echo) */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} xdp_mode SEC(".maps");

/* ---- 统计 Map: 数据包计数 ---- */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} xdp_stats SEC(".maps");

/* 统计索引 */
#define STAT_RX_PACKETS   0  /* 接收的总包数 */
#define STAT_RX_BYTES     1  /* 接收的总字节数 */
#define STAT_PASS_PACKETS 2  /* 传递给内核的包数 */
#define STAT_DROP_PACKETS 3  /* 丢弃的包数 */
#define STAT_TX_PACKETS   4  /* 发回的包数 */
#define STAT_ERR_PACKETS  5  /* 错误包数 */

/* ---- 配置 Map: 目标端口过滤 ---- */
/* key=0, value: 目标端口（网络字节序），0=不过滤 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u16);
} xdp_port_filter SEC(".maps");

/* ---- 辅助宏 ---- */
static __always_inline void inc_stat(__u32 idx, __u64 delta) {
    __u64 *val = bpf_map_lookup_elem(&xdp_stats, &idx);
    if (val) {
        *val += delta;
    }
}

/* ---- XDP 主程序 ---- */
SEC("xdp")
int xdp_prog(struct xdp_md *ctx) {
    /* 获取数据指针 */
    void *data_end = (void *)(long)ctx->data_end;
    void *data     = (void *)(long)ctx->data;

    /* 解析以太网头 */
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) {
        inc_stat(STAT_ERR_PACKETS, 1);
        return XDP_DROP;
    }

    /* 只处理 IPv4 */
    if (eth->h_proto != bpf_htons(0x0800)) {
        /* 非 IPv4 包传递给内核 */
        return XDP_PASS;
    }

    /* 解析 IP 头 */
    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end) {
        inc_stat(STAT_ERR_PACKETS, 1);
        return XDP_DROP;
    }

    /* 只处理 TCP */
    if (ip->protocol != 6) { /* IPPROTO_TCP */
        return XDP_PASS;
    }

    /* 解析 TCP 头 */
    struct tcphdr *tcp = (void *)(ip + 1);
    if ((void *)(tcp + 1) > data_end) {
        inc_stat(STAT_ERR_PACKETS, 1);
        return XDP_DROP;
    }

    /* 端口过滤 */
    __u32 key = 0;
    __u16 *port_filter = bpf_map_lookup_elem(&xdp_port_filter, &key);
    if (port_filter && *port_filter != 0) {
        if (tcp->dest != *port_filter && tcp->source != *port_filter) {
            return XDP_PASS; /* 不匹配的端口传给内核 */
        }
    }

    /* 统计 */
    __u32 pkt_size = (__u32)(data_end - data);
    inc_stat(STAT_RX_PACKETS, 1);
    inc_stat(STAT_RX_BYTES, pkt_size);

    /* 获取运行模式 */
    __u32 *mode = bpf_map_lookup_elem(&xdp_mode, &key);
    __u32 action = mode ? *mode : 0; /* 默认 PASS */

    switch (action) {
    case 1: /* DROP */
        inc_stat(STAT_DROP_PACKETS, 1);
        return XDP_DROP;

    case 2: /* TX (echo) — 交换 MAC 地址并发回 */
        {
            /* 交换源/目标 MAC */
            unsigned char tmp[6];
            __builtin_memcpy(tmp, eth->h_source, 6);
            __builtin_memcpy(eth->h_source, eth->h_dest, 6);
            __builtin_memcpy(eth->h_dest, tmp, 6);

            /* 交换 IP 地址 */
            __be32 tmp_ip = ip->saddr;
            ip->saddr = ip->daddr;
            ip->daddr = tmp_ip;

            /* 交换 TCP 端口 */
            __be16 tmp_port = tcp->source;
            tcp->source = tcp->dest;
            tcp->dest = tmp_port;

            /* 清除 TCP 标志（简化处理） */
            /* 在实际 echo 场景中，需要更完整的 TCP 处理 */

            inc_stat(STAT_TX_PACKETS, 1);
            return XDP_TX;
        }

    case 0: /* PASS */
    default:
        inc_stat(STAT_PASS_PACKETS, 1);
        return XDP_PASS;
    }
}
