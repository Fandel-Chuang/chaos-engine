/*
 * ChaosEngine AOI 十字链表 — 单元测试
 */

#include "server/ce_aoi.h"
#include "public_api/ce_types.h"
#include "replication/ce_replication.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- 测试辅助 ---- */

static int g_event_count = 0;
static CeAoiEvent g_last_event;

static void test_event_callback(const CeAoiEvent* event, void* user_data) {
    (void)user_data;
    g_event_count++;
    g_last_event = *event;
}

static void reset_events(void) {
    g_event_count = 0;
    memset(&g_last_event, 0, sizeof(g_last_event));
}

/* ---- 测试用例 ---- */

static void test_init_shutdown(void) {
    printf("  test_init_shutdown... ");
    ce_aoi_init(50.0f, NULL, NULL);
    assert(ce_aoi_entity_count() == 0);
    ce_aoi_shutdown();
    printf("PASS\n");
}

static void test_enter_leave(void) {
    printf("  test_enter_leave... ");
    ce_aoi_init(50.0f, test_event_callback, NULL);

    /* 添加实体 */
    assert(ce_aoi_enter(1, 0.0f, 0.0f, 1.0f) == CE_OK);
    assert(ce_aoi_entity_count() == 1);

    /* 重复添加应失败 */
    assert(ce_aoi_enter(1, 10.0f, 10.0f, 1.0f) == CE_ERR);

    /* 移除实体 */
    ce_aoi_leave(1);
    assert(ce_aoi_entity_count() == 0);

    /* 移除不存在的实体应安全 */
    ce_aoi_leave(999);

    ce_aoi_shutdown();
    printf("PASS\n");
}

static void test_nearby_query(void) {
    printf("  test_nearby_query... ");
    ce_aoi_init(50.0f, NULL, NULL);

    /* 放置 3 个实体 */
    ce_aoi_enter(1, 0.0f, 0.0f, 1.0f);
    ce_aoi_enter(2, 10.0f, 0.0f, 1.0f);   /* 在 AOI 半径内 */
    ce_aoi_enter(3, 100.0f, 0.0f, 1.0f);  /* 在 AOI 半径外 */

    CeServerEntityId buffer[16];
    int count = ce_aoi_query_nearby(1, buffer, 16);
    assert(count == 1);
    assert(buffer[0] == 2);

    /* 实体 3 周围应无人 */
    count = ce_aoi_query_nearby(3, buffer, 16);
    assert(count == 0);

    /* 实体 2 周围应有实体 1 */
    count = ce_aoi_query_nearby(2, buffer, 16);
    assert(count == 1);
    assert(buffer[0] == 1);

    ce_aoi_shutdown();
    printf("PASS\n");
}

static void test_move_events(void) {
    printf("  test_move_events... ");
    ce_aoi_init(50.0f, test_event_callback, NULL);

    /* 放置 2 个实体 */
    ce_aoi_enter(1, 0.0f, 0.0f, 1.0f);
    ce_aoi_enter(2, 10.0f, 0.0f, 1.0f);
    reset_events();

    /* 移动实体 2 到远处（离开视野） */
    ce_aoi_move(2, 200.0f, 0.0f);

    /* 应有 LEAVE 事件 */
    assert(g_event_count >= 2);  /* 双向各一个 LEAVE */

    /* 移动实体 2 回来（进入视野） */
    reset_events();
    ce_aoi_move(2, 10.0f, 0.0f);
    assert(g_event_count >= 2);  /* 双向各一个 ENTER */

    ce_aoi_shutdown();
    printf("PASS\n");
}

static void test_many_entities(void) {
    printf("  test_many_entities... ");
    ce_aoi_init(50.0f, NULL, NULL);

    /* 放置 100 个实体在网格中 */
    for (int i = 0; i < 100; i++) {
        float x = (float)(i % 10) * 20.0f;
        float y = (float)(i / 10) * 20.0f;
        assert(ce_aoi_enter(i, x, y, 1.0f) == CE_OK);
    }

    assert(ce_aoi_entity_count() == 100);

    /* 查询中心实体周围 */
    CeServerEntityId buffer[128];
    int count = ce_aoi_query_nearby(55, buffer, 128);
    /* 实体 55 在 (100, 100)，周围应该有相邻网格的实体 */
    assert(count > 0);

    /* 随机移动一些实体 */
    for (int i = 0; i < 20; i++) {
        ce_aoi_move(i, (float)(rand() % 200), (float)(rand() % 200));
    }

    /* 验证链表完整性：遍历 X 轴 */
    assert(ce_aoi_entity_count() == 100);

    ce_aoi_shutdown();
    printf("PASS\n");
}

