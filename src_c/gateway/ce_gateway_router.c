/*
 * ChaosEngine Gateway - 消息路由
 *
 * 路由表：排序数组 + 二分查找。
 * 每条规则匹配一个 msg_type 区间 [start, end]，映射到 backend_index。
 */

#define _POSIX_C_SOURCE 200112L

#include "gateway/ce_gateway.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * 路由表
 * ================================================================ */

#define CE_GATEWAY_MAX_ROUTES 64

typedef struct CeGatewayRoute {
    uint16_t    msg_type_start;  /* 区间起始（含） */
    uint16_t    msg_type_end;    /* 区间结束（含） */
    int         backend_index;   /* 目标后端索引，-1 = Gateway 本地处理 */
} CeGatewayRoute;

typedef struct CeGatewayRouteTable {
    CeGatewayRoute routes[CE_GATEWAY_MAX_ROUTES];
    int            count;
} CeGatewayRouteTable;

/* ================================================================
 * 创建/销毁路由表
 * ================================================================ */

CeGatewayRouteTable* ce_gateway_router_create(void)
{
    CeGatewayRouteTable* rt = (CeGatewayRouteTable*)calloc(1, sizeof(*rt));
    if (!rt) return NULL;

    /* 默认路由规则：
     *   0x0001-0x0FFF -> backend 0 (系统消息，Gateway 本地处理)
     *   0x0100-0x7FFF -> backend 0 (Game 服务)
     *   0x8000-0xFFFF -> backend 0 (用户消息，默认转发到 Game)
     *
     * 由于 0x0001-0x0FFF 是系统消息（PING/PONG/LOGIN 等），
     * Gateway 自己处理，标记 backend_index = -1。
     * 其余转发到 backend 0。
     */
    rt->count = 0;

    /* 系统消息：Gateway 本地处理，不转发 */
    rt->routes[rt->count].msg_type_start = 0x0001;
    rt->routes[rt->count].msg_type_end   = 0x0FFF;
    rt->routes[rt->count].backend_index  = -1;  /* 本地处理 */
    rt->count++;

    /* Game 数据消息：转发到 backend 0 */
    rt->routes[rt->count].msg_type_start = 0x1000;
    rt->routes[rt->count].msg_type_end   = 0x7FFF;
    rt->routes[rt->count].backend_index  = 0;
    rt->count++;

    /* 用户消息：转发到 backend 0 */
    rt->routes[rt->count].msg_type_start = 0x8000;
    rt->routes[rt->count].msg_type_end   = 0xFFFF;
    rt->routes[rt->count].backend_index  = 0;
    rt->count++;

    return rt;
}

void ce_gateway_router_destroy(CeGatewayRouteTable* rt)
{
    free(rt);
}

/* ================================================================
 * 路由查找（二分查找）
 * ================================================================ */

/**
 * 查找消息类型对应的后端索引。
 *
 * @param rt        路由表
 * @param msg_type  消息类型
 * @return          backend_index（>=0 转发），-1 本地处理，-2 无匹配规则
 */
int ce_gateway_router_find(const CeGatewayRouteTable* rt, uint16_t msg_type)
{
    if (!rt || rt->count == 0) return -2;

    /* 二分查找：路由表已按 msg_type_start 排序 */
    int lo = 0, hi = rt->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const CeGatewayRoute* r = &rt->routes[mid];

        if (msg_type < r->msg_type_start) {
            hi = mid - 1;
        } else if (msg_type > r->msg_type_end) {
            lo = mid + 1;
        } else {
            /* 命中区间 */
            return r->backend_index;
        }
    }

    return -2;  /* 无匹配规则 */
}

/**
 * 添加路由规则（保持有序）
 */
CeResult ce_gateway_router_add(CeGatewayRouteTable* rt,
                               uint16_t msg_type_start,
                               uint16_t msg_type_end,
                               int backend_index)
{
    if (!rt) return CE_ERR;
    if (rt->count >= CE_GATEWAY_MAX_ROUTES) return CE_ERR;
    if (msg_type_start > msg_type_end) return CE_ERR;

    /* 找到插入位置（保持按 start 排序） */
    int pos = 0;
    while (pos < rt->count && rt->routes[pos].msg_type_start < msg_type_start) {
        pos++;
    }

    /* 后移 */
    for (int i = rt->count; i > pos; i--) {
        rt->routes[i] = rt->routes[i - 1];
    }

    rt->routes[pos].msg_type_start = msg_type_start;
    rt->routes[pos].msg_type_end   = msg_type_end;
    rt->routes[pos].backend_index  = backend_index;
    rt->count++;

    return CE_OK;
}
