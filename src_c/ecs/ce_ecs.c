/*
 * ChaosEngine ECS — 实体组件系统
 * 基于 Archetype（原型）模式，Cache-Friendly 数据布局
 */

#include "ecs/ce_ecs_internal.h"
#include "core/ce_memory.h"
#include "replication/ce_replication.h"
#include "public_api/ce_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 默认 World (向后兼容旧 API) ---- */

static CeEcsWorld* g_default_world = NULL;

/* ---- 复制管理器上下文 (旧 API: 操作默认 world) ---- */

void ce_ecs_set_replication_context(CeReplContext* ctx) {
    ce_ecs_set_replication_context_world(ce_ecs_get_default_world(), ctx);
}

void ce_ecs_set_replication_context_world(CeEcsWorld* world, CeReplContext* ctx) {
    if (!world) return;
    world->repl_ctx = ctx;
    CE_LOG_INFO("ECS", "replication context %s", ctx ? "set" : "cleared");
}

/* ---- World 创建 / 销毁 ---- */

CeEcsWorld* ce_ecs_world_create(void) {
    CeEcsWorld* w = (CeEcsWorld*)calloc(1, sizeof(CeEcsWorld));
    if (!w) return NULL;

    w->allocator        = NULL;  /* NULL allocator => malloc/free */
    w->initialized      = CE_TRUE;
    w->entity_capacity  = 1024;
    w->entity_count     = 0;
    w->free_entity_count= 0;

    w->entity_generations = (CeEntity*)ce_alloc_zero(w->allocator,
        sizeof(CeEntity) * w->entity_capacity);
    w->entity_archetypes  = (CeArchetype**)ce_alloc_zero(w->allocator,
        sizeof(CeArchetype*) * w->entity_capacity);
    w->entity_rows        = (uint32_t*)ce_alloc_zero(w->allocator,
        sizeof(uint32_t) * w->entity_capacity);
    w->free_entities      = (uint32_t*)ce_alloc_zero(w->allocator,
        sizeof(uint32_t) * w->entity_capacity);

    /* Archetype 动态数组: 初始 64 */
    w->archetype_capacity = 64;
    w->archetype_count    = 0;
    w->archetypes         = (CeArchetype**)ce_alloc_zero(w->allocator,
        sizeof(CeArchetype*) * w->archetype_capacity);

    /* System 动态数组: 初始 32 */
    w->system_capacity    = 32;
    w->system_count       = 0;
    w->systems            = (CeSystemInfo*)ce_alloc_zero(w->allocator,
        sizeof(CeSystemInfo) * w->system_capacity);

    w->component_count    = 0;
    w->repl_ctx           = NULL;

    return w;
}

void ce_ecs_world_destroy(CeEcsWorld* world) {
    if (!world) return;

    /* 释放所有 Archetype */
    for (uint32_t i = 0; i < world->archetype_count; i++) {
        CeArchetype* arch = world->archetypes[i];
        if (!arch) continue;
        ce_free(world->allocator, arch->entities);
        for (uint32_t c = 0; c < arch->component_count; c++) {
            ce_free(world->allocator, arch->component_data[c]);
        }
        ce_free(world->allocator, arch->component_ids);
        ce_free(world->allocator, arch->component_data);
        ce_free(world->allocator, arch);
    }

    ce_free(world->allocator, world->entity_generations);
    ce_free(world->allocator, world->entity_archetypes);
    ce_free(world->allocator, world->entity_rows);
    ce_free(world->allocator, world->free_entities);
    ce_free(world->allocator, world->archetypes);
    ce_free(world->allocator, world->systems);

    free(world);
}

CeEcsWorld* ce_ecs_get_default_world(void) {
    if (!g_default_world) {
        g_default_world = ce_ecs_world_create();
    }
    return g_default_world;
}

/* ---- 旧 API 兼容: init / shutdown ---- */

CeResult ce_ecs_init(CeAllocator* allocator) {
    if (g_default_world && g_default_world->initialized) return CE_OK;

    /* 如果已有默认 world 但 allocator 不同，重建 */
    if (g_default_world) {
        ce_ecs_world_destroy(g_default_world);
        g_default_world = NULL;
    }

    g_default_world = ce_ecs_world_create();
    if (!g_default_world) return CE_ERR;

    /* 覆盖 allocator (init 时传入的引擎分配器) */
    g_default_world->allocator = allocator;

    return CE_OK;
}

