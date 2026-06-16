/*
 * ChaosEngine KCP 封装 — 实现
 * 纯 C99，封装 ikcp_create/ikcp_release/ikcp_input/ikcp_recv/ikcp_send/ikcp_update
 */

#include "network/ce_kcp.h"
#include "network/ikcp.h"

#include <stdlib.h>
#include <string.h>

/* ---- 内部结构 ---- */

struct CeKcpContext {
    ikcpcb*     kcp;        /* KCP 控制块 */
    void*       user_data;  /* 用户自定义数据 */
    CeKcpOutputFn output_fn; /* UDP 输出回调 */
};

/* ---- 输出回调桥接 ---- */

static int ce_kcp_output_bridge(const char* buf, int len, ikcpcb* kcp, void* user) {
    (void)kcp;
    CeKcpContext* ctx = (CeKcpContext*)user;
    if (ctx && ctx->output_fn) {
        return ctx->output_fn(buf, len, ctx->user_data);
    }
    return 0;
}

/* ---- 生命周期 ---- */

CeKcpContext* ce_kcp_create(uint32_t conv, void* user_data) {
    CeKcpContext* ctx = (CeKcpContext*)malloc(sizeof(CeKcpContext));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(CeKcpContext));
    ctx->user_data = user_data;
    ctx->output_fn = NULL;

    ctx->kcp = ikcp_create(conv, ctx);
    if (!ctx->kcp) {
        free(ctx);
        return NULL;
    }

    /* 设置默认输出回调（桥接到我们的回调管理） */
    ikcp_setoutput(ctx->kcp, ce_kcp_output_bridge);

    return ctx;
}

void ce_kcp_destroy(CeKcpContext* ctx) {
    if (!ctx) return;

    if (ctx->kcp) {
        ikcp_release(ctx->kcp);
        ctx->kcp = NULL;
    }

    free(ctx);
}

/* ---- 数据收发 ---- */

int ce_kcp_input(CeKcpContext* ctx, const void* data, int len) {
    if (!ctx || !ctx->kcp || !data || len <= 0) return -1;
    return ikcp_input(ctx->kcp, (const char*)data, (long)len);
}

int ce_kcp_recv(CeKcpContext* ctx, void* buf, int max_len) {
    if (!ctx || !ctx->kcp || !buf || max_len <= 0) return -1;
    return ikcp_recv(ctx->kcp, (char*)buf, max_len);
}

int ce_kcp_send(CeKcpContext* ctx, const void* data, int len) {
    if (!ctx || !ctx->kcp || !data || len <= 0) return -1;
    return ikcp_send(ctx->kcp, (const char*)data, len);
}

/* ---- 状态驱动 ---- */

void ce_kcp_update(CeKcpContext* ctx, uint32_t current_ms) {
    if (!ctx || !ctx->kcp) return;
    ikcp_update(ctx->kcp, (IUINT32)current_ms);
}

uint32_t ce_kcp_check(const CeKcpContext* ctx, uint32_t current_ms) {
    if (!ctx || !ctx->kcp) return current_ms;
    return (uint32_t)ikcp_check(ctx->kcp, (IUINT32)current_ms);
}

/* ---- 配置 ---- */

int ce_kcp_set_config(CeKcpContext* ctx, int nodelay, int interval,
                      int resend, int nc) {
    if (!ctx || !ctx->kcp) return -1;
    return ikcp_nodelay(ctx->kcp, nodelay, interval, resend, nc);
}

int ce_kcp_set_wndsize(CeKcpContext* ctx, int sndwnd, int rcvwnd) {
    if (!ctx || !ctx->kcp) return -1;
    return ikcp_wndsize(ctx->kcp, sndwnd, rcvwnd);
}

int ce_kcp_set_mtu(CeKcpContext* ctx, int mtu) {
    if (!ctx || !ctx->kcp) return -1;
    return ikcp_setmtu(ctx->kcp, mtu);
}

/* ---- 输出回调 ---- */

void ce_kcp_set_output_callback(CeKcpContext* ctx, CeKcpOutputFn callback) {
    if (!ctx) return;
    ctx->output_fn = callback;
}

void* ce_kcp_get_user_data(CeKcpContext* ctx) {
    if (!ctx) return NULL;
    return ctx->user_data;
}
