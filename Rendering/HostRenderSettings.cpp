#include "pch.h"
#include "HostRenderSettings.h"
#include "Effects/Performance.h"

namespace ShaderLab::Rendering::HostRenderSettings
{
    namespace
    {
        // Toggled from the UI thread and MCP routes, read by the render worker.
        std::atomic<bool> s_unthrottledRender{ false };
        std::atomic<bool> s_hintAllNodes{ false };

        struct ReadoutPreset { bool allNodes; uint32_t throttleMs; };
        constexpr ReadoutPreset scReadoutPresets[] = {
            { false, 2000 },
            { true,  2000 },
            { true,   250 },
            { true,     0 },
        };
    }

    bool IsUnthrottledRenderEnabled()
    {
        return s_unthrottledRender.load(std::memory_order_acquire);
    }

    void SetUnthrottledRenderEnabled(bool enabled)
    {
        s_unthrottledRender.store(enabled, std::memory_order_release);
    }

    bool IsCpuAnalysisHintAllNodesEnabled()
    {
        return s_hintAllNodes.load(std::memory_order_acquire);
    }

    void SetCpuAnalysisHintAllNodesEnabled(bool enabled)
    {
        s_hintAllNodes.store(enabled, std::memory_order_release);
    }

    bool SetAnalysisReadoutPreset(int32_t preset)
    {
        if (preset < 0 || preset >= static_cast<int32_t>(std::size(scReadoutPresets)))
            return false;
        SetCpuAnalysisHintAllNodesEnabled(scReadoutPresets[preset].allNodes);
        ::ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(scReadoutPresets[preset].throttleMs);
        return true;
    }

    int32_t AnalysisReadoutPreset()
    {
        const bool allNodes = IsCpuAnalysisHintAllNodesEnabled();
        const uint32_t throttleMs = ::ShaderLab::Performance::CpuAnalysisHintThrottleMs();
        for (int32_t i = 0; i < static_cast<int32_t>(std::size(scReadoutPresets)); ++i)
            if (scReadoutPresets[i].allNodes == allNodes && scReadoutPresets[i].throttleMs == throttleMs)
                return i;
        return -1;
    }
}
