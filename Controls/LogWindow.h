#pragma once

#include "pch.h"
#include "NodeLog.h"

namespace ShaderLab::Controls
{
    // A lightweight window that displays scrollable log entries for a single graph node.
    // Updated in real-time via a timer. Closed when node is deleted.
    class LogWindow
    {
    public:
        LogWindow() = default;
        ~LogWindow();

        LogWindow(const LogWindow&) = delete;
        LogWindow& operator=(const LogWindow&) = delete;

        void Create(uint32_t nodeId, const std::wstring& nodeName);
        void Update(const NodeLog& log);
        void Close();

        bool IsOpen() const { return m_isOpen; }
        uint32_t NodeId() const { return m_nodeId; }

    private:
        void AppendEntry(const NodeLogEntry& entry);
        std::wstring FormatEntry(const NodeLogEntry& entry) const;

        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::RichTextBlock m_logView{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ScrollViewer m_scrollViewer{ nullptr };

        uint32_t m_nodeId{ 0 };
        uint64_t m_lastSeq{ 0 };
        bool m_isOpen{ false };
        winrt::event_token m_closedToken{};
    };
}
