/*
 * ChaosEngine XDP 可编程包处理 — 用户态接口
 *
 * 提供 XDP 数据路径的性能基准测试和包过滤功能。
 * Linux 专属，需要网卡支持 XDP。
 */

#ifndef CE_XDP_H
#define CE_XDP_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 不透明句柄 ---- */

typedef struct CeXdpContext CeXdpContext;

/* ---- XDP 运行模式 ---- */

typedef enum CeXdpMode {
    CE_XDP_PASS = 0,  /* 传递给内核网络栈 */
    CE_XDP_DROP = 1,  /* 在网卡层丢弃 */
    CE_XDP_TX   = 2,  /* 从同一网卡发回 */
} CeXdpMode;

/* ---- 生命周期 ---- */

/** 初始化 XDP 上下文（加载 BPF 程序到指定网卡） */
CeXdpContext* ce_xdp_init(const char* iface_name);

/** 关闭 XDP 上下文（从网卡卸载 BPF 程序） */
void ce_xdp_shutdown(CeXdpContext* ctx);

/* ---- 运行模式 ---- */

/** 设置 XDP 运行模式 */
CeResult ce_xdp_set_mode(CeXdpContext* ctx, CeXdpMode mode);

/** 获取当前 XDP 运行模式 */
CeXdpMode ce_xdp_get_mode(CeXdpContext* ctx);

/* ---- 端口过滤 ---- */

/** 设置目标端口过滤（仅处理指定端口的包，0=不过滤） */
CeResult ce_xdp_set_port_filter(CeXdpContext* ctx, uint16_t port);

/* ---- 统计查询 ---- */

/** 获取接收的数据包总数 */
uint64_t ce_xdp_get_rx_packets(CeXdpContext* ctx);

/** 获取接收的字节总数 */
uint64_t ce_xdp_get_rx_bytes(CeXdpContext* ctx);

/** 获取传递给内核的包数 */
uint64_t ce_xdp_get_pass_packets(CeXdpContext* ctx);

/** 获取丢弃的包数 */
uint64_t ce_xdp_get_drop_packets(CeXdpContext* ctx);

/** 获取发回的包数 */
uint64_t ce_xdp_get_tx_packets(CeXdpContext* ctx);

/* ---- 查询 ---- */

/** XDP 是否可用（检查网卡是否支持 XDP） */
CeBool ce_xdp_available(const char* iface_name);

#ifdef __cplusplus
}
#endif

#endif /* CE_XDP_H */
