/*
 * ChaosEngine AOI Grid - 实现
 * 纯 C99，单线程安全
 *
 * 空间网格索引：
 *   - 世界划分为 cols×rows 的二维 cell 数组
 *   - 每个 cell 维护一个实体链表
 *   - entity_id -> 实体节点 的哈希表实现 O(1) 查找
 *   - enter/move/leave: O(1)（哈希查找 + 链表头插/删）
 *   - query: O(k)，k = 覆盖 cell 数 × cell 内实体数
 */

#include "server/ce_aoi.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- 哈希表参数 ---- */

/** 哈希表初始桶数（2 的幂） */
#define CE_GRID_HASH_INIT_BITS  10
#define CE_GRID_HASH_INIT_SIZE  (1u << CE_GRID_HASH_INIT_BITS)

/** 哈希表负载因子阈值，超过则扩容 */
#define CE_GRID_HASH_LOAD_FACTOR  0.75f

/* ---- 实体节点 ---- */

typedef struct CeGridEntity {
    uint32_t              entity_id;
    float                 x;
    float                 y;
    int                   cell_x;   /* 所在 cell 列索引 */
    int                   cell_y;   /* 所在 cell 行索引 */
    struct CeGridEntity*  cell_prev;  /* 同一 cell 链表中的前驱 */
    struct CeGridEntity*  cell_next;  /* 同一 cell 链表中的后继 */
    struct CeGridEntity*  hash_next;  /* 哈希桶链表中的下一个 */
} CeGridEntity;

/* ---- Cell ---- */

typedef struct CeGridCell {
    CeGridEntity* head;  /* 该 cell 中的实体链表头 */
} CeGridCell;

/* ---- Grid AOI 结构 ---- */

struct CeAoiGrid {
    float          world_w;
    float          world_h;
    float          cell_w;
    float          cell_h;
    float          default_radius;

    int            cols;    /* 列数 = ceil(world_w / cell_w) */
    int            rows;    /* 行数 = ceil(world_h / cell_h) */
    CeGridCell*    cells;   /* cols×rows 二维数组（一维展开） */

    /* 哈希表: entity_id -> CeGridEntity* */
    CeGridEntity** hash_buckets;
    int            hash_cap;     /* 桶数 */
    int            hash_count;   /* 已存储实体数 */

    int            entity_count; /* 实体总数（== hash_count） */
};

/* ---- 辅助函数 ---- */

static int grid_index(CeAoiGrid* g, int cx, int cy) {
    return cy * g->cols + cx;
}

static void world_to_cell(CeAoiGrid* g, float x, float y,
                           int* out_cx, int* out_cy) {
    int cx = (int)(x / g->cell_w);
    int cy = (int)(y / g->cell_h);
    if (cx < 0) cx = 0;
    if (cx >= g->cols) cx = g->cols - 1;
    if (cy < 0) cy = 0;
    if (cy >= g->rows) cy = g->rows - 1;
    *out_cx = cx;
    *out_cy = cy;
}

static float dist_sq(float x1, float y1, float x2, float y2) {
    float dx = x1 - x2;
    float dy = y1 - y2;
    return dx * dx + dy * dy;
}

/* ---- 哈希表操作 ---- */

static uint32_t hash_u32(uint32_t key) {
    /* Knuth 乘法哈希 */
    return key * 2654435761u;
}

static int hash_bucket(CeAoiGrid* g, uint32_t entity_id) {
    return (int)(hash_u32(entity_id) & (g->hash_cap - 1));
}

static CeGridEntity* hash_find(CeAoiGrid* g, uint32_t entity_id) {
    int idx = hash_bucket(g, entity_id);
    CeGridEntity* e = g->hash_buckets[idx];
    while (e) {
        if (e->entity_id == entity_id) return e;
        e = e->hash_next;
    }
    return NULL;
}

static void hash_insert(CeAoiGrid* g, CeGridEntity* e) {
    int idx = hash_bucket(g, e->entity_id);
    e->hash_next = g->hash_buckets[idx];
    g->hash_buckets[idx] = e;
    g->hash_count++;
}

static void hash_remove(CeAoiGrid* g, uint32_t entity_id) {
    int idx = hash_bucket(g, entity_id);
    CeGridEntity* prev = NULL;
    CeGridEntity* cur = g->hash_buckets[idx];
    while (cur) {
        if (cur->entity_id == entity_id) {
            if (prev) {
                prev->hash_next = cur->hash_next;
            } else {
                g->hash_buckets[idx] = cur->hash_next;
            }
            g->hash_count--;
            return;
        }
        prev = cur;
        cur = cur->hash_next;
    }
}

