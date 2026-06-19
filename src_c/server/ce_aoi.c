/*
 * ChaosEngine AOI 十字链表 — 实现
 * 纯 C99，单线程安全
 */

#include "server/ce_aoi.h"
#include "public_api/ce_log.h"
#include "core/ce_memory.h"
#include "replication/ce_replication.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---- AOI 节点 ---- */

typedef struct CeAoiNode {
    CeServerEntityId entity_id;

    /* 位置（2D 地图坐标） */
    float x;
    float y;

    /* 十字链表指针 */
    struct CeAoiNode* x_prev;
    struct CeAoiNode* x_next;
    struct CeAoiNode* y_prev;
    struct CeAoiNode* y_next;

    /* 实体半径 */
    float radius;

    /* 标记位（用于查询去重） */
    int visit_mark;
} CeAoiNode;

/* ---- 全局状态 ---- */

static struct {
    CeAoiNode**        nodes;          /* 按 entity_id 索引的节点数组 */
    int                node_capacity;  /* 数组容量 */
    int                node_count;     /* 当前节点数 */

    CeAoiNode*         x_head;         /* X 轴链表头 */
    CeAoiNode*         y_head;         /* Y 轴链表头 */

    float              aoi_radius;     /* AOI 视野半径 */
    int                visit_counter;  /* 查询去重计数器 */

    CeAoiEventCallback event_callback;
    void*              event_user_data;

    CeReplContext*     repl_ctx;       /* 复制管理器上下文 (NULL = 禁用) */

    CeBool             initialized;
} g_aoi;

/* ---- 辅助函数 ---- */

static float distance_sq(float x1, float y1, float x2, float y2) {
    float dx = x1 - x2;
    float dy = y1 - y2;
    return dx * dx + dy * dy;
}

static void ensure_capacity(CeServerEntityId entity_id) {
    if ((int)entity_id >= g_aoi.node_capacity) {
        int new_cap = g_aoi.node_capacity ? g_aoi.node_capacity * 2 : 1024;
        while ((int)entity_id >= new_cap) new_cap *= 2;

        CeAoiNode** new_nodes = (CeAoiNode**)realloc(
            g_aoi.nodes, sizeof(CeAoiNode*) * new_cap);
        if (!new_nodes) return;

        memset(new_nodes + g_aoi.node_capacity, 0,
               sizeof(CeAoiNode*) * (new_cap - g_aoi.node_capacity));
        g_aoi.nodes = new_nodes;
        g_aoi.node_capacity = new_cap;
    }
}

static void fire_event(CeAoiEventType type, CeServerEntityId subject,
                       CeServerEntityId object, float x, float y) {
    /* 复制集成: ENTER 事件 → 标记进入实体所有组件为脏 */
    if (g_aoi.repl_ctx) {
        if (type == CE_AOI_ENTER) {
            /* 当 object 进入 subject 的 AOI 范围时，
             * 标记 object 的所有非 SERVER_ONLY 字段为脏，
             * 触发完整同步给 subject 的客户端 */
            ce_repl_mark_dirty(g_aoi.repl_ctx, object, CE_REPL_ALL_COMPONENTS);
        } else if (type == CE_AOI_LEAVE) {
            /* MVP: 记录离开事件，实际离开通知在后续 Phase 实现 */
            CE_LOG_INFO("AOI", "entity %u left %u's AOI range (%.1f, %.1f)",
                        object, subject, x, y);
        }
    }

    if (!g_aoi.event_callback) return;
    CeAoiEvent event = {type, subject, object, x, y};
    g_aoi.event_callback(&event, g_aoi.event_user_data);
}

/* ---- X 轴链表操作 ---- */

static void x_list_insert(CeAoiNode* node) {
    if (!g_aoi.x_head) {
        g_aoi.x_head = node;
        node->x_prev = NULL;
        node->x_next = NULL;
        return;
    }

    CeAoiNode* cur = g_aoi.x_head;
    CeAoiNode* prev = NULL;

    /* 找到插入位置（X 坐标升序） */
    while (cur && cur->x < node->x) {
        prev = cur;
        cur = cur->x_next;
    }

    node->x_prev = prev;
    node->x_next = cur;

    if (prev) {
        prev->x_next = node;
    } else {
        g_aoi.x_head = node;
    }

    if (cur) {
        cur->x_prev = node;
    }
}

