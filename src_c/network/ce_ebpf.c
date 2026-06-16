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

/* ---- 内部结构 ---- */

#define CE_EBPF_MAX_PROGS 16

struct CeEbpfContext {
    struct bpf_object*  obj;
    struct bpf_program* progs[CE_EBPF_MAX_PROGS];
    int                 prog_count;
    struct bpf_link*    links[CE_EBPF_MAX_PROGS];
    int                 link_count;
    CeBool              loaded;
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
CeBool ce_ebpf_available(void) { return CE_FALSE; }

#endif /* CHAOS_HAS_EBPF */