void ce_ecs_shutdown(void) {
    if (g_default_world) {
        ce_ecs_world_destroy(g_default_world);
        g_default_world = NULL;
    }
}

/* ---- 组件注册 ---- */

CeComponentId ce_component_register_world(CeEcsWorld* world, const char* name, size_t size, size_t align) {
    if (!world || world->component_count >= CE_MAX_COMPONENTS) return (CeComponentId)-1;

    CeComponentId id = world->component_count++;
    CeComponentInfo* info = &world->components[id];
    strncpy(info->name, name, sizeof(info->name) - 1);
    info->size  = size;
    info->align = align;
    info->id    = id;

    return id;
}

CeComponentId ce_component_find_world(CeEcsWorld* world, const char* name) {
    if (!world) return (CeComponentId)-1;
    for (uint32_t i = 0; i < world->component_count; i++) {
        if (strcmp(world->components[i].name, name) == 0) {
            return world->components[i].id;
        }
    }
    return (CeComponentId)-1;
}

CeComponentId ce_component_register(const char* name, size_t size, size_t align) {
    return ce_component_register_world(ce_ecs_get_default_world(), name, size, align);
}

CeComponentId ce_component_find(const char* name) {
    return ce_component_find_world(ce_ecs_get_default_world(), name);
}

/* ---- Archetype 管理 ---- */

static int compare_component_ids(const void* a, const void* b) {
    return (int)(*(CeComponentId*)a - *(CeComponentId*)b);
}

/* 确保 archetype 数组有足够容量，不够则翻倍扩容 */
static CeBool ensure_archetype_capacity(CeEcsWorld* world) {
    if (world->archetype_count < world->archetype_capacity) return CE_TRUE;
    uint32_t new_cap = world->archetype_capacity * 2;
    CeArchetype** new_arr = (CeArchetype**)ce_realloc(world->allocator,
        world->archetypes, sizeof(CeArchetype*) * new_cap);
    if (!new_arr) return CE_FALSE;
    world->archetypes = new_arr;
    world->archetype_capacity = new_cap;
    return CE_TRUE;
}

