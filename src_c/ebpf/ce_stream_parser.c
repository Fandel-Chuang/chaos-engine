/*
 * ChaosEngine Phase 4.3: BPF Stream Parser — 用户态加载器
 *
 * 使用 libbpf 加载 ce_stream_parser_kern.o 到内核，
 * 管理 sockmap，提供 socket 附加/分离接口。
 *
 * 编译条件: CHAOS_HAS_EBPF
 */

#ifdef CHAOS_HAS_EBPF

#include "ebpf/ce_stream_parser.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/bpf.h>

/* ---- 内部结构 ---- */

#define CE_SP_MAX_PROGS  8
#define CE_SP_MAX_LINKS  8

struct CeStreamParserCtx {
    struct bpf_object*  obj;
    struct bpf_program* progs[CE_SP_MAX_PROGS];
    int                 prog_count;
    struct bpf_link*    links[CE_SP_MAX_LINKS];
    int                 link_count;
    int                 sockmap_fd;
    CeBool              loaded;
};

/* ---- 生命周期 ---- */

CeStreamParserCtx* ce_stream_parser_init(void) {
    CeStreamParserCtx* ctx = (CeStreamParserCtx*)calloc(1, sizeof(CeStreamParserCtx));
    if (!ctx) return NULL;

    /* 尝试多个路径打开 BPF 对象 */
    const char* obj_paths[] = {
        "src_c/ebpf/ce_stream_parser_kern.o",
        "ce_stream_parser_kern.o",
        NULL
    };

    for (int i = 0; obj_paths[i]; i++) {
        ctx->obj = bpf_object__open(obj_paths[i]);
        if (ctx->obj) {
            CE_LOG_INFO("STREAM_PARSER", "BPF object opened: %s", obj_paths[i]);
            break;
        }
    }

    if (!ctx->obj) {
        CE_LOG_WARN("STREAM_PARSER", "Failed to open BPF object: %s", strerror(errno));
        free(ctx);
        return NULL;
    }

    /* 加载 BPF 程序到内核 */
    int ret = bpf_object__load(ctx->obj);
    if (ret < 0) {
        CE_LOG_ERROR("STREAM_PARSER", "Failed to load BPF object: %d (%s)", ret, strerror(-ret));
        bpf_object__close(ctx->obj);
        free(ctx);
        return NULL;
    }
    ctx->loaded = CE_TRUE;
    CE_LOG_INFO("STREAM_PARSER", "BPF programs loaded into kernel");

    /* 获取 sockmap fd */
    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "ce_sock_map");
    if (map) {
        ctx->sockmap_fd = bpf_map__fd(map);
        CE_LOG_INFO("STREAM_PARSER", "sockmap fd: %d", ctx->sockmap_fd);
    } else {
        CE_LOG_WARN("STREAM_PARSER", "sockmap 'ce_sock_map' not found");
        ctx->sockmap_fd = -1;
    }

    return ctx;
}

void ce_stream_parser_shutdown(CeStreamParserCtx* ctx) {
    if (!ctx) return;

    /* 销毁所有 link */
    for (int i = 0; i < ctx->link_count; i++) {
        if (ctx->links[i]) bpf_link__destroy(ctx->links[i]);
    }

    if (ctx->obj) bpf_object__close(ctx->obj);
    free(ctx);
    CE_LOG_INFO("STREAM_PARSER", "Shut down");
}

/* ---- Sockmap 管理 ---- */

CeResult ce_stream_parser_add_sock(CeStreamParserCtx* ctx, int fd, int channel) {
    if (!ctx || ctx->sockmap_fd < 0 || fd < 0) return CE_ERR;

    /* sockmap 使用 socket fd 作为 key，value 为 socket fd */
    __u32 key = (__u32)channel;
    __u64 val = (__u64)fd;

    int ret = bpf_map_update_elem(ctx->sockmap_fd, &key, &val, BPF_ANY);
    if (ret < 0) {
        CE_LOG_WARN("STREAM_PARSER", "Failed to add sock fd=%d to channel=%d: %s",
                    fd, channel, strerror(errno));
        return CE_ERR;
    }

    CE_LOG_INFO("STREAM_PARSER", "Socket fd=%d added to channel=%d", fd, channel);
    return CE_OK;
}

