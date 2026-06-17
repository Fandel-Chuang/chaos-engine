/*
 * ChaosEngine Replication Mailbox — 公共头文件
 *
 * Mailbox: entity_id → server_id 映射表
 *
 * 用途: 在分布式部署中，快速查找实体所在的服务器。
 *       基于线性探测哈希表，O(1) 平均查找。
 *
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 */

#ifndef CE_MAILBOX_H
#define CE_MAILBOX_H

#include "replication/ce_replication.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 注册实体到服务器的映射
 *
 * 将 entity_id 映射到 server_id。如果实体已存在，则覆盖映射。
 *
 * @param ctx        复制管理器上下文
 * @param entity_id  实体 ID
 * @param server_id  服务器 ID
 * @return           CE_OK 成功，CE_ERR 表满
 */
CeResult ce_mailbox_register(CeReplContext* ctx, uint64_t entity_id, uint32_t server_id);

/**
 * 注销实体的映射
 *
 * 从 Mailbox 中移除 entity_id 的映射。
 * 如果实体不存在，无操作。
 *
 * @param ctx        复制管理器上下文
 * @param entity_id  实体 ID
 */
void ce_mailbox_unregister(CeReplContext* ctx, uint64_t entity_id);

/**
 * 查找实体所在的服务器
 *
 * @param ctx           复制管理器上下文
 * @param entity_id     实体 ID
 * @param out_server_id 输出: 服务器 ID (仅当找到时写入)
 * @return              CE_TRUE 找到，CE_FALSE 未找到
 */
CeBool ce_mailbox_lookup(CeReplContext* ctx, uint64_t entity_id, uint32_t* out_server_id);

/**
 * 获取 Mailbox 中已注册的实体数量
 *
 * @param ctx  复制管理器上下文
 * @return     已注册实体数
 */
uint32_t ce_mailbox_count(CeReplContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CE_MAILBOX_H */
