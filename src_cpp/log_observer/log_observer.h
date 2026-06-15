/*
 * ChaosEngine 日志观测面板（第八模式）
 * 独立于 Console 面板，提供更丰富的日志过滤/搜索功能
 */

#pragma once

#include <cstdint>
#include <vector>
#include <string>

struct CeLogEntry;

namespace ChaosEditor {

    /** 日志条目（编辑器内部表示） */
    struct LogEntry {
        std::string level;
        std::string category;
        std::string message;
        uint64_t    timestamp_us;
    };

    /**
     * LogObserver — 第八模式日志面板
     *
     * 与 Console 面板不同，LogObserver 维护自己的日志缓冲区，
     * 通过回调实时收集引擎日志，支持过滤和搜索。
     */
    class LogObserver {
    public:
        LogObserver() = default;
        ~LogObserver();

        /** 初始化：注册日志回调 */
        void Init();

        /** 每帧更新：从引擎拉取最新日志快照 */
        void Update();

        /** 渲染日志面板 */
        void Render();

        /** 获取所有缓存的日志条目 */
        const std::vector<LogEntry>& GetEntries() const { return entries_; }

        /** 清空日志缓冲区 */
        void Clear() { entries_.clear(); }

        /** 设置最大条目数 */
        void SetMaxEntries(size_t max) { max_entries_ = max; }

    private:
        /** 引擎日志回调（静态函数，通过 user_data 传递 this） */
        static void LogCallback(const CeLogEntry* entry, void* user_data);

        /** 添加一条日志到内部缓冲区 */
        void AddEntry(const CeLogEntry* entry);

        std::vector<LogEntry> entries_;
        size_t max_entries_ = 500;
    };

} // namespace ChaosEditor
