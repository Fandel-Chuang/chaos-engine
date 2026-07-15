/*
 * ChaosEngine Cell 大地图管理 - 向后兼容层
 * 纯 C99，单线程安全
 *
 * 旧全局 API 通过内部默认 CeCellManager 实例委托实现。
 * 实际逻辑在 ce_cell_manager.c 中。
 */

#include "server/ce_cell.h"
#include "server/ce_cell_manager.h"
#include "server/ce_aoi.h"
#include "public_api/ce_log.h"

/* ---- 旧全局 API（委托给默认 CeCellManager）---- */

CeResult ce_cell_init(float world_width, float world_height,
                       float cell_width, float cell_height,
                       int max_entities_per_cell, int min_entities_per_cell) {
    CeCellManager* mgr = ce_cell_mgr_get_default();
    if (ce_cell_mgr_is_initialized(mgr)) return CE_OK;

    mgr = ce_cell_mgr_create(world_width, world_height,
                             cell_width, cell_height,
                             max_entities_per_cell, min_entities_per_cell);
    if (!mgr) return CE_ERR;

    ce_cell_mgr_set_default(mgr);

    /* 初始化全局 AOI（旧 API 兼容） */
    ce_aoi_init(ce_cell_mgr_get_aoi_radius(mgr), NULL, NULL);

    return CE_OK;
}

void ce_cell_shutdown(void) {
    CeCellManager* mgr = ce_cell_mgr_get_default();
    if (!mgr) return;

    ce_aoi_shutdown();
    ce_cell_mgr_destroy(mgr);
    ce_cell_mgr_set_default(NULL);
    CE_LOG_INFO("CELL", "Shut down");
}

CeCellId ce_cell_find_by_position(float x, float y) {
    return ce_cell_mgr_find_by_position(ce_cell_mgr_get_default(), x, y);
}

const CeCell* ce_cell_get(CeCellId cell_id) {
    return ce_cell_mgr_get(ce_cell_mgr_get_default(), cell_id);
}

int ce_cell_count(void) {
    return ce_cell_mgr_count(ce_cell_mgr_get_default());
}

CeResult ce_cell_enter_entity(CeServerEntityId entity_id, float x, float y, float radius) {
    return ce_cell_mgr_enter_entity(ce_cell_mgr_get_default(), entity_id, x, y, radius);
}

void ce_cell_leave_entity(CeServerEntityId entity_id) {
    ce_cell_mgr_leave_entity(ce_cell_mgr_get_default(), entity_id);
}

CeResult ce_cell_move_entity(CeServerEntityId entity_id, float new_x, float new_y) {
    return ce_cell_mgr_move_entity(ce_cell_mgr_get_default(), entity_id, new_x, new_y);
}

void ce_cell_update(void) {
    ce_cell_mgr_update(ce_cell_mgr_get_default());
}

CeResult ce_cell_split(CeCellId cell_id) {
    return ce_cell_mgr_split(ce_cell_mgr_get_default(), cell_id);
}

CeResult ce_cell_merge(CeCellId cell_a, CeCellId cell_b) {
    return ce_cell_mgr_merge(ce_cell_mgr_get_default(), cell_a, cell_b);
}

void ce_cell_assign_process(CeCellId cell_id, int process_id) {
    ce_cell_mgr_assign_process(ce_cell_mgr_get_default(), cell_id, process_id);
}

int ce_cell_get_process(CeCellId cell_id) {
    return ce_cell_mgr_get_process(ce_cell_mgr_get_default(), cell_id);
}

int ce_cell_query_nearby(CeServerEntityId entity_id,
                          CeServerEntityId* buffer, int max_count) {
    return ce_cell_mgr_query_nearby(ce_cell_mgr_get_default(), entity_id, buffer, max_count);
}

void ce_cell_debug_print(void) {
    ce_cell_mgr_debug_print(ce_cell_mgr_get_default());
}
