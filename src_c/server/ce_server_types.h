/*
 * ChaosEngine 服务端共享类型定义
 * 纯 C99，服务端专属模块使用
 */

#ifndef CE_SERVER_TYPES_H
#define CE_SERVER_TYPES_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 基础 ID 类型 ---- */

/** 服务端实体 ID（对应 ECS 的 CeEntity） */
typedef uint32_t CeServerEntityId;

/** Cell ID */
typedef uint32_t CeCellId;

/** 无效 ID 常量 */
#define CE_INVALID_ENTITY_ID  ((CeServerEntityId)-1)
#define CE_INVALID_CELL_ID    ((CeCellId)-1)

/* ---- AOI 事件 ---- */

typedef enum CeAoiEventType {
    CE_AOI_ENTER = 0,   /* 实体进入视野 */
    CE_AOI_LEAVE = 1,   /* 实体离开视野 */
    CE_AOI_MOVE  = 2,   /* 实体在视野内移动 */
} CeAoiEventType;

typedef struct CeAoiEvent {
    CeAoiEventType    type;
    CeServerEntityId  subject;    /* 谁触发了事件 */
    CeServerEntityId  object;     /* 与谁相关 */
    float             x, y;       /* 当前位置 */
} CeAoiEvent;

/** AOI 事件回调 */
typedef void (*CeAoiEventCallback)(const CeAoiEvent* event, void* user_data);

/* ---- Cell 类型 ---- */

typedef enum CeCellState {
    CE_CELL_ACTIVE = 0,    /* 正常运行 */
    CE_CELL_SPLITTING,     /* 分裂中 */
    CE_CELL_MERGING,       /* 合并中 */
    CE_CELL_MIGRATING,     /* 迁移中 */
} CeCellState;

typedef struct CeCellBounds {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
} CeCellBounds;

/** Cell 结构 */
typedef struct CeCell {
    CeCellId     id;
    CeCellBounds bounds;
    CeCellState  state;

    /* 负载指标 */
    int          entity_count;
    int          max_entities;    /* 分裂阈值 */
    int          min_entities;    /* 合并阈值 */

    /* 进程分配 */
    int          process_id;

    /* 邻居 Cell */
    CeCellId     neighbors[8];
    int          neighbor_count;
} CeCell;

#ifdef __cplusplus
}
#endif

#endif /* CE_SERVER_TYPES_H */
