/*
 * ChaosEngine Public API
 * 
 * 这是编辑器与引擎内核的唯一通信接口。
 * 规则：
 *   - 纯 C 语法，禁止 class/namespace/template 等 C++ 关键字
 *   - 所有函数使用 CE_ 前缀，类型使用 Ce 前缀
 *   - 编辑器通过 extern "C" 调用这些接口
 */

#ifndef CHAOS_ENGINE_H
#define CHAOS_ENGINE_H

#include "ce_types.h"
#include "ce_ecs.h"
#include "ce_plugin.h"
#include "ce_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 引擎生命周期 ---- */

/** 初始化引擎，传入配置参数 */
CeResult ce_init(const CeEngineConfig* config);

/** 关闭引擎，释放所有资源 */
void ce_shutdown(void);

/** 主循环：每帧调用一次，返回 false 表示退出 */
CeBool ce_update(void);

/** 获取引擎当前运行状态 */
CeEngineState ce_get_state(void);

/* ---- 场景管理 ---- */

/** 加载场景 */
CeResult ce_scene_load(const char* scene_path);

/** 卸载当前场景 */
void ce_scene_unload(void);

/** 获取当前场景名称 */
const char* ce_scene_get_name(void);

/* ---- 资源管理 ---- */

/** 查询已加载资源数量 */
uint32_t ce_resource_get_count(CeResourceType type);

/** 获取资源信息快照（只读） */
CeResult ce_resource_query_info(uint32_t index, CeResourceInfo* out_info);

/* ---- 渲染 ---- */

/** 获取当前帧渲染统计 */
CeRenderStats ce_render_get_stats(void);

/* ---- 时间 ---- */

/** 获取引擎运行总时间（秒） */
double ce_time_get_total(void);

/** 获取上一帧耗时（秒） */
double ce_time_get_delta(void);

#ifdef __cplusplus
}
#endif

#endif /* CHAOS_ENGINE_H */
