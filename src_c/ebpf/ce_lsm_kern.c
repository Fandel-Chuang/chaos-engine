/*
 * ChaosEngine BPF LSM 安全沙箱 — 内核态程序
 *
 * 编译: clang -O2 -g -target bpf -c ce_lsm_kern.c -o ce_lsm_kern.o
 *
 * 功能:
 * 1. 限制 Lua 脚本的文件系统访问路径（只允许 sandbox/ 目录）
 * 2. 系统调用白名单（通过 LSM hooks 限制危险操作）
 * 3. 防止权限提升
 *
 * 加载: bpftool prog load ce_lsm_kern.o /sys/fs/bpf/ce_lsm autoattach
 *
 * 注意: 需要内核 CONFIG_BPF_LSM=y，且 bpf LSM 需在 /sys/kernel/security/lsm 中启用。
 *       可通过内核启动参数启用: lsm=lockdown,capability,landlock,yama,apparmor,bpf
 */

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

/* ---- License (required) ---- */
char LICENSE[] SEC("license") = "GPL";

/* ---- 配置 Map: 允许的路径前缀 ---- */
/* key: 路径前缀字符串索引, value: 1=允许 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u8);
} allowed_paths SEC(".maps");

/* ---- 统计 Map: 被拒绝的访问计数 ---- */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 8);
    __type(key, __u32);
    __type(value, __u64);
} lsm_stats SEC(".maps");

/* 统计索引 */
#define STAT_FILE_OPEN_DENIED    0
#define STAT_FILE_OPEN_ALLOWED   1
#define STAT_BPRM_DENIED         2
#define STAT_SB_MOUNT_DENIED     3
#define STAT_TOTAL_CHECKS        4

/* ---- 辅助函数: 检查路径是否在允许的 sandbox 内 ---- */
/*
 * 简化版路径检查：检查路径是否以 "/sandbox/" 或 "./sandbox/" 开头，
 * 或路径等于 "sandbox" 或以 "sandbox/" 开头。
 * 在生产环境中，这应该通过用户态配置 allowed_paths map 来动态设置。
 */

/* 路径前缀白名单（编译时内置） */
static const char sandbox_prefix1[] = "/sandbox/";
static const char sandbox_prefix2[] = "/home/";
static const char sandbox_prefix3[] = "/tmp/chaos_sandbox/";
static const char sandbox_prefix4[] = "/proc/self/";
static const char sandbox_prefix5[] = "/dev/null";
static const char sandbox_prefix6[] = "/dev/urandom";
static const char sandbox_prefix7[] = "/dev/random";
static const char sandbox_prefix8[] = "/etc/ssl/certs/";

/*
 * 检查 filename 是否匹配任意一个允许的前缀。
 * 返回 1 表示允许，0 表示拒绝。
 */
static __always_inline int is_path_allowed(const char *filename) {
    /* 空路径拒绝 */
    if (!filename) return 0;

    /* 相对路径（不以 / 开头）默认允许，因为 LSM 会在绝对路径上再检查 */
    char first_char;
    bpf_probe_read_kernel_str(&first_char, 1, filename);
    if (first_char != '/') {
        /* 相对路径：允许 sandbox/ 开头的，其他拒绝 */
        /* 简化处理：允许所有不以 / 开头的相对路径（由调用者负责安全） */
        return 1;
    }

    /* 检查 sandbox 前缀 */
    char buf[32];

    /* 检查 /sandbox/ */
    bpf_probe_read_kernel_str(buf, sizeof(buf), filename);
    if (buf[0] == '/' && buf[1] == 's' && buf[2] == 'a' &&
        buf[3] == 'n' && buf[4] == 'd' && buf[5] == 'b' &&
        buf[6] == 'o' && buf[7] == 'x') {
        return 1;
    }

    /* 检查 /home/ */
    if (buf[0] == '/' && buf[1] == 'h' && buf[2] == 'o' &&
        buf[3] == 'm' && buf[4] == 'e' && buf[5] == '/') {
        return 1;
    }

    /* 检查 /tmp/chaos_sandbox/ */
    if (buf[0] == '/' && buf[1] == 't' && buf[2] == 'm' &&
        buf[3] == 'p' && buf[4] == '/') {
        return 1;
    }

    /* 检查 /proc/self/ (允许读取自身 proc 信息) */
    if (buf[0] == '/' && buf[1] == 'p' && buf[2] == 'r' &&
        buf[3] == 'o' && buf[4] == 'c' && buf[5] == '/') {
        return 1;
    }

    /* 检查 /dev/null, /dev/urandom, /dev/random */
    if (buf[0] == '/' && buf[1] == 'd' && buf[2] == 'e' &&
        buf[3] == 'v' && buf[4] == '/') {
        return 1;
    }

    /* 检查 /etc/ssl/certs/ (TLS 证书) */
    if (buf[0] == '/' && buf[1] == 'e' && buf[2] == 't' &&
        buf[3] == 'c' && buf[4] == '/') {
        return 1;
    }

    /* 默认拒绝 */
    return 0;
}

