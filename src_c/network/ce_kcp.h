/*
 * ChaosEngine KCP 封装 — 头文件
 * 纯 C99，封装 KCP 可靠 UDP 传输协议
 * KCP: https://github.com/skywind3000/kcp
 */

#ifndef CE_KCP_H
#define CE_KCP_H

#include "public_api/ce_types.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明句柄 ---- */

typedef struct CeKcpContext CeKcpContext;

/* ---- 输出回调 ---- */

/** KCP 输出回调：将数据通过 UDP 发送出去
 *  @param buf      待发送数据
 *  @param len      数据长度
 *  @param user_data 用户自定义数据（创建时传入）
 *  @return 实际发送字节数，<0 表示错误
 */
typedef int (*CeKcpOutputFn)(const char* buf, int len, void* user_data);

/* ---- 生命周期 ---- */

/** 创建 KCP 上下文
 *  @param conv      会话 ID，两端必须一致
 *  @param user_data 用户自定义数据，会传递给输出回调
 *  @return 新创建的 KCP 上下文，失败返回 NULL
 */
CeKcpContext* ce_kcp_create(uint32_t conv, void* user_data);

/** 销毁 KCP 上下文，释放所有资源 */
void ce_kcp_destroy(CeKcpContext* ctx);

/* ---- 数据收发 ---- */

/** 输入原始 UDP 数据到 KCP 协议栈
 *  @param ctx   KCP 上下文
 *  @param data  原始数据
 *  @param len   数据长度
 *  @return 0 成功，<0 失败
 */
int ce_kcp_input(CeKcpContext* ctx, const void* data, int len);

/** 从 KCP 接收可靠数据（应用层读取）
 *  @param ctx     KCP 上下文
 *  @param buf     接收缓冲区
 *  @param max_len 缓冲区最大长度
 *  @return 实际读取字节数，<0 表示暂无数据
 */
int ce_kcp_recv(CeKcpContext* ctx, void* buf, int max_len);

/** 通过 KCP 发送可靠数据（应用层写入）
 *  @param ctx  KCP 上下文
 *  @param data 待发送数据
 *  @param len  数据长度
 *  @return 0 成功，<0 失败
 */
int ce_kcp_send(CeKcpContext* ctx, const void* data, int len);

/* ---- 状态驱动 ---- */

/** 驱动 KCP 状态机（需定时调用，建议 10-100ms 间隔）
 *  @param ctx        KCP 上下文
 *  @param current_ms 当前时间戳（毫秒）
 */
void ce_kcp_update(CeKcpContext* ctx, uint32_t current_ms);

/** 查询下次 update 的建议时间戳
 *  @param ctx        KCP 上下文
 *  @param current_ms 当前时间戳（毫秒）
 *  @return 建议下次调用 update 的时间戳
 */
uint32_t ce_kcp_check(const CeKcpContext* ctx, uint32_t current_ms);

/* ---- 配置 ---- */

/** 设置 KCP 参数
 *  @param ctx      KCP 上下文
 *  @param nodelay  0:禁用(默认), 1:启用快速模式
 *  @param interval 内部更新间隔(ms)，默认 100
 *  @param resend   0:禁用快速重传(默认), 1:启用
 *  @param nc       0:正常拥塞控制(默认), 1:禁用拥塞控制
 *  @return 0 成功
 *
 *  最快模式: ce_kcp_set_config(ctx, 1, 10, 2, 1)
 */
int ce_kcp_set_config(CeKcpContext* ctx, int nodelay, int interval,
                      int resend, int nc);

/** 设置窗口大小
 *  @param ctx     KCP 上下文
 *  @param sndwnd  发送窗口大小（默认 32）
 *  @param rcvwnd  接收窗口大小（默认 32）
 *  @return 0 成功
 */
int ce_kcp_set_wndsize(CeKcpContext* ctx, int sndwnd, int rcvwnd);

/** 设置 MTU
 *  @param ctx KCP 上下文
 *  @param mtu MTU 大小（默认 1400）
 *  @return 0 成功
 */
int ce_kcp_set_mtu(CeKcpContext* ctx, int mtu);

/* ---- 输出回调 ---- */

/** 设置 UDP 输出回调
 *  @param ctx       KCP 上下文
 *  @param callback  输出回调函数
 */
void ce_kcp_set_output_callback(CeKcpContext* ctx, CeKcpOutputFn callback);

/** 获取用户数据指针 */
void* ce_kcp_get_user_data(CeKcpContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CE_KCP_H */
