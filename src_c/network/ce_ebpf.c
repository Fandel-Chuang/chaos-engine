/*
 * ChaosEngine eBPF 可观测性 — 用户态加载器
 *
 * 使用 libbpf 加载 BPF 程序到内核，读取 BPF map 数据。
 * 编译条件: CHAOS_HAS_EBPF
 */

#ifdef CHAOS_HAS_EBPF

#include "network/ce_ebpf.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>       /* IF_NAMESIZE, if_nametoindex */

/* ---- 内部结构 ---- */

#define CE_EBPF_MAX_PROGS 16

struct CeEbpfContext {
    struct bpf_object*  obj;          /* 可观测性 BPF 对象 */
    struct bpf_program* progs[CE_EBPF_MAX_PROGS];
    int                 prog_count;
    struct bpf_link*    links[CE_EBPF_MAX_PROGS];
    int                 link_count;
    CeBool              loaded;

    /* XDP 包过滤 (Phase 4.1) */
    struct bpf_object*  xdp_filter_obj;
    struct bpf_link*    xdp_filter_link;
    CeBool              xdp_filter_attached;

    /* XDP 连接分发 (Phase 4.2) */
    struct bpf_object*  xdp_dispatch_obj;
    struct bpf_link*    xdp_dispatch_link;
    CeBool              xdp_dispatch_attached;
};

/* ---- 生命周期 ---- */

CeEbpfContext* ce_ebpf_init(void) {
    CeEbpfContext* ctx = (CeEbpfContext*)calloc(1, sizeof(CeEbpfContext));
    if (!ctx) return NULL;

    /* 打开 BPF 对象文件 */
    const char* obj_path = "src_c/network/ce_ebpf_kern.o";
    ctx->obj = bpf_object__open(obj_path);
    if (!ctx->obj) {
        /* 尝试相对路径 */
        obj_path = "ce_ebpf_kern.o";
        ctx->obj = bpf_object__open(obj_path);
    }
    if (!ctx->obj) {
        CE_LOG_WARN("EBPF", "Failed to open BPF object: %s", strerror(errno));
        free(ctx);
        return NULL;
    }

    CE_LOG_INFO("EBPF", "BPF object opened: %s", obj_path);
    return ctx;
}

void ce_ebpf_shutdown(CeEbpfContext* ctx) {
    if (!ctx) return;

    /* 销毁所有 link */
    for (int i = 0; i < ctx->link_count; i++) {
        if (ctx->links[i]) bpf_link__destroy(ctx->links[i]);
    }

    /* 释放 BPF 对象 */
    if (ctx->obj) bpf_object__close(ctx->obj);

    /* 清理 XDP filter */
    if (ctx->xdp_filter_link) bpf_link__destroy(ctx->xdp_filter_link);
    if (ctx->xdp_filter_obj)  bpf_object__close(ctx->xdp_filter_obj);

    /* 清理 XDP dispatch */
    if (ctx->xdp_dispatch_link) bpf_link__destroy(ctx->xdp_dispatch_link);
    if (ctx->xdp_dispatch_obj)  bpf_object__close(ctx->xdp_dispatch_obj);

    free(ctx);
    CE_LOG_INFO("EBPF", "Shut down");
}

/* ---- 函数追踪 ---- */

CeResult ce_ebpf_trace_function(CeEbpfContext* ctx, const char* func_name) {
    if (!ctx || !ctx->obj) return CE_ERR;

    /* 加载所有 BPF 程序到内核 */
    if (!ctx->loaded) {
        int ret = bpf_object__load(ctx->obj);
        if (ret < 0) {
            CE_LOG_ERROR("EBPF", "Failed to load BPF object: %d", ret);
            return CE_ERR;
        }
        ctx->loaded = CE_TRUE;
        CE_LOG_INFO("EBPF", "BPF programs loaded into kernel");
    }

    /* 查找并 attach kprobe/kretprobe */
    struct bpf_program* prog;
    bpf_object__for_each_program(prog, ctx->obj) {
        const char* name = bpf_program__name(prog);
        if (!name) continue;

        /* 只 attach 匹配函数名的探针 */
        if (strstr(name, func_name)) {
            struct bpf_link* link = bpf_program__attach(prog);
            if (!link) {
                CE_LOG_WARN("EBPF", "Failed to attach %s: %s", name, strerror(errno));
                continue;
            }
            ctx->links[ctx->link_count++] = link;
            CE_LOG_INFO("EBPF", "Attached: %s", name);
        }
    }

    return CE_OK;
}

