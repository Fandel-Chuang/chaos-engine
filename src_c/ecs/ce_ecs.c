/*
 * ChaosEngine ECS — 实体组件系统
 * 基于 Archetype（原型）模式，Cache-Friendly 数据布局
 */

#include "ecs/ce_ecs_internal.h"
#include "core/ce_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 全局 ECS 上下文 ---- */

static struct {
    CeAllocator* allocator;
    CeBool       initialized;

    /* 实体 */
    uint32_t     entity_count;
    uint32_t     entity_capacity;
    CeEntity*    entity_generations;   /* 第 i 个槽位的 generation */
    CeArchetype** entity_archetypes;   /* 第 i 个实体所在的 Archetype */
    uint32_t*    entity_rows;          /* 第 i 个实体在 Archetype 中的行 */
    uint32_t     free_entity_count;
    uint32_t*    free_entities;        /* 空闲实体索引栈 */

    /* 组件 */
    uint32_t     component_count;
    CeComponentInfo components[CE_MAX_COMPONENTS];

    /* Archetype */
    uint32_t     archetype_count;
    CeArchetype* archetypes[256];      /* 最多 256 个 Archetype */

    /* 系统 */
    uint32_t     system_count;
    CeSystemInfo systems[64];          /* 最多 64 个系统 */
} g_ecs;

/* ---- 初始化 ---- */

CeResult ce_ecs_init(CeAllocator* allocator) {
    if (g_ecs.initialized) return CE_OK;

    memset(&g_ecs, 0, sizeof(g_ecs));
    g_ecs.allocator = allocator;
    g_ecs.entity_capacity = 1024;

    g_ecs.entity_generations = ce_alloc_zero(allocator,
        sizeof(CeEntity) * g_ecs.entity_capacity);
    g_ecs.entity_archetypes = ce_alloc_zero(allocator,
        sizeof(CeArchetype*) * g_ecs.entity_capacity);
    g_ecs.entity_rows = ce_alloc_zero(allocator,
        sizeof(uint32_t) * g_ecs.entity_capacity);
    g_ecs.free_entities = ce_alloc_zero(allocator,
        sizeof(uint32_t) * g_ecs.entity_capacity);

    g_ecs.initialized = CE_TRUE;
    return CE_OK;
}

void ce_ecs_shutdown(void) {
    if (!g_ecs.initialized) return;

    /* 释放所有 Archetype */
    for (uint32_t i = 0; i < g_ecs.archetype_count; i++) {
        CeArchetype* arch = g_ecs.archetypes[i];
        ce_free(g_ecs.allocator, arch->entities);
        for (uint32_t c = 0; c < arch->component_count; c++) {
            ce_free(g_ecs.allocator, arch->component_data[c]);
        }
        ce_free(g_ecs.allocator, arch->component_ids);
        ce_free(g_ecs.allocator, arch->component_data);
        ce_free(g_ecs.allocator, arch);
    }

    ce_free(g_ecs.allocator, g_ecs.entity_generations);
    ce_free(g_ecs.allocator, g_ecs.entity_archetypes);
    ce_free(g_ecs.allocator, g_ecs.entity_rows);
    ce_free(g_ecs.allocator, g_ecs.free_entities);

    memset(&g_ecs, 0, sizeof(g_ecs));
}

/* ---- 组件注册 ---- */

CeComponentId ce_component_register(const char* name, size_t size, size_t align) {
    if (g_ecs.component_count >= CE_MAX_COMPONENTS) return (CeComponentId)-1;

    CeComponentId id = g_ecs.component_count++;
    CeComponentInfo* info = &g_ecs.components[id];
    strncpy(info->name, name, sizeof(info->name) - 1);
    info->size  = size;
    info->align = align;
    info->id    = id;

    return id;
}

CeComponentId ce_component_find(const char* name) {
    for (uint32_t i = 0; i < g_ecs.component_count; i++) {
        if (strcmp(g_ecs.components[i].name, name) == 0) {
            return g_ecs.components[i].id;
        }
    }
    return (CeComponentId)-1;
}

/* ---- Archetype 管理 ---- */

static int compare_component_ids(const void* a, const void* b) {
    return (int)(*(CeComponentId*)a - *(CeComponentId*)b);
}

