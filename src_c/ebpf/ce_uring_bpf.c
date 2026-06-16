/*
 * ChaosEngine Phase 4.4: io_uring + BPF 联动 — 用户态加载器
 *
 * 加载 ce_uring_bpf_kern.o，管理 sockmap 和路由表。
 * 与 io_uring 集成: BPF 解析协议 → io_uring 直接投递到正确缓冲区。
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
#include <net/if.h>
#include <linux/bpf.h>

/* ---- 内部结构 ---- */

#define CE_UB_MAX_PROGS  16
#define CE_UB_MAX_LINKS  16

struct CeUringBpfCtx {
    struct bpf_object*  obj;
    struct bpf_program* progs[CE_UB_MAX_PROGS];
    int                 prog_count;
    struct bpf_link*    links[CE_UB_MAX_LINKS];
    int                 link_count;
    int                 sockmap_fd;
    int                 route_table_fd;
    int                 buf_index_fd;
    CeBool              loaded;
};

/* ---- 生命周期 ---- */

CeUringBpfCtx* ce_uring_bpf_init(void) {
    CeUringBpfCtx* ctx = (CeUringBpfCtx*)calloc(1, sizeof(CeUringBpfCtx));
    if (!ctx) return NULL;

    const char* obj_paths[] = {
        "src_c/ebpf/ce_uring_bpf_kern.o",
        "ce_uring_bpf_kern.o",
        NULL
    };

    for (int i = 0; obj_paths[i]; i++) {
        ctx->obj = bpf_object__open(obj_paths[i]);
        if (ctx->obj) {
            CE_LOG_INFO("URING_BPF", "BPF object opened: %s", obj_paths[i]);
            break;
        }
    }

    if (!ctx->obj) {
        CE_LOG_WARN("URING_BPF", "Failed to open BPF object: %s", strerror(errno));
        free(ctx);
        return NULL;
    }

    int ret = bpf_object__load(ctx->obj);
    if (ret < 0) {
        CE_LOG_ERROR("URING_BPF", "Failed to load BPF object: %d (%s)", ret, strerror(-ret));
        bpf_object__close(ctx->obj);
        free(ctx);
        return NULL;
    }
    ctx->loaded = CE_TRUE;
    CE_LOG_INFO("URING_BPF", "BPF programs loaded into kernel");

    /* 获取 sockmap fd */
    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "ce_uring_sock_map");
    if (map) {
        ctx->sockmap_fd = bpf_map__fd(map);
        CE_LOG_INFO("URING_BPF", "sockmap fd: %d", ctx->sockmap_fd);
    } else {
        ctx->sockmap_fd = -1;
    }

    /* 获取路由表 fd */
    map = bpf_object__find_map_by_name(ctx->obj, "ce_route_table");
    if (map) {
        ctx->route_table_fd = bpf_map__fd(map);
    } else {
        ctx->route_table_fd = -1;
    }

    /* 获取缓冲区索引表 fd */
    map = bpf_object__find_map_by_name(ctx->obj, "ce_buf_index_map");
    if (map) {
        ctx->buf_index_fd = bpf_map__fd(map);
    } else {
        ctx->buf_index_fd = -1;
    }

    return ctx;
}

void ce_uring_bpf_shutdown(CeUringBpfCtx* ctx) {
    if (!ctx) return;

    for (int i = 0; i < ctx->link_count; i++) {
        if (ctx->links[i]) bpf_link__destroy(ctx->links[i]);
    }

    if (ctx->obj) bpf_object__close(ctx->obj);
    free(ctx);
    CE_LOG_INFO("URING_BPF", "Shut down");
}

/* ---- 路由表配置 ---- */

CeResult ce_uring_bpf_set_route(CeUringBpfCtx* ctx, int msg_type, int channel) {
    if (!ctx || ctx->route_table_fd < 0) return CE_ERR;
    if (msg_type < 0 || msg_type >= CE_MSG_TYPE_MAX) return CE_ERR;
    if (channel < 0 || channel >= CE_CHANNEL_MAX) return CE_ERR;

    __u32 key = (__u32)msg_type;
    __u32 val = (__u32)channel;

    int ret = bpf_map_update_elem(ctx->route_table_fd, &key, &val, BPF_ANY);
    if (ret < 0) {
        CE_LOG_WARN("URING_BPF", "Failed to set route: msg_type=%d -> channel=%d: %s",
                    msg_type, channel, strerror(errno));
        return CE_ERR;
    }

    CE_LOG_INFO("URING_BPF", "Route: msg_type=%d -> channel=%d", msg_type, channel);
    return CE_OK;
}

/* ---- Sockmap 管理 ---- */

CeResult ce_uring_bpf_add_sock(CeUringBpfCtx* ctx, int fd, int channel) {
    if (!ctx || ctx->sockmap_fd < 0 || fd < 0) return CE_ERR;

    __u32 key = (__u32)channel;
    __u64 val = (__u64)fd;

    int ret = bpf_map_update_elem(ctx->sockmap_fd, &key, &val, BPF_ANY);
    if (ret < 0) {
        CE_LOG_WARN("URING_BPF", "Failed to add sock fd=%d to channel=%d: %s",
                    fd, channel, strerror(errno));
        return CE_ERR;
    }

    CE_LOG_INFO("URING_BPF", "Socket fd=%d added to channel=%d", fd, channel);
    return CE_OK;
}

CeResult ce_uring_bpf_del_sock(CeUringBpfCtx* ctx, int fd) {
    if (!ctx || ctx->sockmap_fd < 0 || fd < 0) return CE_ERR;

    for (__u32 key = 0; key < CE_CHANNEL_MAX; key++) {
        __u64 val = 0;
        if (bpf_map_lookup_elem(ctx->sockmap_fd, &key, &val) == 0 && (int)val == fd) {
            bpf_map_delete_elem(ctx->sockmap_fd, &key);
            CE_LOG_INFO("URING_BPF", "Socket fd=%d removed from channel=%u", fd, key);
            return CE_OK;
        }
    }

    return CE_ERR;
}