static CeArchetype* find_or_create_archetype(CeEcsWorld* world, CeComponentId* comp_ids, uint32_t count) {
    /* 排序组件 ID，确保唯一性 */
    CeComponentId sorted[CE_MAX_COMPONENTS];
    memcpy(sorted, comp_ids, sizeof(CeComponentId) * count);
    qsort(sorted, count, sizeof(CeComponentId), compare_component_ids);

    /* 查找已有 Archetype */
    for (uint32_t i = 0; i < world->archetype_count; i++) {
        CeArchetype* arch = world->archetypes[i];
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
    CeArchetype* arch = ce_alloc_zero(world->allocator, sizeof(CeArchetype));
    arch->component_count = count;
    arch->entity_capacity = 64;
    arch->entity_count    = 0;

    arch->component_ids = ce_alloc(world->allocator, sizeof(CeComponentId) * count);
    memcpy(arch->component_ids, sorted, sizeof(CeComponentId) * count);

    arch->component_data = ce_alloc_zero(world->allocator, sizeof(void*) * count);
    arch->entities = ce_alloc(world->allocator, sizeof(CeEntity) * arch->entity_capacity);

    for (uint32_t c = 0; c < count; c++) {
        CeComponentInfo* info = &world->components[sorted[c]];
        arch->component_data[c] = ce_alloc_zero(world->allocator,
            info->size * arch->entity_capacity);
    }

    /* 动态扩容 archetype 数组 */
    if (!ensure_archetype_capacity(world)) {
        /* 扩容失败，清理并返回 NULL */
        ce_free(world->allocator, arch->entities);
        ce_free(world->allocator, arch->component_ids);
        ce_free(world->allocator, arch->component_data);
        ce_free(world->allocator, arch);
        return NULL;
    }

    world->archetypes[world->archetype_count++] = arch;
    return arch;
}

static void archetype_grow(CeEcsWorld* world, CeArchetype* arch) {
    uint32_t new_cap = arch->entity_capacity * 2;

    arch->entities = ce_realloc(world->allocator, arch->entities,
        sizeof(CeEntity) * new_cap);

    for (uint32_t c = 0; c < arch->component_count; c++) {
        CeComponentInfo* info = &world->components[arch->component_ids[c]];
        void* new_data = ce_alloc_zero(world->allocator, info->size * new_cap);
        memcpy(new_data, arch->component_data[c], info->size * arch->entity_count);
        ce_free(world->allocator, arch->component_data[c]);
        arch->component_data[c] = new_data;
    }

    arch->entity_capacity = new_cap;
}

/* ---- 实体操作 ---- */

CeEntity ce_entity_create_world(CeEcsWorld* world) {
    if (!world) return CE_ENTITY_NULL;
    uint32_t index;

    if (world->free_entity_count > 0) {
        /* 复用已释放的槽位 */
        index = world->free_entities[--world->free_entity_count];
    } else {
        /* 扩容 */
        if (world->entity_count >= world->entity_capacity) {
            uint32_t new_cap = world->entity_capacity * 2;
            world->entity_generations = ce_realloc(world->allocator,
                world->entity_generations, sizeof(CeEntity) * new_cap);
            world->entity_archetypes = ce_realloc(world->allocator,
                world->entity_archetypes, sizeof(CeArchetype*) * new_cap);
            world->entity_rows = ce_realloc(world->allocator,
                world->entity_rows, sizeof(uint32_t) * new_cap);
            world->free_entities = ce_realloc(world->allocator,
                world->free_entities, sizeof(uint32_t) * new_cap);
            world->entity_capacity = new_cap;
        }
        index = world->entity_count++;
    }

    uint32_t gen = (uint32_t)(world->entity_generations[index] >> 32);
    world->entity_generations[index] = CE_ENTITY_MAKE(index, gen);
    world->entity_archetypes[index] = NULL;
    world->entity_rows[index] = 0;

    return CE_ENTITY_MAKE(index, gen);
}

void ce_entity_destroy_world(CeEcsWorld* world, CeEntity entity) {
    if (!world) return;
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (index >= world->entity_count) return;
    if (!ce_entity_is_alive_world(world, entity)) return;

    /* 从 Archetype 中移除 */
    CeArchetype* arch = world->entity_archetypes[index];
    if (arch) {
        uint32_t row = world->entity_rows[index];
        uint32_t last = arch->entity_count - 1;

        if (row != last) {
            /* 用最后一个实体覆盖当前位置 */
            CeEntity last_entity = arch->entities[last];
            arch->entities[row] = last_entity;
            world->entity_rows[CE_ENTITY_INDEX(last_entity)] = row;

            for (uint32_t c = 0; c < arch->component_count; c++) {
                CeComponentInfo* info = &world->components[arch->component_ids[c]];
                uint8_t* data = (uint8_t*)arch->component_data[c];
                memcpy(data + row * info->size,
                       data + last * info->size, info->size);
            }
        }
        arch->entity_count--;
    }

    /* 增加 generation，标记为无效 */
    uint32_t gen = (uint32_t)(world->entity_generations[index] >> 32) + 1;
    world->entity_generations[index] = CE_ENTITY_MAKE(index, gen);
    world->entity_archetypes[index] = NULL;

    /* 放入空闲列表 */
    world->free_entities[world->free_entity_count++] = index;
}

CeBool ce_entity_is_alive_world(CeEcsWorld* world, CeEntity entity) {
    if (!world) return CE_FALSE;
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (index >= world->entity_count) return CE_FALSE;
    return world->entity_generations[index] == entity;
}

CeEntity ce_entity_create(void) {
    return ce_entity_create_world(ce_ecs_get_default_world());
}

void ce_entity_destroy(CeEntity entity) {
    ce_entity_destroy_world(ce_ecs_get_default_world(), entity);
}

CeBool ce_entity_is_alive(CeEntity entity) {
    return ce_entity_is_alive_world(ce_ecs_get_default_world(), entity);
}

/* ---- 组件操作 ---- */

void* ce_entity_add_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id) {
    if (!world) return NULL;
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (!ce_entity_is_alive_world(world, entity)) return NULL;

    CeArchetype* old_arch = world->entity_archetypes[index];
    uint32_t old_row = world->entity_rows[index];

    /* 构建新的组件 ID 列表 */
    CeComponentId new_ids[CE_MAX_COMPONENTS];
    uint32_t new_count = 0;

    if (old_arch) {
        for (uint32_t c = 0; c < old_arch->component_count; c++) {
            if (old_arch->component_ids[c] == comp_id) {
                /* 已存在，直接返回 */
                CeComponentInfo* info = &world->components[comp_id];
                uint8_t* data = (uint8_t*)old_arch->component_data[c];
                /* 标记脏 (复制管线) */
                if (world->repl_ctx) ce_repl_mark_dirty(world->repl_ctx, entity, comp_id);
                return data + old_row * info->size;
            }
            new_ids[new_count++] = old_arch->component_ids[c];
        }
    }
    new_ids[new_count++] = comp_id;

    /* 找到或创建目标 Archetype */
    CeArchetype* new_arch = find_or_create_archetype(world, new_ids, new_count);
    if (!new_arch) return NULL;
    if (new_arch->entity_count >= new_arch->entity_capacity) {
        archetype_grow(world, new_arch);
    }

    uint32_t new_row = new_arch->entity_count++;

    /* 复制旧组件数据 */
    if (old_arch) {
        for (uint32_t c = 0; c < old_arch->component_count; c++) {
            CeComponentId old_cid = old_arch->component_ids[c];
            CeComponentInfo* info = &world->components[old_cid];

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
            world->entity_rows[CE_ENTITY_INDEX(last_entity)] = old_row;

            for (uint32_t c = 0; c < old_arch->component_count; c++) {
                CeComponentInfo* info = &world->components[old_arch->component_ids[c]];
                uint8_t* data = (uint8_t*)old_arch->component_data[c];
                memcpy(data + old_row * info->size,
                       data + last * info->size, info->size);
            }
        }
        old_arch->entity_count--;
    }

    /* 更新实体映射 */
    new_arch->entities[new_row] = entity;
    world->entity_archetypes[index] = new_arch;
    world->entity_rows[index] = new_row;

    /* 返回新组件的指针 */
    for (uint32_t c = 0; c < new_count; c++) {
        if (new_ids[c] == comp_id) {
            CeComponentInfo* info = &world->components[comp_id];
            uint8_t* data = (uint8_t*)new_arch->component_data[c];
            /* 标记脏 (复制管线) */
            if (world->repl_ctx) ce_repl_mark_dirty(world->repl_ctx, entity, comp_id);
            return data + new_row * info->size;
        }
    }

    return NULL;
}

