/*
 * ChaosEngine Game Session — 单元测试
 *
 * 测试游戏会话管理器的核心功能：
 *   - 初始化和关闭
 *   - 玩家加入/离开
 *   - 位置更新
 *   - AOI 可见性
 */

#include "server/ce_game_session.h"
#include "server/ce_game_protocol.h"
#include "server/ce_aoi.h"
#include "public_api/ce_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- 测试辅助 ---- */

static int g_event_count = 0;

static void reset_events(void) {
    g_event_count = 0;
}

/* ---- 测试用例 ---- */

static void test_session_init_shutdown(void) {
    printf("  test_session_init_shutdown... ");

    CeGameSession session;
    ce_game_session_init(&session, 0.0f);

    assert(session.initialized == CE_TRUE);
    assert(session.entity_count == 0);
    assert(session.next_entity_id == 1);
    assert(session.aoi_radius == CE_GAME_DEFAULT_AOI_RADIUS);

    /* 重复 init 应安全 */
    ce_game_session_init(&session, 0.0f);

    ce_game_session_shutdown(&session);
    assert(session.initialized == CE_FALSE);

    /* 重复 shutdown 应安全 */
    ce_game_session_shutdown(&session);

    printf("PASS\n");
}

static void test_session_join_leave(void) {
    printf("  test_session_join_leave... ");

    CeGameSession session;
    ce_game_session_init(&session, 0.0f);

    CeGameClientAddr addr;
    memset(&addr, 0, sizeof(addr));
    addr.fd = 100;
    strcpy(addr.host, "test-client");
    addr.port = 7777;

    uint32_t entity_id = 0;
    CeResult result = ce_game_session_join(&session, &addr, &entity_id);
    assert(result == CE_OK);
    assert(entity_id == 1);
    assert(session.entity_count == 1);

    /* 验证实体数据 */
    CeGameEntity* entity = ce_game_session_find(&session, entity_id);
    assert(entity != NULL);
    assert(entity->active == CE_TRUE);
    assert(entity->x == CE_GAME_SPAWN_X);
    assert(entity->y == CE_GAME_SPAWN_Y);
    assert(entity->z == CE_GAME_SPAWN_Z);
    assert(entity->client_addr.fd == 100);

    /* 第二个玩家加入 */
    CeGameClientAddr addr2;
    memset(&addr2, 0, sizeof(addr2));
    addr2.fd = 101;
    strcpy(addr2.host, "test-client-2");
    addr2.port = 7777;

    uint32_t entity_id2 = 0;
    result = ce_game_session_join(&session, &addr2, &entity_id2);
    assert(result == CE_OK);
    assert(entity_id2 == 2);
    assert(session.entity_count == 2);

    /* 玩家离开 */
    ce_game_session_leave(&session, entity_id);
    assert(session.entity_count == 1);
    assert(ce_game_session_find(&session, entity_id) == NULL);

    /* 重复离开应安全 */
    ce_game_session_leave(&session, entity_id);

    /* 查找不存在的实体 */
    assert(ce_game_session_find(&session, 9999) == NULL);

    ce_game_session_shutdown(&session);
    printf("PASS\n");
}

static void test_session_position_update(void) {
    printf("  test_session_position_update... ");

    CeGameSession session;
    ce_game_session_init(&session, 0.0f);

    CeGameClientAddr addr;
    memset(&addr, 0, sizeof(addr));
    addr.fd = 200;

    uint32_t entity_id = 0;
    ce_game_session_join(&session, &addr, &entity_id);
    assert(entity_id == 1);

    /* 更新位置 */
    CeResult result = ce_game_session_update_position(&session, entity_id,
                                                      10.0f, 20.0f, 30.0f);
    assert(result == CE_OK);

    /* 验证位置已更新 */
    CeGameEntity* entity = ce_game_session_find(&session, entity_id);
    assert(entity != NULL);
    assert(entity->x == 10.0f);
    assert(entity->y == 20.0f);
    assert(entity->z == 30.0f);

    /* 更新不存在的实体应失败 */
    result = ce_game_session_update_position(&session, 9999, 0, 0, 0);
    assert(result == CE_ERR);

    ce_game_session_shutdown(&session);
    printf("PASS\n");
}