static CeArchetype* find_or_create_archetype(CeComponentId* comp_ids, uint32_t count) {
    /* 排序组件 ID，确保唯一性 */
    CeComponentId sorted[CE_MAX_COMPONENTS];
    memcpy(sorted, comp_ids, sizeof(CeComponentId) * count);
    qsort(sorted, count, sizeof(CeComponentId), compare_component_ids);

    /* 查找已有 Archetype */
    for (uint32_t i = 0; i < g_ecs.archetype_count; i++) {
        CeArchetype* arch = g_ecs.archetypes[i];
        if (arch->component_count != count) continue;

        CeBool match = CE_TRUE;
        for (uint32_t c = 0; c < count; c++) {
            if (arch->component_ids[c] != sorted[c]) {
                match = CE_FALSE;
                break;
            }
        }
        if (match) return arch;
    }

    /* 创建新 Archetype */
    CeArchetype* arch = ce_alloc_zero(g_ecs.allocator, sizeof(CeArchetype));
    arch->component_count = count;
    arch->entity_capacity = 64;
    arch->entity_count    = 0;

    arch->component_ids = ce_alloc(g_ecs.allocator, sizeof(CeComponentId) * count);
    memcpy(arch->component_ids, sorted, sizeof(CeComponentId) * count);

    arch->component_data = ce_alloc_zero(g_ecs.allocator, sizeof(void*) * count);
    arch->entities = ce_alloc(g_ecs.allocator, sizeof(CeEntity) * arch->entity_capacity);

    for (uint32_t c = 0; c < count; c++) {
        CeComponentInfo* info = &g_ecs.components[sorted[c]];
        arch->component_data[c] = ce_alloc_zero(g_ecs.allocator,
            info->size * arch->entity_capacity);
    }

    g_ecs.archetypes[g_ecs.archetype_count++] = arch;
    return arch;
}

static void archetype_grow(CeArchetype* arch) {
    uint32_t new_cap = arch->entity_capacity * 2;

    arch->entities = ce_realloc(g_ecs.allocator, arch->entities,
        sizeof(CeEntity) * new_cap);

    for (uint32_t c = 0; c < arch->component_count; c++) {
        CeComponentInfo* info = &g_ecs.components[arch->component_ids[c]];
        void* new_data = ce_alloc_zero(g_ecs.allocator, info->size * new_cap);
        memcpy(new_data, arch->component_data[c], info->size * arch->entity_count);
        ce_free(g_ecs.allocator, arch->component_data[c]);
        arch->component_data[c] = new_data;
    }

    arch->entity_capacity = new_cap;
}

/* ---- 实体操作 ---- */

CeEntity ce_entity_create(void) {
    uint32_t index;

    if (g_ecs.free_entity_count > 0) {
        /* 复用已释放的槽位 */
        index = g_ecs.free_entities[--g_ecs.free_entity_count];
    } else {
        /* 扩容 */
        if (g_ecs.entity_count >= g_ecs.entity_capacity) {
            uint32_t new_cap = g_ecs.entity_capacity * 2;
            g_ecs.entity_generations = ce_realloc(g_ecs.allocator,
                g_ecs.entity_generations, sizeof(CeEntity) * new_cap);
            g_ecs.entity_archetypes = ce_realloc(g_ecs.allocator,
                g_ecs.entity_archetypes, sizeof(CeArchetype*) * new_cap);
            g_ecs.entity_rows = ce_realloc(g_ecs.allocator,
                g_ecs.entity_rows, sizeof(uint32_t) * new_cap);
            g_ecs.free_entities = ce_realloc(g_ecs.allocator,
                g_ecs.free_entities, sizeof(uint32_t) * new_cap);
            g_ecs.entity_capacity = new_cap;
        }
        index = g_ecs.entity_count++;
    }

    uint32_t gen = (uint32_t)(g_ecs.entity_generations[index] >> 32);
    g_ecs.entity_generations[index] = CE_ENTITY_MAKE(index, gen);
    g_ecs.entity_archetypes[index] = NULL;
    g_ecs.entity_rows[index] = 0;

    return CE_ENTITY_MAKE(index, gen);
}

