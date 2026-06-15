/*
 * ChaosEngine Cell 大地图管理
 * 纯 C99，单线程安全
 *
 * 将大地图划分为多个 Cell，每个 Cell 可分配给不同进程。
 * Cell 根据负载（实体数量）动态分裂和合并。
 */

#ifndef CE_CELL_H
#define CE_CELL_H

#include "server/ce_server_types.h"
#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 ---- */

/** 初始化 Cell 管理器 */
CeResult ce_cell_init(float world_width, float world_height,
                       float cell_width, float cell_height,
                       int max_entities_per_cell, int min_entities_per_cell);

/** 关闭 Cell 管理器 */
void ce_cell_shutdown(void);

/* ---- Cell 操作 ---- */

/** 根据坐标查找 Cell */
CeCellId ce_cell_find_by_position(float x, float y);

/** 获取 Cell 信息 */
const CeCell* ce_cell_get(CeCellId cell_id);

/** 获取 Cell 数量 */
int ce_cell_count(void);

/* ---- 实体管理 ---- */

/** 实体进入世界（自动分配到对应 Cell） */
CeResult ce_cell_enter_entity(CeServerEntityId entity_id, float x, float y, float radius);

/** 实体离开世界 */
void ce_cell_leave_entity(CeServerEntityId entity_id);

/** 实体移动（可能触发跨 Cell 迁移） */
CeResult ce_cell_move_entity(CeServerEntityId entity_id, float new_x, float new_y);

/* ---- 动态管理 ---- */

/** 检查并执行 Cell 分裂/合并（每帧调用） */
void ce_cell_update(void);

/** 手动分裂指定 Cell */
CeResult ce_cell_split(CeCellId cell_id);

/** 手动合并两个相邻 Cell */
CeResult ce_cell_merge(CeCellId cell_a, CeCellId cell_b);

/* ---- 进程分配 ---- */

/** 将 Cell 分配给指定进程 */
void ce_cell_assign_process(CeCellId cell_id, int process_id);

/** 获取 Cell 的进程 ID */
int ce_cell_get_process(CeCellId cell_id);

/* ---- 跨 Cell 查询 ---- */

/** 查询实体周围的实体（自动处理跨 Cell 边界） */
int ce_cell_query_nearby(CeServerEntityId entity_id,
                          CeServerEntityId* buffer, int max_count);

/* ---- 调试 ---- */

/** 打印 Cell 网格状态 */
void ce_cell_debug_print(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_CELL_H */
