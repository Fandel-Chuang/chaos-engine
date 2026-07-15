/*
 * etcd 服务发现单元测试
 * 测试: 客户端创建/销毁, 健康检查, 注册/发现/注销, 心跳, 负载均衡
 *
 * 注意: 需要运行 etcd 实例（端口 2379）
 *       无 etcd 时测试会跳过，不报错
 */

#include "public_api/ce_types.h"
#include "rpc/ce_etcd_registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* 检查 etcd 是否可用 */
static int etcd_available(void) {
    CeEtcdClient* cli = ce_etcd_create("http://127.0.0.1:2379");
    if (!cli) return 0;
    int ok = ce_etcd_health_check(cli);
    ce_etcd_destroy(cli);
    return ok ? 1 : 0;
}

static void test_create_destroy(void) {
    CeEtcdClient* cli = ce_etcd_create(NULL);
    assert(cli != NULL);
    ce_etcd_destroy(cli);
    printf("[OK] test_create_destroy\n");
}

static void test_health_check(void) {
    CeEtcdClient* cli = ce_etcd_create("http://127.0.0.1:2379");
    assert(cli != NULL);

    if (!ce_etcd_health_check(cli)) {
        printf("[SKIP] test_health_check (etcd not running)\n");
        ce_etcd_destroy(cli);
        return;
    }

    printf("[OK] test_health_check (etcd is running)\n");
    ce_etcd_destroy(cli);
}

static void test_register_discover_deregister(void) {
    if (!etcd_available()) {
        printf("[SKIP] test_register_discover_deregister (etcd not running)\n");
        return;
    }

    CeEtcdClient* cli = ce_etcd_create("http://127.0.0.1:2379");
    assert(cli != NULL);

    /* 注册测试服务 */
    CeResult ret = ce_etcd_register(cli, "test_svc", "127.0.0.1", 8001, 1, "{\"zone\":\"test\"}");
    assert(ret == CE_OK);

    /* 注册第二个实例 */
    ret = ce_etcd_register(cli, "test_svc", "127.0.0.1", 8002, 5, "{\"zone\":\"test\"}");
    assert(ret == CE_OK);

    /* 发现服务（round-robin） */
    CeEtcdInstance inst;
    ret = ce_etcd_discover(cli, "test_svc", CE_ETCD_LB_ROUND_ROBIN, &inst);
    assert(ret == CE_OK);
    printf("  discovered: %s:%d (weight=%d)\n", inst.host, inst.port, inst.weight);

    /* 再次发现应该轮询到下一个 */
    CeEtcdInstance inst2;
    ret = ce_etcd_discover(cli, "test_svc", CE_ETCD_LB_ROUND_ROBIN, &inst2);
    assert(ret == CE_OK);
    printf("  discovered: %s:%d (weight=%d)\n", inst2.host, inst2.port, inst2.weight);

    /* 两个结果应该不同 */
    assert(inst.port != inst2.port || strcmp(inst.host, inst2.host) != 0);

    /* 获取所有实例 */
    CeEtcdInstance all[CE_ETCD_MAX_INSTANCES];
    int count = 0;
    ret = ce_etcd_list(cli, "test_svc", all, &count);
    assert(ret == CE_OK);
    assert(count >= 2);
    printf("  list: %d instances\n", count);

    /* 注销 */
    ret = ce_etcd_deregister(cli, "test_svc", "127.0.0.1", 8001);
    assert(ret == CE_OK);

    ret = ce_etcd_deregister(cli, "test_svc", "127.0.0.1", 8002);
    assert(ret == CE_OK);

    ce_etcd_destroy(cli);
    printf("[OK] test_register_discover_deregister\n");
}

static void test_heartbeat(void) {
    if (!etcd_available()) {
        printf("[SKIP] test_heartbeat (etcd not running)\n");
        return;
    }

    CeEtcdClient* cli = ce_etcd_create("http://127.0.0.1:2379");
    assert(cli != NULL);

    /* 注册 */
    CeResult ret = ce_etcd_register(cli, "hb_svc", "127.0.0.1", 9001, 1, NULL);
    assert(ret == CE_OK);

    /* 手动心跳 */
    ret = ce_etcd_heartbeat(cli);
    assert(ret == CE_OK);

    /* 启动后台心跳 */
    ret = ce_etcd_heartbeat_start(cli);
    assert(ret == CE_OK);

    /* 等待几秒 */
    sleep(2);

    /* 服务应该还活着 */
    CeEtcdInstance inst;
    ret = ce_etcd_discover(cli, "hb_svc", CE_ETCD_LB_RANDOM, &inst);
    assert(ret == CE_OK);

    /* 停止心跳 */
    ce_etcd_heartbeat_stop(cli);

    /* 清理 */
    ce_etcd_deregister(cli, "hb_svc", "127.0.0.1", 9001);
    ce_etcd_destroy(cli);
    printf("[OK] test_heartbeat\n");
}

static void test_load_balancing(void) {
    if (!etcd_available()) {
        printf("[SKIP] test_load_balancing (etcd not running)\n");
        return;
    }

    CeEtcdClient* cli = ce_etcd_create("http://127.0.0.1:2379");
    assert(cli != NULL);

    /* 注册 3 个实例 */
    for (int i = 0; i < 3; i++) {
        char meta[64];
        snprintf(meta, sizeof(meta), "{\"id\":%d}", i);
        ce_etcd_register(cli, "lb_svc", "10.0.0.1", 7000 + i, i + 1, meta);
    }

    /* Round-robin: 连续 6 次应该遍历所有实例 */
    int ports_seen[6];
    for (int i = 0; i < 6; i++) {
        CeEtcdInstance inst;
        ce_etcd_discover(cli, "lb_svc", CE_ETCD_LB_ROUND_ROBIN, &inst);
        ports_seen[i] = inst.port;
    }

    /* 验证 round-robin: 前两个不应该连续相同 */
    int has_variety = 0;
    for (int i = 1; i < 6; i++) {
        if (ports_seen[i] != ports_seen[0]) has_variety = 1;
    }
    assert(has_variety);

    /* Random */
    CeEtcdInstance inst;
    ce_etcd_discover(cli, "lb_svc", CE_ETCD_LB_RANDOM, &inst);
    printf("  random: %s:%d\n", inst.host, inst.port);

    /* Least-conn (选权重最高的) */
    ce_etcd_discover(cli, "lb_svc", CE_ETCD_LB_LEAST_CONN, &inst);
    assert(inst.weight == 3);  /* 权重最高的 */
    printf("  least_conn (highest weight): %s:%d (weight=%d)\n", inst.host, inst.port, inst.weight);

    /* 清理 */
    for (int i = 0; i < 3; i++) {
        ce_etcd_deregister(cli, "lb_svc", "10.0.0.1", 7000 + i);
    }
    ce_etcd_destroy(cli);
    printf("[OK] test_load_balancing\n");
}

int main(void) {
    printf("=== test_etcd_registry ===\n");

    test_create_destroy();
    test_health_check();
    test_register_discover_deregister();
    test_heartbeat();
    test_load_balancing();

    printf("=== All tests passed ===\n");
    return 0;
}
