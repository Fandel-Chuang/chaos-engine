/*
 * ChaosEngine Cell 管理器 - 实例化接口
 * 纯 C99，单线程安全
 *
 * 将全局单例 g_cell 改为可实例化的 CeCellManager。
 * 旧全局 API (ce_cell_init 等) 通过内部默认实例保持向后兼容。
 */

#ifndef CE_CELL_MANAGER_H
#define CE_CELL_MANAGER_H

#include "server/ce_server_types.h"
#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

typedef struct CeCellManager CeCellManager;

/* ---- 生命周期 ---- */

/**
 * 创建 Cell 管理器实例。
 *
 * @param world_width         世界宽度
 * @param world_height        世界高度
 * @param cell_width          单个 Cell 宽度
 * @param cell_height         单个 Cell 高度
 * @param max_entities_per_cell  每个 Cell 最大实体数（分裂阈值）
 * @param min_entities_per_cell  每个 Cell 最小实体数（合并阈值）
 * @return 成功返回 CeCellManager*，失败返回 NULL
 */
CeCellManager* ce_cell_mgr_create(float world_width, float world_height,
                                   float cell_width, float cell_height,
                                   int max_entities_per_cell,
                                   int min_entities_per_cell);

/** 销毁 Cell 管理器实例，释放所有资源 */
void ce_cell_mgr_destroy(CeCellManager* mgr);

/** 获取默认管理器实例（若不存在则返回 NULL，不自动创建） */
CeCellManager* ce_cell_mgr_get_default(void);

/** 设置默认管理器实例（由 ce_cell_init 内部调用） */
void ce_cell_mgr_set_default(CeCellManager* mgr);

/** 检查管理器是否已初始化 */
CeBool ce_cell_mgr_is_initialized(CeCellManager* mgr);

/** 获取管理器的 AOI 半径 */
float ce_cell_mgr_get_aoi_radius(CeCellManager* mgr);

/* ---- Cell 操作 ---- */

/** 根据坐标查找 Cell */
CeCellId ce_cell_mgr_find_by_position(CeCellManager* mgr, float x, float y);

/** 获取 Cell 信息 */
const CeCell* ce_cell_mgr_get(CeCellManager* mgr, CeCellId cell_id);

/** 获取 Cell 数量 */
int ce_cell_mgr_count(CeCellManager* mgr);

/* ---- 实体管理 ---- */

/** 实体进入世界（自动分配到对应 Cell） */
CeResult ce_cell_mgr_enter_entity(CeCellManager* mgr,
                                   CeServerEntityId entity_id,
                                   float x, float y, float radius);

/** 实体离开世界 */
void ce_cell_mgr_leave_entity(CeCellManager* mgr, CeServerEntityId entity_id);

/** 实体移动（可能触发跨 Cell 迁移） */
CeResult ce_cell_mgr_move_entity(CeCellManager* mgr,
                                  CeServerEntityId entity_id,
                                  float new_x, float new_y);

/* ---- 动态管理 ---- */

/** 检查并执行 Cell 分裂/合并（每帧调用） */
void ce_cell_mgr_update(CeCellManager* mgr);

/** 手动分裂指定 Cell */
CeResult ce_cell_mgr_split(CeCellManager* mgr, CeCellId cell_id);

/** 手动合并两个相邻 Cell */
CeResult ce_cell_mgr_merge(CeCellManager* mgr, CeCellId cell_a, CeCellId cell_b);

/* ---- 进程分配 ---- */

/** 将 Cell 分配给指定进程 */
void ce_cell_mgr_assign_process(CeCellManager* mgr, CeCellId cell_id, int process_id);

/** 获取 Cell 的进程 ID */
int ce_cell_mgr_get_process(CeCellManager* mgr, CeCellId cell_id);

/* ---- 跨 Cell 查询 ---- */

/** 查询实体周围的实体（自动处理跨 Cell 边界） */
int ce_cell_mgr_query_nearby(CeCellManager* mgr,
                              CeServerEntityId entity_id,
                              CeServerEntityId* buffer, int max_count);

/* ---- 调试 ---- */

/** 打印 Cell 网格状态 */
void ce_cell_mgr_debug_print(CeCellManager* mgr);

#ifdef __cplusplus
}
#endif

#endif /* CE_CELL_MANAGER_H */
