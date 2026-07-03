/*
 * ChaosEngine Game Session Manager — 实现
 *
 * 管理已连接的客户端、它们的实体和位置。
 * 使用 AOI 模块确定实体之间的可见性。
 *
 * 纯 C99，单线程安全。
 */

#include "server/ce_game_session.h"
#include "server/ce_game_protocol.h"
#include "server/ce_aoi.h"
#include "public_api/ce_log.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ============================================================
 * 协议打包/解包实现
 * ============================================================ */

int ce_game_pack_entity_state(uint8_t* buffer,
                              const CeGameEntityState* entities, int count) {
    if (!buffer || !entities || count < 0) return 0;

    uint8_t* ptr = buffer;

    /* 跳过头部 (caller 负责填充 header) */
    ptr += CE_GAME_HEADER_SIZE;

    /* count (网络字节序大端) */
    *ptr++ = (uint8_t)((count >> 8) & 0xFF);
    *ptr++ = (uint8_t)( count       & 0xFF);

    /* entities */
    for (int i = 0; i < count; i++) {
        /* entity_id (大端) */
        uint32_t eid = entities[i].entity_id;
        *ptr++ = (uint8_t)((eid >> 24) & 0xFF);
        *ptr++ = (uint8_t)((eid >> 16) & 0xFF);
        *ptr++ = (uint8_t)((eid >>  8) & 0xFF);
        *ptr++ = (uint8_t)( eid        & 0xFF);

        /* x, y, z 用 memcpy (float 按 IEEE 754 传输) */
        /* 为简化 MVP，直接拷贝 float 字节 */
        float fbuf[3];
        fbuf[0] = entities[i].x;
        fbuf[1] = entities[i].y;
        fbuf[2] = entities[i].z;
        memcpy(ptr, fbuf, sizeof(float) * 3);
        ptr += sizeof(float) * 3;
    }

    /* 填充总长度 */
    int total_len = (int)(ptr - buffer);
    buffer[0] = (uint8_t)((total_len >> 24) & 0xFF);
    buffer[1] = (uint8_t)((total_len >> 16) & 0xFF);
    buffer[2] = (uint8_t)((total_len >>  8) & 0xFF);
    buffer[3] = (uint8_t)( total_len        & 0xFF);

    return total_len;
}

CeResult ce_game_unpack_position_update(const uint8_t* data, int data_len,
                                        CeGamePositionUpdate* out) {
    if (!data || !out) return CE_ERR;
    if (data_len < (int)sizeof(CeGamePositionUpdate)) return CE_ERR;

    float fbuf[3];
    memcpy(fbuf, data, sizeof(float) * 3);
    out->x = fbuf[0];
    out->y = fbuf[1];
    out->z = fbuf[2];

    return CE_OK;
}

CeResult ce_game_unpack_join_response(const uint8_t* data, int data_len,
                                      CeGameJoinResponse* out) {
    if (!data || !out) return CE_ERR;
    if (data_len < (int)sizeof(CeGameJoinResponse)) return CE_ERR;

    /* result (大端 int32) */
    int32_t result;
    result  = ((int32_t)data[0]) << 24;
    result |= ((int32_t)data[1]) << 16;
    result |= ((int32_t)data[2]) << 8;
    result |=  (int32_t)data[3];
    out->result = result;

    /* assigned_entity_id (大端 uint32) */
    uint32_t eid;
    eid  = ((uint32_t)data[4]) << 24;
    eid |= ((uint32_t)data[5]) << 16;
    eid |= ((uint32_t)data[6]) << 8;
    eid |=  (uint32_t)data[7];
    out->assigned_entity_id = eid;

    return CE_OK;
}