static void test_session_visibility(void) {
    printf("  test_session_visibility... ");

    CeGameSession session;
    ce_game_session_init(&session, 100.0f);  /* AOI radius = 100 */

    /* 玩家 A 在 (0, 0, 0) */
    CeGameClientAddr addr_a;
    memset(&addr_a, 0, sizeof(addr_a));
    addr_a.fd = 300;
    uint32_t entity_a = 0;
    ce_game_session_join(&session, &addr_a, &entity_a);

    /* 玩家 B 在 (0, 0, 0) — 也在生成点，应在 AOI 范围内 */
    CeGameClientAddr addr_b;
    memset(&addr_b, 0, sizeof(addr_b));
    addr_b.fd = 301;
    uint32_t entity_b = 0;
    ce_game_session_join(&session, &addr_b, &entity_b);

    /* 玩家 A 应能看到玩家 B */
    CeGameEntityState visible[CE_GAME_MAX_VISIBLE];
    int visible_count = 0;
    CeResult result = ce_game_session_get_visible(&session, entity_a,
                                                  visible, &visible_count);
    assert(result == CE_OK);
    assert(visible_count == 1);
    assert(visible[0].entity_id == entity_b);

    /* 玩家 B 应能看到玩家 A */
    visible_count = 0;
    result = ce_game_session_get_visible(&session, entity_b,
                                         visible, &visible_count);
    assert(result == CE_OK);
    assert(visible_count == 1);
    assert(visible[0].entity_id == entity_a);

    /* 移动玩家 B 到远处 (200, 0, 0) — 超出 AOI 范围 */
    ce_game_session_update_position(&session, entity_b, 200.0f, 0.0f, 0.0f);

    /* 玩家 A 不再能看到玩家 B */
    visible_count = 0;
    result = ce_game_session_get_visible(&session, entity_a,
                                         visible, &visible_count);
    assert(result == CE_OK);
    assert(visible_count == 0);

    /* 玩家 B 不再能看到玩家 A */
    visible_count = 0;
    result = ce_game_session_get_visible(&session, entity_b,
                                         visible, &visible_count);
    assert(result == CE_OK);
    assert(visible_count == 0);

    /* 移动玩家 B 回到近处 (50, 0, 0) — 应在 AOI 范围内 */
    ce_game_session_update_position(&session, entity_b, 50.0f, 0.0f, 0.0f);

    /* 玩家 A 应重新看到玩家 B */
    visible_count = 0;
    result = ce_game_session_get_visible(&session, entity_a,
                                         visible, &visible_count);
    assert(result == CE_OK);
    assert(visible_count == 1);
    assert(visible[0].entity_id == entity_b);

    ce_game_session_shutdown(&session);
    printf("PASS\n");
}

static void test_session_max_clients(void) {
    printf("  test_session_max_clients... ");

    CeGameSession session;
    ce_game_session_init(&session, 100.0f);

    /* 填满服务器 */
    int joined = 0;
    for (int i = 0; i < CE_GAME_MAX_CLIENTS + 10; i++) {
        CeGameClientAddr addr;
        memset(&addr, 0, sizeof(addr));
        addr.fd = 400 + i;

        uint32_t entity_id = 0;
        CeResult result = ce_game_session_join(&session, &addr, &entity_id);
        if (result == CE_OK) {
            joined++;
        }
    }

    /* 应只能加入 CE_GAME_MAX_CLIENTS 个 */
    assert(joined == CE_GAME_MAX_CLIENTS);
    assert(session.entity_count == CE_GAME_MAX_CLIENTS);

    /* 离开一个 */
    ce_game_session_leave(&session, 1);
    assert(session.entity_count == CE_GAME_MAX_CLIENTS - 1);

    /* 应能再加入一个 */
    CeGameClientAddr addr;
    memset(&addr, 0, sizeof(addr));
    addr.fd = 500;
    uint32_t entity_id = 0;
    CeResult result = ce_game_session_join(&session, &addr, &entity_id);
    assert(result == CE_OK);
    assert(session.entity_count == CE_GAME_MAX_CLIENTS);

    ce_game_session_shutdown(&session);
    printf("PASS\n");
}

static void test_session_count(void) {
    printf("  test_session_count... ");

    CeGameSession session;
    ce_game_session_init(&session, 0.0f);

    assert(ce_game_session_count(&session) == 0);
    assert(ce_game_session_count(NULL) == 0);

    CeGameClientAddr addr;
    memset(&addr, 0, sizeof(addr));
    addr.fd = 600;

    uint32_t eid;
    ce_game_session_join(&session, &addr, &eid);
    assert(ce_game_session_count(&session) == 1);

    ce_game_session_leave(&session, eid);
    assert(ce_game_session_count(&session) == 0);

    ce_game_session_shutdown(&session);
    printf("PASS\n");
}

int main(void) {
    printf("=== Game Session Tests ===\n");

    test_session_init_shutdown();
    test_session_join_leave();
    test_session_position_update();
    test_session_visibility();
    test_session_max_clients();
    test_session_count();

    printf("\nAll game session tests passed!\n");
    return 0;
}
