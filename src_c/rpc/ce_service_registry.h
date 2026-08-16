/*
 * ChaosEngine 服务注册/发现 - 头文件
 *
 * 内置注册中心（TCP 服务），不依赖 etcd。
 *
 * 功能:
 *   - 服务注册/注销/查找
 *   - 多实例: 同一服务名可注册多个实例，查找时按负载均衡策略选择
 *   - 心跳: 服务定期发心跳，超时自动摘除
 *   - Watch: 客户端可订阅服务变更通知
 *   - 客户端本地缓存: lookup 优先走本地缓存，降低注册中心压力
 *
 * 纯 C99，ce_ 前缀。
 */

#ifndef CE_SERVICE_REGISTRY_H
#define CE_SERVICE_REGISTRY_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CE_REGISTRY_DEFAULT_PORT  9300

/** 默认心跳间隔（秒） */
#define CE_REGISTRY_HEARTBEAT_INTERVAL_SEC  5

/** 心跳超时（秒），超过此时间未心跳则摘除 */
#define CE_REGISTRY_HEARTBEAT_TIMEOUT_SEC   15

/** 客户端本地缓存 TTL（秒） */
#define CE_REGISTRY_CACHE_TTL_SEC           10

/** 单服务最大实例数 */
#define CE_REGISTRY_MAX_INSTANCES           64

/* ---- 负载均衡策略 ---- */

typedef enum {
    CE_LB_ROUND_ROBIN = 0,    /* 轮询 */
    CE_LB_RANDOM = 1,         /* 随机 */
    CE_LB_LEAST_CONN = 2,     /* 最少连接（需配合心跳上报） */
} CeRegistryLbStrategy;

/* ---- 不透明句柄 ---- */

typedef struct CeServiceRegistry CeServiceRegistry;

/** 服务实例信息 */
typedef struct CeServiceInstance {
    char     name[64];         /* 服务名 */
    char     host[64];         /* 主机地址 */
    int      port;             /* 端口 */
    char     metadata[256];    /* 元数据 JSON */
    int64_t  last_heartbeat_ms;/* 最后心跳时间(ms) */
    int      conn_count;       /* 当前连接数（最少连接策略用） */
    int      weight;           /* 权重（默认 1） */
} CeServiceInstance;

/* ================================================================
 * 服务端 API
 * ================================================================ */

/**
 * 创建注册中心服务端
 *
 * @param port     监听端口
 * @param strategy 负载均衡策略
 */
CeServiceRegistry* ce_registry_create_server(int port, CeRegistryLbStrategy strategy);

/**
 * 运行注册中心（阻塞）
 * 内部 select 循环，处理:
 *   - 客户端连接
 *   - REGISTER/LOOKUP/DEREGISTER/HEARTBEAT/WATCH 请求
 *   - 定期清理心跳超时的服务实例
 */
CeResult ce_registry_run(CeServiceRegistry* reg);

/** 停止注册中心 */
void ce_registry_stop(CeServiceRegistry* reg);

/** 获取已注册服务总数（含多实例） */
int ce_registry_service_count(CeServiceRegistry* reg);

/** 获取服务实例数（指定服务名） */
int ce_registry_instance_count(CeServiceRegistry* reg, const char* name);

/* ================================================================
 * 客户端 API
 * ================================================================ */

/**
 * 连接到注册中心
 *
 * @param host  注册中心地址
 * @param port  注册中心端口
 */
CeServiceRegistry* ce_registry_connect(const char* host, int port);

/** 断开并释放 */
void ce_registry_destroy(CeServiceRegistry* reg);

/**
 * 注册服务实例
 *
 * @param reg       连接
 * @param name      服务名
 * @param host      监听地址
 * @param port      监听端口
 * @param weight    权重（1=普通，10=高配）
 * @param metadata  元数据 JSON（可为 NULL）
 */
CeResult ce_registry_register(CeServiceRegistry* reg,
                                const char* name,
                                const char* host, int port,
                                int weight,
                                const char* metadata);

/**
 * 注销服务实例
 */
CeResult ce_registry_deregister(CeServiceRegistry* reg,
                                  const char* name,
                                  const char* host, int port);

/**
 * 查找服务实例（按负载均衡策略选一个）
 * 客户端有本地缓存，TTL 内直接返回缓存结果
 *
 * @param reg       连接
 * @param name      服务名
 * @param out_inst  输出选中的实例信息
 */
CeResult ce_registry_lookup(CeServiceRegistry* reg,
                              const char* name,
                              CeServiceInstance* out_inst);

/**
 * 获取服务所有实例列表
 *
 * @param reg        连接
 * @param name       服务名
 * @param out_arr    输出实例数组（调用方分配，大小 CE_REGISTRY_MAX_INSTANCES）
 * @param out_count  输出实例数
 */
CeResult ce_registry_list(CeServiceRegistry* reg,
                            const char* name,
                            CeServiceInstance* out_arr,
                            int* out_count);

/**
 * 发送心跳（保活）
 * 客户端定期调用，否则注册中心会摘除该实例
 *
 * @param reg   连接
 * @param name  服务名
 * @param host  监听地址
 * @param port  监听端口
 * @param conn_count 当前连接数（最少连接策略用）
 */
CeResult ce_registry_heartbeat(CeServiceRegistry* reg,
                                 const char* name,
                                 const char* host, int port,
                                 int conn_count);

/* ---- Watch 机制 ---- */

/** 服务变更事件类型 */
typedef enum {
    CE_REG_EVENT_REGISTER = 1,   /* 新实例注册 */
    CE_REG_EVENT_DEREGISTER = 2, /* 实例注销 */
    CE_REG_EVENT_HEARTBEAT_TIMEOUT = 3, /* 心跳超时摘除 */
} CeRegEventType;

/** Watch 回调函数 */
typedef void (*CeRegWatchCallback)(CeRegEventType event,
                                     const CeServiceInstance* instance,
                                     void* user_data);

/**
 * 订阅服务变更通知
 *
 * @param reg        连接
 * @param name       要监听的服务名（NULL = 所有服务）
 * @param callback   事件回调
 * @param user_data  传给回调的用户数据
 */
CeResult ce_registry_watch(CeServiceRegistry* reg,
                             const char* name,
                             CeRegWatchCallback callback,
                             void* user_data);

/**
 * 轮询 Watch 事件（非阻塞，在主循环中调用）
 * 返回处理的事件数
 */
int ce_registry_watch_poll(CeServiceRegistry* reg);

/* ---- 工具函数 ---- */

/** 获取负载均衡策略名称 */
const char* ce_registry_lb_name(CeRegistryLbStrategy strategy);

#ifdef __cplusplus
}
#endif

#endif /* CE_SERVICE_REGISTRY_H */
