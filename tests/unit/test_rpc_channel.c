/*
 * ChaosEngine RPC Channel — 单元测试
 *
 * 测试:
 *   1. RPC 初始化/关闭 (通过 ce_repl_init/ce_repl_shutdown)
 *   2. Handler 注册
 *   3. RPC 发送与分发
 *   4. MERGE_ATTRS 标志
 *   5. 多个 handler
 *   6. Handler 未找到 (不崩溃)
 */

#include "replication/ce_rpc_channel.h"
#include "replication/ce_replication_internal.h"
#include "public_api/ce_types.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ---- 测试用组件定义 ---- */

#define COMP_TEST_PLAYER    1

typedef struct {
    int32_t  hp;
    int32_t  max_hp;
    int32_t  mp;
    uint8_t  level;
    float    speed;
} TestPlayerComponent;

/* ---- 测试 handler 回调数据 ---- */

typedef struct {
    bool     called;
    uint64_t source_entity;
    char     method[64];
    uint32_t params_len;
    uint8_t  params[256];
    void*    user_data;
} TestHandlerCtx;

/* ---- 测试 handler 回调 ---- */

static void test_handler_callback(uint64_t source_entity,
                                  const uint8_t* params, uint32_t params_len,
                                  void* user_data) {
    TestHandlerCtx* ctx = (TestHandlerCtx*)user_data;
    ctx->called = true;
    ctx->source_entity = source_entity;
    ctx->params_len = params_len;
    if (params && params_len > 0 && params_len <= sizeof(ctx->params)) {
        memcpy(ctx->params, params, params_len);
    }
}

/* ---- 测试 1: 初始化/关闭 ---- */