int ce_ebpf_dump_latency(CeEbpfContext* ctx, const char* func_name) {
    if (!ctx || !ctx->obj) return 0;
    (void)func_name;

    /* 读取 func_latency_hist map */
    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "func_latency_hist");
    if (!map) {
        CE_LOG_WARN("EBPF", "Map 'func_latency_hist' not found");
        return 0;
    }

    int fd = bpf_map__fd(map);
    if (fd < 0) return 0;

    int total = 0;
    __u64 key = 0, next_key;
    __u64 value;

    CE_LOG_INFO("EBPF", "=== Latency Histogram: %s ===", func_name);

    while (bpf_map_get_next_key(fd, &key, &next_key) == 0) {
        if (bpf_map_lookup_elem(fd, &next_key, &value) == 0) {
            CE_LOG_INFO("EBPF", "  <= %llu us: %llu samples", 
                        (unsigned long long)next_key, (unsigned long long)value);
            total += (int)value;
        }
        key = next_key;
    }

    CE_LOG_INFO("EBPF", "  Total samples: %d", total);
    return total;
}

/* ---- TCP 重传监控 ---- */

CeResult ce_ebpf_trace_tcp_retransmit(CeEbpfContext* ctx) {
    if (!ctx || !ctx->obj) return CE_ERR;

    if (!ctx->loaded) {
        int ret = bpf_object__load(ctx->obj);
        if (ret < 0) { CE_LOG_ERROR("EBPF", "Load failed: %d", ret); return CE_ERR; }
        ctx->loaded = CE_TRUE;
    }

    /* Attach tcp_retransmit_skb kprobe */
    struct bpf_program* prog = bpf_object__find_program_by_name(ctx->obj, "kprobe_tcp_retransmit");
    if (!prog) { CE_LOG_WARN("EBPF", "kprobe_tcp_retransmit not found"); return CE_ERR; }

    struct bpf_link* link = bpf_program__attach(prog);
    if (!link) { CE_LOG_WARN("EBPF", "Attach tcp_retransmit failed: %s", strerror(errno)); return CE_ERR; }

    ctx->links[ctx->link_count++] = link;
    CE_LOG_INFO("EBPF", "TCP retransmit monitor attached");
    return CE_OK;
}

int ce_ebpf_get_retransmit_count(CeEbpfContext* ctx) {
    if (!ctx || !ctx->obj) return 0;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "tcp_retransmit_count");
    if (!map) return 0;

    __u32 key = 0;
    __u64 value = 0;
    bpf_map_lookup_elem(bpf_map__fd(map), &key, &value);
    return (int)value;
}

/* ---- I/O 延迟 ---- */

CeResult ce_ebpf_trace_io_latency(CeEbpfContext* ctx) {
    if (!ctx || !ctx->obj) return CE_ERR;

    if (!ctx->loaded) {
        int ret = bpf_object__load(ctx->obj);
        if (ret < 0) { CE_LOG_ERROR("EBPF", "Load failed: %d", ret); return CE_ERR; }
        ctx->loaded = CE_TRUE;
    }

    /* Attach recvfrom tracepoints */
    struct bpf_program* prog;
    bpf_object__for_each_program(prog, ctx->obj) {
        const char* name = bpf_program__name(prog);
        if (name && strstr(name, "recvfrom")) {
            struct bpf_link* link = bpf_program__attach(prog);
            if (link) {
                ctx->links[ctx->link_count++] = link;
                CE_LOG_INFO("EBPF", "Attached: %s", name);
            }
        }
    }

    return CE_OK;
}

