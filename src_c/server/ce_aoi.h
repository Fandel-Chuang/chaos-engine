/*
 * ChaosEngine AOI 十字链表
 * 纯 C99，单线程安全
 *
 * 每个实体同时挂在 X 轴和 Y 轴的有序链表上。
 * 移动时只需在链表中调整位置，查询周围实体时沿两个轴向遍历。
 */

#ifndef CE_AOI_H
#define CE_AOI_H

#include "server/ce_server_types.h"
#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

/** 复制管理器上下文 (来自 replication/ce_replication.h) */
typedef struct CeReplContext CeReplContext;

/* ---- 生命周期 ---- */

/** 初始化 AOI 系统 */
void ce_aoi_init(float aoi_radius, CeAoiEventCallback callback, void* user_data);

/** 关闭 AOI 系统，释放所有节点 */
void ce_aoi_shutdown(void);

/* ---- 实体管理 ---- */

/** 添加实体到 AOI */
CeResult ce_aoi_enter(CeServerEntityId entity_id, float x, float y, float radius);

/** 从 AOI 移除实体 */
void ce_aoi_leave(CeServerEntityId entity_id);

/** 移动实体（自动检测进出视野事件） */
CeResult ce_aoi_move(CeServerEntityId entity_id, float new_x, float new_y);

/* ---- 查询 ---- */

/** 获取实体周围的实体列表（结果写入 buffer，返回数量） */
int ce_aoi_query_nearby(CeServerEntityId entity_id, CeServerEntityId* buffer, int max_count);

/** 获取实体周围的实体数量 */
int ce_aoi_count_nearby(CeServerEntityId entity_id);

/* ---- 调试 ---- */

/** 获取 AOI 系统中的实体总数 */
int ce_aoi_entity_count(void);

/** 打印 AOI 链表结构（调试用） */
void ce_aoi_debug_print(void);

/* ---- 复制集成 ---- */

/**
 * 设置复制管理器上下文
 *
 * 设置后，AOI 的 enter/leave 事件会自动触发复制脏标：
 *   - ENTER: 标记进入实体的所有非 SERVER_ONLY 字段为脏
 *   - LEAVE: 记录日志（实际离开通知在后续 Phase 实现）
 *
 * @param ctx  复制管理器上下文 (NULL 则禁用复制集成)
 */
void ce_aoi_set_replication_context(CeReplContext* ctx);

/* ================================================================
 * Grid AOI API (Phase 1.4)
 *
 * 可实例化的 Grid 空间索引，enter/move/leave O(1)，query O(k)。
 * k = 覆盖的 cell 数量 × cell 内实体数。
 * 与旧十字链表 API 完全独立，向后兼容。
 * ================================================================ */

/** Grid AOI 不透明句柄 */
typedef struct CeAoiGrid CeAoiGrid;

/**
 * 创建 Grid AOI 实例。
 *
 * @param world_w   世界宽度
 * @param world_h   世界高度
 * @param cell_w    单个 cell 宽度（>0）
 * @param cell_h    单个 cell 高度（>0）
 * @param radius    默认查询半径（用于 query 时的回退值）
 * @return 成功返回 CeAoiGrid*，失败返回 NULL
 */
CeAoiGrid* ce_aoi_grid_create(float world_w, float world_h,
                               float cell_w, float cell_h, float radius);

/** 销毁 Grid AOI 实例，释放所有资源 */
void ce_aoi_grid_destroy(CeAoiGrid* grid);

/**
 * 实体进入 Grid AOI。O(1)
 *
 * @param grid       Grid 实例
 * @param entity_id  实体 ID
 * @param x          X 坐标
 * @param y          Y 坐标
 * @param z          Z 坐标（当前 2D 实现，z 被忽略）
 * @return CE_OK 成功，CE_ERR 失败（未初始化/重复进入/越界）
 */
CeResult ce_aoi_grid_enter(CeAoiGrid* grid, uint32_t entity_id,
                            float x, float y, float z);

/**
 * 实体移动。O(1)
 * 自动从旧 cell 移除，加入新 cell。
 *
 * @param grid       Grid 实例
 * @param entity_id  实体 ID
 * @param x          新 X 坐标
 * @param y          新 Y 坐标
 * @param z          新 Z 坐标（当前 2D 实现，z 被忽略）
 * @return CE_OK 成功，CE_ERR 失败（实体不存在/越界）
 */
CeResult ce_aoi_grid_move(CeAoiGrid* grid, uint32_t entity_id,
                           float x, float y, float z);

/**
 * 实体离开 Grid AOI。O(1)
 *
 * @param grid       Grid 实例
 * @param entity_id  实体 ID
 * @return CE_OK 成功，CE_ERR 失败（实体不存在）
 */
CeResult ce_aoi_grid_leave(CeAoiGrid* grid, uint32_t entity_id);

/**
 * 查询给定位置和半径范围内的所有实体。O(k)
 * k = 覆盖的 cell 数量 × cell 内实体数
 *
 * @param grid       Grid 实例
 * @param x          查询中心 X
 * @param y          查询中心 Y
 * @param z          查询中心 Z（当前 2D 实现，z 被忽略）
 * @param radius     查询半径
 * @param out_ids    输出缓冲区，存储找到的实体 ID
 * @param max_count  缓冲区最大容量
 * @return 找到的实体数量（不超过 max_count）
 */
int ce_aoi_grid_query(CeAoiGrid* grid, float x, float y, float z,
                       float radius, uint32_t* out_ids, int max_count);

/** 获取 Grid AOI 中的实体总数 */
int ce_aoi_grid_entity_count(CeAoiGrid* grid);

#ifdef __cplusplus
}
#endif

#endif /* CE_AOI_H */
