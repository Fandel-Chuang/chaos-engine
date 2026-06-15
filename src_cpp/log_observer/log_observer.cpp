/*
 * ChaosEngine 日志观测面板实现（第八模式）
 * 通过回调实时收集引擎日志，支持过滤和搜索
 */

#include "log_observer.h"
#include "ce_log.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ChaosEditor {

// ============================================================
// 生命周期
// ============================================================

LogObserver::~LogObserver() {
    // 注销回调
    ce_log_remove_callback(LogCallback);
}

void LogObserver::Init() {
    entries_.clear();
    entries_.reserve(max_entries_);

    // 注册日志回调，将 this 作为 user_data 传递
    ce_log_add_callback(LogCallback, this);
}

// ============================================================
// 日志回调
// ============================================================

void LogObserver::LogCallback(const CeLogEntry* entry, void* user_data) {
    if (!entry || !user_data) return;

    auto* self = static_cast<LogObserver*>(user_data);
    self->AddEntry(entry);
}

void LogObserver::AddEntry(const CeLogEntry* entry) {
    LogEntry e;
    e.timestamp_us = entry->timestamp_us;

    // 转换日志级别
    switch (entry->level) {
        case CE_LOG_TRACE: e.level = "TRACE"; break;
        case CE_LOG_DEBUG: e.level = "DEBUG"; break;
        case CE_LOG_INFO:  e.level = "INFO";  break;
        case CE_LOG_WARN:  e.level = "WARN";  break;
        case CE_LOG_ERROR: e.level = "ERROR"; break;
        case CE_LOG_FATAL: e.level = "FATAL"; break;
        default:           e.level = "????";  break;
    }

    e.category = entry->category ? entry->category : "";
    e.message  = entry->message  ? entry->message  : "";

    // 环形缓冲区：超出上限时移除最旧的
    if (entries_.size() >= max_entries_) {
        entries_.erase(entries_.begin());
    }
    entries_.push_back(std::move(e));
}

// ============================================================
// 更新与渲染
// ============================================================

void LogObserver::Update() {
    // 回调已经实时推送，这里可以额外拉取快照作为补充
    // 使用 ce_log_get_recent 获取环形缓冲区中的日志
    static const uint32_t SNAPSHOT_SIZE = 64;
    CeLogEntry snapshot[SNAPSHOT_SIZE];
    uint32_t count = ce_log_get_recent(snapshot, SNAPSHOT_SIZE);

    for (uint32_t i = 0; i < count; i++) {
        AddEntry(&snapshot[i]);
    }
}

void LogObserver::Render() {
    printf("+----------------------------------------+ LOG OBSERVER (Mode 8)\n");

    if (entries_.empty()) {
        printf("|  (no log entries)\n");
    } else {
        // 只显示最近 30 条
        size_t start = (entries_.size() > 30) ? (entries_.size() - 30) : 0;
        for (size_t i = start; i < entries_.size(); i++) {
            const auto& e = entries_[i];
            printf("| [%s][%s] %s\n",
                   e.level.c_str(), e.category.c_str(), e.message.c_str());
        }
    }

    printf("+----------------------------------------+ %zu entries total\n",
           entries_.size());
    printf("\n");
}

} // namespace ChaosEditor
