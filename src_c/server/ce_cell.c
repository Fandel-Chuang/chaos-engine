/*
 * ChaosEngine Cell 大地图管理 — 实现
 * 纯 C99，单线程安全
 *
 * 将大地图划分为多个 Cell，每个 Cell 内嵌独立 AOI。
 * Cell 根据负载（实体数量）动态分裂和合并。
 */

#include "server/ce_cell.h"
#include "server/ce_aoi.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---- Cell 管理器 ---- */

static struct {
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
} g_cell;

/* ---- 辅助函数 ---- */

static int pos_to_cell_index(float x, float y) {
    if (x < g_cell.world_min_x || x >= g_cell.world_max_x ||
        y < g_cell.world_min_y || y >= g_cell.world_max_y) {
        return -1;
    }

    int col = (int)((x - g_cell.world_min_x) / g_cell.cell_width);
    int row = (int)((y - g_cell.world_min_y) / g_cell.cell_height);

    /* 边界裁剪 */
    if (col >= g_cell.cells_per_row) col = g_cell.cells_per_row - 1;
    if (row >= g_cell.cells_per_col) row = g_cell.cells_per_col - 1;
    if (col < 0) col = 0;
    if (row < 0) row = 0;

    return row * g_cell.cells_per_row + col;
}

static void build_neighbors(int cell_index) {
    CeCell* cell = &g_cell.cells[cell_index];
    cell->neighbor_count = 0;

    int row = cell_index / g_cell.cells_per_row;
    int col = cell_index % g_cell.cells_per_row;

    /* 8 个方向 */
    static const int dr[] = {-1, -1, -1,  0, 0,  1, 1, 1};
    static const int dc[] = {-1,  0,  1, -1, 1, -1, 0, 1};

    for (int i = 0; i < 8; i++) {
        int nr = row + dr[i];
        int nc = col + dc[i];
        if (nr >= 0 && nr < g_cell.cells_per_col &&
            nc >= 0 && nc < g_cell.cells_per_row) {
            int ni = nr * g_cell.cells_per_row + nc;
            cell->neighbors[cell->neighbor_count++] = g_cell.cells[ni].id;
        }
    }
}

/* ---- 公共 API ---- */

CeResult ce_cell_init(float world_width, float world_height,
                       float cell_width, float cell_height,
                       int max_entities_per_cell, int min_entities_per_cell) {
    if (g_cell.initialized) return CE_OK;

    memset(&g_cell, 0, sizeof(g_cell));

    g_cell.cell_width  = cell_width;
    g_cell.cell_height = cell_height;
    g_cell.world_min_x = 0.0f;
    g_cell.world_min_y = 0.0f;
    g_cell.world_max_x = world_width;
    g_cell.world_max_y = world_height;
    g_cell.max_entities_per_cell = max_entities_per_cell;
    g_cell.min_entities_per_cell = min_entities_per_cell;
    g_cell.aoi_radius = cell_width * 0.5f;  /* AOI 半径 = 半个 Cell 宽度 */

    /* 计算 Cell 网格 */
    g_cell.cells_per_row = (int)ceilf(world_width / cell_width);
    g_cell.cells_per_col = (int)ceilf(world_height / cell_height);
    g_cell.cell_capacity = g_cell.cells_per_row * g_cell.cells_per_col;
    g_cell.cell_count    = g_cell.cell_capacity;

    g_cell.cells = (CeCell*)calloc(g_cell.cell_capacity, sizeof(CeCell));
    if (!g_cell.cells) return CE_ERR;

    /* 初始化每个 Cell */
    for (int i = 0; i < g_cell.cell_capacity; i++) {
        int row = i / g_cell.cells_per_row;
        int col = i % g_cell.cells_per_row;

        CeCell* cell = &g_cell.cells[i];
        cell->id = g_cell.next_cell_id++;
        cell->bounds.min_x = g_cell.world_min_x + col * cell_width;
        cell->bounds.min_y = g_cell.world_min_y + row * cell_height;
        cell->bounds.max_x = cell->bounds.min_x + cell_width;
        cell->bounds.max_y = cell->bounds.min_y + cell_height;
        cell->state        = CE_CELL_ACTIVE;
        cell->max_entities = max_entities_per_cell;
        cell->min_entities = min_entities_per_cell;
        cell->process_id   = -1;
        cell->entity_count = 0;

        build_neighbors(i);
    }

    /* 初始化全局 AOI */
    ce_aoi_init(g_cell.aoi_radius, NULL, NULL);

    g_cell.initialized = CE_TRUE;
    CE_LOG_INFO("CELL", "Initialized: %dx%d grid, cell=%.0fx%.0f, max_entities=%d",
                g_cell.cells_per_row, g_cell.cells_per_col,
                cell_width, cell_height, max_entities_per_cell);
    return CE_OK;
}

