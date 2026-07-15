/*
 * ChaosEngine ECS 单元测试
 * 测试: ce_entity_create/destroy/is_alive, ce_entity_add_component/get_component/has_component
 */

#include "public_api/ce_types.h"
#include "public_api/ce_ecs.h"
#include "ecs/ce_ecs_internal.h"
#include "core/ce_memory.h"
#include "replication/ce_replication.h"
#include <stdio.h>
#include <string.h>

#define TEST(name) printf("  TEST: %s ... ", name)
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while(0)
#define PASS() printf("PASS\n")

/* 测试用的组件类型 */
typedef struct {
    float x, y, z;
} Position;

typedef struct {
    float vx, vy, vz;
} Velocity;

typedef struct {
    int hp;
    int max_hp;
} Health;

/* 复制脏标测试用的编辑回调 */
static void health_edit_callback(void* component, void* user_data) {
    (void)user_data;
    Health* h = (Health*)component;
    h->hp = 75;
}

int main(void) {
    printf("=== ECS Tests ===\n");

    /* 初始化 ECS */
    CeResult init_result = ce_ecs_init(NULL);
    CHECK(init_result == CE_OK);

    /* 注册组件 */
    CeComponentId pos_id = ce_component_register("Position", sizeof(Position), 4);
    CeComponentId vel_id = ce_component_register("Velocity", sizeof(Velocity), 4);
    CeComponentId hp_id  = ce_component_register("Health", sizeof(Health), 4);
    CHECK(pos_id != (CeComponentId)-1);
    CHECK(vel_id != (CeComponentId)-1);
    CHECK(hp_id != (CeComponentId)-1);

    /* ---- entity_create ---- */
    TEST("entity_create");
    {
        CeEntity e = ce_entity_create();
        /* 第一个实体 index=0, gen=0，值可能为 0 == CE_ENTITY_NULL，
           但 is_alive 才是正确判断方式 */
        CHECK(ce_entity_is_alive(e) == CE_TRUE);
    }
    PASS();

    /* ---- entity_is_alive ---- */
    TEST("entity_is_alive_new");
    {
        CeEntity e = ce_entity_create();
        CHECK(ce_entity_is_alive(e) == CE_TRUE);
        ce_entity_destroy(e);
    }
    PASS();

    /* ---- entity_destroy ---- */
    TEST("entity_destroy");
    {
        CeEntity e = ce_entity_create();
        CHECK(ce_entity_is_alive(e) == CE_TRUE);
        ce_entity_destroy(e);
        CHECK(ce_entity_is_alive(e) == CE_FALSE);
    }
    PASS();

    /* ---- entity_create_reuse ---- */
    TEST("entity_create_reuse_index");
    {
        CeEntity e1 = ce_entity_create();
        uint32_t idx1 = CE_ENTITY_INDEX(e1);
        ce_entity_destroy(e1);

        CeEntity e2 = ce_entity_create();
        uint32_t idx2 = CE_ENTITY_INDEX(e2);
        /* 索引应被复用 */
        CHECK(idx1 == idx2);
        /* 但 generation 不同，所以 e1 != e2 */
        CHECK(e1 != e2);
        CHECK(ce_entity_is_alive(e1) == CE_FALSE);
        CHECK(ce_entity_is_alive(e2) == CE_TRUE);
        ce_entity_destroy(e2);
    }
    PASS();

    /* ---- entity_add_component ---- */
    TEST("entity_add_component");
    {
        CeEntity e = ce_entity_create();
        Position* pos = (Position*)ce_entity_add_component(e, pos_id);
        CHECK(pos != NULL);
        pos->x = 1.0f;
        pos->y = 2.0f;
        pos->z = 3.0f;
        ce_entity_destroy(e);
    }
    PASS();

    /* ---- entity_get_component ---- */
    TEST("entity_get_component");
    {
        CeEntity e = ce_entity_create();
        Position* pos = (Position*)ce_entity_add_component(e, pos_id);
        pos->x = 10.0f;
        pos->y = 20.0f;
        pos->z = 30.0f;

        const Position* got = (const Position*)ce_entity_get_component(e, pos_id);
        CHECK(got != NULL);
        CHECK(got->x == 10.0f && got->y == 20.0f && got->z == 30.0f);
        ce_entity_destroy(e);
    }
    PASS();

    /* ---- entity_has_component ---- */
    TEST("entity_has_component");
    {
        CeEntity e = ce_entity_create();
        CHECK(ce_entity_has_component(e, pos_id) == CE_FALSE);

        ce_entity_add_component(e, pos_id);
        CHECK(ce_entity_has_component(e, pos_id) == CE_TRUE);
        CHECK(ce_entity_has_component(e, vel_id) == CE_FALSE);
        ce_entity_destroy(e);
    }
    PASS();

    /* ---- entity_multiple_components ---- */
    TEST("entity_multiple_components");
    {
        CeEntity e = ce_entity_create();

        Position* pos = (Position*)ce_entity_add_component(e, pos_id);
        pos->x = 5.0f; pos->y = 6.0f; pos->z = 7.0f;

        Velocity* vel = (Velocity*)ce_entity_add_component(e, vel_id);
        vel->vx = 1.0f; vel->vy = 0.0f; vel->vz = -1.0f;

        Health* hp = (Health*)ce_entity_add_component(e, hp_id);
        hp->hp = 100; hp->max_hp = 100;

        CHECK(ce_entity_has_component(e, pos_id) == CE_TRUE);
        CHECK(ce_entity_has_component(e, vel_id) == CE_TRUE);
        CHECK(ce_entity_has_component(e, hp_id) == CE_TRUE);

        const Position* got_pos = (const Position*)ce_entity_get_component(e, pos_id);
        CHECK(got_pos->x == 5.0f && got_pos->y == 6.0f && got_pos->z == 7.0f);

        const Velocity* got_vel = (const Velocity*)ce_entity_get_component(e, vel_id);
        CHECK(got_vel->vx == 1.0f && got_vel->vy == 0.0f && got_vel->vz == -1.0f);

        const Health* got_hp = (const Health*)ce_entity_get_component(e, hp_id);
        CHECK(got_hp->hp == 100 && got_hp->max_hp == 100);

        ce_entity_destroy(e);
    }
    PASS();

    /* ---- entity_get_component_dead ---- */
    TEST("entity_get_component_dead");
    {
        CeEntity e = ce_entity_create();
        ce_entity_add_component(e, pos_id);
        ce_entity_destroy(e);

        const void* got = ce_entity_get_component(e, pos_id);
        CHECK(got == NULL);
        CHECK(ce_entity_has_component(e, pos_id) == CE_FALSE);
    }
    PASS();

    /* ---- entity_get_component_nonexistent ---- */
    TEST("entity_get_component_nonexistent");
    {
        CeEntity e = ce_entity_create();
        const void* got = ce_entity_get_component(e, pos_id);
        CHECK(got == NULL);
        ce_entity_destroy(e);
    }
    PASS();

    /* ---- entity_add_component_twice ---- */
    TEST("entity_add_component_twice");
    {
        CeEntity e = ce_entity_create();
        Position* p1 = (Position*)ce_entity_add_component(e, pos_id);
        p1->x = 42.0f;

        /* 再次添加同一组件应返回同一指针 */
        Position* p2 = (Position*)ce_entity_add_component(e, pos_id);
        CHECK(p1 == p2);
        CHECK(p2->x == 42.0f);

        ce_entity_destroy(e);
    }
    PASS();

    /* ---- entity_count ---- */
    TEST("entity_count");
    {
        uint32_t before = ce_ecs_get_entity_count();

        CeEntity e1 = ce_entity_create();
        CeEntity e2 = ce_entity_create();
        CHECK(ce_ecs_get_entity_count() == before + 2);

        ce_entity_destroy(e1);
        CHECK(ce_ecs_get_entity_count() == before + 1);

        ce_entity_destroy(e2);
        CHECK(ce_ecs_get_entity_count() == before);
    }
    PASS();

    /* ---- replication_dirty_marking ---- */
    TEST("replication_dirty_marking");
    {
        /* 初始化复制管理器 */
        CeReplContext* repl = ce_repl_init(NULL);
        CHECK(repl != NULL);

        /* 注册 Health 组件的可复制字段 */
        CeReplField health_fields[] = {
            { "hp",     CE_REPL_TYPE_I32, CE_FLAG_AOI_BROADCAST,
              (uint32_t)__builtin_offsetof(Health, hp),     sizeof(int), {0} },
            { "max_hp", CE_REPL_TYPE_I32, CE_FLAG_SERVER_ONLY,
              (uint32_t)__builtin_offsetof(Health, max_hp), sizeof(int), {0} },
            { NULL, 0, 0, 0, 0, {0} }  /* 终止标记 */
        };
        Health default_health = { 100, 100 };
        CeResult reg_result = ce_repl_register_component(
            repl, hp_id, "Health", health_fields, &default_health);
        CHECK(reg_result == CE_OK);

        /* 注入复制上下文到 ECS */
        ce_ecs_set_replication_context(repl);

        /* 创建实体并添加组件 (应自动标记脏) */
        CeEntity e = ce_entity_create();
        CHECK(ce_entity_is_alive(e) == CE_TRUE);

        Health* hp = (Health*)ce_entity_add_component(e, hp_id);
        CHECK(hp != NULL);
        hp->hp = 50;
        hp->max_hp = 100;

        /* 修改组件 (通过 edit 接口，也应标记脏) */
        ce_entity_edit_component(e, hp_id, health_edit_callback, NULL);

        /* flush 复制管线 */
        ce_repl_flush(repl);

        /* 验证实体被同步 */
        CeReplStats stats;
        ce_repl_get_stats(repl, &stats);
        CHECK(stats.total_entities_synced >= 1);
        CHECK(stats.total_flushes == 1);

        /* 清除上下文 (headless 模式) */
        ce_ecs_set_replication_context(NULL);

        /* 清理 */
        ce_entity_destroy(e);
        ce_repl_shutdown(repl);
    }
    PASS();

    /* ---- multi_world (Phase 1.2 实例化测试) ---- */
    TEST("multi_world_isolation");
    {
        /* 创建两个独立的 world */
        CeEcsWorld* world_a = ce_ecs_world_create();
        CeEcsWorld* world_b = ce_ecs_world_create();
        CHECK(world_a != NULL);
        CHECK(world_b != NULL);
        CHECK(world_a != world_b);

        /* 在 world_a 中注册组件 */
        CeComponentId pos_a = ce_component_register_world(world_a, "Position", sizeof(Position), 4);
        CHECK(pos_a != (CeComponentId)-1);

        /* 在 world_b 中注册同名组件，ID 应各自独立 */
        CeComponentId pos_b = ce_component_register_world(world_b, "Position", sizeof(Position), 4);
        CHECK(pos_b != (CeComponentId)-1);
        CHECK(pos_a == pos_b);  /* 两个 world 独立计数，ID 都从 0 开始 */

        /* 在 world_a 创建实体并添加组件 */
        CeEntity ea = ce_entity_create_world(world_a);
        CHECK(ce_entity_is_alive_world(world_a, ea) == CE_TRUE);

        Position* pa = (Position*)ce_entity_add_component_world(world_a, ea, pos_a);
        CHECK(pa != NULL);
        pa->x = 100.0f; pa->y = 200.0f; pa->z = 300.0f;

        /* 在 world_b 创建实体并添加组件 */
        CeEntity eb = ce_entity_create_world(world_b);
        CHECK(ce_entity_is_alive_world(world_b, eb) == CE_TRUE);

        Position* pb = (Position*)ce_entity_add_component_world(world_b, eb, pos_b);
        CHECK(pb != NULL);
        pb->x = 1.0f; pb->y = 2.0f; pb->z = 3.0f;

        /* 验证隔离: world_a 的实体在 world_b 中不应存活 (index 相同但 generation 不同) */
        /* 注意: 如果 index 相同且 generation 相同，is_alive 可能返回 true，
           但这是合理的——隔离保证的是数据不串，而非实体 ID 唯一 */
        CHECK(ce_ecs_get_entity_count_world(world_a) == 1);
        CHECK(ce_ecs_get_entity_count_world(world_b) == 1);

        /* 验证数据隔离: world_a 的 Position 值未被 world_b 影响 */
        const Position* got_a = (const Position*)ce_entity_get_component_world(world_a, ea, pos_a);
        CHECK(got_a != NULL);
        CHECK(got_a->x == 100.0f && got_a->y == 200.0f && got_a->z == 300.0f);

        const Position* got_b = (const Position*)ce_entity_get_component_world(world_b, eb, pos_b);
        CHECK(got_b != NULL);
        CHECK(got_b->x == 1.0f && got_b->y == 2.0f && got_b->z == 3.0f);

        /* 销毁 world_a 中的实体，不影响 world_b */
        ce_entity_destroy_world(world_a, ea);
        CHECK(ce_entity_is_alive_world(world_a, ea) == CE_FALSE);
        CHECK(ce_entity_is_alive_world(world_b, eb) == CE_TRUE);
        CHECK(ce_ecs_get_entity_count_world(world_a) == 0);
        CHECK(ce_ecs_get_entity_count_world(world_b) == 1);

        /* 组件计数隔离 */
        CHECK(ce_ecs_get_component_count_world(world_a) == 1);
        CHECK(ce_ecs_get_component_count_world(world_b) == 1);

        /* 清理 */
        ce_ecs_world_destroy(world_a);
        ce_ecs_world_destroy(world_b);
    }
    PASS();

    /* 清理 */
    ce_ecs_shutdown();

    printf("\nAll ECS tests passed!\n");
    return 0;
}
