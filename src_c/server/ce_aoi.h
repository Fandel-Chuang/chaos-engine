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

#ifdef __cplusplus
}
#endif

#endif /* CE_AOI_H */