static void test_rpc_init_shutdown(void) {
    printf("  test_rpc_init_shutdown...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 验证 RPC 相关字段已初始化 */
    assert(ctx->rpc_handler_count == 0);
    assert(ctx->rpc_pending_count == 0);
    assert(ctx->rpc_call_id_counter == 0);

    ce_repl_shutdown(ctx);
    printf("  test_rpc_init_shutdown: PASS\n");
}

/* ---- 测试 2: Handler 注册 ---- */

static void test_rpc_register_handler(void) {
    printf("  test_rpc_register_handler...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    TestHandlerCtx hctx = {0};
    CeResult r = ce_repl_rpc_register_handler(ctx, "TestMethod",
                                              test_handler_callback, &hctx);
    assert(r == CE_OK);
    assert(ctx->rpc_handler_count == 1);
    assert(strcmp(ctx->rpc_handlers[0].method, "TestMethod") == 0);
    assert(ctx->rpc_handlers[0].handler == test_handler_callback);
    assert(ctx->rpc_handlers[0].user_data == &hctx);

    /* 重复注册应失败 */
    r = ce_repl_rpc_register_handler(ctx, "TestMethod",
                                     test_handler_callback, &hctx);
    assert(r == CE_ERR);

    /* NULL 参数应失败 */
    r = ce_repl_rpc_register_handler(ctx, NULL, test_handler_callback, &hctx);
    assert(r == CE_ERR);
    r = ce_repl_rpc_register_handler(ctx, "Test", NULL, &hctx);
    assert(r == CE_ERR);

    ce_repl_shutdown(ctx);
    printf("  test_rpc_register_handler: PASS\n");
}

/* ---- 测试 3: RPC 发送与分发 ---- */

static void test_rpc_send_and_dispatch(void) {
    printf("  test_rpc_send_and_dispatch...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 注册 handler */
    TestHandlerCtx hctx = {0};
    hctx.user_data = (void*)0xDEADBEEF;
    CeResult r = ce_repl_rpc_register_handler(ctx, "MoveTo",
                                              test_handler_callback, &hctx);
    assert(r == CE_OK);

    /* 构建 RPC 参数 */
    const char* test_params = "hello world";
    uint32_t test_params_len = (uint32_t)strlen(test_params);

    /* 发送 RPC */
    r = ce_repl_rpc_send(ctx, 42ULL, CE_RPC_TARGET_SERVER,
                         CE_RPC_UNRELIABLE, CE_RPC_SEND_NONE,
                         "MoveTo",
                         (const uint8_t*)test_params, test_params_len);
    assert(r == CE_OK);

    /* 手动构建二进制数据来模拟 dispatch (因为 send 在 MVP 不实际发送) */
    /* 构建: [1B type=0x01][4B call_id=0][2B method_len=6]["MoveTo"][4B params_len=11]["hello world"] */
    uint8_t rpc_data[256];
    uint8_t* p = rpc_data;
    *p++ = 0x01;  /* msg_type CALL */
    /* call_id = 0 */
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
    /* method_len = 6 */
    *p++ = 6; *p++ = 0;
    /* method = "MoveTo" */
    memcpy(p, "MoveTo", 6); p += 6;
    /* params_len = 11 */
    *p++ = 11; *p++ = 0; *p++ = 0; *p++ = 0;
    /* params = "hello world" */
    memcpy(p, "hello world", 11); p += 11;

    uint32_t data_len = (uint32_t)(p - rpc_data);

    /* 分发 */
    ce_repl_rpc_dispatch(ctx, 42ULL, rpc_data, data_len);

    /* 验证 handler 被调用 */
    assert(hctx.called);
    assert(hctx.source_entity == 42ULL);
    assert(hctx.params_len == 11);
    assert(memcmp(hctx.params, "hello world", 11) == 0);

    ce_repl_shutdown(ctx);
    printf("  test_rpc_send_and_dispatch: PASS\n");
}

/* ---- 测试 4: MERGE_ATTRS ---- */

static void test_rpc_merge_attrs(void) {
    printf("  test_rpc_merge_attrs...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 注册组件 */
    TestPlayerComponent default_player = {
        .hp = 100, .max_hp = 100, .mp = 50, .level = 1, .speed = 5.0f
    };

    ce_repl_register_component(ctx, COMP_TEST_PLAYER, "TestPlayer",
        (CeReplField[]){
            {
                .name       = "hp",
                .type       = CE_REPL_TYPE_I32,
                .flags      = CE_FLAG_AOI_BROADCAST,
                .offset     = offsetof(TestPlayerComponent, hp),
                .size       = sizeof(int32_t),
                .constraint = {0},
            },
            {
                .name       = "mp",
                .type       = CE_REPL_TYPE_I32,
                .flags      = CE_FLAG_OWNER_ONLY,
                .offset     = offsetof(TestPlayerComponent, mp),
                .size       = sizeof(int32_t),
                .constraint = {0},
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

    /* 标记脏 */
    ce_repl_mark_dirty(ctx, 100ULL, COMP_TEST_PLAYER);

    /* 发送带 MERGE_ATTRS 的 RPC */
    CeResult r = ce_repl_rpc_send(ctx, 100ULL, CE_RPC_TARGET_SERVER,
                                  CE_RPC_UNRELIABLE, CE_RPC_SEND_MERGE_ATTRS,
                                  "UpdateAttrs", NULL, 0);
    assert(r == CE_OK);

    /* 验证脏实体存在 */
    CeReplStats stats;
    ce_repl_get_stats(ctx, &stats);
    assert(stats.current_dirty_entities == 1);

    ce_repl_shutdown(ctx);
    printf("  test_rpc_merge_attrs: PASS\n");
}

/* ---- 测试 5: 多个 handler ---- */

static void test_rpc_multiple_handlers(void) {
    printf("  test_rpc_multiple_handlers...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    TestHandlerCtx hctx1 = {0};
    TestHandlerCtx hctx2 = {0};
    TestHandlerCtx hctx3 = {0};

    assert(ce_repl_rpc_register_handler(ctx, "MethodA",
                                        test_handler_callback, &hctx1) == CE_OK);
    assert(ce_repl_rpc_register_handler(ctx, "MethodB",
                                        test_handler_callback, &hctx2) == CE_OK);
    assert(ce_repl_rpc_register_handler(ctx, "MethodC",
                                        test_handler_callback, &hctx3) == CE_OK);
    assert(ctx->rpc_handler_count == 3);

    /* 分发到 MethodB */
    uint8_t rpc_data[256];
    uint8_t* p = rpc_data;
    *p++ = 0x01;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 7; *p++ = 0;
    memcpy(p, "MethodB", 7); p += 7;
    *p++ = 3; *p++ = 0; *p++ = 0; *p++ = 0;
    memcpy(p, "XYZ", 3); p += 3;

    ce_repl_rpc_dispatch(ctx, 1ULL, rpc_data, (uint32_t)(p - rpc_data));

    assert(hctx1.called == false);
    assert(hctx2.called == true);
    assert(hctx3.called == false);
    assert(hctx2.source_entity == 1ULL);
    assert(hctx2.params_len == 3);
    assert(memcmp(hctx2.params, "XYZ", 3) == 0);

    ce_repl_shutdown(ctx);
    printf("  test_rpc_multiple_handlers: PASS\n");
}

/* ---- 测试 6: Handler 未找到 ---- */

static void test_rpc_handler_not_found(void) {
    printf("  test_rpc_handler_not_found...\n");

    CeReplContext* ctx = ce_repl_init(NULL);
    assert(ctx != NULL);

    /* 不注册任何 handler，直接分发 */

    uint8_t rpc_data[256];
    uint8_t* p = rpc_data;
    *p++ = 0x01;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 9; *p++ = 0;
    memcpy(p, "NoHandler", 9); p += 9;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;

    /* 不应崩溃 */
    ce_repl_rpc_dispatch(ctx, 99ULL, rpc_data, (uint32_t)(p - rpc_data));

    /* 空数据也不应崩溃 */
    ce_repl_rpc_dispatch(ctx, 99ULL, NULL, 0);

    /* 过短数据也不应崩溃 */
    uint8_t short_data[3] = {0x01, 0x00, 0x00};
    ce_repl_rpc_dispatch(ctx, 99ULL, short_data, 3);

    ce_repl_shutdown(ctx);
    printf("  test_rpc_handler_not_found: PASS\n");
}

/* ---- 主函数 ---- */

int main(void) {
    printf("=== RPC Channel Unit Tests ===\n\n");

    test_rpc_init_shutdown();
    test_rpc_register_handler();
    test_rpc_send_and_dispatch();
    test_rpc_merge_attrs();
    test_rpc_multiple_handlers();
    test_rpc_handler_not_found();

    printf("\n=== All RPC channel tests passed! ===\n");
    return 0;
}
