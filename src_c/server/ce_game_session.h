/*
 * ChaosEngine Game Session Manager — 游戏会话管理
 *
 * 管理已连接的客户端、它们的实体和位置。
 * 使用 AOI 模块确定实体之间的可见性。
 *
 * 纯 C99，单线程安全。
 */

#ifndef CE_GAME_SESSION_H
#define CE_GAME_SESSION_H

#include "public_api/ce_types.h"
#include "server/ce_server_types.h"
#include "server/ce_game_protocol.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 常量 ---- */

/** 默认 AOI 视野半径 */
#define CE_GAME_DEFAULT_AOI_RADIUS  100.0f

/** 默认生成位置 */
#define CE_GAME_SPAWN_X  0.0f
#define CE_GAME_SPAWN_Y  0.0f
#define CE_GAME_SPAWN_Z  0.0f

/** 最大同时连接客户端数 */
#define CE_GAME_MAX_CLIENTS  1024

/** 最大可见实体数 */
#define CE_GAME_MAX_VISIBLE  256

/* ---- 客户端地址 ---- */

/** 客户端网络地址（简化，用于 MVP） */
typedef struct CeGameClientAddr {
    int    fd;              /* 客户端 socket fd */
    char   host[64];        /* 客户端 IP 字符串 */
    uint16_t port;          /* 客户端端口 */
    CeBool via_gateway;     /* 是否通过 Gateway 转发（非直连） */
} CeGameClientAddr;

/* ---- 游戏实体 ---- */

/** 游戏中的实体 */
typedef struct CeGameEntity {
    uint32_t          entity_id;     /* 实体 ID */
    float             x, y, z;      /* 位置 */
    CeGameClientAddr  client_addr;   /* 客户端地址 */
    CeBool            active;        /* 是否活跃 */
} CeGameEntity;

/* ---- 游戏会话 ---- */

/** 游戏会话管理器 */
typedef struct CeGameSession {
    CeGameEntity  entities[CE_GAME_MAX_CLIENTS];  /* 实体数组 */
    int           entity_count;                    /* 当前实体数 */
    float         aoi_radius;                      /* AOI 视野半径 */
    uint32_t      next_entity_id;                  /* 下一个可用实体 ID */
    CeBool        initialized;                     /* 是否已初始化 */
} CeGameSession;

/* ---- 生命周期 ---- */

/**
 * 初始化游戏会话管理器
 *
 * @param session     游戏会话管理器
 * @param aoi_radius  AOI 视野半径 (<=0 使用默认值)
 */
void ce_game_session_init(CeGameSession* session, float aoi_radius);

/**
 * 关闭游戏会话管理器
 *
 * 移除所有实体，关闭所有客户端连接。
 *
 * @param session  游戏会话管理器
 */
void ce_game_session_shutdown(CeGameSession* session);

/* ---- 实体管理 ---- */

/**
 * 玩家加入游戏
 *
 * 创建实体，分配 entity_id，添加到 AOI，注册 mailbox。
 * 所有玩家在 (0, 0, 0) 生成。
 *
 * @param session     游戏会话管理器
 * @param client_addr 客户端地址
 * @param out_entity_id 输出: 分配的实体 ID
 * @return            CE_OK 成功，CE_ERR 失败（服务器满等）
 */
CeResult ce_game_session_join(CeGameSession* session,
                              const CeGameClientAddr* client_addr,
                              uint32_t* out_entity_id);

/**
 * 玩家离开游戏
 *
 * 移除实体，从 AOI 移除，注销 mailbox。
 *
 * @param session    游戏会话管理器
 * @param entity_id  实体 ID
 */
void ce_game_session_leave(CeGameSession* session, uint32_t entity_id);

/**
 * 更新实体位置
 *
 * 更新位置并通知 AOI 系统。
 *
 * @param session    游戏会话管理器
 * @param entity_id  实体 ID
 * @param x          新 X 坐标
 * @param y          新 Y 坐标
 * @param z          新 Z 坐标
 * @return           CE_OK 成功，CE_ERR 实体不存在
 */
CeResult ce_game_session_update_position(CeGameSession* session,
                                         uint32_t entity_id,
                                         float x, float y, float z);

/* ---- 查询 ---- */

/**
 * 获取实体视野内的其他实体
 *
 * 使用 AOI 模块查询。
 *
 * @param session    游戏会话管理器
 * @param entity_id  实体 ID
 * @param out_buffer 输出缓冲区
 * @param out_count  输出: 可见实体数量
 * @return           CE_OK 成功，CE_ERR 实体不存在
 */
CeResult ce_game_session_get_visible(CeGameSession* session,
                                     uint32_t entity_id,
                                     CeGameEntityState* out_buffer,
                                     int* out_count);

/**
 * 根据实体 ID 查找实体
 *
 * @param session    游戏会话管理器
 * @param entity_id  实体 ID
 * @return           实体指针，未找到返回 NULL
 */
CeGameEntity* ce_game_session_find(CeGameSession* session, uint32_t entity_id);

/**
 * 获取当前实体数量
 *
 * @param session  游戏会话管理器
 * @return         实体数量
 */
int ce_game_session_count(CeGameSession* session);

#ifdef __cplusplus
}
#endif

#endif /* CE_GAME_SESSION_H */