void ce_ebpf_get_io_latency_stats(CeEbpfContext* ctx, int* p50, int* p90, int* p99) {
    *p50 = *p90 = *p99 = 0;
    if (!ctx || !ctx->obj) return;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "io_latency_hist");
    if (!map) return;

    int fd = bpf_map__fd(map);
    if (fd < 0) return;

    /* 收集所有桶 */
    __u64 buckets[256];
    __u64 counts[256];
    int n = 0;
    __u64 key = 0, next_key;
    __u64 total = 0;

    while (n < 256 && bpf_map_get_next_key(fd, &key, &next_key) == 0) {
        __u64 val = 0;
        bpf_map_lookup_elem(fd, &next_key, &val);
        buckets[n] = next_key;
        counts[n] = val;
        total += val;
        key = next_key;
        n++;
    }

    if (total == 0) return;

    /* 简单排序 (冒泡，桶数少) */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (buckets[j] > buckets[j + 1]) {
                __u64 tb = buckets[j]; buckets[j] = buckets[j+1]; buckets[j+1] = tb;
                __u64 tc = counts[j];  counts[j]  = counts[j+1];  counts[j+1]  = tc;
            }
        }
    }

    /* 计算百分位 */
    __u64 cum = 0;
    for (int i = 0; i < n; i++) {
        cum += counts[i];
        if (*p50 == 0 && cum >= total * 50 / 100)  *p50 = (int)buckets[i];
        if (*p90 == 0 && cum >= total * 90 / 100)  *p90 = (int)buckets[i];
        if (*p99 == 0 && cum >= total * 99 / 100)  *p99 = (int)buckets[i];
    }
}

/* ---- XDP 包过滤 (Phase 4.1) ---- */

CeResult ce_ebpf_xdp_filter_attach(CeEbpfContext* ctx, const char* ifname) {
    if (!ctx || !ifname) return CE_ERR;

    /* 打开 XDP filter BPF 对象 */
    const char* obj_path = "src_c/ebpf/ce_xdp_filter.bpf.o";
    ctx->xdp_filter_obj = bpf_object__open(obj_path);
    if (!ctx->xdp_filter_obj) {
        obj_path = "ce_xdp_filter.bpf.o";
        ctx->xdp_filter_obj = bpf_object__open(obj_path);
    }
    if (!ctx->xdp_filter_obj) {
        CE_LOG_WARN("EBPF", "Failed to open XDP filter BPF object: %s", strerror(errno));
        return CE_ERR;
    }

    /* 加载到内核 */
    int ret = bpf_object__load(ctx->xdp_filter_obj);
    if (ret < 0) {
        CE_LOG_ERROR("EBPF", "Failed to load XDP filter: %d", ret);
        bpf_object__close(ctx->xdp_filter_obj);
        ctx->xdp_filter_obj = NULL;
        return CE_ERR;
    }

    /* 查找 xdp_filter 程序 */
    struct bpf_program* prog = bpf_object__find_program_by_name(ctx->xdp_filter_obj, "xdp_filter");
    if (!prog) {
        CE_LOG_ERROR("EBPF", "XDP filter program not found in object");
        bpf_object__close(ctx->xdp_filter_obj);
        ctx->xdp_filter_obj = NULL;
        return CE_ERR;
    }

    /* 获取接口索引 */
    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        CE_LOG_ERROR("EBPF", "Invalid interface: %s", ifname);
        bpf_object__close(ctx->xdp_filter_obj);
        ctx->xdp_filter_obj = NULL;
        return CE_ERR;
    }

    /* Attach XDP 程序 */
    ctx->xdp_filter_link = bpf_program__attach_xdp(prog, ifindex);
    if (!ctx->xdp_filter_link) {
        CE_LOG_ERROR("EBPF", "Failed to attach XDP filter to %s: %s", ifname, strerror(errno));
        bpf_object__close(ctx->xdp_filter_obj);
        ctx->xdp_filter_obj = NULL;
        return CE_ERR;
    }

    ctx->xdp_filter_attached = CE_TRUE;
    CE_LOG_INFO("EBPF", "XDP filter attached to %s (ifindex=%u)", ifname, ifindex);
    return CE_OK;
}