CeResult ce_stream_parser_del_sock(CeStreamParserCtx* ctx, int fd) {
    if (!ctx || ctx->sockmap_fd < 0 || fd < 0) return CE_ERR;

    /* 遍历所有通道删除 */
    for (__u32 key = 0; key < CE_CHANNEL_MAX; key++) {
        __u64 val = 0;
        if (bpf_map_lookup_elem(ctx->sockmap_fd, &key, &val) == 0 && (int)val == fd) {
            bpf_map_delete_elem(ctx->sockmap_fd, &key);
            CE_LOG_INFO("STREAM_PARSER", "Socket fd=%d removed from channel=%u", fd, key);
            return CE_OK;
        }
    }

    return CE_ERR;
}

/* ---- Stream Parser 附加 ---- */

CeResult ce_stream_parser_attach(CeStreamParserCtx* ctx, int fd) {
    if (!ctx || !ctx->obj || fd < 0) return CE_ERR;

    /* 使用 BPF_PROG_ATTACH 将 stream_parser 程序附加到 sockmap */
    struct bpf_program* parser_prog = bpf_object__find_program_by_name(
        ctx->obj, "ce_stream_parser");
    if (!parser_prog) {
        CE_LOG_WARN("STREAM_PARSER", "Program 'ce_stream_parser' not found");
        return CE_ERR;
    }

    int prog_fd = bpf_program__fd(parser_prog);
    if (prog_fd < 0) {
        CE_LOG_WARN("STREAM_PARSER", "Failed to get program fd");
        return CE_ERR;
    }

    /* 附加 stream parser 到 sockmap */
    /* BPF_SK_SKB_STREAM_PARSER 附加到 sockmap */
    __u32 attach_type = BPF_SK_SKB_STREAM_PARSER;
    int ret = bpf_prog_attach(prog_fd, ctx->sockmap_fd, attach_type, 0);
    if (ret < 0) {
        CE_LOG_WARN("STREAM_PARSER", "Failed to attach stream_parser: %s", strerror(errno));
        return CE_ERR;
    }

    CE_LOG_INFO("STREAM_PARSER", "Stream parser attached to sockmap");

    /* 附加 stream verdict */
    struct bpf_program* verdict_prog = bpf_object__find_program_by_name(
        ctx->obj, "ce_stream_verdict");
    if (verdict_prog) {
        int verdict_fd = bpf_program__fd(verdict_prog);
        if (verdict_fd >= 0) {
            ret = bpf_prog_attach(verdict_fd, ctx->sockmap_fd,
                                  BPF_SK_SKB_STREAM_VERDICT, 0);
            if (ret < 0) {
                CE_LOG_WARN("STREAM_PARSER", "Failed to attach stream_verdict: %s", strerror(errno));
            } else {
                CE_LOG_INFO("STREAM_PARSER", "Stream verdict attached to sockmap");
            }
        }
    }

    return CE_OK;
}

/* ---- 统计 ---- */

CeResult ce_stream_parser_get_stats(CeStreamParserCtx* ctx, CeStreamParserStats* stats) {
    if (!ctx || !ctx->obj || !stats) return CE_ERR;
    memset(stats, 0, sizeof(*stats));

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "ce_msg_stats");
    if (!map) return CE_ERR;

    int fd = bpf_map__fd(map);
    if (fd < 0) return CE_ERR;

    __u64 val;
    __u32 key;

    key = 0; val = 0; bpf_map_lookup_elem(fd, &key, &val); stats->msgs_ctrl = val;
    key = 1; val = 0; bpf_map_lookup_elem(fd, &key, &val); stats->msgs_data = val;
    key = 2; val = 0; bpf_map_lookup_elem(fd, &key, &val); stats->msgs_hb   = val;
    key = 3; val = 0; bpf_map_lookup_elem(fd, &key, &val); stats->msgs_err  = val;

    stats->msgs_parsed = stats->msgs_ctrl + stats->msgs_data +
                         stats->msgs_hb + stats->msgs_err;

    return CE_OK;
}

int ce_stream_parser_sockmap_fd(CeStreamParserCtx* ctx) {
    return ctx ? ctx->sockmap_fd : -1;
}

/* ---- 协议编解码工具 ---- */

void ce_proto_encode_header(CeProtoHeader* hdr, uint16_t msg_type, uint32_t msg_len) {
    if (!hdr) return;
    /* 大端编码 */
    hdr->msg_type = ((msg_type & 0xFF) << 8) | ((msg_type >> 8) & 0xFF);
    hdr->msg_len  = ((msg_len & 0xFF) << 24) |
                    (((msg_len >> 8) & 0xFF) << 16) |
                    (((msg_len >> 16) & 0xFF) << 8) |
                    ((msg_len >> 24) & 0xFF);
}

