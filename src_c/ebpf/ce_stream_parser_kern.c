/*
 * ChaosEngine Phase 4.3: BPF Stream Parser (BPF_PROG_TYPE_SK_SKB)
 *
 * 在 socket 层解析自定义协议头 [2B type][4B len]，完成消息分帧，
 * 使用 sockmap 重定向到目标 socket。
 *
 * 编译: clang -O2 -target bpf -c ce_stream_parser_kern.c -o ce_stream_parser_kern.o
 *
 * 自定义协议格式 (大端):
 *   Offset  Size  Field
 *   0       2     msg_type   (消息类型: 0=控制, 1=数据, 2=心跳, 3=错误)
 *   2       4     msg_len    (payload 长度，不含头)
 *   6       N     payload    (消息体)
 *
 * 总消息长度 = 6 + msg_len
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

char LICENSE[] SEC("license") = "GPL";

/* ---- 自定义协议常量 ---- */
#define CE_PROTO_HDR_SIZE  6    /* 2B type + 4B len */
#define CE_PROTO_TYPE_OFF  0
#define CE_PROTO_LEN_OFF   2
#define CE_PROTO_MAX_PAYLOAD 65535

/* 消息类型 */
#define CE_MSG_TYPE_CTRL    0
#define CE_MSG_TYPE_DATA    1
#define CE_MSG_TYPE_HB      2
#define CE_MSG_TYPE_ERR     3

/* ---- BPF Maps ---- */

/* sockmap: 用于 BPF stream verdict 重定向。
 * key = socket index, value = target socket fd
 * 通过 bpf_msg_redirect_map() 将解析后的消息重定向到目标 socket */
struct {
    __uint(type, BPF_MAP_TYPE_SOCKMAP);
    __uint(max_entries, 256);
    __type(key, __u32);
    __type(value, __u64);
} ce_sock_map SEC(".maps");

/* 消息统计: 按消息类型计数 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} ce_msg_stats SEC(".maps");

/* 解析状态缓存: 记录每个 socket 当前已接收但未完成分帧的字节数
 * key = socket cookie (u64), value = 累积字节数 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, __u32);
} ce_parse_state SEC(".maps");

/* ---- 辅助内联函数 ---- */

/* 从 skb 数据中读取大端 16 位值 */
static __always_inline __u16 read_be16(const unsigned char *data) {
    return ((__u16)data[0] << 8) | (__u16)data[1];
}

/* 从 skb 数据中读取大端 32 位值 */
static __always_inline __u32 read_be32(const unsigned char *data) {
    return ((__u32)data[0] << 24) |
           ((__u32)data[1] << 16) |
           ((__u32)data[2] << 8)  |
           (__u32)data[3];
}

/* ============================================================
 * Stream Parser: 解析 TCP 流，识别消息边界
 *
 * 返回: 完整消息的长度（含 6 字节头），或 0 表示数据不足
 *
 * BPF 运行时会在每次 recv 时调用此函数。
 * skb->data 指向本次接收到的数据。
 * 返回值为正数时，内核将该长度的数据视为一条完整消息，
 * 然后调用 stream_verdict 程序决定如何处理。
 * ============================================================ */
SEC("sk_skb/stream_parser")
int ce_stream_parser(struct __sk_buff *skb) {
    /* 获取 skb 数据长度 */
    __u32 data_len = skb->len;
    __u32 data_end = skb->data + data_len;

    /* 至少需要 6 字节协议头 */
    if (skb->data + CE_PROTO_HDR_SIZE > data_end) {
        /* 数据不足，返回 0 让内核继续累积 */
        return 0;
    }

    /* 读取协议头 (直接通过 skb 指针访问) */
    /* 注意: BPF verifier 要求边界检查 */
    unsigned char *data = (unsigned char *)(long)skb->data;

    /* 边界检查 — verifier 需要 */
    if ((void *)(data + CE_PROTO_HDR_SIZE) > (void *)(long)data_end) {
        return 0;
    }

    __u16 msg_type = read_be16(data + CE_PROTO_TYPE_OFF);
    __u32 msg_len  = read_be32(data + CE_PROTO_LEN_OFF);

    /* 合理性检查 */
    if (msg_len > CE_PROTO_MAX_PAYLOAD) {
        /* 无效长度，跳过此消息（返回 1 丢弃首字节让解析器重新同步） */
        return 1;
    }

    /* 消息总长度 = 头 + payload */
    __u32 total_len = CE_PROTO_HDR_SIZE + msg_len;

    /* 检查是否有完整的消息 */
    if (data_len < total_len) {
        /* 数据不足，返回 0 等待更多数据 */
        return 0;
    }

    /* 更新统计: 按消息类型计数 */
    __u32 stat_key = msg_type;
    if (stat_key > 7) stat_key = 7; /* 安全限制 */
    __u64 *count = bpf_map_lookup_elem(&ce_msg_stats, &stat_key);
    if (count) {
        __sync_fetch_and_add(count, 1);
    }

    /* 返回完整消息长度，内核将把这段数据作为一条消息
     * 传递给 stream_verdict 程序 */
    return total_len;
}

/* ============================================================
 * Stream Verdict: 决定解析后的消息去向
 *
 * 根据消息类型决定:
 *   - CE_MSG_TYPE_CTRL (0): 重定向到 sockmap index 0 (控制通道)
 *   - CE_MSG_TYPE_DATA (1): 重定向到 sockmap index 1 (数据通道)
 *   - CE_MSG_TYPE_HB   (2): 重定向到 sockmap index 2 (心跳通道)
 *   - CE_MSG_TYPE_ERR  (3): 丢弃 (SK_DROP)
 *   - 其他: 传递给本地 socket (SK_PASS)
 * ============================================================ */
SEC("sk_skb/stream_verdict")
int ce_stream_verdict(struct __sk_buff *skb) {
    unsigned char *data = (unsigned char *)(long)skb->data;
    __u32 data_end = skb->data + skb->len;

    /* 边界检查 */
    if ((void *)(data + CE_PROTO_HDR_SIZE) > (void *)(long)data_end) {
        return bpf_skb_pull_data(skb, CE_PROTO_HDR_SIZE);
    }

    __u16 msg_type = read_be16(data + CE_PROTO_TYPE_OFF);

    switch (msg_type) {
    case CE_MSG_TYPE_CTRL:
        /* 重定向到 sockmap index 0 */
        return bpf_sk_redirect_map(skb, &ce_sock_map, 0, 0);

    case CE_MSG_TYPE_DATA:
        /* 重定向到 sockmap index 1 */
        return bpf_sk_redirect_map(skb, &ce_sock_map, 1, 0);

    case CE_MSG_TYPE_HB:
        /* 重定向到 sockmap index 2 */
        return bpf_sk_redirect_map(skb, &ce_sock_map, 2, 0);

    case CE_MSG_TYPE_ERR:
        /* 丢弃错误消息 */
        return SK_DROP;

    default:
        /* 未知类型，传递给本地 */
        return SK_PASS;
    }
}
