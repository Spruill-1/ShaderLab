#pragma once

#include "pch.h"

namespace ShaderLab::Controls
{
    enum class LogLevel : uint8_t { Info, Warning, Error };

    struct NodeLogEntry {
        uint64_t sequence{};
        std::chrono::system_clock::time_point timestamp;
        LogLevel level{ LogLevel::Info };
        std::wstring message;
    };

    // Per-node ring buffer of log entries. Lives in a central map, not on EffectNode.
    class NodeLog
    {
    public:
        static constexpr size_t MaxEntries = 200;

        void Add(LogLevel level, std::wstring message)
        {
            NodeLogEntry entry;
            entry.sequence = m_nextSeq++;
            entry.timestamp = std::chrono::system_clock::now();
            entry.level = level;
            entry.message = std::move(message);
            m_entries.push_back(std::move(entry));
            while (m_entries.size() > MaxEntries)
                m_entries.pop_front();
        }

        void Info(std::wstring msg)    { Add(LogLevel::Info, std::move(msg)); }
        void Warning(std::wstring msg) { Add(LogLevel::Warning, std::move(msg)); }
        void Error(std::wstring msg)   { Add(LogLevel::Error, std::move(msg)); }

        void Clear() { m_entries.clear(); }

        const std::deque<NodeLogEntry>& Entries() const { return m_entries; }
        uint64_t NextSequence() const { return m_nextSeq; }

    private:
        std::deque<NodeLogEntry> m_entries;
        uint64_t m_nextSeq{ 1 };
    };
}