void ce_cell_shutdown(void) {
    if (!g_cell.initialized) return;

    ce_aoi_shutdown();
    free(g_cell.cells);
    memset(&g_cell, 0, sizeof(g_cell));
    CE_LOG_INFO("CELL", "Shut down");
}

CeCellId ce_cell_find_by_position(float x, float y) {
    if (!g_cell.initialized) return CE_INVALID_CELL_ID;

    int idx = pos_to_cell_index(x, y);
    if (idx < 0 || idx >= g_cell.cell_capacity) return CE_INVALID_CELL_ID;

    return g_cell.cells[idx].id;
}

const CeCell* ce_cell_get(CeCellId cell_id) {
    if (!g_cell.initialized) return NULL;

    for (int i = 0; i < g_cell.cell_capacity; i++) {
        if (g_cell.cells[i].id == cell_id) return &g_cell.cells[i];
    }
    return NULL;
}

int ce_cell_count(void) {
    return g_cell.cell_count;
}

CeResult ce_cell_enter_entity(CeServerEntityId entity_id, float x, float y, float radius) {
    if (!g_cell.initialized) return CE_ERR;

    int idx = pos_to_cell_index(x, y);
    if (idx < 0) return CE_ERR;

    CeCell* cell = &g_cell.cells[idx];
    cell->entity_count++;

    /* 添加到全局 AOI */
    return ce_aoi_enter(entity_id, x, y, radius);
}

void ce_cell_leave_entity(CeServerEntityId entity_id) {
    if (!g_cell.initialized) return;

    /* 从 AOI 移除（AOI 内部知道实体位置，不需要我们提供 Cell 信息） */
    ce_aoi_leave(entity_id);

    /* 更新 Cell 计数（简化：遍历所有 Cell 找到包含该实体的） */
    /* 注：当前实现中，实体计数由 AOI 管理，Cell 计数为近似值 */
}

CeResult ce_cell_move_entity(CeServerEntityId entity_id, float new_x, float new_y) {
    if (!g_cell.initialized) return CE_ERR;

    /* 检查是否需要跨 Cell 迁移 */
    int new_idx = pos_to_cell_index(new_x, new_y);
    if (new_idx < 0) return CE_ERR;

    /* AOI 处理移动（自动检测进出视野事件） */
    CeResult result = ce_aoi_move(entity_id, new_x, new_y);
    if (result != CE_OK) return result;

    /* 更新 Cell 负载 */
    CeCell* new_cell = &g_cell.cells[new_idx];
    new_cell->entity_count++;

    return CE_OK;
}

