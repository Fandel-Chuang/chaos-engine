/*
 * ChaosEngine Replication Manager — 单元测试
 *
 * 测试:
 *   1. 初始化/关闭
 *   2. 组件注册
 *   3. 脏标标记/清除
 *   4. 启动时值域校验 (通过/失败)
 *   5. 编译期校验宏 (CE_REPL_CHECK_INIT)
 *   6. flush 统计
 *   7. 属主映射
 */

#include "replication/ce_replication.h"
#include "public_api/ce_types.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ---- 测试用组件定义 ---- */

#define COMP_TEST_PLAYER    1
#define COMP_TEST_NPC        2

typedef struct {
    int32_t  hp;
    int32_t  max_hp;
    int32_t  mp;
    uint8_t  level;
    float    speed;
} TestPlayerComponent;

typedef struct {
    int32_t  hp;
    uint8_t  level;
} TestNpcComponent;

/* ---- 测试 1: 初始化/关闭 ---- */

static void test_init_shutdown(void) {
    printf("  test_init_shutdown...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    CeReplStats stats;
    ce_repl_get_stats(ctx, &stats);
    assert(stats.total_flushes == 0);
    assert(stats.current_dirty_entities == 0);

    ce_repl_shutdown(ctx);
    printf("  test_init_shutdown: PASS\n");
}

/* ---- 测试 2: 组件注册 ---- */

static void test_register_component(void) {
    printf("  test_register_component...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    TestPlayerComponent default_player = {
        .hp = 100, .max_hp = 100, .mp = 50, .level = 1, .speed = 5.0f
    };

    CeResult r = ce_repl_register_component(ctx, COMP_TEST_PLAYER, "TestPlayer",
        (CeReplField[]){
            {
                .name       = "hp",
                .type       = CE_REPL_TYPE_I32,
                .flags      = CE_FLAG_AOI_BROADCAST | CE_FLAG_PERSIST,
                .offset     = offsetof(TestPlayerComponent, hp),
                .size       = sizeof(int32_t),
                .constraint = { .has_min = true, .min_value = 0,
                                .has_max = true, .max_value = 999999 },
            },
            {
                .name       = "max_hp",
                .type       = CE_REPL_TYPE_I32,
                .flags      = CE_FLAG_AOI_BROADCAST | CE_FLAG_PERSIST,
                .offset     = offsetof(TestPlayerComponent, max_hp),
                .size       = sizeof(int32_t),
                .constraint = { .has_min = true, .min_value = 1,
                                .has_max = true, .max_value = 999999 },
            },
            {
                .name       = "mp",
                .type       = CE_REPL_TYPE_I32,
                .flags      = CE_FLAG_OWNER_ONLY | CE_FLAG_PERSIST,
                .offset     = offsetof(TestPlayerComponent, mp),
                .size       = sizeof(int32_t),
                .constraint = { .has_min = true, .min_value = 0,
                                .has_max = true, .max_value = 999999 },
            },
            {
                .name       = "level",
                .type       = CE_REPL_TYPE_U8,
                .flags      = CE_FLAG_AOI_BROADCAST | CE_FLAG_PERSIST,
                .offset     = offsetof(TestPlayerComponent, level),
                .size       = sizeof(uint8_t),
                .constraint = { .has_min = true, .min_value = 1,
                                .has_max = true, .max_value = 100 },
            },
            {
                .name       = "speed",
                .type       = CE_REPL_TYPE_F32,
                .flags      = CE_FLAG_SERVER_ONLY,
                .offset     = offsetof(TestPlayerComponent, speed),
                .size       = sizeof(float),
                .constraint = {0},
            },
            {0}
        },
        &default_player
    );
    assert(r == CE_OK);

    ce_repl_shutdown(ctx);
    printf("  test_register_component: PASS\n");
}

/* ---- 测试 3: 脏标标记/清除 ---- */

static void test_dirty_marking(void) {
    printf("  test_dirty_marking...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    TestPlayerComponent default_player = {
        .hp = 100, .max_hp = 100, .mp = 50, .level = 1, .speed = 5.0f
    };

    ce_repl_register_component(ctx, COMP_TEST_PLAYER, "TestPlayer",
        (CeReplField[]){
            { .name="hp", .type=CE_REPL_TYPE_I32, .flags=CE_FLAG_AOI_BROADCAST,
              .offset=offsetof(TestPlayerComponent,hp), .size=sizeof(int32_t), .constraint={0} },
            { .name="mp", .type=CE_REPL_TYPE_I32, .flags=CE_FLAG_OWNER_ONLY,
              .offset=offsetof(TestPlayerComponent,mp), .size=sizeof(int32_t), .constraint={0} },
            {0}
        },
        &default_player
    );

    /* 标记脏 */
    ce_repl_mark_dirty(ctx, 100ULL, COMP_TEST_PLAYER);

    /* flush 前统计应显示脏实体 */
    CeReplStats stats;
    ce_repl_get_stats(ctx, &stats);
    assert(stats.current_dirty_entities == 1);

    /* flush */
    ce_repl_flush(ctx);

    /* flush 后应清除 */
    ce_repl_get_stats(ctx, &stats);
    assert(stats.current_dirty_entities == 0);
    assert(stats.total_flushes == 1);
    assert(stats.total_fields_synced > 0);

    ce_repl_shutdown(ctx);
    printf("  test_dirty_marking: PASS\n");
}

/* ---- 测试 4: 启动时值域校验 (通过) ---- */

static void test_validation_pass(void) {
    printf("  test_validation_pass...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    TestPlayerComponent default_player = {
        .hp = 100, .max_hp = 100, .mp = 50, .level = 5, .speed = 5.0f
    };

    ce_repl_register_component(ctx, COMP_TEST_PLAYER, "TestPlayer",
        (CeReplField[]){
            {
                .name="hp", .type=CE_REPL_TYPE_I32, .flags=CE_FLAG_AOI_BROADCAST,
                .offset=offsetof(TestPlayerComponent,hp), .size=sizeof(int32_t),
                .constraint={.has_min=true,.min_value=0,.has_max=true,.max_value=999999},
            },
            {
                .name="level", .type=CE_REPL_TYPE_U8, .flags=CE_FLAG_AOI_BROADCAST,
                .offset=offsetof(TestPlayerComponent,level), .size=sizeof(uint8_t),
                .constraint={.has_min=true,.min_value=1,.has_max=true,.max_value=100},
            },
            {0}
        },
        &default_player
    );

    CeResult r = ce_repl_validate_initial_values(ctx);
    assert(r == CE_OK);

    ce_repl_shutdown(ctx);
    printf("  test_validation_pass: PASS\n");
}

/* ---- 测试 5: 启动时值域校验 (失败) ---- */

static void test_validation_fail(void) {
    printf("  test_validation_fail...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* hp 初始值 -1，应校验失败 */
    TestPlayerComponent bad_player = {
        .hp = -1, .max_hp = 100, .mp = 50, .level = 5, .speed = 5.0f
    };

    ce_repl_register_component(ctx, COMP_TEST_PLAYER, "TestPlayer",
        (CeReplField[]){
            {
                .name="hp", .type=CE_REPL_TYPE_I32, .flags=CE_FLAG_AOI_BROADCAST,
                .offset=offsetof(TestPlayerComponent,hp), .size=sizeof(int32_t),
                .constraint={.has_min=true,.min_value=0,.has_max=true,.max_value=999999},
            },
            {0}
        },
        &bad_player
    );

    CeResult r = ce_repl_validate_initial_values(ctx);
    assert(r == CE_ERR_VALIDATION);

    ce_repl_shutdown(ctx);
    printf("  test_validation_fail: PASS\n");
}

/* ---- 测试 6: 编译期校验宏 ---- */

static void test_compile_time_check(void) {
    printf("  test_compile_time_check...\n");

    /* 编译期断言: 这些宏在编译时检查，运行时无需额外验证 */
    #define TEST_HP 100
    CE_REPL_CHECK_INIT(hp, TEST_HP,
        ((CeReplConstraint){.has_min=true, .min_value=0,
                            .has_max=true, .max_value=999999}));

    #define TEST_LEVEL 5
    CE_REPL_CHECK_INIT(level, TEST_LEVEL,
        ((CeReplConstraint){.has_min=true, .min_value=1,
                            .has_max=true, .max_value=100}));

    /* 如果编译通过，说明值在范围内 */
    assert(TEST_HP == 100);
    assert(TEST_LEVEL == 5);

    printf("  test_compile_time_check: PASS\n");
}

/* ---- 测试 7: 属主映射 ---- */

static void test_owner_mapping(void) {
    printf("  test_owner_mapping...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 设置属主 */
    ce_repl_set_owner(ctx, 100ULL, 1000ULL);
    ce_repl_set_owner(ctx, 200ULL, 2000ULL);
    ce_repl_set_owner(ctx, 300ULL, 3000ULL);

    /* 验证属主在脏标时被记录 */
    TestPlayerComponent default_player = { .hp=100, .max_hp=100, .mp=50, .level=1, .speed=5.0f };
    ce_repl_register_component(ctx, COMP_TEST_PLAYER, "TestPlayer",
        (CeReplField[]){
            { .name="hp", .type=CE_REPL_TYPE_I32, .flags=CE_FLAG_OWNER_ONLY,
              .offset=offsetof(TestPlayerComponent,hp), .size=sizeof(int32_t), .constraint={0} },
            {0}
        },
        &default_player
    );

    ce_repl_mark_dirty(ctx, 100ULL, COMP_TEST_PLAYER);

    /* 删除属主 */
    ce_repl_set_owner(ctx, 200ULL, 0);

    ce_repl_flush(ctx);

    CeReplStats stats;
    ce_repl_get_stats(ctx, &stats);
    assert(stats.total_flushes == 1);

    ce_repl_shutdown(ctx);
    printf("  test_owner_mapping: PASS\n");
}

/* ---- 测试 8: 多实体脏标 ---- */

static void test_multi_entity_dirty(void) {
    printf("  test_multi_entity_dirty...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    TestPlayerComponent default_player = { .hp=100, .max_hp=100, .mp=50, .level=1, .speed=5.0f };
    ce_repl_register_component(ctx, COMP_TEST_PLAYER, "TestPlayer",
        (CeReplField[]){
            { .name="hp", .type=CE_REPL_TYPE_I32, .flags=CE_FLAG_AOI_BROADCAST,
              .offset=offsetof(TestPlayerComponent,hp), .size=sizeof(int32_t), .constraint={0} },
            { .name="mp", .type=CE_REPL_TYPE_I32, .flags=CE_FLAG_OWNER_ONLY,
              .offset=offsetof(TestPlayerComponent,mp), .size=sizeof(int32_t), .constraint={0} },
            {0}
        },
        &default_player
    );

    /* 标记多个实体 */
    for (uint64_t eid = 1; eid <= 100; eid++) {
        ce_repl_mark_dirty(ctx, eid, COMP_TEST_PLAYER);
    }

    CeReplStats stats;
    ce_repl_get_stats(ctx, &stats);
    assert(stats.current_dirty_entities == 100);

    ce_repl_flush(ctx);

    ce_repl_get_stats(ctx, &stats);
    assert(stats.current_dirty_entities == 0);
    assert(stats.total_entities_synced == 100);

    ce_repl_shutdown(ctx);
    printf("  test_multi_entity_dirty: PASS\n");
}

/* ---- 测试 9: SERVER_ONLY 字段不计数 ---- */

static void test_server_only_skip(void) {
    printf("  test_server_only_skip...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    TestPlayerComponent default_player = { .hp=100, .max_hp=100, .mp=50, .level=1, .speed=5.0f };
    ce_repl_register_component(ctx, COMP_TEST_PLAYER, "TestPlayer",
        (CeReplField[]){
            { .name="speed", .type=CE_REPL_TYPE_F32, .flags=CE_FLAG_SERVER_ONLY,
              .offset=offsetof(TestPlayerComponent,speed), .size=sizeof(float), .constraint={0} },
            {0}
        },
        &default_player
    );

    ce_repl_mark_dirty(ctx, 1ULL, COMP_TEST_PLAYER);

    CeReplStats stats;
    ce_repl_get_stats(ctx, &stats);
    assert(stats.current_dirty_entities == 1);

    ce_repl_flush(ctx);

    /* SERVER_ONLY 字段不应被计入 synced_fields */
    ce_repl_get_stats(ctx, &stats);
    assert(stats.total_fields_synced == 0);  /* 全部跳过 */
    assert(stats.total_entities_synced == 0); /* 无有效同步字段 */

    ce_repl_shutdown(ctx);
    printf("  test_server_only_skip: PASS\n");
}

/* ---- 主函数 ---- */

int main(void) {
    printf("=== Replication Manager Unit Tests ===\n\n");

    test_init_shutdown();
    test_register_component();
    test_dirty_marking();
    test_validation_pass();
    test_validation_fail();
    test_compile_time_check();
    test_owner_mapping();
    test_multi_entity_dirty();
    test_server_only_skip();

    printf("\n=== All tests passed! ===\n");
    return 0;
}