int ce_game_pack_join_response(uint8_t* buffer,
                               const CeGameJoinResponse* response) {
    if (!buffer || !response) return 0;

    uint8_t* ptr = buffer;

    /* 跳过头部 */
    ptr += CE_GAME_HEADER_SIZE;

    /* result (大端 int32) */
    int32_t result = response->result;
    *ptr++ = (uint8_t)((result >> 24) & 0xFF);
    *ptr++ = (uint8_t)((result >> 16) & 0xFF);
    *ptr++ = (uint8_t)((result >>  8) & 0xFF);
    *ptr++ = (uint8_t)( result        & 0xFF);

    /* assigned_entity_id (大端 uint32) */
    uint32_t eid = response->assigned_entity_id;
    *ptr++ = (uint8_t)((eid >> 24) & 0xFF);
    *ptr++ = (uint8_t)((eid >> 16) & 0xFF);
    *ptr++ = (uint8_t)((eid >>  8) & 0xFF);
    *ptr++ = (uint8_t)( eid        & 0xFF);

    /* 填充总长度 */
    int total_len = (int)(ptr - buffer);
    buffer[0] = (uint8_t)((total_len >> 24) & 0xFF);
    buffer[1] = (uint8_t)((total_len >> 16) & 0xFF);
    buffer[2] = (uint8_t)((total_len >>  8) & 0xFF);
    buffer[3] = (uint8_t)( total_len        & 0xFF);

    return total_len;
}

int ce_game_pack_position_update(uint8_t* buffer,
                                 const CeGamePositionUpdate* update) {
    if (!buffer || !update) return 0;

    uint8_t* ptr = buffer;

    /* 跳过头部 */
    ptr += CE_GAME_HEADER_SIZE;

    /* x, y, z */
    float fbuf[3] = {update->x, update->y, update->z};
    memcpy(ptr, fbuf, sizeof(float) * 3);
    ptr += sizeof(float) * 3;

    /* 填充总长度 */
    int total_len = (int)(ptr - buffer);
    buffer[0] = (uint8_t)((total_len >> 24) & 0xFF);
    buffer[1] = (uint8_t)((total_len >> 16) & 0xFF);
    buffer[2] = (uint8_t)((total_len >>  8) & 0xFF);
    buffer[3] = (uint8_t)( total_len        & 0xFF);

    return total_len;
}

/* ============================================================
 * 游戏会话管理实现
 * ============================================================ */

void ce_game_session_init(CeGameSession* session, float aoi_radius) {
    if (!session) return;

    memset(session, 0, sizeof(CeGameSession));

    if (aoi_radius <= 0.0f) {
        aoi_radius = CE_GAME_DEFAULT_AOI_RADIUS;
    }
    session->aoi_radius = aoi_radius;
    session->next_entity_id = 1;  /* 从 1 开始，0 保留为无效 */

    /* 初始化 AOI 系统 */
    ce_aoi_init(aoi_radius, NULL, NULL);

    session->initialized = CE_TRUE;

    CE_LOG_INFO("GAME", "Game session initialized (aoi_radius=%.1f, max_clients=%d)",
                aoi_radius, CE_GAME_MAX_CLIENTS);
}

void ce_game_session_shutdown(CeGameSession* session) {
    if (!session || !session->initialized) return;

    CE_LOG_INFO("GAME", "Shutting down game session (%d entities)",
                session->entity_count);

    /* 关闭所有客户端连接 */
    for (int i = 0; i < session->entity_count; i++) {
        if (session->entities[i].active) {
            if (session->entities[i].client_addr.fd >= 0) {
                close(session->entities[i].client_addr.fd);
            }
            ce_aoi_leave(session->entities[i].entity_id);
        }
    }

    /* 关闭 AOI 系统 */
    ce_aoi_shutdown();

    memset(session, 0, sizeof(CeGameSession));
    CE_LOG_INFO("GAME", "Game session shut down");
}