static void hash_rehash(CeAoiGrid* g) {
    int new_cap = g->hash_cap * 2;
    CeGridEntity** new_buckets = (CeGridEntity**)calloc(
        (size_t)new_cap, sizeof(CeGridEntity*));
    if (!new_buckets) return;

    CeGridEntity** old_buckets = g->hash_buckets;
    int old_cap = g->hash_cap;

    g->hash_buckets = new_buckets;
    g->hash_cap = new_cap;

    for (int i = 0; i < old_cap; i++) {
        CeGridEntity* e = old_buckets[i];
        while (e) {
            CeGridEntity* next = e->hash_next;
            int new_idx = (int)(hash_u32(e->entity_id) & (new_cap - 1));
            e->hash_next = new_buckets[new_idx];
            new_buckets[new_idx] = e;
            e = next;
        }
    }
    free(old_buckets);
}

/* ---- Cell 链表操作 ---- */

static void cell_insert(CeGridCell* cell, CeGridEntity* e) {
    e->cell_prev = NULL;
    e->cell_next = cell->head;
    if (cell->head) {
        cell->head->cell_prev = e;
    }
    cell->head = e;
}

static void cell_remove(CeGridCell* cell, CeGridEntity* e) {
    if (e->cell_prev) {
        e->cell_prev->cell_next = e->cell_next;
    } else {
        cell->head = e->cell_next;
    }
    if (e->cell_next) {
        e->cell_next->cell_prev = e->cell_prev;
    }
    e->cell_prev = NULL;
    e->cell_next = NULL;
}

/* ---- 公共 API ---- */

CeAoiGrid* ce_aoi_grid_create(float world_w, float world_h,
                               float cell_w, float cell_h, float radius) {
    if (world_w <= 0.0f || world_h <= 0.0f ||
        cell_w <= 0.0f || cell_h <= 0.0f) {
        CE_LOG_ERROR("AOI_GRID", "create: invalid dimensions "
                     "(w=%.1f h=%.1f cw=%.1f ch=%.1f)",
                     world_w, world_h, cell_w, cell_h);
        return NULL;
    }

    CeAoiGrid* g = (CeAoiGrid*)calloc(1, sizeof(CeAoiGrid));
    if (!g) return NULL;

    g->world_w = world_w;
    g->world_h = world_h;
    g->cell_w = cell_w;
    g->cell_h = cell_h;
    g->default_radius = radius;

    g->cols = (int)ceilf(world_w / cell_w);
    if (g->cols < 1) g->cols = 1;
    g->rows = (int)ceilf(world_h / cell_h);
    if (g->rows < 1) g->rows = 1;

    g->cells = (CeGridCell*)calloc(
        (size_t)g->cols * g->rows, sizeof(CeGridCell));
    if (!g->cells) {
        free(g);
        return NULL;
    }

    g->hash_cap = CE_GRID_HASH_INIT_SIZE;
    g->hash_count = 0;
    g->hash_buckets = (CeGridEntity**)calloc(
        (size_t)g->hash_cap, sizeof(CeGridEntity*));
    if (!g->hash_buckets) {
        free(g->cells);
        free(g);
        return NULL;
    }

    g->entity_count = 0;

    CE_LOG_INFO("AOI_GRID", "created: world=%.0fx%.0f cell=%.0fx%.0f "
                "grid=%dx%d radius=%.1f",
                world_w, world_h, cell_w, cell_h,
                g->cols, g->rows, radius);

    return g;
}

void ce_aoi_grid_destroy(CeAoiGrid* grid) {
    if (!grid) return;

    /* 释放所有实体节点 */
    for (int i = 0; i < grid->hash_cap; i++) {
        CeGridEntity* e = grid->hash_buckets[i];
        while (e) {
            CeGridEntity* next = e->hash_next;
            free(e);
            e = next;
        }
    }

    free(grid->hash_buckets);
    free(grid->cells);
    free(grid);

    CE_LOG_INFO("AOI_GRID", "destroyed");
}

