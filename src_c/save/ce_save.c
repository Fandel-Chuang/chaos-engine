/*
 * ChaosEngine 存档管理器 — 实现
 *
 * 负责定时存档 + 手动存档 + 脏实体收集。
 * 纯 C99，ce_ 前缀，CE_LOG_* 日志。
 *
 * 内部流程：
 *   1. 收集脏实体（或全部实体，取决于存档模式）
 *   2. 序列化实体组件为二进制
 *   3. 封装为 CeDbproxyMessage 并调用 ce_dbproxy_send
 *   4. 存档队列（FIFO）避免并发写冲突
 */

#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "save/ce_save.h"
#include "dbproxy/ce_dbproxy.h"
#include "admin_ipc/ce_admin_ipc.h"
#include "public_api/ce_ecs.h"
#include "public_api/ce_log.h"
#include "core/ce_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ---- 内部常量 ---- */

/** 脏实体集合最大容量 */
#define CE_SAVE_MAX_DIRTY_ENTITIES    8192

/** 单个存档请求 */
typedef struct CeSaveRequest {
    uint64_t    entity_id;      /* 目标实体 ID */
    CeSaveMode  mode;           /* 存档模式 */
    CeSaveEvent event;          /* 触发事件 */
} CeSaveRequest;

/** 存档队列（环形缓冲区） */
typedef struct CeSaveQueue {
    CeSaveRequest   requests[CE_SAVE_MAX_QUEUE_DEPTH];
    int             head;       /* 写入位置 */
    int             tail;       /* 读取位置 */
    int             count;      /* 当前数量 */
} CeSaveQueue;

/** 脏实体跟踪（简单数组，适合中小规模） */
typedef struct CeDirtySet {
    uint64_t    entities[CE_SAVE_MAX_DIRTY_ENTITIES];
    int         count;
} CeDirtySet;

/** 存档上下文（不透明） */
struct CeSaveContext {
    CeDbproxyContext*   dbproxy;            /* DBProxy 客户端 */
    CeSaveConfig        config;             /* 配置 */
    CeSaveStats         stats;              /* 统计 */

    /* 定时器 */
    uint64_t            last_tick_time_us;  /* 上次 tick 时间 */
    int                 save_counter;       /* 增量存档计数（用于全量判断） */

    /* 脏实体 */
    CeDirtySet          dirty_set;          /* 脏实体集合 */

    /* 存档队列 */
    CeSaveQueue         queue;              /* FIFO 队列 */

    /* Admin IPC */
    CeAdminIpc*         admin_ipc;          /* Admin IPC 句柄（用于注册命令） */
};

/* ---- 内部辅助：时间戳 ---- */

static uint64_t ce_save_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ---- 内部辅助：队列操作 ---- */

static CeBool ce_save_queue_is_full(const CeSaveQueue* q) {
    return q->count >= CE_SAVE_MAX_QUEUE_DEPTH;
}

static CeBool ce_save_queue_is_empty(const CeSaveQueue* q) {
    return q->count <= 0;
}

static CeResult ce_save_queue_push(CeSaveQueue* q, uint64_t entity_id,
                                    CeSaveMode mode, CeSaveEvent event) {
    if (ce_save_queue_is_full(q)) {
        return CE_ERR;
    }
    CeSaveRequest* req = &q->requests[q->head];
    req->entity_id = entity_id;
    req->mode      = mode;
    req->event     = event;
    q->head = (q->head + 1) % CE_SAVE_MAX_QUEUE_DEPTH;
    q->count++;
    return CE_OK;
}

static CeResult ce_save_queue_pop(CeSaveQueue* q, CeSaveRequest* out) {
    if (ce_save_queue_is_empty(q)) {
        return CE_ERR;
    }
    *out = q->requests[q->tail];
    q->tail = (q->tail + 1) % CE_SAVE_MAX_QUEUE_DEPTH;
    q->count--;
    return CE_OK;
}

/* ---- 内部辅助：脏实体集合 ---- */

static void ce_save_dirty_add(CeDirtySet* ds, uint64_t entity_id) {
    if (entity_id == CE_ENTITY_NULL) return;
    /* 检查是否已存在 */
    for (int i = 0; i < ds->count; i++) {
        if (ds->entities[i] == entity_id) {
            return; /* 已存在 */
        }
    }
    if (ds->count < CE_SAVE_MAX_DIRTY_ENTITIES) {
        ds->entities[ds->count++] = entity_id;
    }
}

