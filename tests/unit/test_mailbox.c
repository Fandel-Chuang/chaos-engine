/*
 * ChaosEngine Mailbox — 单元测试
 *
 * 测试:
 *   1. register + lookup
 *   2. unregister
 *   3. lookup not found
 *   4. many entities (1000)
 *   5. overwrite (register same entity twice)
 */

#include "replication/ce_mailbox.h"
#include "replication/ce_replication.h"
#include "public_api/ce_types.h"
#include <stdio.h>
#include <assert.h>

/* ---- 测试 1: register + lookup ---- */

static void test_mailbox_register_lookup(void) {
    printf("  test_mailbox_register_lookup...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 注册几个实体 */
    CeResult r1 = ce_mailbox_register(ctx, 100ULL, 1);
    assert(r1 == CE_OK);
    CeResult r2 = ce_mailbox_register(ctx, 200ULL, 2);
    assert(r2 == CE_OK);
    CeResult r3 = ce_mailbox_register(ctx, 300ULL, 3);
    assert(r3 == CE_OK);

    /* 查找 */
    uint32_t sid;
    assert(ce_mailbox_lookup(ctx, 100ULL, &sid) == CE_TRUE);
    assert(sid == 1);
    assert(ce_mailbox_lookup(ctx, 200ULL, &sid) == CE_TRUE);
    assert(sid == 2);
    assert(ce_mailbox_lookup(ctx, 300ULL, &sid) == CE_TRUE);
    assert(sid == 3);

    /* 计数 */
    assert(ce_mailbox_count(ctx) == 3);

    ce_repl_shutdown(ctx);
    printf("  test_mailbox_register_lookup: PASS\n");
}

/* ---- 测试 2: unregister ---- */

static void test_mailbox_unregister(void) {
    printf("  test_mailbox_unregister...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    ce_mailbox_register(ctx, 100ULL, 1);
    ce_mailbox_register(ctx, 200ULL, 2);
    ce_mailbox_register(ctx, 300ULL, 3);
    assert(ce_mailbox_count(ctx) == 3);

    /* 删除中间实体 */
    ce_mailbox_unregister(ctx, 200ULL);
    assert(ce_mailbox_count(ctx) == 2);

    /* 被删除的实体应查不到 */
    uint32_t sid;
    assert(ce_mailbox_lookup(ctx, 200ULL, &sid) == CE_FALSE);

    /* 其他实体仍可查到 */
    assert(ce_mailbox_lookup(ctx, 100ULL, &sid) == CE_TRUE);
    assert(sid == 1);
    assert(ce_mailbox_lookup(ctx, 300ULL, &sid) == CE_TRUE);
    assert(sid == 3);

    /* 删除不存在的实体不应崩溃 */
    ce_mailbox_unregister(ctx, 999ULL);
    assert(ce_mailbox_count(ctx) == 2);

    /* 删除全部 */
    ce_mailbox_unregister(ctx, 100ULL);
    ce_mailbox_unregister(ctx, 300ULL);
    assert(ce_mailbox_count(ctx) == 0);

    ce_repl_shutdown(ctx);
    printf("  test_mailbox_unregister: PASS\n");
}

/* ---- 测试 3: lookup not found ---- */

static void test_mailbox_lookup_not_found(void) {
    printf("  test_mailbox_lookup_not_found...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 空表查找 */
    uint32_t sid = 999;
    assert(ce_mailbox_lookup(ctx, 100ULL, &sid) == CE_FALSE);
    assert(sid == 999);  /* out 参数不应被修改 */

    /* 注册一些实体后再查不存在的 */
    ce_mailbox_register(ctx, 100ULL, 1);
    ce_mailbox_register(ctx, 200ULL, 2);

    assert(ce_mailbox_lookup(ctx, 300ULL, &sid) == CE_FALSE);
    assert(ce_mailbox_lookup(ctx, 0ULL, &sid) == CE_FALSE);
    assert(ce_mailbox_lookup(ctx, 999999ULL, &sid) == CE_FALSE);

    ce_repl_shutdown(ctx);
    printf("  test_mailbox_lookup_not_found: PASS\n");
}

/* ---- 测试 4: many entities (1000) ---- */

static void test_mailbox_many_entities(void) {
    printf("  test_mailbox_many_entities...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 注册 1000 个实体 */
    for (uint64_t eid = 1; eid <= 1000; eid++) {
        CeResult r = ce_mailbox_register(ctx, eid, (uint32_t)(eid % 100));
        assert(r == CE_OK);
    }

    assert(ce_mailbox_count(ctx) == 1000);

    /* 验证全部 */
    for (uint64_t eid = 1; eid <= 1000; eid++) {
        uint32_t sid;
        assert(ce_mailbox_lookup(ctx, eid, &sid) == CE_TRUE);
        assert(sid == (uint32_t)(eid % 100));
    }

    ce_repl_shutdown(ctx);
    printf("  test_mailbox_many_entities: PASS\n");
}

/* ---- 测试 5: overwrite (register same entity twice) ---- */

static void test_mailbox_overwrite(void) {
    printf("  test_mailbox_overwrite...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 首次注册 */
    CeResult r1 = ce_mailbox_register(ctx, 100ULL, 1);
    assert(r1 == CE_OK);
    assert(ce_mailbox_count(ctx) == 1);

    uint32_t sid;
    assert(ce_mailbox_lookup(ctx, 100ULL, &sid) == CE_TRUE);
    assert(sid == 1);

    /* 覆盖注册 */
    CeResult r2 = ce_mailbox_register(ctx, 100ULL, 42);
    assert(r2 == CE_OK);
    assert(ce_mailbox_count(ctx) == 1);  /* 数量不变 */

    assert(ce_mailbox_lookup(ctx, 100ULL, &sid) == CE_TRUE);
    assert(sid == 42);  /* 新值 */

    /* 多次覆盖 */
    ce_mailbox_register(ctx, 100ULL, 99);
    assert(ce_mailbox_count(ctx) == 1);
    assert(ce_mailbox_lookup(ctx, 100ULL, &sid) == CE_TRUE);
    assert(sid == 99);

    ce_repl_shutdown(ctx);
    printf("  test_mailbox_overwrite: PASS\n");
}

/* ---- 主函数 ---- */

int main(void) {
    printf("=== Mailbox Unit Tests ===\n\n");

    test_mailbox_register_lookup();
    test_mailbox_unregister();
    test_mailbox_lookup_not_found();
    test_mailbox_many_entities();
    test_mailbox_overwrite();

    printf("\n=== All Mailbox tests passed! ===\n");
    return 0;
}