CeResult ce_ebpf_xdp_filter_blacklist_add(CeEbpfContext* ctx, uint32_t ip_addr) {
    if (!ctx || !ctx->xdp_filter_obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_filter_obj, "ip_blacklist");
    if (!map) return CE_ERR;

    __u8 val = 1;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &ip_addr, &val, BPF_ANY);
    if (ret < 0) {
        CE_LOG_WARN("EBPF", "Failed to add IP to blacklist: %d", ret);
        return CE_ERR;
    }
    return CE_OK;
}

CeResult ce_ebpf_xdp_filter_blacklist_del(CeEbpfContext* ctx, uint32_t ip_addr) {
    if (!ctx || !ctx->xdp_filter_obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_filter_obj, "ip_blacklist");
    if (!map) return CE_ERR;

    int ret = bpf_map_delete_elem(bpf_map__fd(map), &ip_addr);
    if (ret < 0) return CE_ERR;
    return CE_OK;
}

CeResult ce_ebpf_xdp_filter_set_syn_threshold(CeEbpfContext* ctx, uint32_t threshold) {
    if (!ctx || !ctx->xdp_filter_obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_filter_obj, "xdp_filter_config");
    if (!map) return CE_ERR;

    __u32 key = 0;
    __u64 val = threshold;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key, &val, BPF_ANY);
    if (ret < 0) return CE_ERR;
    return CE_OK;
}

CeResult ce_ebpf_xdp_filter_set_rate_limit(CeEbpfContext* ctx, uint32_t tokens_per_sec) {
    if (!ctx || !ctx->xdp_filter_obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_filter_obj, "xdp_filter_config");
    if (!map) return CE_ERR;

    /* 设置速率和桶大小 */
    __u32 key_rate = 2;
    __u32 key_burst = 3;
    __u64 val = tokens_per_sec;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key_rate, &val, BPF_ANY);
    if (ret < 0) return CE_ERR;
    ret = bpf_map_update_elem(bpf_map__fd(map), &key_burst, &val, BPF_ANY);
    if (ret < 0) return CE_ERR;
    return CE_OK;
}

CeResult ce_ebpf_xdp_filter_set_enabled(CeEbpfContext* ctx, CeBool enabled) {
    if (!ctx || !ctx->xdp_filter_obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_filter_obj, "xdp_filter_config");
    if (!map) return CE_ERR;

    __u32 key = 4;
    __u64 val = enabled ? 1 : 0;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key, &val, BPF_ANY);
    if (ret < 0) return CE_ERR;
    return CE_OK;
}

void ce_ebpf_xdp_filter_get_stats(CeEbpfContext* ctx,
                                   uint64_t* total, uint64_t* passed,
                                   uint64_t* dropped_blacklist,
                                   uint64_t* dropped_syn_flood,
                                   uint64_t* dropped_rate_limit) {
    if (total) *total = 0;
    if (passed) *passed = 0;
    if (dropped_blacklist) *dropped_blacklist = 0;
    if (dropped_syn_flood) *dropped_syn_flood = 0;
    if (dropped_rate_limit) *dropped_rate_limit = 0;

    if (!ctx || !ctx->xdp_filter_obj) return;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_filter_obj, "xdp_filter_stats");
    if (!map) return;

    int fd = bpf_map__fd(map);
    if (fd < 0) return;

    /* PERCPU_ARRAY: lookup returns nr_cpus * sizeof(value) bytes.
     * We allocate a buffer for up to 256 CPUs and sum across them. */
    int ncpus = libbpf_num_possible_cpus();
    if (ncpus <= 0) ncpus = 1;
    if (ncpus > 256) ncpus = 256;

    size_t buf_size = (size_t)ncpus * sizeof(__u64);
    __u64* buf = (__u64*)malloc(buf_size);
    if (!buf) return;

    __u32 key;
    __u64 agg_total = 0, agg_passed = 0, agg_bl = 0, agg_syn = 0, agg_rl = 0;

    key = 0;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_total += buf[i];
    key = 1;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_passed += buf[i];
    key = 2;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_bl += buf[i];
    key = 3;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_syn += buf[i];
    key = 4;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_rl += buf[i];

    free(buf);

    if (total) *total = agg_total;
    if (passed) *passed = agg_passed;
    if (dropped_blacklist) *dropped_blacklist = agg_bl;
    if (dropped_syn_flood) *dropped_syn_flood = agg_syn;
    if (dropped_rate_limit) *dropped_rate_limit = agg_rl;
}

