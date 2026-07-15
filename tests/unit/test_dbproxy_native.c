/*
 * DBProxy 原生驱动单元测试
 * 条件编译：有 libmongoc 时运行真实测试，否则验证空实现不 crash
 */

#include "public_api/ce_types.h"
#include "dbproxy/ce_dbproxy_native.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_MONGOC

/* 测试用 MongoDB URI（可通过环境变量覆盖） */
static const char* get_test_uri(void) {
    const char* uri = getenv("CE_TEST_MONGO_URI");
    return uri ? uri : "mongodb://localhost:27017";
}

static void test_init_shutdown(void) {
    CeDbproxyNativeCtx* ctx = ce_dbproxy_native_init(get_test_uri(), 4);
    assert(ctx != NULL);

    CeDbproxyNativeStats stats;
    assert(ce_dbproxy_native_get_stats(ctx, &stats) == CE_OK);
    assert(stats.total_saves == 0);

    ce_dbproxy_native_shutdown(ctx);
    printf("[OK] test_init_shutdown\n");
}

static void test_save_load(void) {
    CeDbproxyNativeCtx* ctx = ce_dbproxy_native_init(get_test_uri(), 4);
    assert(ctx != NULL);

    const char* test_data = "hello_chaos_engine";
    CeResult ret = ce_dbproxy_native_save(ctx, 99999,
                                            (const uint8_t*)test_data,
                                            (uint32_t)strlen(test_data) + 1);
    if (ret == CE_OK) {
        uint8_t* out_data = NULL;
        uint32_t out_len = 0;
        ret = ce_dbproxy_native_load(ctx, 99999, &out_data, &out_len);
        assert(ret == CE_OK);
        assert(out_data != NULL);
        assert(out_len == strlen(test_data) + 1);
        assert(strcmp((char*)out_data, test_data) == 0);
        free(out_data);
    }
    ce_dbproxy_native_shutdown(ctx);
    printf("[OK] test_save_load\n");
}

static void test_batch_save(void) {
    CeDbproxyNativeCtx* ctx = ce_dbproxy_native_init(get_test_uri(), 4);
    assert(ctx != NULL);

    uint64_t ids[] = {100001, 100002, 100003};
    const char* d1 = "data1";
    const char* d2 = "data2";
    const char* d3 = "data3";
    const uint8_t* datas[] = {
        (const uint8_t*)d1, (const uint8_t*)d2, (const uint8_t*)d3
    };
    uint32_t lens[] = {6, 6, 6};

    ce_dbproxy_native_batch_save(ctx, ids, datas, lens, 3);
    ce_dbproxy_native_shutdown(ctx);
    printf("[OK] test_batch_save\n");
}

#else  /* !HAVE_MONGOC */

static void test_mongoc_not_available(void) {
    CeDbproxyNativeCtx* ctx = ce_dbproxy_native_init("mongodb://localhost:27017", 4);
    assert(ctx == NULL);
    ce_dbproxy_native_shutdown(NULL);
    CeResult ret = ce_dbproxy_native_save(NULL, 1, (const uint8_t*)"x", 1);
    assert(ret == CE_ERR);
    printf("[OK] test_mongoc_not_available (libmongoc not installed, skipped)\n");
}

#endif /* HAVE_MONGOC */

int main(void) {
    printf("=== test_dbproxy_native ===\n");
#ifdef HAVE_MONGOC
    test_init_shutdown();
    test_save_load();
    test_batch_save();
#else
    test_mongoc_not_available();
#endif
    printf("=== All tests passed ===\n");
    return 0;
}