void ce_cell_update(void) {
    if (!g_cell.initialized) return;

    /* 检查每个 Cell 是否需要分裂或合并 */
    for (int i = 0; i < g_cell.cell_capacity; i++) {
        CeCell* cell = &g_cell.cells[i];
        if (cell->state != CE_CELL_ACTIVE) continue;

        if (cell->entity_count > cell->max_entities) {
            CE_LOG_INFO("CELL", "Cell %u overloaded (%d > %d), marking for split",
                        cell->id, cell->entity_count, cell->max_entities);
            cell->state = CE_CELL_SPLITTING;
        }
    }

    /* 检查相邻 Cell 合并 */
    for (int i = 0; i < g_cell.cell_capacity; i++) {
        CeCell* cell = &g_cell.cells[i];
        if (cell->state != CE_CELL_ACTIVE) continue;

        for (int j = 0; j < cell->neighbor_count; j++) {
            CeCellId nid = cell->neighbors[j];
            const CeCell* neighbor = ce_cell_get(nid);
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

CeResult ce_cell_split(CeCellId cell_id) {
    if (!g_cell.initialized) return CE_ERR;

    /* 找到 Cell */
    int cell_idx = -1;
    for (int i = 0; i < g_cell.cell_capacity; i++) {
        if (g_cell.cells[i].id == cell_id) { cell_idx = i; break; }
    }
    if (cell_idx < 0) return CE_ERR;

    CeCell* cell = &g_cell.cells[cell_idx];

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
    if (g_cell.cell_capacity - g_cell.cell_count < new_cells_needed) {
        int new_cap = g_cell.cell_capacity * 2;
        CeCell* new_cells = (CeCell*)realloc(g_cell.cells, sizeof(CeCell) * new_cap);
        if (!new_cells) return CE_ERR;
        memset(new_cells + g_cell.cell_capacity, 0,
               sizeof(CeCell) * (new_cap - g_cell.cell_capacity));
        g_cell.cells = new_cells;
        g_cell.cell_capacity = new_cap;
    }

    /* 原 Cell 变为第一个子 Cell */
    cell->bounds = sub_bounds[0];
    cell->entity_count = 0;
    cell->state = CE_CELL_ACTIVE;

    /* 创建 3 个新 Cell */
    for (int i = 1; i < 4; i++) {
        CeCell* sub = &g_cell.cells[g_cell.cell_count++];
        sub->id            = g_cell.next_cell_id++;
        sub->bounds        = sub_bounds[i];
        sub->state         = CE_CELL_ACTIVE;
        sub->max_entities  = cell->max_entities;
        sub->min_entities  = cell->min_entities;
        sub->process_id    = -1;
        sub->entity_count  = 0;
    }

    /* 重建邻居关系 */
    for (int i = 0; i < g_cell.cell_count; i++) {
        build_neighbors(i);
    }

    CE_LOG_INFO("CELL", "Cell %u split into 4 sub-cells", cell_id);
    return CE_OK;
}

CeResult ce_cell_merge(CeCellId cell_a, CeCellId cell_b) {
    if (!g_cell.initialized) return CE_ERR;

    /* 找到两个 Cell */
    int idx_a = -1, idx_b = -1;
    for (int i = 0; i < g_cell.cell_capacity; i++) {
        if (g_cell.cells[i].id == cell_a) idx_a = i;
        if (g_cell.cells[i].id == cell_b) idx_b = i;
    }
    if (idx_a < 0 || idx_b < 0) return CE_ERR;

    CeCell* ca = &g_cell.cells[idx_a];
    CeCell* cb = &g_cell.cells[idx_b];

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
    for (int i = 0; i < g_cell.cell_count; i++) {
        if (g_cell.cells[i].id != CE_INVALID_CELL_ID) {
            build_neighbors(i);
        }
    }

    CE_LOG_INFO("CELL", "Cells %u + %u merged", cell_a, cell_b);
    return CE_OK;
}

void ce_cell_assign_process(CeCellId cell_id, int process_id) {
    for (int i = 0; i < g_cell.cell_capacity; i++) {
        if (g_cell.cells[i].id == cell_id) {
            g_cell.cells[i].process_id = process_id;
            return;
        }
    }
}

int ce_cell_get_process(CeCellId cell_id) {
    const CeCell* cell = ce_cell_get(cell_id);
    return cell ? cell->process_id : -1;
}

int ce_cell_query_nearby(CeServerEntityId entity_id,
                          CeServerEntityId* buffer, int max_count) {
    if (!g_cell.initialized) return 0;

    /* 委托给全局 AOI */
    return ce_aoi_query_nearby(entity_id, buffer, max_count);
}

void ce_cell_debug_print(void) {
    if (!g_cell.initialized) {
        printf("[CELL] Not initialized\n");
        return;
    }

    printf("[CELL] Grid: %dx%d, Cell size: %.0fx%.0f, World: %.0fx%.0f\n",
           g_cell.cells_per_row, g_cell.cells_per_col,
           g_cell.cell_width, g_cell.cell_height,
           g_cell.world_max_x, g_cell.world_max_y);

    for (int i = 0; i < g_cell.cell_count; i++) {
        CeCell* cell = &g_cell.cells[i];
        if (cell->id == CE_INVALID_CELL_ID) continue;
        printf("  Cell %u: bounds=[%.0f,%.0f]-[%.0f,%.0f] entities=%d state=%d proc=%d\n",
               cell->id,
               cell->bounds.min_x, cell->bounds.min_y,
               cell->bounds.max_x, cell->bounds.max_y,
               cell->entity_count, cell->state, cell->process_id);
    }
}
