/*
 * ChaosEngine XDP 可编程包处理 — 用户态加载器
 *
 * 使用 libbpf 加载 XDP BPF 程序到网卡。
 * 编译条件: CHAOS_HAS_EBPF
 */

#ifdef CHAOS_HAS_EBPF

#include "ebpf/ce_xdp.h"
#include "public_api/ce_log.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <linux/if_link.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>

/* ---- 内部结构 ---- */

struct CeXdpContext {
    struct bpf_object*  obj;
    struct bpf_link*    link;
    int                 ifindex;
    char                iface_name[32];
    CeBool              attached;
};

/* ---- 生命周期 ---- */

CeXdpContext* ce_xdp_init(const char* iface_name) {
    if (!iface_name) return NULL;

    CeXdpContext* ctx = (CeXdpContext*)calloc(1, sizeof(CeXdpContext));
    if (!ctx) return NULL;

    strncpy(ctx->iface_name, iface_name, sizeof(ctx->iface_name) - 1);
    ctx->ifindex = if_nametoindex(iface_name);
    if (ctx->ifindex == 0) {
        CE_LOG_ERROR("XDP", "Invalid interface: %s", iface_name);
        free(ctx);
        return NULL;
    }

    /* 打开 BPF XDP 对象文件 */
    const char* obj_path = "src_c/ebpf/ce_xdp_kern.o";
    ctx->obj = bpf_object__open(obj_path);
    if (!ctx->obj) {
        obj_path = "ce_xdp_kern.o";
        ctx->obj = bpf_object__open(obj_path);
    }
    if (!ctx->obj) {
        CE_LOG_WARN("XDP", "Failed to open BPF XDP object: %s", strerror(errno));
        free(ctx);
        return NULL;
    }

    /* 加载 BPF 程序到内核 */
    int ret = bpf_object__load(ctx->obj);
    if (ret < 0) {
        CE_LOG_ERROR("XDP", "Failed to load BPF XDP object: %d (%s)", ret, strerror(-ret));
        bpf_object__close(ctx->obj);
        free(ctx);
        return NULL;
    }

    /* 查找 XDP 程序 */
    struct bpf_program* prog = bpf_object__find_program_by_name(ctx->obj, "xdp_prog");
    if (!prog) {
        CE_LOG_ERROR("XDP", "XDP program 'xdp_prog' not found in object");
        bpf_object__close(ctx->obj);
        free(ctx);
        return NULL;
    }

    /* Attach XDP 程序到网卡（使用 generic/skb 模式，兼容性更好） */
    ctx->link = bpf_program__attach_xdp(prog, ctx->ifindex);
    if (!ctx->link) {
        /* 尝试 XDP generic 模式 */
        CE_LOG_WARN("XDP", "Native XDP attach failed, trying generic mode: %s", strerror(errno));

        /* 使用 libbpf 的 bpf_link 方式 */
        int prog_fd = bpf_program__fd(prog);
        if (prog_fd < 0) {
            CE_LOG_ERROR("XDP", "Failed to get XDP program fd");
            bpf_object__close(ctx->obj);
            free(ctx);
            return NULL;
        }

        /* 尝试 XDP generic attach via netlink */
        DECLARE_LIBBPF_OPTS(bpf_xdp_attach_opts, opts,
            .old_prog_fd = -1);
        ret = bpf_xdp_attach(ctx->ifindex, prog_fd, XDP_FLAGS_SKB_MODE, &opts);
        if (ret < 0) {
            CE_LOG_ERROR("XDP", "XDP generic attach failed: %s", strerror(-ret));
            bpf_object__close(ctx->obj);
            free(ctx);
            return NULL;
        }
        ctx->attached = CE_TRUE;
        CE_LOG_INFO("XDP", "XDP program attached to %s (generic mode)", iface_name);
    } else {
        ctx->attached = CE_TRUE;
        CE_LOG_INFO("XDP", "XDP program attached to %s (native mode)", iface_name);
    }

    return ctx;
}

void ce_xdp_shutdown(CeXdpContext* ctx) {
    if (!ctx) return;

    if (ctx->link) {
        bpf_link__destroy(ctx->link);
    } else if (ctx->attached && ctx->obj) {
        /* Detach via netlink if attached without link */
        struct bpf_program* prog = bpf_object__find_program_by_name(ctx->obj, "xdp_prog");
        if (prog) {
            int prog_fd = bpf_program__fd(prog);
            if (prog_fd >= 0) {
                DECLARE_LIBBPF_OPTS(bpf_xdp_attach_opts, opts,
                    .old_prog_fd = prog_fd);
                bpf_xdp_detach(ctx->ifindex, XDP_FLAGS_SKB_MODE, &opts);
            }
        }
    }

    if (ctx->obj) bpf_object__close(ctx->obj);
    free(ctx);
    CE_LOG_INFO("XDP", "XDP shut down on %s", ctx->iface_name);
}

/* ---- 运行模式 ---- */