static void x_list_remove(CeAoiNode* node) {
    if (node->x_prev) {
        node->x_prev->x_next = node->x_next;
    } else {
        g_aoi.x_head = node->x_next;
    }

    if (node->x_next) {
        node->x_next->x_prev = node->x_prev;
    }

    node->x_prev = NULL;
    node->x_next = NULL;
}

/* ---- Y 轴链表操作 ---- */

static void y_list_insert(CeAoiNode* node) {
    if (!g_aoi.y_head) {
        g_aoi.y_head = node;
        node->y_prev = NULL;
        node->y_next = NULL;
        return;
    }

    CeAoiNode* cur = g_aoi.y_head;
    CeAoiNode* prev = NULL;

    /* 找到插入位置（Y 坐标升序） */
    while (cur && cur->y < node->y) {
        prev = cur;
        cur = cur->y_next;
    }

    node->y_prev = prev;
    node->y_next = cur;

    if (prev) {
        prev->y_next = node;
    } else {
        g_aoi.y_head = node;
    }

    if (cur) {
        cur->y_prev = node;
    }
}

static void y_list_remove(CeAoiNode* node) {
    if (node->y_prev) {
        node->y_prev->y_next = node->y_next;
    } else {
        g_aoi.y_head = node->y_next;
    }

    if (node->y_next) {
        node->y_next->y_prev = node->y_prev;
    }

    node->y_prev = NULL;
    node->y_next = NULL;
}

/* ---- 公共 API ---- */

void ce_aoi_init(float aoi_radius, CeAoiEventCallback callback, void* user_data) {
    if (g_aoi.initialized) return;

    memset(&g_aoi, 0, sizeof(g_aoi));
    g_aoi.aoi_radius     = aoi_radius;
    g_aoi.event_callback = callback;
    g_aoi.event_user_data = user_data;
    g_aoi.visit_counter  = 1;
    g_aoi.initialized    = CE_TRUE;

    CE_LOG_INFO("AOI", "Initialized (radius=%.1f)", aoi_radius);
}

void ce_aoi_shutdown(void) {
    if (!g_aoi.initialized) return;

    /* 释放所有节点 */
    for (int i = 0; i < g_aoi.node_capacity; i++) {
        if (g_aoi.nodes[i]) {
            free(g_aoi.nodes[i]);
        }
    }
    free(g_aoi.nodes);

    memset(&g_aoi, 0, sizeof(g_aoi));
    CE_LOG_INFO("AOI", "Shut down");
}

CeResult ce_aoi_enter(CeServerEntityId entity_id, float x, float y, float radius) {
    if (!g_aoi.initialized) return CE_ERR;

    ensure_capacity(entity_id);
    if (entity_id >= (CeServerEntityId)g_aoi.node_capacity) return CE_ERR;

    /* 不允许重复进入 */
    if (g_aoi.nodes[entity_id]) return CE_ERR;

    CeAoiNode* node = (CeAoiNode*)calloc(1, sizeof(CeAoiNode));
    if (!node) return CE_ERR;

    node->entity_id = entity_id;
    node->x         = x;
    node->y         = y;
    node->radius    = radius;

    /* 插入链表 */
    x_list_insert(node);
    y_list_insert(node);

    g_aoi.nodes[entity_id] = node;
    g_aoi.node_count++;

    /* 通知周围实体 */
    float radius_sq = g_aoi.aoi_radius * g_aoi.aoi_radius;

    /* 沿 X 轴向左遍历 */
    CeAoiNode* cur = node->x_prev;
    while (cur) {
        if (node->x - cur->x > g_aoi.aoi_radius) break;
        if (distance_sq(node->x, node->y, cur->x, cur->y) <= radius_sq) {
            fire_event(CE_AOI_ENTER, entity_id, cur->entity_id, x, y);
            fire_event(CE_AOI_ENTER, cur->entity_id, entity_id, cur->x, cur->y);
        }
        cur = cur->x_prev;
    }

    /* 沿 X 轴向右遍历 */
    cur = node->x_next;
    while (cur) {
        if (cur->x - node->x > g_aoi.aoi_radius) break;
        if (distance_sq(node->x, node->y, cur->x, cur->y) <= radius_sq) {
            fire_event(CE_AOI_ENTER, entity_id, cur->entity_id, x, y);
            fire_event(CE_AOI_ENTER, cur->entity_id, entity_id, cur->x, cur->y);
        }
        cur = cur->x_next;
    }

    return CE_OK;
}

