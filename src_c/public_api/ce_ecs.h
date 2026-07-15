/*
 * ChaosEngine ECS (Entity-Component-System)
 * 纯 C 实现，基于稀疏集合 + 原型（Archetype）模式
 */

#ifndef CE_ECS_H
#define CE_ECS_H

#include "ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 前向声明 ---- */

/** 复制管理器上下文 (不透明) */
typedef struct CeReplContext CeReplContext;

/** ECS World 实例 (不透明) - Phase 1.2 实例化 */
typedef struct CeEcsWorld CeEcsWorld;

/* ---- World 实例化 ---- */

/** 创建 ECS World 实例 */
CeEcsWorld* ce_ecs_world_create(void);

/** 销毁 ECS World 实例 */
void ce_ecs_world_destroy(CeEcsWorld* world);

/* ---- 实体 ---- */

/** 实体句柄：高 32 位为 generation，低 32 位为 index */
typedef uint64_t CeEntity;

#define CE_ENTITY_NULL       0
#define CE_ENTITY_INDEX(e)   ((uint32_t)((e) & 0xFFFFFFFF))
#define CE_ENTITY_GEN(e)     ((uint32_t)(((e) >> 32) & 0xFFFFFFFF))
#define CE_ENTITY_MAKE(i, g) ((((uint64_t)(g)) << 32) | ((uint64_t)(i)))

/* ---- 组件 ID ---- */

typedef uint32_t CeComponentId;

#define CE_MAX_COMPONENTS 128

/** 注册组件类型（返回唯一 ID） */
CeComponentId ce_component_register(const char* name, size_t size, size_t align);

/** 通过名称查找组件 ID */
CeComponentId ce_component_find(const char* name);

/** [World] 注册组件类型 */
CeComponentId ce_component_register_world(CeEcsWorld* world, const char* name, size_t size, size_t align);

/** [World] 通过名称查找组件 ID */
CeComponentId ce_component_find_world(CeEcsWorld* world, const char* name);

/* ---- 实体操作 ---- */

/** 创建实体 */
CeEntity ce_entity_create(void);

/** 销毁实体及其所有组件 */
void ce_entity_destroy(CeEntity entity);

/** 检查实体是否存活 */
CeBool ce_entity_is_alive(CeEntity entity);

/** [World] 创建实体 */
CeEntity ce_entity_create_world(CeEcsWorld* world);

/** [World] 销毁实体及其所有组件 */
void ce_entity_destroy_world(CeEcsWorld* world, CeEntity entity);

/** [World] 检查实体是否存活 */
CeBool ce_entity_is_alive_world(CeEcsWorld* world, CeEntity entity);

/* ---- 组件操作 ---- */

/** 为实体添加组件（若已存在则覆盖） */
void* ce_entity_add_component(CeEntity entity, CeComponentId comp_id);

/** 移除实体的组件 */
void ce_entity_remove_component(CeEntity entity, CeComponentId comp_id);

/** 获取实体的组件指针（只读，编辑器不得写入） */
const void* ce_entity_get_component(CeEntity entity, CeComponentId comp_id);

/** 检查实体是否拥有某组件 */
CeBool ce_entity_has_component(CeEntity entity, CeComponentId comp_id);

/** 修改实体组件（通过回调，确保安全写入） */
typedef void (*CeComponentEditFn)(void* component, void* user_data);
CeResult ce_entity_edit_component(CeEntity entity, CeComponentId comp_id,
                                   CeComponentEditFn edit_fn, void* user_data);

/** [World] 为实体添加组件 */
void* ce_entity_add_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id);

/** [World] 移除实体的组件 */
void ce_entity_remove_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id);

/** [World] 获取实体的组件指针 */
const void* ce_entity_get_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id);

/** [World] 检查实体是否拥有某组件 */
CeBool ce_entity_has_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id);

/** [World] 修改实体组件（通过回调） */
CeResult ce_entity_edit_component_world(CeEcsWorld* world, CeEntity entity, CeComponentId comp_id,
                                         CeComponentEditFn edit_fn, void* user_data);

/* ---- 系统 ---- */

typedef void (*CeSystemFn)(float delta_time, void* user_data);

/** 注册系统（按优先级排序执行） */
CeResult ce_system_register(const char* name, CeSystemFn fn, 
                             void* user_data, int priority);

/** 注销系统 */
void ce_system_unregister(const char* name);

/** [World] 注册系统 */
CeResult ce_system_register_world(CeEcsWorld* world, const char* name, CeSystemFn fn,
                                    void* user_data, int priority);

/** [World] 注销系统 */
void ce_system_unregister_world(CeEcsWorld* world, const char* name);

/* ---- 查询 ---- */

/** 实体迭代器：遍历拥有指定组件集合的所有实体 */
typedef struct CeQuery CeQuery;

/** 创建查询 */
CeQuery* ce_query_create(CeComponentId* component_ids, uint32_t count);

/** 销毁查询 */
void ce_query_destroy(CeQuery* query);

/** 执行查询迭代 */
uint32_t ce_query_execute(CeQuery* query, CeEntity* out_entities, uint32_t max_count);

/** [World] 创建查询 */
CeQuery* ce_query_create_world(CeEcsWorld* world, CeComponentId* component_ids, uint32_t count);

/** [World] 销毁查询 */
void ce_query_destroy_world(CeEcsWorld* world, CeQuery* query);

/** [World] 执行查询迭代 */
uint32_t ce_query_execute_world(CeEcsWorld* world, CeQuery* query, CeEntity* out_entities, uint32_t max_count);

/* ---- 更新 ---- */

/** 更新所有 ECS 系统（每帧调用，由引擎主循环驱动） */
void ce_ecs_update(float delta_time);

/** [World] 更新所有 ECS 系统 */
void ce_ecs_update_world(CeEcsWorld* world, float delta_time);

/* ---- 统计 ---- */

/** 获取当前存活实体数 */
uint32_t ce_ecs_get_entity_count(void);

/** 获取组件类型注册数 */
uint32_t ce_ecs_get_component_count(void);

/** [World] 获取当前存活实体数 */
uint32_t ce_ecs_get_entity_count_world(CeEcsWorld* world);

/** [World] 获取组件类型注册数 */
uint32_t ce_ecs_get_component_count_world(CeEcsWorld* world);

/* ---- 复制管线集成 ---- */

/**
 * 设置复制管理器上下文
 *
 * 设置后，所有 ECS 组件写入操作 (ce_entity_add_component,
 * ce_entity_edit_component) 会自动标记脏实体，供复制管线帧末 flush。
 *
 * 传 NULL 可清除上下文 (headless 模式)。
 *
 * @param ctx  复制管理器上下文 (NULL 禁用自动脏标)
 */
void ce_ecs_set_replication_context(CeReplContext* ctx);

/** [World] 设置复制管理器上下文 */
void ce_ecs_set_replication_context_world(CeEcsWorld* world, CeReplContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CE_ECS_H */
