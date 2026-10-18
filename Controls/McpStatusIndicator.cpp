#include "pch.h"
#include "McpStatusIndicator.h"

namespace ShaderLab::Controls
{
    namespace
    {
        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // Slate with no requests or after 1.5 s, amber for 250 ms after one,
        // and a linear fade between. Both contrast with the toggle's checked
        // (accent) and unchecked backgrounds.
        winrt::Windows::UI::Color DotColor(int64_t ageMs)
        {
            constexpr float cAmber[] = { 0xFF, 0xC1, 0x07 };
            constexpr float cSlate[] = { 0x30, 0x30, 0x30 };
            float t = 1.0f;   // 0 = amber, 1 = slate
            if (ageMs >= 0 && ageMs < 250) t = 0.0f;
            else if (ageMs >= 250 && ageMs < 1500) t = static_cast<float>(ageMs - 250) / 1250.0f;
            auto mix = [t](float amber, float slate) { return static_cast<uint8_t>(amber * (1.0f - t) + slate * t); };
            return { 0xFF, mix(cAmber[0], cSlate[0]), mix(cAmber[1], cSlate[1]), mix(cAmber[2], cSlate[2]) };
        }
    }

    void McpStatusIndicator::RecordRequest(const std::string& method, const std::wstring& path,
        uint16_t status, const std::string& peer)
    {
        m_lastActivityMs.store(NowMs(), std::memory_order_relaxed);
        m_requestCount.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(m_lastRequestMutex);
            m_lastMethod = method;
            m_lastPath = path;
            m_lastPeer = peer;
            m_lastStatus = status;
            if (!peer.empty())
                m_knownPeers.insert(peer);
        }
        m_updateSeq.fetch_add(1, std::memory_order_release);
    }

    void McpStatusIndicator::Reset()
    {
        m_lastActivityMs.store(0, std::memory_order_relaxed);
        m_requestCount.store(0, std::memory_order_relaxed);
        m_updateSeq.store(0, std::memory_order_relaxed);
        m_shownSeq = 0;
        std::lock_guard lock(m_lastRequestMutex);
        m_lastMethod.clear();
        m_lastPath.clear();
        m_lastPeer.clear();
        m_lastStatus = 0;
        m_knownPeers.clear();
    }

    void McpStatusIndicator::Update(bool sessionActive,
        winrt::Microsoft::UI::Xaml::UIElement const& dot,
        winrt::Microsoft::UI::Xaml::Media::SolidColorBrush const& dotBrush,
        winrt::Microsoft::UI::Xaml::DependencyObject const& tooltipOwner)
    {
        using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;
        if (!sessionActive)
        {
            dot.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            ToolTipService::SetToolTip(tooltipOwner,
                winrt::box_value(winrt::hstring(L"Expose this window to MCP (AI assistant integration)")));
            return;
        }
        dot.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);

        const int64_t lastMs = m_lastActivityMs.load(std::memory_order_relaxed);
        const int64_t ageMs = (lastMs == 0) ? -1 : (NowMs() - lastMs);
        const uint64_t totalCount = m_requestCount.load(std::memory_order_relaxed);
        dotBrush.Color(DotColor(ageMs));

        // The tooltip changes with a new request, and during the fade so its
        // "Xs ago" stays current.
        const uint64_t seq = m_updateSeq.load(std::memory_order_acquire);
        const bool inFade = ageMs >= 0 && ageMs < 1500;
        if (seq == m_shownSeq && !inFade)
            return;
        m_shownSeq = seq;
        ToolTipService::SetToolTip(tooltipOwner, winrt::box_value(winrt::hstring(TooltipText(ageMs, totalCount))));
    }

    std::wstring McpStatusIndicator::TooltipText(int64_t ageMs, uint64_t totalCount)
    {
        if (totalCount == 0)
            return L"MCP session — registered (no requests yet)";

        std::string method, peer;
        std::wstring path;
        uint16_t status = 0;
        size_t peerCount = 0;
        {
            std::lock_guard lock(m_lastRequestMutex);
            method = m_lastMethod;
            path = m_lastPath;
            peer = m_lastPeer;
            status = m_lastStatus;
            peerCount = m_knownPeers.size();
        }
        // Method and peer address are ASCII.
        const std::wstring methodW(method.begin(), method.end());
        const std::wstring peerW(peer.begin(), peer.end());
        std::wstring age;
        if (ageMs < 1000)       age = L"just now";
        else if (ageMs < 60000) age = std::format(L"{}s ago", ageMs / 1000);
        else                    age = std::format(L"{}m ago", ageMs / 60000);
        const std::wstring clients = peerCount > 1
            ? std::format(L"  (×{} distinct clients)", peerCount) : std::wstring();

        return std::format(
            L"MCP session — {} request{}\n"
            L"Last: {} {} → {} ({})\n"
            L"From: {}{}",
            totalCount, (totalCount == 1 ? L"" : L"s"),
            methodW, path, status, age,
            peerW.empty() ? std::wstring(L"(unknown)") : peerW, clients);
    }
}