void ce_aoi_leave(CeServerEntityId entity_id) {
    if (!g_aoi.initialized) return;
    if (entity_id >= (CeServerEntityId)g_aoi.node_capacity) return;

    CeAoiNode* node = g_aoi.nodes[entity_id];
    if (!node) return;

    /* 通知周围实体 */
    float radius_sq = g_aoi.aoi_radius * g_aoi.aoi_radius;

    CeAoiNode* cur = node->x_prev;
    while (cur) {
        if (node->x - cur->x > g_aoi.aoi_radius) break;
        if (distance_sq(node->x, node->y, cur->x, cur->y) <= radius_sq) {
            fire_event(CE_AOI_LEAVE, entity_id, cur->entity_id, node->x, node->y);
            fire_event(CE_AOI_LEAVE, cur->entity_id, entity_id, cur->x, cur->y);
        }
        cur = cur->x_prev;
    }

    cur = node->x_next;
    while (cur) {
        if (cur->x - node->x > g_aoi.aoi_radius) break;
        if (distance_sq(node->x, node->y, cur->x, cur->y) <= radius_sq) {
            fire_event(CE_AOI_LEAVE, entity_id, cur->entity_id, node->x, node->y);
            fire_event(CE_AOI_LEAVE, cur->entity_id, entity_id, cur->x, cur->y);
        }
        cur = cur->x_next;
    }

    /* 从链表移除 */
    x_list_remove(node);
    y_list_remove(node);

    g_aoi.nodes[entity_id] = NULL;
    g_aoi.node_count--;

    free(node);
}

CeResult ce_aoi_move(CeServerEntityId entity_id, float new_x, float new_y) {
    if (!g_aoi.initialized) return CE_ERR;
    if (entity_id >= (CeServerEntityId)g_aoi.node_capacity) return CE_ERR;

    CeAoiNode* node = g_aoi.nodes[entity_id];
    if (!node) return CE_ERR;

    float old_x = node->x;
    float old_y = node->y;

    /* 收集移动前的周围实体 */
    float radius_sq = g_aoi.aoi_radius * g_aoi.aoi_radius;
    int old_count = 0;
    CeServerEntityId old_set[256];  /* 栈上分配，足够 */

    CeAoiNode* cur = node->x_prev;
    while (cur) {
        if (old_x - cur->x > g_aoi.aoi_radius) break;
        if (distance_sq(old_x, old_y, cur->x, cur->y) <= radius_sq) {
            if (old_count < 256) old_set[old_count++] = cur->entity_id;
        }
        cur = cur->x_prev;
    }
    cur = node->x_next;
    while (cur) {
        if (cur->x - old_x > g_aoi.aoi_radius) break;
        if (distance_sq(old_x, old_y, cur->x, cur->y) <= radius_sq) {
            if (old_count < 256) old_set[old_count++] = cur->entity_id;
        }
        cur = cur->x_next;
    }

    /* 从链表移除 */
    x_list_remove(node);
    y_list_remove(node);

    /* 更新坐标 */
    node->x = new_x;
    node->y = new_y;

    /* 重新插入链表 */
    x_list_insert(node);
    y_list_insert(node);

    /* 收集移动后的周围实体 */
    int new_count = 0;
    CeServerEntityId new_set[256];

    cur = node->x_prev;
    while (cur) {
        if (new_x - cur->x > g_aoi.aoi_radius) break;
        if (distance_sq(new_x, new_y, cur->x, cur->y) <= radius_sq) {
            if (new_count < 256) new_set[new_count++] = cur->entity_id;
        }
        cur = cur->x_prev;
    }
    cur = node->x_next;
    while (cur) {
        if (cur->x - new_x > g_aoi.aoi_radius) break;
        if (distance_sq(new_x, new_y, cur->x, cur->y) <= radius_sq) {
            if (new_count < 256) new_set[new_count++] = cur->entity_id;
        }
        cur = cur->x_next;
    }

    /* 触发事件：LEAVE (old - new), ENTER (new - old), MOVE (old ∩ new) */
    for (int i = 0; i < old_count; i++) {
        CeBool found = CE_FALSE;
        for (int j = 0; j < new_count; j++) {
            if (new_set[j] == old_set[i]) { found = CE_TRUE; break; }
        }
        if (found) {
            fire_event(CE_AOI_MOVE, entity_id, old_set[i], new_x, new_y);
            fire_event(CE_AOI_MOVE, old_set[i], entity_id, new_x, new_y);
        } else {
            fire_event(CE_AOI_LEAVE, entity_id, old_set[i], new_x, new_y);
            fire_event(CE_AOI_LEAVE, old_set[i], entity_id, new_x, new_y);
        }
    }

    for (int i = 0; i < new_count; i++) {
        CeBool found = CE_FALSE;
        for (int j = 0; j < old_count; j++) {
            if (old_set[j] == new_set[i]) { found = CE_TRUE; break; }
        }
        if (!found) {
            fire_event(CE_AOI_ENTER, entity_id, new_set[i], new_x, new_y);
            fire_event(CE_AOI_ENTER, new_set[i], entity_id, new_x, new_y);
        }
    }

    return CE_OK;
}

