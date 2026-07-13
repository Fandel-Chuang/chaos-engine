/*
 * ChaosEngine Gateway - 协议编解码（零拷贝）
 *
 * 帧格式：[4B total_len][2B msg_type][N payload]  大端序
 * total_len = 6 + payload_len
 */

#define _POSIX_C_SOURCE 200112L

#include "gateway/ce_gateway.h"
#include "network/ce_net_base.h"

#include <string.h>

/* ================================================================
 * 协议解析（零拷贝：直接返回 buf 内指针）
 * ================================================================ */

/**
 * 在接收缓冲区上解析一个完整消息帧。
 *
 * @param buf            接收缓冲区
 * @param len            缓冲区有效数据长度
 * @param out_msg_type   输出：消息类型
 * @param out_payload    输出：payload 指针（指向 buf 内部，零拷贝）
 * @param out_payload_len 输出：payload 长度
 * @return               >0 消息总长度（含头），0 数据不完整，-1 协议错误
 */
int ce_gateway_protocol_parse(const uint8_t* buf, int len,
                              uint16_t* out_msg_type,
                              const uint8_t** out_payload,
                              int* out_payload_len)
{
    if (!buf || len < CE_GATEWAY_HEADER_SIZE) {
        return 0;  /* 头部不完整 */
    }

    /* 用 ce_net_base_peek_len 读取 total_len（大端序） */
    uint32_t total_len = ce_net_base_peek_len(buf, (uint32_t)len);
    if (total_len == 0) {
        return 0;  /* 头部不完整 */
    }

    /* 合法性检查：total_len 至少包含头部 */
    if (total_len < CE_GATEWAY_HEADER_SIZE) {
        return -1;  /* 协议错误 */
    }

    /* 防止单条消息过大（上限 256 KiB） */
    if (total_len > CE_NET_BASE_MAX_MSG_SIZE) {
        return -1;
    }

    /* 数据是否已完整到达？ */
    if ((int)total_len > len) {
        return 0;  /* 等待更多数据 */
    }

    /* 读取消息类型 */
    *out_msg_type = ce_net_base_peek_type(buf, (uint32_t)len);

    /* payload 指针直接指向 buf 内部（零拷贝） */
    *out_payload = buf + CE_GATEWAY_HEADER_SIZE;
    *out_payload_len = (int)(total_len - CE_GATEWAY_HEADER_SIZE);

    return (int)total_len;
}

/* ================================================================
 * 协议打包
 * ================================================================ */

/**
 * 将消息打包到 out_buf。
 *
 * @param msg_type      消息类型
 * @param payload       payload 数据
 * @param payload_len   payload 长度
 * @param out_buf       输出缓冲区
 * @param out_buf_size  输出缓冲区大小
 * @return              总长度（含头），0 缓冲区不足
 */
int ce_gateway_protocol_pack(uint16_t msg_type,
                             const uint8_t* payload, int payload_len,
                             uint8_t* out_buf, int out_buf_size)
{
    if (!out_buf || payload_len < 0) {
        return 0;
    }
    if (payload_len > 0 && !payload) {
        return 0;
    }

    int total_len = CE_GATEWAY_HEADER_SIZE + payload_len;
    if (total_len > out_buf_size) {
        return 0;  /* 缓冲区不足 */
    }

    /* 复用 ce_net_base_pack：写入 [4B total_len][2B msg_type][N payload] */
    uint32_t written = ce_net_base_pack(out_buf, (uint32_t)out_buf_size,
                                        msg_type, payload, (uint32_t)payload_len);
    return (int)written;
}