void ce_proto_decode_header(const unsigned char* data, uint16_t* msg_type, uint32_t* msg_len) {
    if (!data) return;
    if (msg_type) {
        *msg_type = ((uint16_t)data[0] << 8) | (uint16_t)data[1];
    }
    if (msg_len) {
        *msg_len = ((uint32_t)data[2] << 24) |
                   ((uint32_t)data[3] << 16) |
                   ((uint32_t)data[4] << 8)  |
                   (uint32_t)data[5];
    }
}

int ce_proto_build_msg(unsigned char* buf, int buf_size,
                       uint16_t msg_type, const void* payload, uint32_t payload_len) {
    int total = CE_PROTO_HDR_SIZE + (int)payload_len;
    if (!buf || buf_size < total) return -1;

    /* 编码头 */
    buf[0] = (unsigned char)((msg_type >> 8) & 0xFF);
    buf[1] = (unsigned char)(msg_type & 0xFF);
    buf[2] = (unsigned char)((payload_len >> 24) & 0xFF);
    buf[3] = (unsigned char)((payload_len >> 16) & 0xFF);
    buf[4] = (unsigned char)((payload_len >> 8) & 0xFF);
    buf[5] = (unsigned char)(payload_len & 0xFF);

    /* 复制 payload */
    if (payload && payload_len > 0) {
        memcpy(buf + CE_PROTO_HDR_SIZE, payload, payload_len);
    }

    return total;
}

#else /* !CHAOS_HAS_EBPF — stubs */

#include "ebpf/ce_stream_parser.h"
#include <string.h>

CeStreamParserCtx* ce_stream_parser_init(void) { return NULL; }
void ce_stream_parser_shutdown(CeStreamParserCtx* ctx) { (void)ctx; }
CeResult ce_stream_parser_add_sock(CeStreamParserCtx* ctx, int fd, int channel) { (void)ctx; (void)fd; (void)channel; return CE_ERR; }
CeResult ce_stream_parser_del_sock(CeStreamParserCtx* ctx, int fd) { (void)ctx; (void)fd; return CE_ERR; }
CeResult ce_stream_parser_attach(CeStreamParserCtx* ctx, int fd) { (void)ctx; (void)fd; return CE_ERR; }
CeResult ce_stream_parser_get_stats(CeStreamParserCtx* ctx, CeStreamParserStats* stats) { (void)ctx; (void)stats; return CE_ERR; }
int ce_stream_parser_sockmap_fd(CeStreamParserCtx* ctx) { (void)ctx; return -1; }

void ce_proto_encode_header(CeProtoHeader* hdr, uint16_t msg_type, uint32_t msg_len) {
    if (!hdr) return;
    hdr->msg_type = ((msg_type & 0xFF) << 8) | ((msg_type >> 8) & 0xFF);
    hdr->msg_len  = ((msg_len & 0xFF) << 24) |
                    (((msg_len >> 8) & 0xFF) << 16) |
                    (((msg_len >> 16) & 0xFF) << 8) |
                    ((msg_len >> 24) & 0xFF);
}

void ce_proto_decode_header(const unsigned char* data, uint16_t* msg_type, uint32_t* msg_len) {
    if (!data) return;
    if (msg_type) *msg_type = ((uint16_t)data[0] << 8) | (uint16_t)data[1];
    if (msg_len)  *msg_len  = ((uint32_t)data[2] << 24) | ((uint32_t)data[3] << 16) |
                              ((uint32_t)data[4] << 8)  | (uint32_t)data[5];
}

int ce_proto_build_msg(unsigned char* buf, int buf_size,
                       uint16_t msg_type, const void* payload, uint32_t payload_len) {
    int total = CE_PROTO_HDR_SIZE + (int)payload_len;
    if (!buf || buf_size < total) return -1;
    buf[0] = (unsigned char)((msg_type >> 8) & 0xFF);
    buf[1] = (unsigned char)(msg_type & 0xFF);
    buf[2] = (unsigned char)((payload_len >> 24) & 0xFF);
    buf[3] = (unsigned char)((payload_len >> 16) & 0xFF);
    buf[4] = (unsigned char)((payload_len >> 8) & 0xFF);
    buf[5] = (unsigned char)(payload_len & 0xFF);
    if (payload && payload_len > 0) memcpy(buf + CE_PROTO_HDR_SIZE, payload, payload_len);
    return total;
}

#endif /* CHAOS_HAS_EBPF */
