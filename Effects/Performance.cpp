#include "pch_engine.h"
#include "Performance.h"

namespace ShaderLab::Performance
{
    namespace {
        // Phase 8 v1.6: GPU-binding fast path defaults ON.
        std::atomic<bool>     g_enabled{ true };
        std::atomic<uint64_t> g_detections{ 0 };
        // Phase 8c: skip-readback opt-in defaults OFF.
        std::atomic<bool>     g_skipReadback{ false };
        std::atomic<uint64_t> g_skipped{ 0 };
    }

    bool IsGpuBindingsEnabled()
    {
        return g_enabled.load(std::memory_order_relaxed);
    }

    void SetGpuBindingsEnabled(bool enabled)
    {
        g_enabled.store(enabled, std::memory_order_relaxed);
    }

    uint64_t GpuBindingDetections()
    {
        return g_detections.load(std::memory_order_relaxed);
    }

    void IncrementGpuBindingDetection()
    {
        g_detections.fetch_add(1, std::memory_order_relaxed);
    }

    bool IsSkipUnneededCpuReadbackEnabled()
    {
        return g_skipReadback.load(std::memory_order_relaxed);
    }

    void SetSkipUnneededCpuReadbackEnabled(bool enabled)
    {
        g_skipReadback.store(enabled, std::memory_order_relaxed);
    }

    namespace { std::atomic<bool> g_asyncReadback{ false }; }

    bool IsAsyncAnalysisReadbackEnabled()
    {
        return g_asyncReadback.load(std::memory_order_relaxed);
    }

    void SetAsyncAnalysisReadbackEnabled(bool enabled)
    {
        g_asyncReadback.store(enabled, std::memory_order_relaxed);
    }

    uint64_t SkippedCpuReadbacks()
    {
        return g_skipped.load(std::memory_order_relaxed);
    }

    void IncrementSkippedCpuReadbacks()
    {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
    }

    namespace { std::atomic<uint32_t> g_hintThrottleMs{ 2000 }; }

    uint32_t CpuAnalysisHintThrottleMs()
    {
        return g_hintThrottleMs.load(std::memory_order_relaxed);
    }

    void SetCpuAnalysisHintThrottleMs(uint32_t ms)
    {
        g_hintThrottleMs.store(ms, std::memory_order_relaxed);
    }

    namespace { std::atomic<bool> g_hintAllNodes{ false }; }

    bool IsCpuAnalysisHintAllNodesEnabled()
    {
        return g_hintAllNodes.load(std::memory_order_relaxed);
    }

    void SetCpuAnalysisHintAllNodesEnabled(bool enabled)
    {
        g_hintAllNodes.store(enabled, std::memory_order_relaxed);
    }

    namespace
    {
        struct ReadoutPreset { bool allNodes; uint32_t throttleMs; };
        constexpr ReadoutPreset kReadoutPresets[] = {
            { false, 2000 },
            { true,  2000 },
            { true,   250 },
            { true,     0 },
        };
    }

    bool SetAnalysisReadoutPreset(int32_t preset)
    {
        if (preset < 0 || preset >= static_cast<int32_t>(std::size(kReadoutPresets)))
            return false;
        SetCpuAnalysisHintAllNodesEnabled(kReadoutPresets[preset].allNodes);
        SetCpuAnalysisHintThrottleMs(kReadoutPresets[preset].throttleMs);
        return true;
    }

    int32_t AnalysisReadoutPreset()
    {
        const bool all = IsCpuAnalysisHintAllNodesEnabled();
        const uint32_t ms = CpuAnalysisHintThrottleMs();
        for (int32_t i = 0; i < static_cast<int32_t>(std::size(kReadoutPresets)); ++i)
            if (kReadoutPresets[i].allNodes == all && kReadoutPresets[i].throttleMs == ms)
                return i;
        return -1;
    }

    namespace { std::atomic<bool> g_outputCaching{ true }; }

    bool IsEffectOutputCachingEnabled()
    {
        return g_outputCaching.load(std::memory_order_relaxed);
    }

    void SetEffectOutputCachingEnabled(bool enabled)
    {
        g_outputCaching.store(enabled, std::memory_order_relaxed);
    }

    // Acquire/release rather than relaxed: the render worker reads this every
    // tick from a different thread than the toggle writes it, and on ARM64 a
    // relaxed (or plain) store is not guaranteed to become visible there --
    // the same weak-memory failure that made the GPU timer's enable flag and
    // the force-redraw flag look broken.
    namespace { std::atomic<bool> g_unthrottledRender{ false }; }

    bool IsUnthrottledRenderEnabled()
    {
        return g_unthrottledRender.load(std::memory_order_acquire);
    }

    void SetUnthrottledRenderEnabled(bool enabled)
    {
        g_unthrottledRender.store(enabled, std::memory_order_release);
    }

    namespace { std::atomic<bool> g_subRectInputDemand{ true }; }

    bool IsSubRectInputDemandEnabled()
    {
        return g_subRectInputDemand.load(std::memory_order_acquire);
    }

    void SetSubRectInputDemandEnabled(bool enabled)
    {
        g_subRectInputDemand.store(enabled, std::memory_order_release);
    }

    namespace { std::atomic<bool> g_computeCommandList{ true }; }

    bool IsComputeCommandListEnabled()
    {
        return g_computeCommandList.load(std::memory_order_acquire);
    }

    void SetComputeCommandListEnabled(bool enabled)
    {
        g_computeCommandList.store(enabled, std::memory_order_release);
    }
}
