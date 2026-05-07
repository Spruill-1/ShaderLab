#include "pch_engine.h"
#include "Performance.h"

namespace ShaderLab::Performance
{
    namespace {
        // Phase 8 v1.6: GPU-binding fast path defaults ON.
        std::atomic<bool>     g_enabled{ true };
        std::atomic<uint64_t> g_detections{ 0 };
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
}