void ce_entity_destroy(CeEntity entity) {
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (index >= g_ecs.entity_count) return;
    if (!ce_entity_is_alive(entity)) return;

    /* 从 Archetype 中移除 */
    CeArchetype* arch = g_ecs.entity_archetypes[index];
    if (arch) {
        uint32_t row = g_ecs.entity_rows[index];
        uint32_t last = arch->entity_count - 1;

        if (row != last) {
            /* 用最后一个实体覆盖当前位置 */
            CeEntity last_entity = arch->entities[last];
            arch->entities[row] = last_entity;
            g_ecs.entity_rows[CE_ENTITY_INDEX(last_entity)] = row;

            for (uint32_t c = 0; c < arch->component_count; c++) {
                CeComponentInfo* info = &g_ecs.components[arch->component_ids[c]];
                uint8_t* data = (uint8_t*)arch->component_data[c];
                memcpy(data + row * info->size,
                       data + last * info->size, info->size);
            }
        }
        arch->entity_count--;
    }

    /* 增加 generation，标记为无效 */
    uint32_t gen = (uint32_t)(g_ecs.entity_generations[index] >> 32) + 1;
    g_ecs.entity_generations[index] = CE_ENTITY_MAKE(index, gen);
    g_ecs.entity_archetypes[index] = NULL;

    /* 放入空闲列表 */
    g_ecs.free_entities[g_ecs.free_entity_count++] = index;
}

CeBool ce_entity_is_alive(CeEntity entity) {
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (index >= g_ecs.entity_count) return CE_FALSE;
    return g_ecs.entity_generations[index] == entity;
}

/* ---- 组件操作 ---- */

void* ce_entity_add_component(CeEntity entity, CeComponentId comp_id) {
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (!ce_entity_is_alive(entity)) return NULL;

    CeArchetype* old_arch = g_ecs.entity_archetypes[index];
    uint32_t old_row = g_ecs.entity_rows[index];

    /* 构建新的组件 ID 列表 */
    CeComponentId new_ids[CE_MAX_COMPONENTS];
    uint32_t new_count = 0;

    if (old_arch) {
        for (uint32_t c = 0; c < old_arch->component_count; c++) {
            if (old_arch->component_ids[c] == comp_id) {
                /* 已存在，直接返回 */
                CeComponentInfo* info = &g_ecs.components[comp_id];
                uint8_t* data = (uint8_t*)old_arch->component_data[c];
                return data + old_row * info->size;
            }
            new_ids[new_count++] = old_arch->component_ids[c];
        }
    }
    new_ids[new_count++] = comp_id;

    /* 找到或创建目标 Archetype */
    CeArchetype* new_arch = find_or_create_archetype(new_ids, new_count);
    if (new_arch->entity_count >= new_arch->entity_capacity) {
        archetype_grow(new_arch);
    }

    uint32_t new_row = new_arch->entity_count++;

    /* 复制旧组件数据 */
    if (old_arch) {
        for (uint32_t c = 0; c < old_arch->component_count; c++) {
            CeComponentId old_cid = old_arch->component_ids[c];
            CeComponentInfo* info = &g_ecs.components[old_cid];

            /* 在新 Archetype 中找到对应列 */
            for (uint32_t nc = 0; nc < new_count; nc++) {
                if (new_ids[nc] == old_cid) {
                    uint8_t* src = (uint8_t*)old_arch->component_data[c];
                    uint8_t* dst = (uint8_t*)new_arch->component_data[nc];
                    memcpy(dst + new_row * info->size,
                           src + old_row * info->size, info->size);
                    break;
                }
            }
        }

        /* 从旧 Archetype 移除 */
        uint32_t last = old_arch->entity_count - 1;
        if (old_row != last) {
            CeEntity last_entity = old_arch->entities[last];
            old_arch->entities[old_row] = last_entity;
            g_ecs.entity_rows[CE_ENTITY_INDEX(last_entity)] = old_row;

            for (uint32_t c = 0; c < old_arch->component_count; c++) {
                CeComponentInfo* info = &g_ecs.components[old_arch->component_ids[c]];
                uint8_t* data = (uint8_t*)old_arch->component_data[c];
                memcpy(data + old_row * info->size,
                       data + last * info->size, info->size);
            }
        }
        old_arch->entity_count--;
    }

    /* 更新实体映射 */
    new_arch->entities[new_row] = entity;
    g_ecs.entity_archetypes[index] = new_arch;
    g_ecs.entity_rows[index] = new_row;

    /* 返回新组件的指针 */
    for (uint32_t c = 0; c < new_count; c++) {
        if (new_ids[c] == comp_id) {
            CeComponentInfo* info = &g_ecs.components[comp_id];
            uint8_t* data = (uint8_t*)new_arch->component_data[c];
            return data + new_row * info->size;
        }
    }

    return NULL;
}

