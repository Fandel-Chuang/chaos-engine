/*
 * ChaosEngine Cell 管理器 - 实例化实现
 * 纯 C99，单线程安全
 *
 * 将全局单例 g_cell 改为可实例化的 CeCellManager。
 * 所有内部函数操作 CeCellManager* 参数。
 */

#include "server/ce_cell_manager.h"
#include "server/ce_cell.h"
#include "server/ce_aoi.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---- CeCellManager 结构体 ---- */

struct CeCellManager {
    CeCell*  cells;
    int      cell_count;
    int      cell_capacity;

    int      cells_per_row;
    int      cells_per_col;

    float    cell_width;
    float    cell_height;

    float    world_min_x;
    float    world_min_y;
    float    world_max_x;
    float    world_max_y;

    float    aoi_radius;

    int      max_entities_per_cell;
    int      min_entities_per_cell;

    int      next_cell_id;

    CeBool   initialized;
};

/* ---- 默认管理器实例（向后兼容旧 API）---- */

static CeCellManager* g_default_cell_mgr = NULL;

CeCellManager* ce_cell_mgr_get_default(void) {
    return g_default_cell_mgr;
}

void ce_cell_mgr_set_default(CeCellManager* mgr) {
    g_default_cell_mgr = mgr;
}

CeBool ce_cell_mgr_is_initialized(CeCellManager* mgr) {
    return mgr ? mgr->initialized : CE_FALSE;
}

float ce_cell_mgr_get_aoi_radius(CeCellManager* mgr) {
    return mgr ? mgr->aoi_radius : 0.0f;
}

/* ---- 辅助函数 ---- */

static int mgr_pos_to_cell_index(CeCellManager* mgr, float x, float y) {
    if (x < mgr->world_min_x || x >= mgr->world_max_x ||
        y < mgr->world_min_y || y >= mgr->world_max_y) {
        return -1;
    }

    int col = (int)((x - mgr->world_min_x) / mgr->cell_width);
    int row = (int)((y - mgr->world_min_y) / mgr->cell_height);

    /* 边界裁剪 */
    if (col >= mgr->cells_per_row) col = mgr->cells_per_row - 1;
    if (row >= mgr->cells_per_col) row = mgr->cells_per_col - 1;
    if (col < 0) col = 0;
    if (row < 0) row = 0;

    return row * mgr->cells_per_row + col;
}

static void mgr_build_neighbors(CeCellManager* mgr, int cell_index) {
    CeCell* cell = &mgr->cells[cell_index];
    cell->neighbor_count = 0;

    int row = cell_index / mgr->cells_per_row;
    int col = cell_index % mgr->cells_per_row;

    /* 8 个方向 */
    static const int dr[] = {-1, -1, -1,  0, 0,  1, 1, 1};
    static const int dc[] = {-1,  0,  1, -1, 1, -1, 0, 1};

    for (int i = 0; i < 8; i++) {
        int nr = row + dr[i];
        int nc = col + dc[i];
        if (nr >= 0 && nr < mgr->cells_per_col &&
            nc >= 0 && nc < mgr->cells_per_row) {
            int ni = nr * mgr->cells_per_row + nc;
            cell->neighbors[cell->neighbor_count++] = mgr->cells[ni].id;
        }
    }
}

static const CeCell* mgr_get_cell(CeCellManager* mgr, CeCellId cell_id) {
    if (!mgr || !mgr->initialized) return NULL;

    for (int i = 0; i < mgr->cell_capacity; i++) {
        if (mgr->cells[i].id == cell_id) return &mgr->cells[i];
    }
    return NULL;
}

/* ---- 生命周期 ---- */