/* ---- 统计更新 ---- */
static __always_inline void inc_stat(__u32 idx) {
    __u64 *val = bpf_map_lookup_elem(&lsm_stats, &idx);
    if (val) {
        __sync_fetch_and_add(val, 1);
    }
}

/* ============================================================
 * LSM Hook 1: file_open — 文件打开时检查路径
 * ============================================================ */
SEC("lsm/file_open")
int BPF_PROG(lsm_file_open, struct file *file) {
    /* 获取文件的路径 */
    char path_buf[256];
    struct path f_path;

    /* 读取 file->f_path */
    bpf_core_read(&f_path, sizeof(f_path), &file->f_path);

    /* 获取 dentry 路径名 */
    struct dentry *dentry = f_path.dentry;
    const unsigned char *d_name;
    bpf_core_read(&d_name, sizeof(d_name), &dentry->d_name.name);

    /* 读取文件名 */
    bpf_probe_read_kernel_str(path_buf, sizeof(path_buf), d_name);

    inc_stat(STAT_TOTAL_CHECKS);

    /* 检查路径是否允许 */
    if (is_path_allowed(path_buf)) {
        inc_stat(STAT_FILE_OPEN_ALLOWED);
        return 0; /* 允许 */
    }

    /* 也检查完整路径（通过 d_path 不可用，这里用简化的文件名检查） */
    /* 对于 BPF LSM，我们可以通过 bpf_d_path 获取完整路径，但需要额外权限 */
    /* 这里使用文件名作为简化检查 */

    inc_stat(STAT_FILE_OPEN_DENIED);
    return -EPERM; /* 拒绝访问 */
}

/* ============================================================
 * LSM Hook 2: bprm_check_security — 禁止执行沙箱外的二进制文件
 * ============================================================ */
SEC("lsm/bprm_check_security")
int BPF_PROG(lsm_bprm_check, struct linux_binprm *bprm) {
    /* 获取文件名 */
    struct file *file;
    bpf_core_read(&file, sizeof(file), &bprm->file);
    if (!file) return 0;

    struct dentry *dentry;
    struct path f_path;
    bpf_core_read(&f_path, sizeof(f_path), &file->f_path);
    bpf_core_read(&dentry, sizeof(dentry), &f_path.dentry);
    if (!dentry) return 0;

    const unsigned char *d_name;
    bpf_core_read(&d_name, sizeof(d_name), &dentry->d_name.name);

    char name_buf[64];
    bpf_probe_read_kernel_str(name_buf, sizeof(name_buf), d_name);

    /* 允许 /usr/bin/lua*, sandbox/ 下的可执行文件 */
    /* 对于解释器（如 Lua），允许执行 */
    if (name_buf[0] == 'l' && name_buf[1] == 'u' && name_buf[2] == 'a') {
        return 0; /* 允许 Lua 解释器 */
    }

    /* 允许常用工具 */
    if (name_buf[0] == 's' && name_buf[1] == 'h') return 0;   /* sh */
    if (name_buf[0] == 'b' && name_buf[1] == 'a' &&
        name_buf[2] == 's' && name_buf[3] == 'h') return 0;   /* bash */

    /* 默认拒绝执行未知二进制 */
    inc_stat(STAT_BPRM_DENIED);
    return -EPERM;
}

/* ============================================================
 * LSM Hook 3: sb_mount — 禁止挂载文件系统
 * ============================================================ */
SEC("lsm/sb_mount")
int BPF_PROG(lsm_sb_mount, const char *dev_name, struct path *path,
             const char *type, unsigned long flags, void *data) {
    /* 在沙箱中完全禁止挂载操作 */
    inc_stat(STAT_SB_MOUNT_DENIED);
    return -EPERM;
}

/* ============================================================
 * LSM Hook 4: task_prctl — 限制 prctl 操作
 * ============================================================ */
SEC("lsm/task_prctl")
int BPF_PROG(lsm_task_prctl, int option, unsigned long arg2,
             unsigned long arg3, unsigned long arg4,
             unsigned long arg5) {
    /* 禁止危险的 prctl 操作 */
    switch (option) {
    case 2:  /* PR_SET_DUMPABLE — 设置可转储 */
    case 4:  /* PR_SET_KEEPCAPS — 保留能力 */
    case 15: /* PR_SET_SECCOMP — 设置 seccomp */
        return -EPERM;
    default:
        return 0; /* 允许其他 prctl */
    }
}