int ce_aoi_query_nearby(CeServerEntityId entity_id,
                         CeServerEntityId* buffer, int max_count) {
    if (!g_aoi.initialized) return 0;
    if (entity_id >= (CeServerEntityId)g_aoi.node_capacity) return 0;

    CeAoiNode* node = g_aoi.nodes[entity_id];
    if (!node) return 0;

    float radius_sq = g_aoi.aoi_radius * g_aoi.aoi_radius;
    int count = 0;

    /* 使用 visit_counter 去重 */
    int mark = ++g_aoi.visit_counter;
    if (mark == 0) mark = ++g_aoi.visit_counter;  /* 防止溢出回绕 */

    /* 沿 X 轴向左 */
    CeAoiNode* cur = node->x_prev;
    while (cur) {
        if (node->x - cur->x > g_aoi.aoi_radius) break;
        if (distance_sq(node->x, node->y, cur->x, cur->y) <= radius_sq) {
            if (cur->visit_mark != mark && count < max_count) {
                cur->visit_mark = mark;
                buffer[count++] = cur->entity_id;
            }
        }
        cur = cur->x_prev;
    }

    /* 沿 X 轴向右 */
    cur = node->x_next;
    while (cur) {
        if (cur->x - node->x > g_aoi.aoi_radius) break;
        if (distance_sq(node->x, node->y, cur->x, cur->y) <= radius_sq) {
            if (cur->visit_mark != mark && count < max_count) {
                cur->visit_mark = mark;
                buffer[count++] = cur->entity_id;
            }
        }
        cur = cur->x_next;
    }

    /* 沿 Y 轴向上 */
    cur = node->y_prev;
    while (cur) {
        if (node->y - cur->y > g_aoi.aoi_radius) break;
        if (distance_sq(node->x, node->y, cur->x, cur->y) <= radius_sq) {
            if (cur->visit_mark != mark && count < max_count) {
                cur->visit_mark = mark;
                buffer[count++] = cur->entity_id;
            }
        }
        cur = cur->y_prev;
    }

    /* 沿 Y 轴向下 */
    cur = node->y_next;
    while (cur) {
        if (cur->y - node->y > g_aoi.aoi_radius) break;
        if (distance_sq(node->x, node->y, cur->x, cur->y) <= radius_sq) {
            if (cur->visit_mark != mark && count < max_count) {
                cur->visit_mark = mark;
                buffer[count++] = cur->entity_id;
            }
        }
        cur = cur->y_next;
    }

    return count;
}

int ce_aoi_count_nearby(CeServerEntityId entity_id) {
    CeServerEntityId buffer[256];
    return ce_aoi_query_nearby(entity_id, buffer, 256);
}

int ce_aoi_entity_count(void) {
    return g_aoi.node_count;
}

void ce_aoi_debug_print(void) {
    if (!g_aoi.initialized) {
        printf("[AOI] Not initialized\n");
        return;
    }

    printf("[AOI] Entities: %d, Radius: %.1f\n", g_aoi.node_count, g_aoi.aoi_radius);

    printf("  X-axis: ");
    CeAoiNode* cur = g_aoi.x_head;
    while (cur) {
        printf("[%u@(%.0f,%.0f)]", cur->entity_id, cur->x, cur->y);
        cur = cur->x_next;
        if (cur) printf(" -> ");
    }
    printf("\n");

    printf("  Y-axis: ");
    cur = g_aoi.y_head;
    while (cur) {
        printf("[%u@(%.0f,%.0f)]", cur->entity_id, cur->x, cur->y);
        cur = cur->y_next;
        if (cur) printf(" -> ");
    }
    printf("\n");
}

/* ---- 复制集成 ---- */

void ce_aoi_set_replication_context(CeReplContext* ctx) {
    g_aoi.repl_ctx = ctx;
    if (ctx) {
        CE_LOG_INFO("AOI", "replication context set (AOI events will trigger dirty marking)");
    } else {
        CE_LOG_INFO("AOI", "replication context cleared");
    }
}