CeCellManager* ce_cell_mgr_create(float world_width, float world_height,
                                   float cell_width, float cell_height,
                                   int max_entities_per_cell,
                                   int min_entities_per_cell) {
    CeCellManager* mgr = (CeCellManager*)calloc(1, sizeof(CeCellManager));
    if (!mgr) return NULL;

    mgr->cell_width  = cell_width;
    mgr->cell_height = cell_height;
    mgr->world_min_x = 0.0f;
    mgr->world_min_y = 0.0f;
    mgr->world_max_x = world_width;
    mgr->world_max_y = world_height;
    mgr->max_entities_per_cell = max_entities_per_cell;
    mgr->min_entities_per_cell = min_entities_per_cell;
    mgr->aoi_radius = cell_width * 0.5f;  /* AOI 半径 = 半个 Cell 宽度 */

    /* 计算 Cell 网格 */
    mgr->cells_per_row = (int)ceilf(world_width / cell_width);
    mgr->cells_per_col = (int)ceilf(world_height / cell_height);
    mgr->cell_capacity = mgr->cells_per_row * mgr->cells_per_col;
    mgr->cell_count    = mgr->cell_capacity;

    mgr->cells = (CeCell*)calloc(mgr->cell_capacity, sizeof(CeCell));
    if (!mgr->cells) {
        free(mgr);
        return NULL;
    }

    /* 初始化每个 Cell */
    for (int i = 0; i < mgr->cell_capacity; i++) {
        int row = i / mgr->cells_per_row;
        int col = i % mgr->cells_per_row;

        CeCell* cell = &mgr->cells[i];
        cell->id = mgr->next_cell_id++;
        cell->bounds.min_x = mgr->world_min_x + col * cell_width;
        cell->bounds.min_y = mgr->world_min_y + row * cell_height;
        cell->bounds.max_x = cell->bounds.min_x + cell_width;
        cell->bounds.max_y = cell->bounds.min_y + cell_height;
        cell->state        = CE_CELL_ACTIVE;
        cell->max_entities = max_entities_per_cell;
        cell->min_entities = min_entities_per_cell;
        cell->process_id   = -1;
        cell->entity_count = 0;

        mgr_build_neighbors(mgr, i);
    }

    mgr->initialized = CE_TRUE;

    CE_LOG_INFO("CELL", "Mgr initialized: %dx%d grid, cell=%.0fx%.0f, max_entities=%d",
                mgr->cells_per_row, mgr->cells_per_col,
                cell_width, cell_height, max_entities_per_cell);

    return mgr;
}

void ce_cell_mgr_destroy(CeCellManager* mgr) {
    if (!mgr) return;

    free(mgr->cells);
    memset(mgr, 0, sizeof(CeCellManager));
    free(mgr);

    if (g_default_cell_mgr == mgr) {
        g_default_cell_mgr = NULL;
    }
}

/* ---- Cell 操作 ---- */

CeCellId ce_cell_mgr_find_by_position(CeCellManager* mgr, float x, float y) {
    if (!mgr || !mgr->initialized) return CE_INVALID_CELL_ID;

    int idx = mgr_pos_to_cell_index(mgr, x, y);
    if (idx < 0 || idx >= mgr->cell_capacity) return CE_INVALID_CELL_ID;

    return mgr->cells[idx].id;
}

const CeCell* ce_cell_mgr_get(CeCellManager* mgr, CeCellId cell_id) {
    return mgr_get_cell(mgr, cell_id);
}

int ce_cell_mgr_count(CeCellManager* mgr) {
    return mgr ? mgr->cell_count : 0;
}

/* ---- 实体管理 ---- */

CeResult ce_cell_mgr_enter_entity(CeCellManager* mgr,
                                   CeServerEntityId entity_id,
                                   float x, float y, float radius) {
    if (!mgr || !mgr->initialized) return CE_ERR;

    int idx = mgr_pos_to_cell_index(mgr, x, y);
    if (idx < 0) return CE_ERR;

    CeCell* cell = &mgr->cells[idx];
    cell->entity_count++;

    /* 添加到全局 AOI */
    return ce_aoi_enter(entity_id, x, y, radius);
}

void ce_cell_mgr_leave_entity(CeCellManager* mgr, CeServerEntityId entity_id) {
    if (!mgr || !mgr->initialized) return;

    /* 从 AOI 移除（AOI 内部知道实体位置，不需要我们提供 Cell 信息） */
    ce_aoi_leave(entity_id);

    /* 更新 Cell 计数（简化：遍历所有 Cell 找到包含该实体的） */
    /* 注：当前实现中，实体计数由 AOI 管理，Cell 计数为近似值 */
}