/* ---- XDP 连接分发 (Phase 4.2) ---- */

CeResult ce_ebpf_xdp_dispatch_attach(CeEbpfContext* ctx, const char* ifname) {
    if (!ctx || !ifname) return CE_ERR;

    /* 打开 XDP dispatch BPF 对象 */
    const char* obj_path = "src_c/ebpf/ce_xdp_dispatch.bpf.o";
    ctx->xdp_dispatch_obj = bpf_object__open(obj_path);
    if (!ctx->xdp_dispatch_obj) {
        obj_path = "ce_xdp_dispatch.bpf.o";
        ctx->xdp_dispatch_obj = bpf_object__open(obj_path);
    }
    if (!ctx->xdp_dispatch_obj) {
        CE_LOG_WARN("EBPF", "Failed to open XDP dispatch BPF object: %s", strerror(errno));
        return CE_ERR;
    }

    /* 加载到内核 */
    int ret = bpf_object__load(ctx->xdp_dispatch_obj);
    if (ret < 0) {
        CE_LOG_ERROR("EBPF", "Failed to load XDP dispatch: %d", ret);
        bpf_object__close(ctx->xdp_dispatch_obj);
        ctx->xdp_dispatch_obj = NULL;
        return CE_ERR;
    }

    /* 查找 xdp_dispatch 程序 */
    struct bpf_program* prog = bpf_object__find_program_by_name(ctx->xdp_dispatch_obj, "xdp_dispatch");
    if (!prog) {
        CE_LOG_ERROR("EBPF", "XDP dispatch program not found in object");
        bpf_object__close(ctx->xdp_dispatch_obj);
        ctx->xdp_dispatch_obj = NULL;
        return CE_ERR;
    }

    /* 获取接口索引 */
    unsigned int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        CE_LOG_ERROR("EBPF", "Invalid interface: %s", ifname);
        bpf_object__close(ctx->xdp_dispatch_obj);
        ctx->xdp_dispatch_obj = NULL;
        return CE_ERR;
    }

    /* Attach XDP 程序 */
    ctx->xdp_dispatch_link = bpf_program__attach_xdp(prog, ifindex);
    if (!ctx->xdp_dispatch_link) {
        CE_LOG_ERROR("EBPF", "Failed to attach XDP dispatch to %s: %s", ifname, strerror(errno));
        bpf_object__close(ctx->xdp_dispatch_obj);
        ctx->xdp_dispatch_obj = NULL;
        return CE_ERR;
    }

    ctx->xdp_dispatch_attached = CE_TRUE;
    CE_LOG_INFO("EBPF", "XDP dispatch attached to %s (ifindex=%u)", ifname, ifindex);
    return CE_OK;
}

CeResult ce_ebpf_xdp_dispatch_set_num_cpus(CeEbpfContext* ctx, uint32_t num_cpus) {
    if (!ctx || !ctx->xdp_dispatch_obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_dispatch_obj, "xdp_dispatch_config");
    if (!map) return CE_ERR;

    __u32 key = 0;
    __u64 val = num_cpus;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key, &val, BPF_ANY);
    if (ret < 0) return CE_ERR;
    return CE_OK;
}