CeResult ce_xdp_set_mode(CeXdpContext* ctx, CeXdpMode mode) {
    if (!ctx || !ctx->obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "xdp_mode");
    if (!map) {
        CE_LOG_WARN("XDP", "Map 'xdp_mode' not found");
        return CE_ERR;
    }

    __u32 key = 0;
    __u32 value = (__u32)mode;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key, &value, BPF_ANY);
    if (ret < 0) {
        CE_LOG_WARN("XDP", "Failed to set XDP mode: %s", strerror(-ret));
        return CE_ERR;
    }

    const char* mode_names[] = {"PASS", "DROP", "TX"};
    CE_LOG_INFO("XDP", "Mode set to %s", mode_names[mode]);
    return CE_OK;
}

CeXdpMode ce_xdp_get_mode(CeXdpContext* ctx) {
    if (!ctx || !ctx->obj) return CE_XDP_PASS;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "xdp_mode");
    if (!map) return CE_XDP_PASS;

    __u32 key = 0;
    __u32 value = 0;
    bpf_map_lookup_elem(bpf_map__fd(map), &key, &value);
    return (CeXdpMode)value;
}

/* ---- 端口过滤 ---- */

CeResult ce_xdp_set_port_filter(CeXdpContext* ctx, uint16_t port) {
    if (!ctx || !ctx->obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "xdp_port_filter");
    if (!map) {
        CE_LOG_WARN("XDP", "Map 'xdp_port_filter' not found");
        return CE_ERR;
    }

    __u32 key = 0;
    __u16 value = port;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key, &value, BPF_ANY);
    if (ret < 0) {
        CE_LOG_WARN("XDP", "Failed to set port filter: %s", strerror(-ret));
        return CE_ERR;
    }

    CE_LOG_INFO("XDP", "Port filter set to %u", port);
    return CE_OK;
}

/* ---- 统计查询 ---- */

static uint64_t xdp_get_stat(CeXdpContext* ctx, __u32 idx) {
    if (!ctx || !ctx->obj) return 0;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "xdp_stats");
    if (!map) return 0;

    /* PERCPU_ARRAY: 需要遍历所有 CPU 求和 */
    int fd = bpf_map__fd(map);
    int n_cpus = libbpf_num_possible_cpus();
    uint64_t total = 0;

    for (int cpu = 0; cpu < n_cpus; cpu++) {
        __u64 value = 0;
        /* PERCPU_ARRAY 的值偏移 = idx * n_cpus + cpu */
        /* 使用 bpf_map_lookup_percpu_elem 更简单 */
        int ret = bpf_map_lookup_elem(fd, &idx, &value);
        if (ret == 0) {
            total += value;
        }
    }

    return total;
}

uint64_t ce_xdp_get_rx_packets(CeXdpContext* ctx) {
    return xdp_get_stat(ctx, 0);
}

uint64_t ce_xdp_get_rx_bytes(CeXdpContext* ctx) {
    return xdp_get_stat(ctx, 1);
}

uint64_t ce_xdp_get_pass_packets(CeXdpContext* ctx) {
    return xdp_get_stat(ctx, 2);
}

uint64_t ce_xdp_get_drop_packets(CeXdpContext* ctx) {
    return xdp_get_stat(ctx, 3);
}

uint64_t ce_xdp_get_tx_packets(CeXdpContext* ctx) {
    return xdp_get_stat(ctx, 4);
}

/* ---- 查询 ---- */

CeBool ce_xdp_available(const char* iface_name) {
    if (!iface_name) return CE_FALSE;

    int ifindex = if_nametoindex(iface_name);
    if (ifindex == 0) return CE_FALSE;

    /* 检查 BTF 是否可用 */
    if (access("/sys/kernel/btf/vmlinux", F_OK) != 0) return CE_FALSE;

    return CE_TRUE;
}

#else /* !CHAOS_HAS_EBPF — stubs */

#include "ebpf/ce_xdp.h"

CeXdpContext* ce_xdp_init(const char* iface_name) { (void)iface_name; return NULL; }
void ce_xdp_shutdown(CeXdpContext* ctx) { (void)ctx; }
CeResult ce_xdp_set_mode(CeXdpContext* ctx, CeXdpMode mode) {
    (void)ctx; (void)mode; return CE_ERR;
}
CeXdpMode ce_xdp_get_mode(CeXdpContext* ctx) { (void)ctx; return CE_XDP_PASS; }
CeResult ce_xdp_set_port_filter(CeXdpContext* ctx, uint16_t port) {
    (void)ctx; (void)port; return CE_ERR;
}
uint64_t ce_xdp_get_rx_packets(CeXdpContext* ctx) { (void)ctx; return 0; }
uint64_t ce_xdp_get_rx_bytes(CeXdpContext* ctx) { (void)ctx; return 0; }
uint64_t ce_xdp_get_pass_packets(CeXdpContext* ctx) { (void)ctx; return 0; }
uint64_t ce_xdp_get_drop_packets(CeXdpContext* ctx) { (void)ctx; return 0; }
uint64_t ce_xdp_get_tx_packets(CeXdpContext* ctx) { (void)ctx; return 0; }
CeBool ce_xdp_available(const char* iface_name) { (void)iface_name; return CE_FALSE; }

#endif /* CHAOS_HAS_EBPF */