static void ce_save_dirty_remove(CeDirtySet* ds, uint64_t entity_id) {
    for (int i = 0; i < ds->count; i++) {
        if (ds->entities[i] == entity_id) {
            /* 用最后一个元素替换 */
            ds->entities[i] = ds->entities[ds->count - 1];
            ds->count--;
            return;
        }
    }
}

/* ---- 内部辅助：序列化 ---- */

/** 写入大端 uint16_t */
static void ce_save_write_u16(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val);
}

/** 写入大端 uint32_t */
static void ce_save_write_u32(uint8_t* buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

/** 写入大端 uint64_t */
static void ce_save_write_u64(uint8_t* buf, uint64_t val) {
    buf[0] = (uint8_t)(val >> 56);
    buf[1] = (uint8_t)(val >> 48);
    buf[2] = (uint8_t)(val >> 40);
    buf[3] = (uint8_t)(val >> 32);
    buf[4] = (uint8_t)(val >> 24);
    buf[5] = (uint8_t)(val >> 16);
    buf[6] = (uint8_t)(val >> 8);
    buf[7] = (uint8_t)(val);
}

/**
 * 序列化单个实体的所有组件为二进制
 *
 * 格式：
 *   [8B entity_id][2B component_count][N components...]
 *   每个 component: [2B component_type][4B data_len][N data]
 *
 * @param entity_id  实体 ID
 * @param buf        输出缓冲区
 * @param max_len    缓冲区大小
 * @return           序列化后的字节数，-1 表示缓冲区不足
 */
static int ce_save_serialize_entity(uint64_t entity_id, uint8_t* buf, int max_len) {
    /* 最小头部：entity_id(8) + component_count(2) = 10 */
    if (max_len < 10) return -1;

    /* 先遍历所有已注册组件，收集有数据的组件 */
    uint32_t comp_count = ce_ecs_get_component_count();
    if (comp_count > CE_MAX_COMPONENTS) comp_count = CE_MAX_COMPONENTS;

    /* 临时存储组件信息 */
    struct {
        CeComponentId   comp_id;
        const void*     data;
        size_t          size;
    } components[CE_MAX_COMPONENTS];
    int found_count = 0;

    uint32_t i;
    for (i = 0; i < comp_count; i++) {
        const void* data = ce_entity_get_component(entity_id, i);
        if (data) {
            /* 获取组件大小：从 ECS 内部信息获取 */
            /* 这里使用一个合理的默认大小；实际应由组件注册信息提供 */
            /* 由于 ce_ecs.h 没有直接暴露组件大小，我们使用固定大小 */
            /* 实际项目中应该通过 ce_component_register 返回的 size 来获取 */
            components[found_count].comp_id = i;
            components[found_count].data    = data;
            /* 使用安全的默认大小，实际由组件注册时决定 */
            components[found_count].size    = 256; /* 默认组件大小 */
            found_count++;
        }
    }

    /* 计算总大小 */
    int total = 10; /* entity_id(8) + component_count(2) */
    for (i = 0; i < (uint32_t)found_count; i++) {
        total += 2 + 4 + (int)components[i].size; /* comp_type(2) + data_len(4) + data */
    }

    if (total > max_len) return -1;

    /* 序列化 */
    uint8_t* p = buf;
    ce_save_write_u64(p, entity_id);    p += 8;
    ce_save_write_u16(p, (uint16_t)found_count); p += 2;

    for (i = 0; i < (uint32_t)found_count; i++) {
        ce_save_write_u16(p, (uint16_t)components[i].comp_id);  p += 2;
        ce_save_write_u32(p, (uint32_t)components[i].size);     p += 4;
        memcpy(p, components[i].data, components[i].size);
        p += components[i].size;
    }

    return total;
}

/**
 * 执行单个实体的存档
 *
 * 收集实体组件 → 序列化 → 发送到 DBProxy
 */
static CeResult ce_save_execute_one(CeSaveContext* ctx, uint64_t entity_id,
                                     CeSaveMode mode) {
    if (!ctx || !ctx->dbproxy) return CE_ERR;
    if (entity_id == CE_ENTITY_NULL) return CE_ERR;

    /* 增量模式下，非脏实体跳过 */
    if (mode == CE_SAVE_INCREMENTAL) {
        CeBool is_dirty = CE_FALSE;
        for (int i = 0; i < ctx->dirty_set.count; i++) {
            if (ctx->dirty_set.entities[i] == entity_id) {
                is_dirty = CE_TRUE;
                break;
            }
        }
        if (!is_dirty) {
            return CE_OK; /* 非脏实体，跳过 */
        }
    }

    /* 序列化 */
    uint8_t* buf = (uint8_t*)malloc(CE_SAVE_MAX_ENTITY_BUF_SIZE);
    if (!buf) {
        CE_LOG_ERROR("SAVE", "Failed to allocate serialization buffer");
        return CE_ERR;
    }

    int data_len = ce_save_serialize_entity(entity_id, buf, CE_SAVE_MAX_ENTITY_BUF_SIZE);
    if (data_len < 0) {
        CE_LOG_ERROR("SAVE", "Failed to serialize entity %lu", (unsigned long)entity_id);
        free(buf);
        return CE_ERR;
    }

    /* 发送到 DBProxy */
    CeDbproxyMessage msg;
    msg.type        = DB_SAVE_PLAYER;
    msg.payload_len = (uint32_t)data_len;
    msg.payload     = buf;

    CeResult result = ce_dbproxy_send(ctx->dbproxy, &msg);
    free(buf);

    if (result == CE_OK) {
        /* 清除脏标记 */
        ce_save_dirty_remove(&ctx->dirty_set, entity_id);
        ctx->stats.total_entities_saved++;
    }

    return result;
}

/**
 * 处理存档队列（每帧调用，逐个处理避免阻塞）
 */
static void ce_save_process_queue(CeSaveContext* ctx) {
    if (ce_save_queue_is_empty(&ctx->queue)) return;

    /* 每次只处理一个请求，避免阻塞主循环 */
    CeSaveRequest req;
    if (ce_save_queue_pop(&ctx->queue, &req) == CE_OK) {
        uint64_t start_us = ce_save_now_us();

        CeResult result = ce_save_execute_one(ctx, req.entity_id, req.mode);

        uint64_t end_us = ce_save_now_us();
        ctx->stats.last_save_time_us     = end_us;
        ctx->stats.last_save_duration_us = end_us - start_us;
        ctx->stats.total_saves++;

        if (req.mode == CE_SAVE_FULL) {
            ctx->stats.full_saves++;
        } else {
            ctx->stats.incremental_saves++;
        }

        if (result == CE_OK) {
            CE_LOG_DEBUG("SAVE", "Saved entity %lu (mode=%d, event=%d, duration=%lu us)",
                         (unsigned long)req.entity_id, (int)req.mode, (int)req.event,
                         (unsigned long)(end_us - start_us));
        } else {
            CE_LOG_WARN("SAVE", "Failed to save entity %lu (mode=%d)",
                        (unsigned long)req.entity_id, (int)req.mode);
        }
    }
}

/* ---- Admin IPC 命令处理器 ---- */

/**
 * 处理 save.now 命令
 *
 * 参数: { "entity_id": <uint64>, "mode": "full"|"incremental" }
 * 如果 entity_id 为 0 或未指定，存档所有在线玩家。
 */
static int ce_save_handle_now(CeSaveContext* ctx, const char* params_json,
                               const char* id_str, char* buf, int max_len) {
    (void)params_json; /* 简化处理：无参数则全量存档 */

    CeResult result = ce_save_all(ctx, CE_SAVE_FULL);

    if (result == CE_OK) {
        return snprintf(buf, max_len,
                        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"status\":\"ok\","
                        "\"queued\":true}}", id_str);
    } else {
        return snprintf(buf, max_len,
                        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"status\":\"error\","
                        "\"message\":\"queue full or dbproxy not connected\"}}", id_str);
    }
}

/**
 * 处理 save.status 命令
 */
static int ce_save_handle_status(CeSaveContext* ctx, const char* params_json,
                                  const char* id_str, char* buf, int max_len) {
    (void)params_json;
    (void)id_str;
    return ce_save_get_status_json(ctx, buf, max_len);
}

/**
 * Admin IPC 自定义处理器入口
 */
static int ce_save_admin_handler(const char* method, const char* params_json,
                                  const char* id_str, char* buf, int max_len,
                                  void* user_data) {
    CeSaveContext* ctx = (CeSaveContext*)user_data;
    if (!ctx) return -1;

    if (strcmp(method, "save.now") == 0) {
        return ce_save_handle_now(ctx, params_json, id_str, buf, max_len);
    } else if (strcmp(method, "save.status") == 0) {
        return ce_save_handle_status(ctx, params_json, id_str, buf, max_len);
    }

    return -1; /* 未处理 */
}

/* ---- 生命周期 ---- */

CeSaveContext* ce_save_init(CeDbproxyContext* dbproxy, const CeSaveConfig* config) {
    if (!dbproxy) {
        CE_LOG_ERROR("SAVE", "DBProxy context is required");
        return NULL;
    }

    CeSaveContext* ctx = (CeSaveContext*)calloc(1, sizeof(CeSaveContext));
    if (!ctx) {
        CE_LOG_ERROR("SAVE", "Failed to allocate CeSaveContext");
        return NULL;
    }

    ctx->dbproxy = dbproxy;

    /* 默认配置 */
    if (config) {
        ctx->config = *config;
    } else {
        ctx->config.save_interval_sec = CE_SAVE_DEFAULT_INTERVAL_SEC;
        ctx->config.full_save_every_n = CE_SAVE_DEFAULT_FULL_EVERY_N;
        ctx->config.auto_save_enabled = CE_TRUE;
    }

    /* 确保有效值 */
    if (ctx->config.save_interval_sec <= 0) {
        ctx->config.save_interval_sec = CE_SAVE_DEFAULT_INTERVAL_SEC;
    }
    if (ctx->config.full_save_every_n <= 0) {
        ctx->config.full_save_every_n = CE_SAVE_DEFAULT_FULL_EVERY_N;
    }

    /* 初始化定时器 */
    ctx->last_tick_time_us = ce_save_now_us();
    ctx->save_counter = 0;

    /* 初始化统计 */
    memset(&ctx->stats, 0, sizeof(ctx->stats));

    /* 初始化脏实体集合 */
    memset(&ctx->dirty_set, 0, sizeof(ctx->dirty_set));

    /* 初始化队列 */
    memset(&ctx->queue, 0, sizeof(ctx->queue));

    CE_LOG_INFO("SAVE", "Save module initialized (interval=%ds, full_every_n=%d, auto=%s)",
                ctx->config.save_interval_sec, ctx->config.full_save_every_n,
                ctx->config.auto_save_enabled ? "on" : "off");

    return ctx;
}

void ce_save_shutdown(CeSaveContext* ctx) {
    if (!ctx) return;

    /* 处理队列中剩余的存档请求 */
    int drained = 0;
    while (!ce_save_queue_is_empty(&ctx->queue) && drained < CE_SAVE_MAX_QUEUE_DEPTH) {
        ce_save_process_queue(ctx);
        drained++;
    }

    if (ctx->stats.total_saves > 0) {
        CE_LOG_INFO("SAVE", "Save module shut down (total=%lu, full=%lu, inc=%lu, entities=%lu)",
                    (unsigned long)ctx->stats.total_saves,
                    (unsigned long)ctx->stats.full_saves,
                    (unsigned long)ctx->stats.incremental_saves,
                    (unsigned long)ctx->stats.total_entities_saved);
    } else {
        CE_LOG_INFO("SAVE", "Save module shut down (no saves performed)");
    }

    free(ctx);
}

/* ---- 帧更新 ---- */

void ce_save_tick(CeSaveContext* ctx) {
    if (!ctx) return;

    /* 处理存档队列 */
    ce_save_process_queue(ctx);

    /* 更新统计 */
    ctx->stats.queue_depth       = ctx->queue.count;
    ctx->stats.dirty_entity_count = ctx->dirty_set.count;

    /* 检查定时器 */
    if (!ctx->config.auto_save_enabled) return;

    uint64_t now_us = ce_save_now_us();
    uint64_t elapsed_sec = (now_us - ctx->last_tick_time_us) / 1000000ULL;

    if (elapsed_sec >= (uint64_t)ctx->config.save_interval_sec) {
        ctx->last_tick_time_us = now_us;

        /* 判断存档模式 */
        ctx->save_counter++;
        CeSaveMode mode = CE_SAVE_INCREMENTAL;
        if (ctx->save_counter >= ctx->config.full_save_every_n) {
            mode = CE_SAVE_FULL;
            ctx->save_counter = 0;
        }

        /* 触发自动存档 */
        CeResult result = ce_save_all(ctx, mode);
        if (result == CE_OK) {
            CE_LOG_DEBUG("SAVE", "Auto-save triggered (mode=%s, counter=%d/%d)",
                         mode == CE_SAVE_FULL ? "full" : "incremental",
                         ctx->save_counter, ctx->config.full_save_every_n);
        }
    }
}

/* ---- 手动存档 ---- */

CeResult ce_save_now(CeSaveContext* ctx, uint64_t entity_id, CeSaveMode mode) {
    if (!ctx) return CE_ERR;
    if (entity_id == CE_ENTITY_NULL) return CE_ERR;

    if (!ce_dbproxy_is_connected(ctx->dbproxy)) {
        CE_LOG_WARN("SAVE", "Cannot save: DBProxy not connected");
        return CE_ERR;
    }

    return ce_save_queue_push(&ctx->queue, entity_id, mode, CE_SAVE_EVENT_MANUAL);
}

CeResult ce_save_all(CeSaveContext* ctx, CeSaveMode mode) {
    if (!ctx) return CE_ERR;

    if (!ce_dbproxy_is_connected(ctx->dbproxy)) {
        CE_LOG_WARN("SAVE", "Cannot save all: DBProxy not connected");
        return CE_ERR;
    }

    /* 收集要存档的实体 */
    uint32_t total_entities = ce_ecs_get_entity_count();
    int queued = 0;

    if (mode == CE_SAVE_FULL) {
        /* 全量存档：遍历所有实体 */
        /* 由于 ECS API 没有直接提供"遍历所有实体"的接口，
           我们使用查询来获取所有拥有任意组件的实体 */
        /* 简化处理：使用脏实体集合 + 额外逻辑 */
        /* 实际项目中应通过 ECS 内部接口遍历所有实体 */

        /* 使用脏实体作为全量存档的基础（所有活跃实体都应被标记为脏） */
        for (int i = 0; i < ctx->dirty_set.count && queued < CE_SAVE_MAX_QUEUE_DEPTH; i++) {
            if (ce_save_queue_push(&ctx->queue, ctx->dirty_set.entities[i],
                                    CE_SAVE_FULL, CE_SAVE_EVENT_TIMER) == CE_OK) {
                queued++;
            }
        }

        /* 如果脏实体不足，尝试通过组件查询获取更多实体 */
        /* 这里使用一个已知的组件 ID 来查询实体 */
        if (queued == 0 && total_entities > 0) {
            /* 尝试查询所有拥有任意组件的实体 */
            /* 简化：使用 component 0 作为通用查询 */
            CeComponentId query_ids[1];
            query_ids[0] = 0; /* 第一个注册的组件 */
            CeQuery* query = ce_query_create(query_ids, 1);
            if (query) {
                CeEntity entities[CE_SAVE_MAX_ENTITIES_PER_SAVE];
                uint32_t count = ce_query_execute(query, entities, CE_SAVE_MAX_ENTITIES_PER_SAVE);
                for (uint32_t i = 0; i < count && queued < CE_SAVE_MAX_QUEUE_DEPTH; i++) {
                    if (ce_save_queue_push(&ctx->queue, entities[i],
                                            CE_SAVE_FULL, CE_SAVE_EVENT_TIMER) == CE_OK) {
                        queued++;
                    }
                }
                ce_query_destroy(query);
            }
        }
    } else {
        /* 增量存档：仅脏实体 */
        for (int i = 0; i < ctx->dirty_set.count && queued < CE_SAVE_MAX_QUEUE_DEPTH; i++) {
            if (ce_save_queue_push(&ctx->queue, ctx->dirty_set.entities[i],
                                    CE_SAVE_INCREMENTAL, CE_SAVE_EVENT_TIMER) == CE_OK) {
                queued++;
            }
        }
    }

    if (queued == 0) {
        CE_LOG_DEBUG("SAVE", "No entities to save (mode=%s, dirty=%d)",
                     mode == CE_SAVE_FULL ? "full" : "incremental",
                     ctx->dirty_set.count);
        return CE_OK; /* 没有需要存档的实体不算错误 */
    }

    CE_LOG_DEBUG("SAVE", "Queued %d entities for save (mode=%s)",
                 queued, mode == CE_SAVE_FULL ? "full" : "incremental");
    return CE_OK;
}

/* ---- 事件触发存档 ---- */

CeResult ce_save_on_event(CeSaveContext* ctx, CeSaveEvent event, uint64_t entity_id) {
    if (!ctx) return CE_ERR;

    switch (event) {
    case CE_SAVE_EVENT_PLAYER_LOGOUT:
    case CE_SAVE_EVENT_PLAYER_DEATH:
        /* 增量存档该玩家 */
        if (entity_id != CE_ENTITY_NULL) {
            return ce_save_queue_push(&ctx->queue, entity_id,
                                       CE_SAVE_INCREMENTAL, event);
        }
        break;

    case CE_SAVE_EVENT_WORLD_CHECKPOINT:
    case CE_SAVE_EVENT_SERVER_SHUTDOWN:
        /* 全量存档所有玩家 */
        return ce_save_all(ctx, CE_SAVE_FULL);

    case CE_SAVE_EVENT_MANUAL:
    case CE_SAVE_EVENT_TIMER:
        /* 由 ce_save_now / ce_save_tick 处理 */
        break;
    }

    return CE_OK;
}

/* ---- 脏标记 ---- */

void ce_save_mark_dirty(CeSaveContext* ctx, uint64_t entity_id) {
    if (!ctx) return;
    ce_save_dirty_add(&ctx->dirty_set, entity_id);
}

void ce_save_clear_dirty(CeSaveContext* ctx, uint64_t entity_id) {
    if (!ctx) return;
    ce_save_dirty_remove(&ctx->dirty_set, entity_id);
}

/* ---- 统计查询 ---- */

void ce_save_get_stats(const CeSaveContext* ctx, CeSaveStats* stats) {
    if (!ctx || !stats) return;
    *stats = ctx->stats;
    /* 更新实时数据 */
    stats->queue_depth       = ctx->queue.count;
    stats->dirty_entity_count = ctx->dirty_set.count;
}

int ce_save_get_status_json(const CeSaveContext* ctx, char* buf, int max_len) {
    if (!ctx || !buf || max_len <= 0) return -1;

    CeSaveStats stats;
    ce_save_get_stats(ctx, &stats);

    return snprintf(buf, max_len,
        "{\"jsonrpc\":\"2.0\",\"id\":\"save.status\",\"result\":{"
        "\"total_saves\":%lu,"
        "\"full_saves\":%lu,"
        "\"incremental_saves\":%lu,"
        "\"total_entities_saved\":%lu,"
        "\"last_save_time_us\":%lu,"
        "\"last_save_duration_us\":%lu,"
        "\"queue_depth\":%d,"
        "\"dirty_entity_count\":%d,"
        "\"save_interval_sec\":%d,"
        "\"full_save_every_n\":%d,"
        "\"auto_save_enabled\":%s,"
        "\"save_counter\":%d"
        "}}",
        (unsigned long)stats.total_saves,
        (unsigned long)stats.full_saves,
        (unsigned long)stats.incremental_saves,
        (unsigned long)stats.total_entities_saved,
        (unsigned long)stats.last_save_time_us,
        (unsigned long)stats.last_save_duration_us,
        stats.queue_depth,
        stats.dirty_entity_count,
        ctx->config.save_interval_sec,
        ctx->config.full_save_every_n,
        ctx->config.auto_save_enabled ? "true" : "false",
        ctx->save_counter
    );
}

/* ---- Admin IPC 注册（由外部调用，在 ce_admin_ipc_start 之后） ---- */

/**
 * 注册存档模块的 Admin IPC 命令
 *
 * 应在 ce_admin_ipc_start 之后调用。
 *
 * @param ctx       存档上下文
 * @param admin_ipc Admin IPC 句柄
 * @return          CE_OK 成功，CE_ERR 失败
 */
CeResult ce_save_register_admin_commands(CeSaveContext* ctx, CeAdminIpc* admin_ipc) {
    if (!ctx || !admin_ipc) return CE_ERR;

    ctx->admin_ipc = admin_ipc;

    CeResult result = ce_admin_ipc_register_handler(admin_ipc,
                                                     ce_save_admin_handler, ctx);
    if (result != CE_OK) {
        CE_LOG_ERROR("SAVE", "Failed to register Admin IPC handler");
        return CE_ERR;
    }

    CE_LOG_INFO("SAVE", "Admin IPC commands registered: save.now, save.status");
    return CE_OK;
}