CeResult ce_ebpf_xdp_dispatch_set_mode(CeEbpfContext* ctx, uint32_t mode) {
    if (!ctx || !ctx->xdp_dispatch_obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_dispatch_obj, "xdp_dispatch_config");
    if (!map) return CE_ERR;

    __u32 key = 1;
    __u64 val = mode;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key, &val, BPF_ANY);
    if (ret < 0) return CE_ERR;
    return CE_OK;
}

CeResult ce_ebpf_xdp_dispatch_set_player_id_offset(CeEbpfContext* ctx, uint32_t offset) {
    if (!ctx || !ctx->xdp_dispatch_obj) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_dispatch_obj, "xdp_dispatch_config");
    if (!map) return CE_ERR;

    __u32 key = 2;
    __u64 val = offset;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key, &val, BPF_ANY);
    if (ret < 0) return CE_ERR;
    return CE_OK;
}

CeResult ce_ebpf_xdp_dispatch_add_xsk(CeEbpfContext* ctx, uint32_t queue_id, int xsk_fd) {
    if (!ctx || !ctx->xdp_dispatch_obj || xsk_fd < 0) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_dispatch_obj, "xsks_map");
    if (!map) return CE_ERR;

    int ret = bpf_map_update_elem(bpf_map__fd(map), &queue_id, &xsk_fd, BPF_ANY);
    if (ret < 0) {
        CE_LOG_WARN("EBPF", "Failed to add XSK fd %d to queue %u: %d", xsk_fd, queue_id, ret);
        return CE_ERR;
    }
    CE_LOG_INFO("EBPF", "Added XSK fd %d to queue %u", xsk_fd, queue_id);
    return CE_OK;
}

void ce_ebpf_xdp_dispatch_get_stats(CeEbpfContext* ctx,
                                     uint64_t* total, uint64_t* tcp,
                                     uint64_t* udp, uint64_t* redirected,
                                     uint64_t* passed, uint64_t* dropped) {
    if (total) *total = 0;
    if (tcp) *tcp = 0;
    if (udp) *udp = 0;
    if (redirected) *redirected = 0;
    if (passed) *passed = 0;
    if (dropped) *dropped = 0;

    if (!ctx || !ctx->xdp_dispatch_obj) return;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->xdp_dispatch_obj, "xdp_dispatch_stats");
    if (!map) return;

    int fd = bpf_map__fd(map);
    if (fd < 0) return;

    int ncpus = libbpf_num_possible_cpus();
    if (ncpus <= 0) ncpus = 1;
    if (ncpus > 256) ncpus = 256;

    size_t buf_size = (size_t)ncpus * sizeof(__u64);
    __u64* buf = (__u64*)malloc(buf_size);
    if (!buf) return;

    __u32 key;
    __u64 agg_total = 0, agg_tcp = 0, agg_udp = 0, agg_redir = 0, agg_pass = 0, agg_drop = 0;

    key = 0;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_total += buf[i];
    key = 1;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_tcp += buf[i];
    key = 2;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_udp += buf[i];
    key = 3;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_redir += buf[i];
    key = 4;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_pass += buf[i];
    key = 5;
    if (bpf_map_lookup_elem(fd, &key, buf) == 0)
        for (int i = 0; i < ncpus; i++) agg_drop += buf[i];

    free(buf);

    if (total) *total = agg_total;
    if (tcp) *tcp = agg_tcp;
    if (udp) *udp = agg_udp;
    if (redirected) *redirected = agg_redir;
    if (passed) *passed = agg_pass;
    if (dropped) *dropped = agg_drop;
}

/* ---- 查询 ---- */

CeBool ce_ebpf_available(void) {
    /* 检查 /sys/kernel/btf/vmlinux 是否存在 */
    return (access("/sys/kernel/btf/vmlinux", F_OK) == 0) ? CE_TRUE : CE_FALSE;
}

#else /* !CHAOS_HAS_EBPF — stubs */

#include "network/ce_ebpf.h"