CeResult ce_cell_mgr_move_entity(CeCellManager* mgr,
                                  CeServerEntityId entity_id,
                                  float new_x, float new_y) {
    if (!mgr || !mgr->initialized) return CE_ERR;

    /* 检查是否需要跨 Cell 迁移 */
    int new_idx = mgr_pos_to_cell_index(mgr, new_x, new_y);
    if (new_idx < 0) return CE_ERR;

    /* AOI 处理移动（自动检测进出视野事件） */
    CeResult result = ce_aoi_move(entity_id, new_x, new_y);
    if (result != CE_OK) return result;

    /* 更新 Cell 负载 */
    CeCell* new_cell = &mgr->cells[new_idx];
    new_cell->entity_count++;

    return CE_OK;
}

/* ---- 动态管理 ---- */

void ce_cell_mgr_update(CeCellManager* mgr) {
    if (!mgr || !mgr->initialized) return;

    /* 检查每个 Cell 是否需要分裂或合并 */
    for (int i = 0; i < mgr->cell_capacity; i++) {
        CeCell* cell = &mgr->cells[i];
        if (cell->state != CE_CELL_ACTIVE) continue;

        if (cell->entity_count > cell->max_entities) {
            CE_LOG_INFO("CELL", "Cell %u overloaded (%d > %d), marking for split",
                        cell->id, cell->entity_count, cell->max_entities);
            cell->state = CE_CELL_SPLITTING;
        }
    }

    /* 检查相邻 Cell 合并 */
    for (int i = 0; i < mgr->cell_capacity; i++) {
        CeCell* cell = &mgr->cells[i];
        if (cell->state != CE_CELL_ACTIVE) continue;

        for (int j = 0; j < cell->neighbor_count; j++) {
            CeCellId nid = cell->neighbors[j];
            const CeCell* neighbor = mgr_get_cell(mgr, nid);
            if (!neighbor || neighbor->state != CE_CELL_ACTIVE) continue;

            int total = cell->entity_count + neighbor->entity_count;
            if (total < cell->min_entities && cell->id < nid) {
                CE_LOG_INFO("CELL", "Cells %u+%u underloaded (%d < %d), marking for merge",
                            cell->id, nid, total, cell->min_entities);
                /* 标记两个 Cell 待合并 */
            }
        }
    }
}

CeResult ce_cell_mgr_split(CeCellManager* mgr, CeCellId cell_id) {
    if (!mgr || !mgr->initialized) return CE_ERR;

    /* 找到 Cell */
    int cell_idx = -1;
    for (int i = 0; i < mgr->cell_capacity; i++) {
        if (mgr->cells[i].id == cell_id) { cell_idx = i; break; }
    }
    if (cell_idx < 0) return CE_ERR;

    CeCell* cell = &mgr->cells[cell_idx];

    /* 四等分边界 */
    float mid_x = (cell->bounds.min_x + cell->bounds.max_x) * 0.5f;
    float mid_y = (cell->bounds.min_y + cell->bounds.max_y) * 0.5f;

    /* 创建 4 个子 Cell（复用当前 Cell 槽位 + 3 个新槽位） */
    CeCellBounds sub_bounds[4] = {
        {cell->bounds.min_x, cell->bounds.min_y, mid_x, mid_y},           /* 左下 */
        {mid_x,              cell->bounds.min_y, cell->bounds.max_x, mid_y}, /* 右下 */
        {cell->bounds.min_x, mid_y,              mid_x, cell->bounds.max_y}, /* 左上 */
        {mid_x,              mid_y,              cell->bounds.max_x, cell->bounds.max_y} /* 右上 */
    };

    /* 扩展容量 */
    int new_cells_needed = 3;
    if (mgr->cell_capacity - mgr->cell_count < new_cells_needed) {
        int new_cap = mgr->cell_capacity * 2;
        CeCell* new_cells = (CeCell*)realloc(mgr->cells, sizeof(CeCell) * new_cap);
        if (!new_cells) return CE_ERR;
        memset(new_cells + mgr->cell_capacity, 0,
               sizeof(CeCell) * (new_cap - mgr->cell_capacity));
        mgr->cells = new_cells;
        mgr->cell_capacity = new_cap;
    }

    /* 原 Cell 变为第一个子 Cell */
    cell->bounds = sub_bounds[0];
    cell->entity_count = 0;
    cell->state = CE_CELL_ACTIVE;

    /* 创建 3 个新 Cell */
    for (int i = 1; i < 4; i++) {
        CeCell* sub = &mgr->cells[mgr->cell_count++];
        sub->id            = mgr->next_cell_id++;
        sub->bounds        = sub_bounds[i];
        sub->state         = CE_CELL_ACTIVE;
        sub->max_entities  = cell->max_entities;
        sub->min_entities  = cell->min_entities;
        sub->process_id    = -1;
        sub->entity_count  = 0;
    }

    /* 重建邻居关系 */
    for (int i = 0; i < mgr->cell_count; i++) {
        mgr_build_neighbors(mgr, i);
    }

    CE_LOG_INFO("CELL", "Cell %u split into 4 sub-cells", cell_id);
    return CE_OK;
}