void ce_entity_remove_component(CeEntity entity, CeComponentId comp_id) {
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (!ce_entity_is_alive(entity)) return;

    CeArchetype* old_arch = g_ecs.entity_archetypes[index];
    if (!old_arch) return;

    uint32_t old_row = g_ecs.entity_rows[index];

    /* 构建新组件列表（排除要删除的） */
    CeComponentId new_ids[CE_MAX_COMPONENTS];
    uint32_t new_count = 0;
    CeBool found = CE_FALSE;

    for (uint32_t c = 0; c < old_arch->component_count; c++) {
        if (old_arch->component_ids[c] == comp_id) {
            found = CE_TRUE;
        } else {
            new_ids[new_count++] = old_arch->component_ids[c];
        }
    }

    if (!found) return;

    CeArchetype* new_arch;
    if (new_count == 0) {
        /* 实体没有组件了，直接移除 */
        uint32_t last = old_arch->entity_count - 1;
        if (old_row != last) {
            CeEntity last_entity = old_arch->entities[last];
            old_arch->entities[old_row] = last_entity;
            g_ecs.entity_rows[CE_ENTITY_INDEX(last_entity)] = old_row;

            for (uint32_t c = 0; c < old_arch->component_count; c++) {
                CeComponentInfo* info = &g_ecs.components[old_arch->component_ids[c]];
                uint8_t* data = (uint8_t*)old_arch->component_data[c];
                memcpy(data + old_row * info->size,
                       data + last * info->size, info->size);
            }
        }
        old_arch->entity_count--;
        g_ecs.entity_archetypes[index] = NULL;
        return;
    }

    new_arch = find_or_create_archetype(new_ids, new_count);
    if (new_arch->entity_count >= new_arch->entity_capacity) {
        archetype_grow(new_arch);
    }

    uint32_t new_row = new_arch->entity_count++;

    /* 复制保留的组件 */
    for (uint32_t c = 0; c < old_arch->component_count; c++) {
        CeComponentId old_cid = old_arch->component_ids[c];
        if (old_cid == comp_id) continue;

        CeComponentInfo* info = &g_ecs.components[old_cid];
        for (uint32_t nc = 0; nc < new_count; nc++) {
            if (new_ids[nc] == old_cid) {
                uint8_t* src = (uint8_t*)old_arch->component_data[c];
                uint8_t* dst = (uint8_t*)new_arch->component_data[nc];
                memcpy(dst + new_row * info->size,
                       src + old_row * info->size, info->size);
                break;
            }
        }
    }

    /* 从旧 Archetype 移除 */
    uint32_t last = old_arch->entity_count - 1;
    if (old_row != last) {
        CeEntity last_entity = old_arch->entities[last];
        old_arch->entities[old_row] = last_entity;
        g_ecs.entity_rows[CE_ENTITY_INDEX(last_entity)] = old_row;

        for (uint32_t c = 0; c < old_arch->component_count; c++) {
            CeComponentInfo* info = &g_ecs.components[old_arch->component_ids[c]];
            uint8_t* data = (uint8_t*)old_arch->component_data[c];
            memcpy(data + old_row * info->size,
                   data + last * info->size, info->size);
        }
    }
    old_arch->entity_count--;

    new_arch->entities[new_row] = entity;
    g_ecs.entity_archetypes[index] = new_arch;
    g_ecs.entity_rows[index] = new_row;
}

const void* ce_entity_get_component(CeEntity entity, CeComponentId comp_id) {
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (!ce_entity_is_alive(entity)) return NULL;

    CeArchetype* arch = g_ecs.entity_archetypes[index];
    if (!arch) return NULL;

    uint32_t row = g_ecs.entity_rows[index];

    for (uint32_t c = 0; c < arch->component_count; c++) {
        if (arch->component_ids[c] == comp_id) {
            uint8_t* data = (uint8_t*)arch->component_data[c];
            return data + row * g_ecs.components[comp_id].size;
        }
    }

    return NULL;
}

CeBool ce_entity_has_component(CeEntity entity, CeComponentId comp_id) {
    return ce_entity_get_component(entity, comp_id) != NULL;
}