/* ---- Stream Parser 附加 ---- */

CeResult ce_uring_bpf_attach_parser(CeUringBpfCtx* ctx, int fd) {
    if (!ctx || !ctx->obj || ctx->sockmap_fd < 0) return CE_ERR;

    /* 附加 stream_parser */
    struct bpf_program* parser_prog = bpf_object__find_program_by_name(
        ctx->obj, "ce_uring_stream_parser");
    if (!parser_prog) {
        CE_LOG_WARN("URING_BPF", "Program 'ce_uring_stream_parser' not found");
        return CE_ERR;
    }

    int prog_fd = bpf_program__fd(parser_prog);
    if (prog_fd < 0) return CE_ERR;

    int ret = bpf_prog_attach(prog_fd, ctx->sockmap_fd,
                              BPF_SK_SKB_STREAM_PARSER, 0);
    if (ret < 0) {
        CE_LOG_WARN("URING_BPF", "Failed to attach stream_parser: %s", strerror(errno));
        return CE_ERR;
    }
    CE_LOG_INFO("URING_BPF", "Stream parser attached");

    /* 附加 stream_verdict */
    struct bpf_program* verdict_prog = bpf_object__find_program_by_name(
        ctx->obj, "ce_uring_stream_verdict");
    if (verdict_prog) {
        int verdict_fd = bpf_program__fd(verdict_prog);
        if (verdict_fd >= 0) {
            ret = bpf_prog_attach(verdict_fd, ctx->sockmap_fd,
                                  BPF_SK_SKB_STREAM_VERDICT, 0);
            if (ret < 0) {
                CE_LOG_WARN("URING_BPF", "Failed to attach stream_verdict: %s", strerror(errno));
            } else {
                CE_LOG_INFO("URING_BPF", "Stream verdict attached");
            }
        }
    }

    return CE_OK;
}

/* ---- TC 附加 ---- */

CeResult ce_uring_bpf_attach_tc(CeUringBpfCtx* ctx, const char* ifname) {
    if (!ctx || !ctx->obj || !ifname) return CE_ERR;

    struct bpf_program* tc_prog = bpf_object__find_program_by_name(
        ctx->obj, "ce_tc_ingress_classify");
    if (!tc_prog) {
        CE_LOG_WARN("URING_BPF", "TC program 'ce_tc_ingress_classify' not found");
        return CE_ERR;
    }

    /* 使用 libbpf 的 TCX hook 附加 (libbpf >= 1.0)
     * bpf_program__attach_tcx 需要 ifindex 而不是 ifname */
    int ifindex = (int)if_nametoindex(ifname);
    if (ifindex <= 0) {
        CE_LOG_WARN("URING_BPF", "Invalid interface: %s", ifname);
        return CE_ERR;
    }

    DECLARE_LIBBPF_OPTS(bpf_tcx_opts, tcx_opts,
        .flags = BPF_TCX_INGRESS);
    struct bpf_link* link = bpf_program__attach_tcx(
        tc_prog, ifindex, &tcx_opts);
    if (!link) {
        CE_LOG_WARN("URING_BPF", "TCX attach to %s failed: %s", ifname, strerror(errno));
        return CE_ERR;
    }

    if (ctx->link_count < CE_UB_MAX_LINKS) {
        ctx->links[ctx->link_count++] = link;
    }

    CE_LOG_INFO("URING_BPF", "TC ingress classifier attached to %s", ifname);
    return CE_OK;
}

/* ---- 查询 ---- */

int ce_uring_bpf_sockmap_fd(CeUringBpfCtx* ctx) {
    return ctx ? ctx->sockmap_fd : -1;
}

CeResult ce_uring_bpf_get_stats(CeUringBpfCtx* ctx, CeStreamParserStats* stats) {
    if (!ctx || !ctx->obj || !stats) return CE_ERR;
    memset(stats, 0, sizeof(*stats));

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "ce_channel_stats");
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

#else /* !CHAOS_HAS_EBPF — stubs */

#include "ebpf/ce_stream_parser.h"
#include <string.h>

CeUringBpfCtx* ce_uring_bpf_init(void) { return NULL; }
void ce_uring_bpf_shutdown(CeUringBpfCtx* ctx) { (void)ctx; }
CeResult ce_uring_bpf_set_route(CeUringBpfCtx* ctx, int msg_type, int channel) { (void)ctx; (void)msg_type; (void)channel; return CE_ERR; }
CeResult ce_uring_bpf_add_sock(CeUringBpfCtx* ctx, int fd, int channel) { (void)ctx; (void)fd; (void)channel; return CE_ERR; }
CeResult ce_uring_bpf_del_sock(CeUringBpfCtx* ctx, int fd) { (void)ctx; (void)fd; return CE_ERR; }
CeResult ce_uring_bpf_attach_parser(CeUringBpfCtx* ctx, int fd) { (void)ctx; (void)fd; return CE_ERR; }
CeResult ce_uring_bpf_attach_tc(CeUringBpfCtx* ctx, const char* ifname) { (void)ctx; (void)ifname; return CE_ERR; }
int ce_uring_bpf_sockmap_fd(CeUringBpfCtx* ctx) { (void)ctx; return -1; }
CeResult ce_uring_bpf_get_stats(CeUringBpfCtx* ctx, CeStreamParserStats* stats) { (void)ctx; (void)stats; return CE_ERR; }

#endif /* CHAOS_HAS_EBPF */
