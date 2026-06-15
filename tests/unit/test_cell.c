/*
 * ChaosEngine Cell 大地图管理 — 单元测试
 */

#include "server/ce_cell.h"
#include "server/ce_aoi.h"
#include "public_api/ce_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_init_shutdown(void) {
    printf("  test_init_shutdown... ");
    assert(ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20) == CE_OK);
    assert(ce_cell_count() == 100);  /* 10x10 grid */
    ce_cell_shutdown();
    printf("PASS\n");
}

static void test_find_by_position(void) {
    printf("  test_find_by_position... ");
    ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20);

    CeCellId c1 = ce_cell_find_by_position(50.0f, 50.0f);
    CeCellId c2 = ce_cell_find_by_position(150.0f, 50.0f);
    CeCellId c3 = ce_cell_find_by_position(50.0f, 150.0f);
    CeCellId c4 = ce_cell_find_by_position(950.0f, 950.0f);

    assert(c1 != CE_INVALID_CELL_ID);
    assert(c2 != CE_INVALID_CELL_ID);
    assert(c3 != CE_INVALID_CELL_ID);
    assert(c4 != CE_INVALID_CELL_ID);

    /* 不同位置应返回不同 Cell */
    assert(c1 != c2);
    assert(c1 != c3);

    /* 边界外 */
    CeCellId out = ce_cell_find_by_position(-10.0f, -10.0f);
    assert(out == CE_INVALID_CELL_ID);

    ce_cell_shutdown();
    printf("PASS\n");
}

static void test_cell_get(void) {
    printf("  test_cell_get... ");
    ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20);

    CeCellId cid = ce_cell_find_by_position(50.0f, 50.0f);
    const CeCell* cell = ce_cell_get(cid);
    assert(cell != NULL);
    assert(cell->id == cid);
    assert(cell->state == CE_CELL_ACTIVE);
    assert(cell->entity_count == 0);

    /* 无效 ID */
    assert(ce_cell_get(CE_INVALID_CELL_ID) == NULL);

    ce_cell_shutdown();
    printf("PASS\n");
}

static void test_enter_leave_entity(void) {
    printf("  test_enter_leave_entity... ");
    ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20);

    assert(ce_cell_enter_entity(1, 50.0f, 50.0f, 1.0f) == CE_OK);
    assert(ce_cell_enter_entity(2, 150.0f, 50.0f, 1.0f) == CE_OK);
    assert(ce_cell_enter_entity(3, 50.0f, 150.0f, 1.0f) == CE_OK);

    assert(ce_aoi_entity_count() == 3);

    ce_cell_leave_entity(1);
    assert(ce_aoi_entity_count() == 2);

    ce_cell_shutdown();
    printf("PASS\n");
}

static void test_move_entity(void) {
    printf("  test_move_entity... ");
    ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20);

    ce_cell_enter_entity(1, 50.0f, 50.0f, 1.0f);
    assert(ce_cell_move_entity(1, 60.0f, 60.0f) == CE_OK);

    /* 跨 Cell 移动 */
    assert(ce_cell_move_entity(1, 150.0f, 50.0f) == CE_OK);

    ce_cell_shutdown();
    printf("PASS\n");
}

static void test_split(void) {
    printf("  test_split... ");
    ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20);

    CeCellId cid = ce_cell_find_by_position(50.0f, 50.0f);
    int old_count = ce_cell_count();

    assert(ce_cell_split(cid) == CE_OK);

    /* 分裂后 Cell 数量应增加 3 */
    assert(ce_cell_count() == old_count + 3);

    /* 原 Cell 仍应存在 */
    const CeCell* cell = ce_cell_get(cid);
    assert(cell != NULL);
    assert(cell->state == CE_CELL_ACTIVE);

    ce_cell_shutdown();
    printf("PASS\n");
}

static void test_merge(void) {
    printf("  test_merge... ");
    ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20);

    CeCellId c1 = ce_cell_find_by_position(50.0f, 50.0f);
    CeCellId c2 = ce_cell_find_by_position(150.0f, 50.0f);

    assert(ce_cell_merge(c1, c2) == CE_OK);

    /* c1 应仍存在，c2 应无效 */
    assert(ce_cell_get(c1) != NULL);
    assert(ce_cell_get(c2) == NULL);

    ce_cell_shutdown();
    printf("PASS\n");
}

static void test_process_assignment(void) {
    printf("  test_process_assignment... ");
    ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20);

    CeCellId cid = ce_cell_find_by_position(50.0f, 50.0f);
    assert(ce_cell_get_process(cid) == -1);

    ce_cell_assign_process(cid, 42);
    assert(ce_cell_get_process(cid) == 42);

    ce_cell_shutdown();
    printf("PASS\n");
}

static void test_query_nearby(void) {
    printf("  test_query_nearby... ");
    ce_cell_init(1000.0f, 1000.0f, 100.0f, 100.0f, 150, 20);

    ce_cell_enter_entity(1, 50.0f, 50.0f, 1.0f);
    ce_cell_enter_entity(2, 60.0f, 50.0f, 1.0f);
    ce_cell_enter_entity(3, 500.0f, 500.0f, 1.0f);

    CeServerEntityId buffer[16];
    int count = ce_cell_query_nearby(1, buffer, 16);
    assert(count == 1);
    assert(buffer[0] == 2);

    ce_cell_shutdown();
    printf("PASS\n");
}

int main(void) {
    printf("=== Cell Manager Tests ===\n");

    test_init_shutdown();
    test_find_by_position();
    test_cell_get();
    test_enter_leave_entity();
    test_move_entity();
    test_split();
    test_merge();
    test_process_assignment();
    test_query_nearby();

    printf("\nAll Cell tests passed!\n");
    return 0;
}
