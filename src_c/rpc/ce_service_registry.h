/*
 * ChaosEngine 服务注册/发现 - 头文件
 *
 * 内置简易注册中心（TCP 服务），不依赖 etcd。
 * 纯 C99，ce_ 前缀。
 */

#ifndef CE_SERVICE_REGISTRY_H
#define CE_SERVICE_REGISTRY_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CE_REGISTRY_DEFAULT_PORT  9300

/** 不透明注册中心 */
typedef struct CeServiceRegistry CeServiceRegistry;

/** 服务实例信息 */
typedef struct CeServiceInstance {
    char     name[64];     /* 服务名 */
    char     host[64];     /* 主机地址 */
    int      port;         /* 端口 */
    char     metadata[256];/* 元数据 JSON */
} CeServiceInstance;

/**
 * 创建注册中心服务端（监听指定端口）
 * 用于启动一个独立的注册中心进程
 */
CeServiceRegistry* ce_registry_create_server(int port);

/**
 * 连接到注册中心（作为客户端）
 */
CeServiceRegistry* ce_registry_connect(const char* host, int port);

/**
 * 关闭并释放
 */
void ce_registry_destroy(CeServiceRegistry* reg);

/* ---- 客户端操作 ---- */

/**
 * 注册服务到注册中心
 *
 * @param reg      注册中心连接
 * @param name     服务名
 * @param host     服务监听地址
 * @param port     服务监听端口
 * @param metadata 元数据 JSON 字符串（可为空）
 */
CeResult ce_registry_register(CeServiceRegistry* reg,
                                const char* name,
                                const char* host, int port,
                                const char* metadata);

/**
 * 查找服务
 *
 * @param reg      注册中心连接
 * @param name     服务名
 * @param out_host 输出主机地址
 * @param out_port 输出端口
 */
CeResult ce_registry_lookup(CeServiceRegistry* reg,
                              const char* name,
                              char* out_host, int* out_port);

/**
 * 注销服务
 */
CeResult ce_registry_deregister(CeServiceRegistry* reg, const char* name);

/* ---- 服务端操作 ---- */

/**
 * 运行注册中心服务（阻塞）
 * 仅当由 ce_registry_create_server 创建时有效
 */
CeResult ce_registry_run(CeServiceRegistry* reg);

/**
 * 获取注册中心中已注册的服务数量
 */
int ce_registry_service_count(CeServiceRegistry* reg);

#ifdef __cplusplus
}
#endif

#endif /* CE_SERVICE_REGISTRY_H */