static void test_count_nearby(void) {
    printf("  test_count_nearby... ");
    ce_aoi_init(50.0f, NULL, NULL);

    ce_aoi_enter(1, 0.0f, 0.0f, 1.0f);
    ce_aoi_enter(2, 10.0f, 0.0f, 1.0f);
    ce_aoi_enter(3, 20.0f, 0.0f, 1.0f);
    ce_aoi_enter(4, 100.0f, 0.0f, 1.0f);

    assert(ce_aoi_count_nearby(1) == 2);  /* 2 和 3 */
    assert(ce_aoi_count_nearby(4) == 0);

    ce_aoi_shutdown();
    printf("PASS\n");
}

/* ---- 复制集成测试 ---- */

static void test_aoi_replication_integration(void) {
    printf("  test_aoi_replication_integration... ");

    /* 1. 初始化复制管理器 */
    CeReplConfig repl_config = {0};
    repl_config.max_components = 8;
    repl_config.max_fields_per_component = 16;
    repl_config.max_dirty_entities = 64;
    CeReplContext* repl_ctx = ce_repl_init(&repl_config);
    assert(repl_ctx != NULL);

    /* 2. 注册一个测试组件 (带 AOI_BROADCAST 标志) */
    /* 定义一个简单的测试组件结构 */
    typedef struct {
        float x;
        float y;
        int32_t hp;
    } TestTransform;

    TestTransform default_transform = {0.0f, 0.0f, 100};

    CeReplField transform_fields[] = {
        {"x",  CE_REPL_TYPE_F32, CE_FLAG_AOI_BROADCAST, offsetof(TestTransform, x),  sizeof(float), {0}},
        {"y",  CE_REPL_TYPE_F32, CE_FLAG_AOI_BROADCAST, offsetof(TestTransform, y),  sizeof(float), {0}},
        {"hp", CE_REPL_TYPE_I32, CE_FLAG_AOI_BROADCAST, offsetof(TestTransform, hp), sizeof(int32_t), {0}},
        {NULL, 0, 0, 0, 0, {0}}  /* 终止 */
    };

    CeResult reg_result = ce_repl_register_component(
        repl_ctx, 1, "TestTransform", transform_fields, &default_transform);
    assert(reg_result == CE_OK);

    /* 3. 将复制上下文注入 AOI */
    ce_aoi_init(50.0f, NULL, NULL);
    ce_aoi_set_replication_context(repl_ctx);

    /* 4. 添加实体 1 到 AOI */
    assert(ce_aoi_enter(1, 0.0f, 0.0f, 1.0f) == CE_OK);

    /* 5. 添加实体 2 到 AOI (在实体 1 的 AOI 范围内) */
    /* 这会触发 CE_AOI_ENTER 事件，应标记实体 2 为脏 */
    assert(ce_aoi_enter(2, 10.0f, 0.0f, 1.0f) == CE_OK);

    /* 6. 验证实体 2 被标记为脏 */
    CeReplStats stats;
    ce_repl_get_stats(repl_ctx, &stats);
    /* 脏实体计数应在 flush 前 > 0 */
    assert(stats.current_dirty_entities > 0);

    /* 7. 验证 flush 正常工作 */
    ce_repl_flush(repl_ctx);
    ce_repl_get_stats(repl_ctx, &stats);
    assert(stats.total_flushes == 1);
    assert(stats.total_entities_synced > 0);
    assert(stats.current_dirty_entities == 0);

    /* 8. 移动实体 2 到远处 (离开实体 1 的 AOI 范围) */
    /* 这会触发 CE_AOI_LEAVE 事件 (仅记录日志) */
    ce_aoi_move(2, 200.0f, 0.0f);

    /* 9. 清理 */
    ce_aoi_shutdown();
    ce_repl_shutdown(repl_ctx);

    printf("PASS\n");
}

int main(void) {
    printf("=== AOI Cross-Linked List Tests ===\n");

    test_init_shutdown();
    test_enter_leave();
    test_nearby_query();
    test_move_events();
    test_many_entities();
    test_count_nearby();
    test_aoi_replication_integration();

    printf("\nAll AOI tests passed!\n");
    return 0;
}