CeResult ce_entity_edit_component(CeEntity entity, CeComponentId comp_id,
                                   CeComponentEditFn edit_fn, void* user_data) {
    /* 注意：这里通过 const 转换获取可写指针，但仅在 edit_fn 回调中使用 */
    /* 编辑器应通过此接口修改组件，而非直接操作内存 */
    const void* comp = ce_entity_get_component(entity, comp_id);
    if (!comp) return CE_ERR;

    edit_fn((void*)comp, user_data);
    return CE_OK;
}

/* ---- 系统 ---- */

CeResult ce_system_register(const char* name, CeSystemFn fn,
                             void* user_data, int priority) {
    if (g_ecs.system_count >= 64) return CE_ERR;

    CeSystemInfo* sys = &g_ecs.systems[g_ecs.system_count++];
    strncpy(sys->name, name, sizeof(sys->name) - 1);
    sys->fn        = fn;
    sys->user_data = user_data;
    sys->priority  = priority;
    sys->enabled   = CE_TRUE;

    /* 按优先级排序 */
    for (uint32_t i = g_ecs.system_count - 1; i > 0; i--) {
        if (g_ecs.systems[i].priority < g_ecs.systems[i - 1].priority) {
            CeSystemInfo tmp = g_ecs.systems[i];
            g_ecs.systems[i] = g_ecs.systems[i - 1];
            g_ecs.systems[i - 1] = tmp;
        }
    }

    return CE_OK;
}

void ce_system_unregister(const char* name) {
    for (uint32_t i = 0; i < g_ecs.system_count; i++) {
        if (strcmp(g_ecs.systems[i].name, name) == 0) {
            /* 用最后一个覆盖 */
            g_ecs.systems[i] = g_ecs.systems[--g_ecs.system_count];
            return;
        }
    }
}

void ce_ecs_update_systems(float delta_time) {
    for (uint32_t i = 0; i < g_ecs.system_count; i++) {
        if (g_ecs.systems[i].enabled) {
            g_ecs.systems[i].fn(delta_time, g_ecs.systems[i].user_data);
        }
    }
}

/* ---- 查询 ---- */

struct CeQuery {
    CeComponentId* component_ids;
    uint32_t       component_count;
    uint32_t       current_archetype;
    uint32_t       current_row;
};

CeQuery* ce_query_create(CeComponentId* component_ids, uint32_t count) {
    CeQuery* query = ce_alloc(g_ecs.allocator, sizeof(CeQuery));
    query->component_ids = ce_alloc(g_ecs.allocator, sizeof(CeComponentId) * count);
    memcpy(query->component_ids, component_ids, sizeof(CeComponentId) * count);
    query->component_count = count;
    query->current_archetype = 0;
    query->current_row = 0;
    return query;
}

void ce_query_destroy(CeQuery* query) {
    if (!query) return;
    ce_free(g_ecs.allocator, query->component_ids);
    ce_free(g_ecs.allocator, query);
}

uint32_t ce_query_execute(CeQuery* query, CeEntity* out_entities, uint32_t max_count) {
    uint32_t found = 0;

    while (found < max_count && query->current_archetype < g_ecs.archetype_count) {
        CeArchetype* arch = g_ecs.archetypes[query->current_archetype];

        /* 检查 Archetype 是否包含所有查询组件 */
        CeBool matches = CE_TRUE;
        for (uint32_t q = 0; q < query->component_count; q++) {
            CeBool has_comp = CE_FALSE;
            for (uint32_t c = 0; c < arch->component_count; c++) {
                if (arch->component_ids[c] == query->component_ids[q]) {
                    has_comp = CE_TRUE;
                    break;
                }
            }
            if (!has_comp) {
                matches = CE_FALSE;
                break;
            }
        }

        if (matches) {
            while (found < max_count && query->current_row < arch->entity_count) {
                out_entities[found++] = arch->entities[query->current_row++];
            }
        }

        if (query->current_row >= arch->entity_count) {
            query->current_archetype++;
            query->current_row = 0;
        }

        if (found >= max_count) break;
    }

    return found;
}

/* ---- 统计 ---- */

uint32_t ce_ecs_get_entity_count(void) {
    return g_ecs.entity_count - g_ecs.free_entity_count;
}

uint32_t ce_ecs_get_component_count(void) {
    return g_ecs.component_count;
}