CeResult ce_aoi_grid_enter(CeAoiGrid* grid, uint32_t entity_id,
                            float x, float y, float z) {
    (void)z;  /* 2D 实现，忽略 z */

    if (!grid) return CE_ERR;

    /* 坐标越界检查 */
    if (x < 0.0f || x >= grid->world_w ||
        y < 0.0f || y >= grid->world_h) {
        CE_LOG_WARN("AOI_GRID", "enter: entity %u out of bounds (%.1f, %.1f)",
                    entity_id, x, y);
        return CE_ERR;
    }

    /* 不允许重复进入 */
    if (hash_find(grid, entity_id)) {
        CE_LOG_WARN("AOI_GRID", "enter: entity %u already exists", entity_id);
        return CE_ERR;
    }

    /* 创建节点 */
    CeGridEntity* e = (CeGridEntity*)calloc(1, sizeof(CeGridEntity));
    if (!e) return CE_ERR;

    e->entity_id = entity_id;
    e->x = x;
    e->y = y;

    /* 计算所属 cell */
    world_to_cell(grid, x, y, &e->cell_x, &e->cell_y);

    /* 插入 cell 链表 */
    CeGridCell* cell = &grid->cells[grid_index(grid, e->cell_x, e->cell_y)];
    cell_insert(cell, e);

    /* 插入哈希表 */
    hash_insert(grid, e);

    /* 检查是否需要扩容 */
    if ((float)grid->hash_count / grid->hash_cap > CE_GRID_HASH_LOAD_FACTOR) {
        hash_rehash(grid);
    }

    grid->entity_count++;

    return CE_OK;
}

CeResult ce_aoi_grid_move(CeAoiGrid* grid, uint32_t entity_id,
                           float x, float y, float z) {
    (void)z;

    if (!grid) return CE_ERR;

    /* 坐标越界检查 */
    if (x < 0.0f || x >= grid->world_w ||
        y < 0.0f || y >= grid->world_h) {
        CE_LOG_WARN("AOI_GRID", "move: entity %u out of bounds (%.1f, %.1f)",
                    entity_id, x, y);
        return CE_ERR;
    }

    CeGridEntity* e = hash_find(grid, entity_id);
    if (!e) {
        CE_LOG_WARN("AOI_GRID", "move: entity %u not found", entity_id);
        return CE_ERR;
    }

    /* 计算新 cell */
    int new_cx, new_cy;
    world_to_cell(grid, x, y, &new_cx, &new_cy);

    /* 更新坐标 */
    e->x = x;
    e->y = y;

    /* 如果 cell 改变了，迁移 */
    if (new_cx != e->cell_x || new_cy != e->cell_y) {
        CeGridCell* old_cell = &grid->cells[grid_index(grid, e->cell_x, e->cell_y)];
        cell_remove(old_cell, e);

        e->cell_x = new_cx;
        e->cell_y = new_cy;

        CeGridCell* new_cell = &grid->cells[grid_index(grid, new_cx, new_cy)];
        cell_insert(new_cell, e);
    }

    return CE_OK;
}

CeResult ce_aoi_grid_leave(CeAoiGrid* grid, uint32_t entity_id) {
    if (!grid) return CE_ERR;

    CeGridEntity* e = hash_find(grid, entity_id);
    if (!e) {
        CE_LOG_WARN("AOI_GRID", "leave: entity %u not found", entity_id);
        return CE_ERR;
    }

    /* 从 cell 链表移除 */
    CeGridCell* cell = &grid->cells[grid_index(grid, e->cell_x, e->cell_y)];
    cell_remove(cell, e);

    /* 从哈希表移除 */
    hash_remove(grid, entity_id);

    /* 释放节点 */
    free(e);

    grid->entity_count--;

    return CE_OK;
}

int ce_aoi_grid_query(CeAoiGrid* grid, float x, float y, float z,
                       float radius, uint32_t* out_ids, int max_count) {
    (void)z;

    if (!grid || !out_ids || max_count <= 0) return 0;
    if (radius <= 0.0f) radius = grid->default_radius;
    if (radius <= 0.0f) return 0;

    /* 计算查询覆盖的 cell 范围 */
    int min_cx, min_cy, max_cx, max_cy;
    world_to_cell(grid, x - radius, y - radius, &min_cx, &min_cy);
    world_to_cell(grid, x + radius, y + radius, &max_cx, &max_cy);

    float radius_sq = radius * radius;
    int count = 0;

    for (int cy = min_cy; cy <= max_cy; cy++) {
        for (int cx = min_cx; cx <= max_cx; cx++) {
            CeGridCell* cell = &grid->cells[grid_index(grid, cx, cy)];
            CeGridEntity* e = cell->head;
            while (e) {
                if (dist_sq(x, y, e->x, e->y) <= radius_sq) {
                    if (count < max_count) {
                        out_ids[count++] = e->entity_id;
                    }
                }
                e = e->cell_next;
            }
        }
    }

    return count;
}

int ce_aoi_grid_entity_count(CeAoiGrid* grid) {
    if (!grid) return 0;
    return grid->entity_count;
}
