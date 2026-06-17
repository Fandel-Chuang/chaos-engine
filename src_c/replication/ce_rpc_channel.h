/*
 * ChaosEngine RPC Channel — 公共头文件
 *
 * 远程过程调用通道，支持：
 *   - 三种目标: CLIENT / SERVER / AOI
 *   - 可靠/不可靠传输
 *   - MERGE_ATTRS: 将当前脏属性与 RPC 合并发送
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 */

#ifndef CE_RPC_CHANNEL_H
#define CE_RPC_CHANNEL_H

#include "replication/ce_replication.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 枚举 ---- */

/** RPC 目标 */
typedef enum CeRpcTarget {
    CE_RPC_TARGET_CLIENT = 0,   /* 发送到属主客户端 */
    CE_RPC_TARGET_SERVER = 1,   /* 发送到服务器 (客户端→服务器) */
    CE_RPC_TARGET_AOI    = 2,   /* 广播到 AOI 范围内所有客户端 */
} CeRpcTarget;

/** RPC 可靠性 */
typedef enum CeRpcReliability {
    CE_RPC_UNRELIABLE = 0,      /* 不可靠 (UDP 风格，不保证送达) */
    CE_RPC_RELIABLE   = 1,      /* 可靠 (TCP 风格，确认+重传) */
} CeRpcReliability;

/** RPC 发送标志 (位掩码) */
typedef enum CeRpcSendFlag {
    CE_RPC_SEND_NONE        = 0x00,   /* 无特殊标志 */
    CE_RPC_SEND_MERGE_ATTRS = 0x01,   /* 将当前脏属性与 RPC 合并发送 */
} CeRpcSendFlag;

/* ---- 类型 ---- */

/**
 * RPC handler 回调
 *
 * @param source_entity  发起 RPC 的实体 ID
 * @param params         RPC 参数 (二进制)
 * @param params_len     参数长度
 * @param user_data      注册时传入的用户数据
 */
typedef void (*CeRpcHandler)(uint64_t source_entity,
                             const uint8_t* params, uint32_t params_len,
                             void* user_data);

/* ---- API ---- */

/**
 * 发送 RPC
 *
 * 将 RPC 序列化为二进制格式并加入发送队列。
 * 如果设置了 CE_RPC_SEND_MERGE_ATTRS，会将实体的当前脏字段
 * 附加到 RPC 数据中一起发送。
 *
 * @param ctx          复制管理器上下文
 * @param entity_id    发起 RPC 的实体 ID
 * @param target       RPC 目标
 * @param reliability  可靠性模式
 * @param send_flags   发送标志 (位掩码)
 * @param method       方法名 (以 null 结尾的字符串)
 * @param params       参数数据 (可为 NULL)
 * @param params_len   参数长度
 * @return             CE_OK 成功，CE_ERR 失败
 */
CeResult ce_repl_rpc_send(CeReplContext* ctx, uint64_t entity_id,
                          CeRpcTarget target, CeRpcReliability reliability,
                          CeRpcSendFlag send_flags,
                          const char* method,
                          const uint8_t* params, uint32_t params_len);

/**
 * 注册 RPC handler
 *
 * 将 method → handler 映射添加到 handler 表。
 * 当收到匹配 method 的 RPC 时，调用对应的 handler。
 *
 * @param ctx       复制管理器上下文
 * @param method    方法名 (以 null 结尾的字符串)
 * @param handler   处理函数
 * @param user_data 用户数据 (透传给 handler)
 * @return          CE_OK 成功，CE_ERR 失败 (表满或重复)
 */
CeResult ce_repl_rpc_register_handler(CeReplContext* ctx, const char* method,
                                      CeRpcHandler handler, void* user_data);

/**
 * 分发收到的 RPC
 *
 * 解析二进制格式的 RPC 数据，查找对应的 handler 并调用。
 * 如果找不到 handler，记录警告日志但不崩溃。
 *
 * @param ctx           复制管理器上下文
 * @param source_entity 发起 RPC 的实体 ID
 * @param data          二进制 RPC 数据
 * @param data_len      数据长度
 */
void ce_repl_rpc_dispatch(CeReplContext* ctx, uint64_t source_entity,
                          const uint8_t* data, uint32_t data_len);

/**
 * RPC 通道每帧更新
 *
 * 处理可靠 RPC 的确认/超时重传。
 * MVP 阶段为空操作。
 *
 * @param ctx  复制管理器上下文
 * @param dt   帧间隔 (秒)
 */
void ce_repl_rpc_tick(CeReplContext* ctx, float dt);

#ifdef __cplusplus
}
#endif

#endif /* CE_RPC_CHANNEL_H */
