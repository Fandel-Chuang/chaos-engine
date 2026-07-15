/*
 * ChaosEngine ECS 内部头文件（非 public_api）
 */

#ifndef CE_ECS_INTERNAL_H
#define CE_ECS_INTERNAL_H

#include "public_api/ce_ecs.h"
#include "public_api/ce_types.h"
#include "core/ce_memory.h"

/* ---- 组件信息 ---- */

typedef struct CeComponentInfo {
    CeComponentId id;
    char          name[32];
    size_t        size;
    size_t        align;
} CeComponentInfo;

/* ---- Archetype（原型）---- */

typedef struct CeArchetype {
    CeComponentId* component_ids;     /* 组件 ID 列表 */
    uint32_t       component_count;   /* 组件数量 */
    void**         component_data;    /* 每个组件的数据数组 */
    CeEntity*      entities;          /* 实体句柄数组 */
    uint32_t       entity_count;      /* 当前实体数 */
    uint32_t       entity_capacity;   /* 容量 */
} CeArchetype;

/* ---- 系统信息 ---- */

typedef struct CeSystemInfo {
    char        name[32];
    CeSystemFn  fn;
    void*       user_data;
    int         priority;
    CeBool      enabled;
} CeSystemInfo;

/* ---- ECS World（实例化上下文）---- */

typedef struct CeEcsWorld {
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

    /* Archetype — 动态扩容 */
    uint32_t     archetype_count;
    uint32_t     archetype_capacity;
    CeArchetype** archetypes;

    /* 系统 — 动态扩容 */
    uint32_t     system_count;
    uint32_t     system_capacity;
    CeSystemInfo* systems;

    /* 复制管理器上下文 (外部注入) */
    struct CeReplContext* repl_ctx;
} CeEcsWorld;

/* ---- 内部 API ---- */

/** 设置复制管理器上下文 (用于自动脏标) - 操作默认 world */
void ce_ecs_set_replication_context(CeReplContext* ctx);

/** [World] 设置复制管理器上下文 */
void ce_ecs_set_replication_context_world(CeEcsWorld* world, CeReplContext* ctx);

CeResult ce_ecs_init(CeAllocator* allocator);
void     ce_ecs_shutdown(void);
void     ce_ecs_update_systems(float delta_time);

/* ---- World 实例化 API ---- */

CeEcsWorld* ce_ecs_world_create(void);
void        ce_ecs_world_destroy(CeEcsWorld* world);

/* 获取默认 world (若不存在则自动创建) */
CeEcsWorld* ce_ecs_get_default_world(void);

#endif /* CE_ECS_INTERNAL_H */
