/*
 * ChaosEngine Phase 4.3-4.4: BPF Stream Parser + io_uring 联动 — 公共头文件
 *
 * 定义自定义协议结构、消息类型、以及 BPF/io_uring 集成 API。
 */

#ifndef CE_STREAM_PARSER_H
#define CE_STREAM_PARSER_H

#include "public_api/ce_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 自定义协议格式 (大端) ---- */

#define CE_PROTO_HDR_SIZE      6     /* 2B type + 4B len */
#define CE_PROTO_MAX_PAYLOAD   65535
#define CE_PROTO_MAX_MSG       (CE_PROTO_HDR_SIZE + CE_PROTO_MAX_PAYLOAD)

/* 消息类型 */
typedef enum CeMsgType {
    CE_MSG_TYPE_CTRL = 0,   /* 控制消息 */
    CE_MSG_TYPE_DATA = 1,   /* 数据消息 */
    CE_MSG_TYPE_HB   = 2,   /* 心跳消息 */
    CE_MSG_TYPE_ERR  = 3,   /* 错误消息 */
    CE_MSG_TYPE_MAX  = 8,
} CeMsgType;

/* 协议头 (网络字节序/大端) */
typedef struct CeProtoHeader {
    uint16_t msg_type;   /* 消息类型 */
    uint32_t msg_len;    /* payload 长度 (不含头) */
} CeProtoHeader;

/* 完整消息 */
typedef struct CeProtoMsg {
    CeProtoHeader hdr;
    unsigned char payload[CE_PROTO_MAX_PAYLOAD];
} CeProtoMsg;

/* ---- 通道定义 ---- */

#define CE_CHANNEL_CTRL  0
#define CE_CHANNEL_DATA  1
#define CE_CHANNEL_HB    2
#define CE_CHANNEL_MAX   8

/* ---- 不透明句柄 ---- */

typedef struct CeStreamParserCtx CeStreamParserCtx;
typedef struct CeUringBpfCtx    CeUringBpfCtx;

/* ---- 统计信息 ---- */

typedef struct CeStreamParserStats {
    uint64_t msgs_parsed;
    uint64_t msgs_ctrl;
    uint64_t msgs_data;
    uint64_t msgs_hb;
    uint64_t msgs_err;
    uint64_t msgs_redirected;
    uint64_t msgs_dropped;
    uint64_t bytes_processed;
} CeStreamParserStats;

/* ---- Phase 4.3: Stream Parser API ---- */

/** 初始化 Stream Parser (加载 BPF 程序，创建 sockmap) */
CeStreamParserCtx* ce_stream_parser_init(void);

/** 关闭 Stream Parser */
void ce_stream_parser_shutdown(CeStreamParserCtx* ctx);

/** 将 socket fd 添加到 sockmap
 *  @param fd          socket 文件描述符
 *  @param channel     通道索引 (CE_CHANNEL_CTRL/DATA/HB) */
CeResult ce_stream_parser_add_sock(CeStreamParserCtx* ctx, int fd, int channel);

/** 从 sockmap 移除 socket */
CeResult ce_stream_parser_del_sock(CeStreamParserCtx* ctx, int fd);

/** 将 BPF stream parser 附加到指定 socket
 *  @param fd  需要解析的 socket fd */
CeResult ce_stream_parser_attach(CeStreamParserCtx* ctx, int fd);

/** 获取统计信息 */
CeResult ce_stream_parser_get_stats(CeStreamParserCtx* ctx, CeStreamParserStats* stats);

/** 获取 sockmap fd (用于 io_uring 集成) */
int ce_stream_parser_sockmap_fd(CeStreamParserCtx* ctx);

/* ---- Phase 4.4: io_uring + BPF 联动 API ---- */

/** 初始化 io_uring + BPF 联动上下文
 *  加载 BPF 程序 (stream parser + TC + LSM)，创建 sockmap 和路由表 */
CeUringBpfCtx* ce_uring_bpf_init(void);

/** 关闭 */
void ce_uring_bpf_shutdown(CeUringBpfCtx* ctx);

/** 配置路由表: 将消息类型映射到 sockmap 通道索引
 *  @param msg_type  消息类型 (CE_MSG_TYPE_*)
 *  @param channel   目标通道 (CE_CHANNEL_*) */
CeResult ce_uring_bpf_set_route(CeUringBpfCtx* ctx, int msg_type, int channel);

/** 将 socket 加入 sockmap
 *  @param fd       socket fd
 *  @param channel  通道索引 */
CeResult ce_uring_bpf_add_sock(CeUringBpfCtx* ctx, int fd, int channel);

/** 将 socket 从 sockmap 移除 */
CeResult ce_uring_bpf_del_sock(CeUringBpfCtx* ctx, int fd);

/** 附加 BPF stream parser 到 socket */
CeResult ce_uring_bpf_attach_parser(CeUringBpfCtx* ctx, int fd);

/** 附加 TC ingress 分类器到网络接口
 *  @param ifname  网络接口名 (如 "eth0", "lo") */
CeResult ce_uring_bpf_attach_tc(CeUringBpfCtx* ctx, const char* ifname);

/** 获取 sockmap fd (用于 io_uring IORING_OP_SENDMSG 零拷贝) */
int ce_uring_bpf_sockmap_fd(CeUringBpfCtx* ctx);

/** 获取通道统计 */
CeResult ce_uring_bpf_get_stats(CeUringBpfCtx* ctx, CeStreamParserStats* stats);

/* ---- 协议编解码工具 (用户态) ---- */

/** 编码协议头 (主机序 → 网络序/大端) */
void ce_proto_encode_header(CeProtoHeader* hdr, uint16_t msg_type, uint32_t msg_len);

/** 解码协议头 (网络序/大端 → 主机序) */
void ce_proto_decode_header(const unsigned char* data, uint16_t* msg_type, uint32_t* msg_len);

/** 构建完整消息到缓冲区
 *  @return 消息总长度 (含头) */
int ce_proto_build_msg(unsigned char* buf, int buf_size,
                       uint16_t msg_type, const void* payload, uint32_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* CE_STREAM_PARSER_H */
