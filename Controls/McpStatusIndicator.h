#pragma once

#include "pch.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>

namespace ShaderLab::Controls
{
    // The MCP activity dot on the toolbar's MCP toggle and the toggle's
    // tooltip. The session thread records each request; the UI tick paints.
    class McpStatusIndicator
    {
    public:
        // Session thread, once per request. Never waits on the UI thread.
        void RecordRequest(const std::string& method, const std::wstring& path,
            uint16_t status, const std::string& peer);

        // Forgets every request (the session was turned on or off).
        void Reset();

        // UI thread. Hides the dot unless this window is an MCP session;
        // otherwise colours it by the age of the last request (amber for
        // 250 ms, fading to slate over the next 1.25 s) and describes the
        // last request in the toggle's tooltip.
        void Update(bool sessionActive,
            winrt::Microsoft::UI::Xaml::UIElement const& dot,
            winrt::Microsoft::UI::Xaml::Media::SolidColorBrush const& dotBrush,
            winrt::Microsoft::UI::Xaml::DependencyObject const& tooltipOwner);

    private:
        std::wstring TooltipText(int64_t ageMs, uint64_t totalCount);

        std::atomic<int64_t>  m_lastActivityMs{ 0 };
        std::atomic<uint64_t> m_requestCount{ 0 };
        std::atomic<uint64_t> m_updateSeq{ 0 };     // bumped per request; the UI compares
        uint64_t              m_shownSeq{ 0 };      // UI thread only

        std::mutex            m_lastRequestMutex;   // guards the fields below
        std::string           m_lastMethod;
        std::wstring          m_lastPath;
        std::string           m_lastPeer;
        uint16_t              m_lastStatus{ 0 };
        std::set<std::string> m_knownPeers;         // distinct peers since the last Reset
    };
}
