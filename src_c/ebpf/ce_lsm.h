/*
 * ChaosEngine BPF LSM 安全沙箱 — 用户态接口
 *
 * 提供 Lua 脚本沙箱的文件系统访问控制和系统调用白名单。
 * Linux 专属，需要 CONFIG_BPF_LSM=y。
 */

#ifndef CE_LSM_H
#define CE_LSM_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明句柄 ---- */

typedef struct CeLsmContext CeLsmContext;

/* ---- 生命周期 ---- */

/** 初始化 LSM 沙箱（加载 BPF LSM 程序到内核） */
CeLsmContext* ce_lsm_init(void);

/** 关闭 LSM 沙箱（卸载 BPF LSM 程序） */
void ce_lsm_shutdown(CeLsmContext* ctx);

/* ---- 路径白名单管理 ---- */

/** 添加允许访问的路径前缀（如 "/sandbox/"） */
CeResult ce_lsm_allow_path(CeLsmContext* ctx, const char* path_prefix);

/** 移除允许访问的路径前缀 */
CeResult ce_lsm_deny_path(CeLsmContext* ctx, const char* path_prefix);

/** 清空所有路径白名单 */
void ce_lsm_clear_paths(CeLsmContext* ctx);

/* ---- 统计查询 ---- */

/** 获取被拒绝的文件访问次数 */
int ce_lsm_get_denied_count(CeLsmContext* ctx);

/** 获取允许的文件访问次数 */
int ce_lsm_get_allowed_count(CeLsmContext* ctx);

/** 获取被拒绝的 exec 次数 */
int ce_lsm_get_exec_denied_count(CeLsmContext* ctx);

/* ---- 查询 ---- */

/** BPF LSM 是否可用 */
CeBool ce_lsm_available(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_LSM_H */