void ce_entity_remove_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id) {
    if (!world) return;
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (!ce_entity_is_alive_world(world, entity)) return;

    CeArchetype* old_arch = world->entity_archetypes[index];
    if (!old_arch) return;

    uint32_t old_row = world->entity_rows[index];

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
            world->entity_rows[CE_ENTITY_INDEX(last_entity)] = old_row;

            for (uint32_t c = 0; c < old_arch->component_count; c++) {
                CeComponentInfo* info = &world->components[old_arch->component_ids[c]];
                uint8_t* data = (uint8_t*)old_arch->component_data[c];
                memcpy(data + old_row * info->size,
                       data + last * info->size, info->size);
            }
        }
        old_arch->entity_count--;
        world->entity_archetypes[index] = NULL;
        return;
    }

    new_arch = find_or_create_archetype(world, new_ids, new_count);
    if (!new_arch) return;
    if (new_arch->entity_count >= new_arch->entity_capacity) {
        archetype_grow(world, new_arch);
    }

    uint32_t new_row = new_arch->entity_count++;

    /* 复制保留的组件 */
    for (uint32_t c = 0; c < old_arch->component_count; c++) {
        CeComponentId old_cid = old_arch->component_ids[c];
        if (old_cid == comp_id) continue;

        CeComponentInfo* info = &world->components[old_cid];
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
        world->entity_rows[CE_ENTITY_INDEX(last_entity)] = old_row;

        for (uint32_t c = 0; c < old_arch->component_count; c++) {
            CeComponentInfo* info = &world->components[old_arch->component_ids[c]];
            uint8_t* data = (uint8_t*)old_arch->component_data[c];
            memcpy(data + old_row * info->size,
                   data + last * info->size, info->size);
        }
    }
    old_arch->entity_count--;

    new_arch->entities[new_row] = entity;
    world->entity_archetypes[index] = new_arch;
    world->entity_rows[index] = new_row;
}