CeResult ce_cell_mgr_merge(CeCellManager* mgr, CeCellId cell_a, CeCellId cell_b) {
    if (!mgr || !mgr->initialized) return CE_ERR;

    /* 找到两个 Cell */
    int idx_a = -1, idx_b = -1;
    for (int i = 0; i < mgr->cell_capacity; i++) {
        if (mgr->cells[i].id == cell_a) idx_a = i;
        if (mgr->cells[i].id == cell_b) idx_b = i;
    }
    if (idx_a < 0 || idx_b < 0) return CE_ERR;

    CeCell* ca = &mgr->cells[idx_a];
    CeCell* cb = &mgr->cells[idx_b];

    /* 合并边界 */
    if (ca->bounds.min_x > cb->bounds.min_x) ca->bounds.min_x = cb->bounds.min_x;
    if (ca->bounds.min_y > cb->bounds.min_y) ca->bounds.min_y = cb->bounds.min_y;
    if (ca->bounds.max_x < cb->bounds.max_x) ca->bounds.max_x = cb->bounds.max_x;
    if (ca->bounds.max_y < cb->bounds.max_y) ca->bounds.max_y = cb->bounds.max_y;

    ca->entity_count += cb->entity_count;
    ca->state = CE_CELL_ACTIVE;

    /* 释放 Cell B */
    memset(cb, 0, sizeof(CeCell));
    cb->id = CE_INVALID_CELL_ID;

    /* 重建邻居 */
    for (int i = 0; i < mgr->cell_count; i++) {
        if (mgr->cells[i].id != CE_INVALID_CELL_ID) {
            mgr_build_neighbors(mgr, i);
        }
    }

    CE_LOG_INFO("CELL", "Cells %u + %u merged", cell_a, cell_b);
    return CE_OK;
}

/* ---- 进程分配 ---- */

void ce_cell_mgr_assign_process(CeCellManager* mgr, CeCellId cell_id, int process_id) {
    if (!mgr) return;
    for (int i = 0; i < mgr->cell_capacity; i++) {
        if (mgr->cells[i].id == cell_id) {
            mgr->cells[i].process_id = process_id;
            return;
        }
    }
}

int ce_cell_mgr_get_process(CeCellManager* mgr, CeCellId cell_id) {
    const CeCell* cell = mgr_get_cell(mgr, cell_id);
    return cell ? cell->process_id : -1;
}

/* ---- 跨 Cell 查询 ---- */

int ce_cell_mgr_query_nearby(CeCellManager* mgr,
                              CeServerEntityId entity_id,
                              CeServerEntityId* buffer, int max_count) {
    if (!mgr || !mgr->initialized) return 0;

    /* 委托给全局 AOI */
    return ce_aoi_query_nearby(entity_id, buffer, max_count);
}

/* ---- 调试 ---- */

void ce_cell_mgr_debug_print(CeCellManager* mgr) {
    if (!mgr || !mgr->initialized) {
        printf("[CELL] Not initialized\n");
        return;
    }

    printf("[CELL] Grid: %dx%d, Cell size: %.0fx%.0f, World: %.0fx%.0f\n",
           mgr->cells_per_row, mgr->cells_per_col,
           mgr->cell_width, mgr->cell_height,
           mgr->world_max_x, mgr->world_max_y);

    for (int i = 0; i < mgr->cell_count; i++) {
        CeCell* cell = &mgr->cells[i];
        if (cell->id == CE_INVALID_CELL_ID) continue;
        printf("  Cell %u: bounds=[%.0f,%.0f]-[%.0f,%.0f] entities=%d state=%d proc=%d\n",
               cell->id,
               cell->bounds.min_x, cell->bounds.min_y,
               cell->bounds.max_x, cell->bounds.max_y,
               cell->entity_count, cell->state, cell->process_id);
    }
}
