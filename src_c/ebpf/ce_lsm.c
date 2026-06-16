/*
 * ChaosEngine BPF LSM 安全沙箱 — 用户态加载器
 *
 * 使用 libbpf 加载 BPF LSM 程序到内核。
 * 编译条件: CHAOS_HAS_EBPF
 */

#ifdef CHAOS_HAS_EBPF

#include "ebpf/ce_lsm.h"
#include "public_api/ce_log.h"

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

/* ---- 内部结构 ---- */

#define CE_LSM_MAX_LINKS 16

struct CeLsmContext {
    struct bpf_object*  obj;
    struct bpf_link*    links[CE_LSM_MAX_LINKS];
    int                 link_count;
    CeBool              loaded;
};

/* ---- 生命周期 ---- */

CeLsmContext* ce_lsm_init(void) {
    CeLsmContext* ctx = (CeLsmContext*)calloc(1, sizeof(CeLsmContext));
    if (!ctx) return NULL;

    /* 打开 BPF LSM 对象文件 */
    const char* obj_path = "src_c/ebpf/ce_lsm_kern.o";
    ctx->obj = bpf_object__open(obj_path);
    if (!ctx->obj) {
        obj_path = "ce_lsm_kern.o";
        ctx->obj = bpf_object__open(obj_path);
    }
    if (!ctx->obj) {
        CE_LOG_WARN("LSM", "Failed to open BPF LSM object: %s", strerror(errno));
        free(ctx);
        return NULL;
    }

    /* 加载 BPF 程序到内核 */
    int ret = bpf_object__load(ctx->obj);
    if (ret < 0) {
        CE_LOG_ERROR("LSM", "Failed to load BPF LSM object: %d (%s)", ret, strerror(-ret));
        bpf_object__close(ctx->obj);
        free(ctx);
        return NULL;
    }
    ctx->loaded = CE_TRUE;

    /* Attach 所有 LSM 程序 */
    struct bpf_program* prog;
    bpf_object__for_each_program(prog, ctx->obj) {
        const char* name = bpf_program__name(prog);
        if (!name) continue;

        struct bpf_link* link = bpf_program__attach(prog);
        if (!link) {
            CE_LOG_WARN("LSM", "Failed to attach LSM program '%s': %s",
                        name, strerror(errno));
            continue;
        }

        if (ctx->link_count < CE_LSM_MAX_LINKS) {
            ctx->links[ctx->link_count++] = link;
            CE_LOG_INFO("LSM", "Attached LSM program: %s", name);
        } else {
            bpf_link__destroy(link);
            CE_LOG_WARN("LSM", "Max LSM links reached, skipping %s", name);
        }
    }

    CE_LOG_INFO("LSM", "BPF LSM sandbox initialized (%d programs attached)",
                ctx->link_count);
    return ctx;
}

void ce_lsm_shutdown(CeLsmContext* ctx) {
    if (!ctx) return;

    for (int i = 0; i < ctx->link_count; i++) {
        if (ctx->links[i]) bpf_link__destroy(ctx->links[i]);
    }

    if (ctx->obj) bpf_object__close(ctx->obj);
    free(ctx);
    CE_LOG_INFO("LSM", "BPF LSM sandbox shut down");
}

/* ---- 路径白名单管理 ---- */

CeResult ce_lsm_allow_path(CeLsmContext* ctx, const char* path_prefix) {
    if (!ctx || !ctx->obj || !path_prefix) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "allowed_paths");
    if (!map) {
        CE_LOG_WARN("LSM", "Map 'allowed_paths' not found");
        return CE_ERR;
    }

    /* 使用路径字符串的 hash 作为 key */
    __u32 key = 0;
    for (const char* p = path_prefix; *p; p++) {
        key = key * 31 + (unsigned char)*p;
    }

    __u8 value = 1;
    int ret = bpf_map_update_elem(bpf_map__fd(map), &key, &value, BPF_ANY);
    if (ret < 0) {
        CE_LOG_WARN("LSM", "Failed to add allowed path '%s': %s",
                    path_prefix, strerror(-ret));
        return CE_ERR;
    }

    CE_LOG_INFO("LSM", "Allowed path: %s", path_prefix);
    return CE_OK;
}