const void* ce_entity_get_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id) {
    if (!world) return NULL;
    uint32_t index = CE_ENTITY_INDEX(entity);
    if (!ce_entity_is_alive_world(world, entity)) return NULL;

    CeArchetype* arch = world->entity_archetypes[index];
    if (!arch) return NULL;

    uint32_t row = world->entity_rows[index];

    for (uint32_t c = 0; c < arch->component_count; c++) {
        if (arch->component_ids[c] == comp_id) {
            uint8_t* data = (uint8_t*)arch->component_data[c];
            return data + row * world->components[comp_id].size;
        }
    }

    return NULL;
}

CeBool ce_entity_has_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id) {
    return ce_entity_get_component_world(world, entity, comp_id) != NULL;
}

CeResult ce_entity_edit_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id,
                                         CeComponentEditFn edit_fn, void* user_data) {
    if (!world) return CE_ERR;
    /* 注意：这里通过 const 转换获取可写指针，但仅在 edit_fn 回调中使用 */
    /* 编辑器应通过此接口修改组件，而非直接操作内存 */
    const void* comp = ce_entity_get_component_world(world, entity, comp_id);
    if (!comp) return CE_ERR;

    edit_fn((void*)comp, user_data);

    /* 标记脏 (复制管线) */
    if (world->repl_ctx) ce_repl_mark_dirty(world->repl_ctx, entity, comp_id);

    return CE_OK;
}

/* ---- 旧 API 兼容包装 ---- */

void* ce_entity_add_component(CeEntity entity, CeComponentId comp_id) {
    return ce_entity_add_component_world(ce_ecs_get_default_world(), entity, comp_id);
}

void ce_entity_remove_component(CeEntity entity, CeComponentId comp_id) {
    ce_entity_remove_component_world(ce_ecs_get_default_world(), entity, comp_id);
}

const void* ce_entity_get_component(CeEntity entity, CeComponentId comp_id) {
    return ce_entity_get_component_world(ce_ecs_get_default_world(), entity, comp_id);
}

CeBool ce_entity_has_component(CeEntity entity, CeComponentId comp_id) {
    return ce_entity_has_component_world(ce_ecs_get_default_world(), entity, comp_id);
}

CeResult ce_entity_edit_component(CeEntity entity, CeComponentId comp_id,
                                   CeComponentEditFn edit_fn, void* user_data) {
    return ce_entity_edit_component_world(ce_ecs_get_default_world(), entity, comp_id,
                                           edit_fn, user_data);
}

/* ---- 系统 ---- */

/* 确保 system 数组有足够容量，不够则翻倍扩容 */
static CeBool ensure_system_capacity(CeEcsWorld* world) {
    if (world->system_count < world->system_capacity) return CE_TRUE;
    uint32_t new_cap = world->system_capacity * 2;
    CeSystemInfo* new_arr = (CeSystemInfo*)ce_realloc(world->allocator,
        world->systems, sizeof(CeSystemInfo) * new_cap);
    if (!new_arr) return CE_FALSE;
    world->systems = new_arr;
    world->system_capacity = new_cap;
    return CE_TRUE;
}

CeResult ce_system_register_world(CeEcsWorld* world, const char* name, CeSystemFn fn,
                                    void* user_data, int priority) {
    if (!world) return CE_ERR;
    if (!ensure_system_capacity(world)) return CE_ERR;

    CeSystemInfo* sys = &world->systems[world->system_count++];
    strncpy(sys->name, name, sizeof(sys->name) - 1);
    sys->fn        = fn;
    sys->user_data = user_data;
    sys->priority  = priority;
    sys->enabled   = CE_TRUE;

    /* 按优先级排序 */
    for (uint32_t i = world->system_count - 1; i > 0; i--) {
        if (world->systems[i].priority < world->systems[i - 1].priority) {
            CeSystemInfo tmp = world->systems[i];
            world->systems[i] = world->systems[i - 1];
            world->systems[i - 1] = tmp;
        }
    }

    return CE_OK;
}

