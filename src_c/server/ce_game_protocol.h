/*
 * ChaosEngine Game Protocol — 客户端↔服务器通信协议
 *
 * 简单二进制协议格式:
 *   [4B total_len][2B msg_type][payload]
 *
 * 纯 C99，无外部依赖。
 */

#ifndef CE_GAME_PROTOCOL_H
#define CE_GAME_PROTOCOL_H

#include "public_api/ce_types.h"
#include "server/ce_server_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 消息类型 ---- */

typedef enum CeGameMsgType {
    /* 客户端 → 服务器 */
    MSG_JOIN_REQUEST     = 0x0001,  /* 客户端请求加入游戏 */
    MSG_POSITION_UPDATE  = 0x0002,  /* 客户端更新自身位置 */

    /* 服务器 → 客户端 */
    MSG_JOIN_RESPONSE    = 0x8001,  /* 服务器回复加入结果 */
    MSG_ENTITY_STATE     = 0x8002,  /* 服务器广播周围实体状态 */
} CeGameMsgType;

/* ---- 协议头 ---- */

/** 消息头: 所有消息的前 6 字节 [4B total_len][2B msg_type] */
#define CE_GAME_HEADER_SIZE  6

/* ---- 消息载荷 ---- */

/** MSG_JOIN_REQUEST: 无载荷，仅头 */

/** MSG_JOIN_RESPONSE: 服务器回复 */
typedef struct CeGameJoinResponse {
    CeResult    result;             /* CE_OK 成功，CE_ERR 失败 */
    uint32_t    assigned_entity_id; /* 分配的实体 ID */
} CeGameJoinResponse;

/** MSG_POSITION_UPDATE: 客户端发送位置 */
typedef struct CeGamePositionUpdate {
    float x;
    float y;
    float z;
} CeGamePositionUpdate;

/** MSG_ENTITY_STATE: 单个实体状态 */
typedef struct CeGameEntityState {
    uint32_t entity_id;
    float    x;
    float    y;
    float    z;
} CeGameEntityState;

/** MSG_ENTITY_STATE 完整消息载荷 (服务器→客户端) */
typedef struct CeGameEntityStatePayload {
    uint16_t           count;       /* 实体数量 */
    CeGameEntityState  entities[];  /* 变长数组 */
} CeGameEntityStatePayload;

/* ---- 打包/解包辅助函数 ---- */

/**
 * 计算 MSG_ENTITY_STATE 消息的总长度
 *
 * @param entity_count  实体数量
 * @return              消息总长度（字节）
 */
static inline int ce_game_calc_entity_state_size(int entity_count) {
    return CE_GAME_HEADER_SIZE
           + (int)sizeof(uint16_t)                    /* count */
           + entity_count * (int)sizeof(CeGameEntityState);
}

/**
 * 将实体状态写入缓冲区 (网络字节序大端)
 *
 * @param buffer    输出缓冲区
 * @param entities  实体状态数组
 * @param count     实体数量
 * @return          写入的总字节数
 */
int ce_game_pack_entity_state(uint8_t* buffer,
                              const CeGameEntityState* entities, int count);

/**
 * 解析 MSG_POSITION_UPDATE 载荷
 *
 * @param data      消息数据（跳过头部后的位置）
 * @param data_len  剩余数据长度
 * @param out       输出位置更新
 * @return          CE_OK 成功，CE_ERR 数据不足
 */
CeResult ce_game_unpack_position_update(const uint8_t* data, int data_len,
                                        CeGamePositionUpdate* out);

/**
 * 解析 MSG_JOIN_RESPONSE 载荷
 *
 * @param data      消息数据（跳过头部后的位置）
 * @param data_len  剩余数据长度
 * @param out       输出加入响应
 * @return          CE_OK 成功，CE_ERR 数据不足
 */
CeResult ce_game_unpack_join_response(const uint8_t* data, int data_len,
                                      CeGameJoinResponse* out);

/**
 * 打包 MSG_JOIN_RESPONSE
 *
 * @param buffer    输出缓冲区
 * @param response  加入响应
 * @return          写入的总字节数
 */
int ce_game_pack_join_response(uint8_t* buffer,
                               const CeGameJoinResponse* response);

/**
 * 打包 MSG_POSITION_UPDATE (服务器回显用)
 *
 * @param buffer    输出缓冲区
 * @param update    位置更新
 * @return          写入的总字节数
 */
int ce_game_pack_position_update(uint8_t* buffer,
                                 const CeGamePositionUpdate* update);

#ifdef __cplusplus
}
#endif

#endif /* CE_GAME_PROTOCOL_H */
