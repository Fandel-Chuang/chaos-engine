/*
 * ChaosEngine 日志系统 — 第八模式观测
 * 环形缓冲区 + 回调机制
 */

#define _POSIX_C_SOURCE 199309L
#include <time.h>
#include "log/ce_log_internal.h"
#include "core/ce_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- 日志上下文 ---- */

#define CE_LOG_BUFFER_SIZE 4096
#define CE_LOG_MAX_CALLBACKS 8

static struct {
    CeAllocator* allocator;
    CeLogLevel   min_level;
    CeBool       observe_mode;  /* 第八模式 */
    FILE*        file;

    /* 环形缓冲区 */
    CeLogEntry   buffer[CE_LOG_BUFFER_SIZE];
    uint32_t     write_index;
    uint32_t     entry_count;

    /* 回调 */
    CeLogCallback callbacks[CE_LOG_MAX_CALLBACKS];
    void*         callback_data[CE_LOG_MAX_CALLBACKS];
    uint32_t      callback_count;

    /* 静态存储（避免动态分配） */
    char         msg_buffer[4096];
} g_log;

/* ---- 初始化 ---- */

CeResult ce_log_init(CeAllocator* allocator, CeLogLevel min_level, const char* file_path) {
    memset(&g_log, 0, sizeof(g_log));
    g_log.allocator = allocator;
    g_log.min_level = min_level;
    g_log.observe_mode = CE_FALSE;

    if (file_path) {
        g_log.file = fopen(file_path, "a");
    }

    return CE_OK;
}

void ce_log_shutdown(void) {
    if (g_log.file) {
        fclose(g_log.file);
        g_log.file = NULL;
    }
    memset(&g_log, 0, sizeof(g_log));
}

/* ---- 第八模式 ---- */

void ce_log_set_observe_mode(CeBool enable) {
    g_log.observe_mode = enable;
    if (enable) {
        g_log.min_level = CE_LOG_TRACE;  /* 全量输出 */
    }
}

CeBool ce_log_get_observe_mode(void) {
    return g_log.observe_mode;
}

/* ---- 回调管理 ---- */

void ce_log_add_callback(CeLogCallback callback, void* user_data) {
    if (g_log.callback_count >= CE_LOG_MAX_CALLBACKS) return;
    g_log.callbacks[g_log.callback_count] = callback;
    g_log.callback_data[g_log.callback_count] = user_data;
    g_log.callback_count++;
}

void ce_log_remove_callback(CeLogCallback callback) {
    for (uint32_t i = 0; i < g_log.callback_count; i++) {
        if (g_log.callbacks[i] == callback) {
            g_log.callbacks[i] = g_log.callbacks[--g_log.callback_count];
            g_log.callback_data[i] = g_log.callback_data[g_log.callback_count];
            return;
        }
    }
}

/* ---- 日志写入 ---- */

static uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static const char* level_to_string(CeLogLevel level) {
    switch (level) {
        case CE_LOG_TRACE: return "TRACE";
        case CE_LOG_DEBUG: return "DEBUG";
        case CE_LOG_INFO:  return "INFO";
        case CE_LOG_WARN:  return "WARN";
        case CE_LOG_ERROR: return "ERROR";
        case CE_LOG_FATAL: return "FATAL";
        default:           return "????";
    }
}

void ce_log_write(CeLogLevel level, const char* category,
                   const char* file, int line, const char* fmt, ...) {
    if (level < g_log.min_level) return;

    /* 格式化消息 */
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_log.msg_buffer, sizeof(g_log.msg_buffer), fmt, args);
    va_end(args);

    uint64_t ts = get_timestamp_us();

    /* 写入环形缓冲区 */
    CeLogEntry* entry = &g_log.buffer[g_log.write_index];
    entry->level        = level;
    entry->timestamp_us = ts;
    entry->category     = category;
    entry->message      = g_log.msg_buffer;
    entry->file         = file;
    entry->line         = line;

    g_log.write_index = (g_log.write_index + 1) % CE_LOG_BUFFER_SIZE;
    if (g_log.entry_count < CE_LOG_BUFFER_SIZE) {
        g_log.entry_count++;
    }

    /* 写入文件 */
    if (g_log.file) {
        fprintf(g_log.file, "[%s] [%s] %s:%d | %s\n",
                level_to_string(level), category, file, line, g_log.msg_buffer);
        fflush(g_log.file);
    }

    /* 通知回调 */
    for (uint32_t i = 0; i < g_log.callback_count; i++) {
        g_log.callbacks[i](entry, g_log.callback_data[i]);
    }
}

/* ---- 日志查询 ---- */

uint32_t ce_log_get_recent(CeLogEntry* out_buffer, uint32_t max_count) {
    if (max_count > g_log.entry_count) max_count = g_log.entry_count;
    if (max_count == 0) return 0;

    /* 从最旧的条目开始读取 */
    uint32_t start = (g_log.write_index + CE_LOG_BUFFER_SIZE - g_log.entry_count)
                     % CE_LOG_BUFFER_SIZE;

    for (uint32_t i = 0; i < max_count; i++) {
        uint32_t idx = (start + i) % CE_LOG_BUFFER_SIZE;
        out_buffer[i] = g_log.buffer[idx];
    }

    return max_count;
}

void ce_log_clear(void) {
    g_log.write_index = 0;
    g_log.entry_count = 0;
}