CeResult ce_game_session_join(CeGameSession* session,
                              const CeGameClientAddr* client_addr,
                              uint32_t* out_entity_id) {
    if (!session || !session->initialized) return CE_ERR;
    if (!client_addr) return CE_ERR;

    /* 检查服务器是否已满 */
    if (session->entity_count >= CE_GAME_MAX_CLIENTS) {
        CE_LOG_WARN("GAME", "Server full, rejecting join (max=%d)",
                    CE_GAME_MAX_CLIENTS);
        return CE_ERR;
    }

    /* 查找空闲槽位 */
    int slot = -1;
    for (int i = 0; i < CE_GAME_MAX_CLIENTS; i++) {
        if (!session->entities[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return CE_ERR;

    /* 分配实体 ID */
    uint32_t entity_id = session->next_entity_id++;

    /* 初始化实体 */
    CeGameEntity* entity = &session->entities[slot];
    memset(entity, 0, sizeof(CeGameEntity));
    entity->entity_id = entity_id;
    entity->x = CE_GAME_SPAWN_X;
    entity->y = CE_GAME_SPAWN_Y;
    entity->z = CE_GAME_SPAWN_Z;
    entity->client_addr = *client_addr;
    entity->active = CE_TRUE;

    /* 添加到 AOI (2D, 忽略 Z) */
    CeResult aoi_result = ce_aoi_enter(entity_id,
                                       CE_GAME_SPAWN_X,
                                       CE_GAME_SPAWN_Y,
                                       1.0f);
    if (aoi_result != CE_OK) {
        entity->active = CE_FALSE;
        CE_LOG_ERROR("GAME", "Failed to add entity %u to AOI", entity_id);
        return CE_ERR;
    }

    session->entity_count++;

    if (out_entity_id) {
        *out_entity_id = entity_id;
    }

    CE_LOG_INFO("GAME", "Player joined: entity=%u, fd=%d, pos=(%.1f, %.1f, %.1f)",
                entity_id, client_addr->fd,
                CE_GAME_SPAWN_X, CE_GAME_SPAWN_Y, CE_GAME_SPAWN_Z);

    return CE_OK;
}

void ce_game_session_leave(CeGameSession* session, uint32_t entity_id) {
    if (!session || !session->initialized) return;

    CeGameEntity* entity = ce_game_session_find(session, entity_id);
    if (!entity) return;

    CE_LOG_INFO("GAME", "Player left: entity=%u, fd=%d",
                entity_id, entity->client_addr.fd);

    /* 从 AOI 移除 */
    ce_aoi_leave(entity_id);

    /* 关闭连接 */
    if (entity->client_addr.fd >= 0) {
        close(entity->client_addr.fd);
    }

    /* 标记为无效 */
    entity->active = CE_FALSE;
    entity->entity_id = 0;
    session->entity_count--;
}

CeResult ce_game_session_update_position(CeGameSession* session,
                                         uint32_t entity_id,
                                         float x, float y, float z) {
    if (!session || !session->initialized) return CE_ERR;

    CeGameEntity* entity = ce_game_session_find(session, entity_id);
    if (!entity) return CE_ERR;

    /* 更新位置 */
    entity->x = x;
    entity->y = y;
    entity->z = z;

    /* 通知 AOI 系统 (2D) */
    CeResult aoi_result = ce_aoi_move(entity_id, x, y);
    if (aoi_result != CE_OK) {
        CE_LOG_WARN("GAME", "AOI move failed for entity %u", entity_id);
    }

    return CE_OK;
}

CeResult ce_game_session_get_visible(CeGameSession* session,
                                     uint32_t entity_id,
                                     CeGameEntityState* out_buffer,
                                     int* out_count) {
    if (!session || !session->initialized) return CE_ERR;
    if (!out_buffer || !out_count) return CE_ERR;

    CeGameEntity* entity = ce_game_session_find(session, entity_id);
    if (!entity) return CE_ERR;

    /* 查询 AOI 附近的实体 ID */
    CeServerEntityId nearby_ids[CE_GAME_MAX_VISIBLE];
    int nearby_count = ce_aoi_query_nearby(entity_id, nearby_ids,
                                           CE_GAME_MAX_VISIBLE);

    /* 填充输出缓冲区 */
    int written = 0;
    for (int i = 0; i < nearby_count && written < CE_GAME_MAX_VISIBLE; i++) {
        CeGameEntity* nearby = ce_game_session_find(session, nearby_ids[i]);
        if (nearby && nearby->active) {
            out_buffer[written].entity_id = nearby->entity_id;
            out_buffer[written].x = nearby->x;
            out_buffer[written].y = nearby->y;
            out_buffer[written].z = nearby->z;
            written++;
        }
    }

    *out_count = written;
    return CE_OK;
}

CeGameEntity* ce_game_session_find(CeGameSession* session, uint32_t entity_id) {
    if (!session) return NULL;

    for (int i = 0; i < CE_GAME_MAX_CLIENTS; i++) {
        if (session->entities[i].active &&
            session->entities[i].entity_id == entity_id) {
            return &session->entities[i];
        }
    }
    return NULL;
}

int ce_game_session_count(CeGameSession* session) {
    if (!session) return 0;
    return session->entity_count;
}