CeResult ce_lsm_deny_path(CeLsmContext* ctx, const char* path_prefix) {
    if (!ctx || !ctx->obj || !path_prefix) return CE_ERR;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "allowed_paths");
    if (!map) return CE_ERR;

    __u32 key = 0;
    for (const char* p = path_prefix; *p; p++) {
        key = key * 31 + (unsigned char)*p;
    }

    int ret = bpf_map_delete_elem(bpf_map__fd(map), &key);
    if (ret < 0 && ret != -ENOENT) {
        CE_LOG_WARN("LSM", "Failed to remove path '%s': %s",
                    path_prefix, strerror(-ret));
        return CE_ERR;
    }

    CE_LOG_INFO("LSM", "Denied path: %s", path_prefix);
    return CE_OK;
}

void ce_lsm_clear_paths(CeLsmContext* ctx) {
    if (!ctx || !ctx->obj) return;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "allowed_paths");
    if (!map) return;

    /* 遍历并删除所有条目 */
    int fd = bpf_map__fd(map);
    __u32 key = 0, next_key;

    while (bpf_map_get_next_key(fd, &key, &next_key) == 0) {
        bpf_map_delete_elem(fd, &next_key);
        key = next_key;
    }

    CE_LOG_INFO("LSM", "All allowed paths cleared");
}

/* ---- 统计查询 ---- */

static int lsm_get_stat(CeLsmContext* ctx, __u32 idx) {
    if (!ctx || !ctx->obj) return 0;

    struct bpf_map* map = bpf_object__find_map_by_name(ctx->obj, "lsm_stats");
    if (!map) return 0;

    __u64 value = 0;
    bpf_map_lookup_elem(bpf_map__fd(map), &idx, &value);
    return (int)value;
}

int ce_lsm_get_denied_count(CeLsmContext* ctx) {
    return lsm_get_stat(ctx, 0); /* STAT_FILE_OPEN_DENIED */
}

int ce_lsm_get_allowed_count(CeLsmContext* ctx) {
    return lsm_get_stat(ctx, 1); /* STAT_FILE_OPEN_ALLOWED */
}

int ce_lsm_get_exec_denied_count(CeLsmContext* ctx) {
    return lsm_get_stat(ctx, 2); /* STAT_BPRM_DENIED */
}

/* ---- 查询 ---- */

CeBool ce_lsm_available(void) {
    /* 检查 /sys/kernel/btf/vmlinux 和 /sys/fs/bpf 是否可用 */
    if (access("/sys/kernel/btf/vmlinux", F_OK) != 0) return CE_FALSE;
    if (access("/sys/fs/bpf", F_OK) != 0) return CE_FALSE;

    /* 检查内核是否支持 BPF LSM */
    FILE* f = fopen("/sys/kernel/security/lsm", "r");
    if (!f) return CE_FALSE;

    char buf[256];
    CeBool found = CE_FALSE;
    if (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "bpf")) found = CE_TRUE;
    }
    fclose(f);
    return found;
}

#else /* !CHAOS_HAS_EBPF — stubs */

#include "ebpf/ce_lsm.h"

CeLsmContext* ce_lsm_init(void) { return NULL; }
void ce_lsm_shutdown(CeLsmContext* ctx) { (void)ctx; }
CeResult ce_lsm_allow_path(CeLsmContext* ctx, const char* path_prefix) {
    (void)ctx; (void)path_prefix; return CE_ERR;
}
CeResult ce_lsm_deny_path(CeLsmContext* ctx, const char* path_prefix) {
    (void)ctx; (void)path_prefix; return CE_ERR;
}
void ce_lsm_clear_paths(CeLsmContext* ctx) { (void)ctx; }
int ce_lsm_get_denied_count(CeLsmContext* ctx) { (void)ctx; return 0; }
int ce_lsm_get_allowed_count(CeLsmContext* ctx) { (void)ctx; return 0; }
int ce_lsm_get_exec_denied_count(CeLsmContext* ctx) { (void)ctx; return 0; }
CeBool ce_lsm_available(void) { return CE_FALSE; }

#endif /* CHAOS_HAS_EBPF */
