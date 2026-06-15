/*
 * ChaosEngine 内存池单元测试
 * 测试: ce_mempool_create/alloc/free/free_count
 */

#include "public_api/ce_types.h"
#include "core/ce_memory.h"
#include <stdio.h>
#include <string.h>

#define TEST(name) printf("  TEST: %s ... ", name)
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while(0)
#define PASS() printf("PASS\n")

int main(void) {
    printf("=== Memory Tests ===\n");

    /* ---- mempool_create ---- */
    TEST("mempool_create");
    {
        CeMemPool* pool = ce_mempool_create(NULL, 64, 10);
        CHECK(pool != NULL);
        CHECK(ce_mempool_free_count(pool) == 10);
        CHECK(ce_mempool_block_size(pool) == 64);
        ce_mempool_destroy(pool);
    }
    PASS();

    /* ---- mempool_create_invalid ---- */
    TEST("mempool_create_invalid_size");
    {
        /* block_size 小于 sizeof(void*) 应返回 NULL */
        CeMemPool* pool = ce_mempool_create(NULL, 1, 10);
        CHECK(pool == NULL);
    }
    PASS();

    TEST("mempool_create_zero_count");
    {
        CeMemPool* pool = ce_mempool_create(NULL, 64, 0);
        CHECK(pool == NULL);
    }
    PASS();

    /* ---- mempool_alloc ---- */
    TEST("mempool_alloc");
    {
        CeMemPool* pool = ce_mempool_create(NULL, 64, 5);
        CHECK(pool != NULL);

        void* ptrs[5];
        for (int i = 0; i < 5; i++) {
            ptrs[i] = ce_mempool_alloc(pool);
            CHECK(ptrs[i] != NULL);
        }
        CHECK(ce_mempool_free_count(pool) == 0);

        /* 第 6 次分配应返回 NULL */
        void* extra = ce_mempool_alloc(pool);
        CHECK(extra == NULL);

        ce_mempool_destroy(pool);
    }
    PASS();

    /* ---- mempool_free ---- */
    TEST("mempool_free");
    {
        CeMemPool* pool = ce_mempool_create(NULL, 64, 3);
        CHECK(pool != NULL);

        void* a = ce_mempool_alloc(pool);
        void* b = ce_mempool_alloc(pool);
        void* c = ce_mempool_alloc(pool);
        CHECK(ce_mempool_free_count(pool) == 0);

        ce_mempool_free(pool, b);
        CHECK(ce_mempool_free_count(pool) == 1);

        /* 重新分配应得到之前释放的块 */
        void* d = ce_mempool_alloc(pool);
        CHECK(d != NULL);
        CHECK(ce_mempool_free_count(pool) == 0);

        ce_mempool_destroy(pool);
    }
    PASS();

    /* ---- mempool_free_count ---- */
    TEST("mempool_free_count_after_alloc_free");
    {
        CeMemPool* pool = ce_mempool_create(NULL, 128, 10);
        CHECK(pool != NULL);
        CHECK(ce_mempool_free_count(pool) == 10);

        void* p1 = ce_mempool_alloc(pool);
        CHECK(ce_mempool_free_count(pool) == 9);

        void* p2 = ce_mempool_alloc(pool);
        CHECK(ce_mempool_free_count(pool) == 8);

        ce_mempool_free(pool, p1);
        CHECK(ce_mempool_free_count(pool) == 9);

        ce_mempool_free(pool, p2);
        CHECK(ce_mempool_free_count(pool) == 10);

        ce_mempool_destroy(pool);
    }
    PASS();

    /* ---- mempool_data_integrity ---- */
    TEST("mempool_data_integrity");
    {
        /* block_size 必须 >= sizeof(void*) 才能创建池 */
        CeMemPool* pool = ce_mempool_create(NULL, sizeof(void*), 4);
        CHECK(pool != NULL);

        int* a = (int*)ce_mempool_alloc(pool);
        int* b = (int*)ce_mempool_alloc(pool);
        CHECK(a != NULL && b != NULL);

        *a = 42;
        *b = 99;
        CHECK(*a == 42);
        CHECK(*b == 99);

        /* 释放 a 后，a 的内存不应影响 b */
        ce_mempool_free(pool, a);
        CHECK(*b == 99);

        ce_mempool_destroy(pool);
    }
    PASS();

    /* ---- mempool_null_safety ---- */
    TEST("mempool_null_safety");
    {
        CHECK(ce_mempool_free_count(NULL) == 0);
        CHECK(ce_mempool_block_size(NULL) == 0);
        ce_mempool_destroy(NULL); /* 不应崩溃 */
        void* p = ce_mempool_alloc(NULL);
        CHECK(p == NULL);
        ce_mempool_free(NULL, (void*)0x1000); /* 不应崩溃 */
    }
    PASS();

    printf("\nAll memory tests passed!\n");
    return 0;
}
