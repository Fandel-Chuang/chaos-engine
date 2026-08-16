/*
 * ChaosEngine etcd 服务发现 - 头文件
 *
 * 通过 etcd v3 HTTP/gRPC Gateway API 实现服务注册与发现。
 * 使用 libcurl HTTP 客户端，无需 gRPC 依赖。
 *
 * 功能:
 *   - 服务注册 (PUT key with lease)
 *   - 心跳保活 (keepalive lease)
 *   - 服务发现 (GET prefix)
 *   - 负载均衡 (round-robin / random / least-conn)
 *   - 本地缓存 + TTL
 *   - Watch 变更通知 (long polling)
 *
 * etcd key 设计:
 *   /chaos/services/{service_name}/{instance_id} = JSON{host, port, weight, metadata}
 *
 * 纯 C99，ce_ 前缀。
 */

#ifndef CE_ETCD_REGISTRY_H
#define CE_ETCD_REGISTRY_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 常量 ---- */

#define CE_ETCD_DEFAULT_ENDPOINT   "http://127.0.0.1:2379"
#define CE_ETCD_KEY_PREFIX         "/chaos/services/"
#define CE_ETCD_DEFAULT_LEASE_TTL  15          /* 秒 */
#define CE_ETCD_HEARTBEAT_INTERVAL 5           /* 秒 */
#define CE_ETCD_CACHE_TTL_SEC      10          /* 秒 */
#define CE_ETCD_MAX_INSTANCES      64
#define CE_ETCD_REQUEST_TIMEOUT_MS 3000        /* 毫秒 */

/* ---- 负载均衡策略 ---- */

typedef enum {
    CE_ETCD_LB_ROUND_ROBIN = 0,
    CE_ETCD_LB_RANDOM = 1,
    CE_ETCD_LB_LEAST_CONN = 2,
} CeEtcdLbStrategy;

/* ---- 服务实例 ---- */

typedef struct CeEtcdInstance {
    char     name[64];
    char     host[64];
    int      port;
    int      weight;
    char     metadata[256];
    int64_t  registered_ms;   /* 注册时间 */
} CeEtcdInstance;

/* ---- etcd 客户端 ---- */

typedef struct CeEtcdClient CeEtcdClient;

/**
 * 创建 etcd 客户端
 *
 * @param endpoint  etcd 地址，如 "http://127.0.0.1:2379"
 *                  NULL = 使用默认地址
 */
CeEtcdClient* ce_etcd_create(const char* endpoint);

/** 销毁客户端（会自动注销已注册的服务） */
void ce_etcd_destroy(CeEtcdClient* cli);

/**
 * 检查 etcd 是否可用
 */
CeBool ce_etcd_health_check(CeEtcdClient* cli);

/* ================================================================
 * 服务注册
 * ================================================================ */

/**
 * 注册服务实例到 etcd
 * 自动创建 lease 并绑定 key
 *
 * @param cli       etcd 客户端
 * @param name      服务名，如 "game_server"
 * @param host      监听地址
 * @param port      监听端口
 * @param weight    权重 (1=普通, 10=高配)
 * @param metadata  元数据 JSON (可为 NULL)
 */
CeResult ce_etcd_register(CeEtcdClient* cli,
                            const char* name,
                            const char* host, int port,
                            int weight,
                            const char* metadata);

/**
 * 注销服务实例
 */
CeResult ce_etcd_deregister(CeEtcdClient* cli,
                              const char* name,
                              const char* host, int port);

/**
 * 发送心跳（续约 lease）
 * 客户端应定期调用，否则 etcd 会自动删除 key
 */
CeResult ce_etcd_heartbeat(CeEtcdClient* cli);

/**
 * 后台心跳线程：自动定期续约
 * 启动后自动每 CE_ETCD_HEARTBEAT_INTERVAL 秒发送心跳
 */
CeResult ce_etcd_heartbeat_start(CeEtcdClient* cli);

/** 停止后台心跳线程 */
void ce_etcd_heartbeat_stop(CeEtcdClient* cli);

/* ================================================================
 * 服务发现
 * ================================================================ */

/**
 * 查找服务实例（按负载均衡策略选一个）
 * 优先走本地缓存
 *
 * @param cli       etcd 客户端
 * @param name      服务名
 * @param strategy  负载均衡策略
 * @param out_inst  输出选中的实例
 */
CeResult ce_etcd_discover(CeEtcdClient* cli,
                            const char* name,
                            CeEtcdLbStrategy strategy,
                            CeEtcdInstance* out_inst);

/**
 * 获取服务所有实例列表
 *
 * @param cli       etcd 客户端
 * @param name      服务名
 * @param out_arr   输出实例数组（调用方分配 CE_ETCD_MAX_INSTANCES 个）
 * @param out_count 输出实例数
 */
CeResult ce_etcd_list(CeEtcdClient* cli,
                        const char* name,
                        CeEtcdInstance* out_arr,
                        int* out_count);

/**
 * 刷新本地缓存（强制从 etcd 拉取）
 */
CeResult ce_etcd_refresh_cache(CeEtcdClient* cli, const char* name);

/* ================================================================
 * Watch 机制
 * ================================================================ */

typedef enum {
    CE_ETCD_EVENT_PUT = 1,        /* 新注册/更新 */
    CE_ETCD_EVENT_DELETE = 2,     /* 注销/过期 */
} CeEtcdEventType;

typedef void (*CeEtcdWatchCallback)(CeEtcdEventType event,
                                      const CeEtcdInstance* instance,
                                      void* user_data);

/**
 * 订阅服务变更通知
 * 内部启动线程 long-polling etcd watch API
 */
CeResult ce_etcd_watch(CeEtcdClient* cli,
                         const char* name,
                         CeEtcdWatchCallback callback,
                         void* user_data);

/** 停止 watch */
void ce_etcd_watch_stop(CeEtcdClient* cli);

/* ---- 工具函数 ---- */

const char* ce_etcd_lb_name(CeEtcdLbStrategy strategy);

#ifdef __cplusplus
}
#endif

#endif /* CE_ETCD_REGISTRY_H */
