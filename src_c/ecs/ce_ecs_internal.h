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

/* ---- 内部 API ---- */

CeResult ce_ecs_init(CeAllocator* allocator);
void     ce_ecs_shutdown(void);
void     ce_ecs_update_systems(float delta_time);

#endif /* CE_ECS_INTERNAL_H */