void ce_system_unregister_world(CeEcsWorld* world, const char* name) {
    if (!world) return;
    for (uint32_t i = 0; i < world->system_count; i++) {
        if (strcmp(world->systems[i].name, name) == 0) {
            /* 用最后一个覆盖 */
            world->systems[i] = world->systems[--world->system_count];
            return;
        }
    }
}

CeResult ce_system_register(const char* name, CeSystemFn fn,
                             void* user_data, int priority) {
    return ce_system_register_world(ce_ecs_get_default_world(), name, fn, user_data, priority);
}

void ce_system_unregister(const char* name) {
    ce_system_unregister_world(ce_ecs_get_default_world(), name);
}

/* ---- 公共更新 API ---- */

void ce_ecs_update_world(CeEcsWorld* world, float delta_time) {
    if (!world) return;
    for (uint32_t i = 0; i < world->system_count; i++) {
        if (world->systems[i].enabled) {
            world->systems[i].fn(delta_time, world->systems[i].user_data);
        }
    }
}

void ce_ecs_update(float delta_time) {
    ce_ecs_update_world(ce_ecs_get_default_world(), delta_time);
}

void ce_ecs_update_systems(float delta_time) {
    ce_ecs_update_world(ce_ecs_get_default_world(), delta_time);
}

/* ---- 查询 ---- */

struct CeQuery {
    CeComponentId* component_ids;
    uint32_t       component_count;
    uint32_t       current_archetype;
    uint32_t       current_row;
};

CeQuery* ce_query_create_world(CeEcsWorld* world, CeComponentId* component_ids, uint32_t count) {
    if (!world) return NULL;
    CeQuery* query = ce_alloc(world->allocator, sizeof(CeQuery));
    query->component_ids = ce_alloc(world->allocator, sizeof(CeComponentId) * count);
    memcpy(query->component_ids, component_ids, sizeof(CeComponentId) * count);
    query->component_count = count;
    query->current_archetype = 0;
    query->current_row = 0;
    return query;
}

void ce_query_destroy_world(CeEcsWorld* world, CeQuery* query) {
    if (!query) return;
    ce_free(world ? world->allocator : NULL, query->component_ids);
    ce_free(world ? world->allocator : NULL, query);
}

uint32_t ce_query_execute_world(CeEcsWorld* world, CeQuery* query, CeEntity* out_entities, uint32_t max_count) {
    if (!world || !query) return 0;
    uint32_t found = 0;

    while (found < max_count && query->current_archetype < world->archetype_count) {
        CeArchetype* arch = world->archetypes[query->current_archetype];

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

/* 旧 API 兼容包装 */
CeQuery* ce_query_create(CeComponentId* component_ids, uint32_t count) {
    return ce_query_create_world(ce_ecs_get_default_world(), component_ids, count);
}

void ce_query_destroy(CeQuery* query) {
    ce_query_destroy_world(ce_ecs_get_default_world(), query);
}

uint32_t ce_query_execute(CeQuery* query, CeEntity* out_entities, uint32_t max_count) {
    return ce_query_execute_world(ce_ecs_get_default_world(), query, out_entities, max_count);
}

/* ---- 统计 ---- */

uint32_t ce_ecs_get_entity_count_world(CeEcsWorld* world) {
    if (!world) return 0;
    return world->entity_count - world->free_entity_count;
}

uint32_t ce_ecs_get_component_count_world(CeEcsWorld* world) {
    if (!world) return 0;
    return world->component_count;
}

uint32_t ce_ecs_get_entity_count(void) {
    return ce_ecs_get_entity_count_world(ce_ecs_get_default_world());
}

uint32_t ce_ecs_get_component_count(void) {
    return ce_ecs_get_component_count_world(ce_ecs_get_default_world());
}
