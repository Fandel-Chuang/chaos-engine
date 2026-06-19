/*
 * ChaosEngine 日志系统（第八模式观测）
 * 日志由引擎内核产生，编辑器仅消费
 */

#ifndef CE_LOG_H
#define CE_LOG_H

#include "ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 日志回调 ---- */

/** 日志条目 */
typedef struct CeLogEntry {
    CeLogLevel  level;
    uint64_t    timestamp_us;
    const char* category;   /* 如 "ECS", "RENDER", "PLUGIN" */
    const char* message;
    const char* file;
    int         line;
} CeLogEntry;

/** 日志消费者回调（编辑器注册此回调来接收日志） */
typedef void (*CeLogCallback)(const CeLogEntry* entry, void* user_data);

/* ---- 日志 API ---- */

/** 注册日志消费者 */
void ce_log_add_callback(CeLogCallback callback, void* user_data);

/** 移除日志消费者 */
void ce_log_remove_callback(CeLogCallback callback);

/** 写入日志（引擎内部使用） */
void ce_log_write(CeLogLevel level, const char* category, 
                   const char* file, int line, const char* fmt, ...);

/** 便捷宏 */
#define CE_LOG_TRACE(cat, ...) \
    ce_log_write(CE_LOG_TRACE, cat, __FILE__, __LINE__, __VA_ARGS__)
#define CE_LOG_DEBUG(cat, ...) \
    ce_log_write(CE_LOG_DEBUG, cat, __FILE__, __LINE__, __VA_ARGS__)
#define CE_LOG_INFO(cat, ...)  \
    ce_log_write(CE_LOG_INFO,  cat, __FILE__, __LINE__, __VA_ARGS__)
#define CE_LOG_WARN(cat, ...)  \
    ce_log_write(CE_LOG_WARN,  cat, __FILE__, __LINE__, __VA_ARGS__)
#define CE_LOG_ERROR(cat, ...) \
    ce_log_write(CE_LOG_ERROR, cat, __FILE__, __LINE__, __VA_ARGS__)
#define CE_LOG_FATAL(cat, ...) \
    ce_log_write(CE_LOG_FATAL, cat, __FILE__, __LINE__, __VA_ARGS__)

/** 获取最近的日志条目（环形缓冲区，用于编辑器快照） */
uint32_t ce_log_get_recent(CeLogEntry* out_buffer, uint32_t max_count);

/** 清空日志缓冲区 */
void ce_log_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* CE_LOG_H */