CeEbpfContext* ce_ebpf_init(void) { return NULL; }
void ce_ebpf_shutdown(CeEbpfContext* ctx) { (void)ctx; }
CeResult ce_ebpf_trace_function(CeEbpfContext* ctx, const char* func_name) { (void)ctx; (void)func_name; return CE_ERR; }
int ce_ebpf_dump_latency(CeEbpfContext* ctx, const char* func_name) { (void)ctx; (void)func_name; return 0; }
CeResult ce_ebpf_trace_tcp_retransmit(CeEbpfContext* ctx) { (void)ctx; return CE_ERR; }
int ce_ebpf_get_retransmit_count(CeEbpfContext* ctx) { (void)ctx; return 0; }
CeResult ce_ebpf_trace_io_latency(CeEbpfContext* ctx) { (void)ctx; return CE_ERR; }
void ce_ebpf_get_io_latency_stats(CeEbpfContext* ctx, int* p50, int* p90, int* p99) { (void)ctx; *p50 = *p90 = *p99 = 0; }

/* XDP stubs */
CeResult ce_ebpf_xdp_filter_attach(CeEbpfContext* ctx, const char* ifname) { (void)ctx; (void)ifname; return CE_ERR; }
CeResult ce_ebpf_xdp_filter_blacklist_add(CeEbpfContext* ctx, uint32_t ip_addr) { (void)ctx; (void)ip_addr; return CE_ERR; }
CeResult ce_ebpf_xdp_filter_blacklist_del(CeEbpfContext* ctx, uint32_t ip_addr) { (void)ctx; (void)ip_addr; return CE_ERR; }
CeResult ce_ebpf_xdp_filter_set_syn_threshold(CeEbpfContext* ctx, uint32_t threshold) { (void)ctx; (void)threshold; return CE_ERR; }
CeResult ce_ebpf_xdp_filter_set_rate_limit(CeEbpfContext* ctx, uint32_t tokens_per_sec) { (void)ctx; (void)tokens_per_sec; return CE_ERR; }
CeResult ce_ebpf_xdp_filter_set_enabled(CeEbpfContext* ctx, CeBool enabled) { (void)ctx; (void)enabled; return CE_ERR; }
void ce_ebpf_xdp_filter_get_stats(CeEbpfContext* ctx, uint64_t* total, uint64_t* passed, uint64_t* dropped_blacklist, uint64_t* dropped_syn_flood, uint64_t* dropped_rate_limit) { (void)ctx; if(total)*total=0; if(passed)*passed=0; if(dropped_blacklist)*dropped_blacklist=0; if(dropped_syn_flood)*dropped_syn_flood=0; if(dropped_rate_limit)*dropped_rate_limit=0; }
CeResult ce_ebpf_xdp_dispatch_attach(CeEbpfContext* ctx, const char* ifname) { (void)ctx; (void)ifname; return CE_ERR; }
CeResult ce_ebpf_xdp_dispatch_set_num_cpus(CeEbpfContext* ctx, uint32_t num_cpus) { (void)ctx; (void)num_cpus; return CE_ERR; }
CeResult ce_ebpf_xdp_dispatch_set_mode(CeEbpfContext* ctx, uint32_t mode) { (void)ctx; (void)mode; return CE_ERR; }
CeResult ce_ebpf_xdp_dispatch_set_player_id_offset(CeEbpfContext* ctx, uint32_t offset) { (void)ctx; (void)offset; return CE_ERR; }
CeResult ce_ebpf_xdp_dispatch_add_xsk(CeEbpfContext* ctx, uint32_t queue_id, int xsk_fd) { (void)ctx; (void)queue_id; (void)xsk_fd; return CE_ERR; }
void ce_ebpf_xdp_dispatch_get_stats(CeEbpfContext* ctx, uint64_t* total, uint64_t* tcp, uint64_t* udp, uint64_t* redirected, uint64_t* passed, uint64_t* dropped) { (void)ctx; if(total)*total=0; if(tcp)*tcp=0; if(udp)*udp=0; if(redirected)*redirected=0; if(passed)*passed=0; if(dropped)*dropped=0; }

CeBool ce_ebpf_available(void) { return CE_FALSE; }

#endif /* CHAOS_HAS_EBPF */
