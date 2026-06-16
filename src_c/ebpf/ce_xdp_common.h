/*
 * ChaosEngine XDP Common — 共享数据结构和包解析辅助函数
 *
 * 供 ce_xdp_filter.bpf.c 和 ce_xdp_dispatch.bpf.c 使用。
 * 定义 Ethernet / IP / TCP / UDP 头部结构（不依赖 vmlinux.h）。
 */

#ifndef CE_XDP_COMMON_H
#define CE_XDP_COMMON_H

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* ---- 协议常量 ---- */
#define ETH_P_IP   0x0800
#define ETH_P_IPV6 0x86DD
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

#define ETH_HDR_LEN  14
#define IP_HDR_MAX_LEN 60
#define TCP_HDR_MAX_LEN 60
#define UDP_HDR_LEN 8

/* ---- 以太网头 ---- */
struct ethhdr {
    __u8  h_dest[6];
    __u8  h_source[6];
    __be16 h_proto;
} __attribute__((packed));

/* ---- IPv4 头 ---- */
struct iphdr {
    __u8    ihl:4, version:4;
    __u8    tos;
    __be16  tot_len;
    __be16  id;
    __be16  frag_off;
    __u8    ttl;
    __u8    protocol;
    __sum16 check;
    __be32  saddr;
    __be32  daddr;
} __attribute__((packed));

/* ---- TCP 头 ---- */
struct tcphdr {
    __be16 source;
    __be16 dest;
    __be32 seq;
    __be32 ack_seq;
    __u16  res1:4, doff:4, fin:1, syn:1, rst:1, psh:1, ack:1, urg:1, ece:1, cwr:1;
    __be16 window;
    __sum16 check;
    __be16 urg_ptr;
} __attribute__((packed));

/* ---- UDP 头 ---- */
struct udphdr {
    __be16 source;
    __be16 dest;
    __be16 len;
    __sum16 check;
} __attribute__((packed));

/* ---- 速率限制状态（per-IP token bucket） ---- */
struct rate_limit_state {
    __u64 last_ns;   /* 上次更新时间 (纳秒) */
    __u64 tokens;    /* 当前令牌数 */
};

/* ---- SYN flood 检测状态 ---- */
struct syn_flood_state {
    __u64 last_window_ns;  /* 当前窗口起始时间 */
    __u32 syn_count;       /* 窗口内 SYN 计数 */
};

/* ---- 分发目标信息 ---- */
struct dispatch_target {
    __u32 cpu;       /* 目标 CPU */
    __u32 weight;    /* 权重 */
};

/* ---- 辅助函数：解析以太网头 ---- */
static __always_inline int xdp_parse_eth(struct xdp_md *ctx,
                                          struct ethhdr **eth,
                                          void **next_hdr,
                                          void *data_end)
{
    void *data = (void *)(long)ctx->data;
    *eth = data;
    if ((void *)(*eth + 1) > data_end)
        return -1;
    *next_hdr = (void *)(*eth + 1);
    return 0;
}

/* ---- 辅助函数：解析 IPv4 头 ---- */
static __always_inline int xdp_parse_ipv4(void *nh,
                                           struct iphdr **ip,
                                           void **next_hdr,
                                           void *data_end)
{
    *ip = nh;
    if ((void *)(*ip + 1) > data_end)
        return -1;
    /* 只处理无分片的包（frag_off 忽略 DF/MF 位） */
    if ((*ip)->frag_off & bpf_htons(0x3FFF))
        return -2;  /* 分片包，跳过 */
    *next_hdr = (void *)(*ip) + ((*ip)->ihl * 4);
    if (*next_hdr > data_end)
        return -1;
    return 0;
}

/* ---- 辅助函数：解析 TCP 头 ---- */
static __always_inline int xdp_parse_tcp(void *nh,
                                          struct tcphdr **tcp,
                                          void **next_hdr,
                                          void *data_end)
{
    *tcp = nh;
    if ((void *)(*tcp + 1) > data_end)
        return -1;
    *next_hdr = (void *)(*tcp) + ((*tcp)->doff * 4);
    if (*next_hdr > data_end)
        return -1;
    return 0;
}

/* ---- 辅助函数：解析 UDP 头 ---- */
static __always_inline int xdp_parse_udp(void *nh,
                                          struct udphdr **udp,
                                          void **next_hdr,
                                          void *data_end)
{
    *udp = nh;
    if ((void *)(*udp + 1) > data_end)
        return -1;
    *next_hdr = (void *)(*udp + 1);
    if (*next_hdr > data_end)
        return -1;
    return 0;
}

#endif /* CE_XDP_COMMON_H */
