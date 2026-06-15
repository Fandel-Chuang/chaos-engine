/*
 * ChaosEngine ECS 单元测试
 * 测试: ce_entity_create/destroy/is_alive, ce_entity_add_component/get_component/has_component
 */

#include "public_api/ce_types.h"
#include "public_api/ce_ecs.h"
#include "ecs/ce_ecs_internal.h"
#include "core/ce_memory.h"
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

    /* 清理 */
    ce_ecs_shutdown();

    printf("\nAll ECS tests passed!\n");
    return 0;
}
