#pragma once

// The GUI's performance MCP routes: /perf (timings) and the /perf/* switches
// that change how the app is measured.

#include "McpRouteTable.h"
#include "../Rendering/FrameTimings.h"

#include <atomic>
#include <functional>

namespace ShaderLab::Graph
{
    class GraphModel;
}
namespace ShaderLab::Rendering
{
    class GpuTimer;
    class PreviewRenderStats;
}

namespace ShaderLab::Controls
{
    // A performance setting shown by a control in the Performance flyout.
    enum class PerfSetting
    {
        Unthrottled,
        ForceRedraw,
        AnalysisRefresh,
        GpuBindings,
    };

    // What the perf routes read and drive. Everything here outlives the
    // router the routes are registered on.
    struct PerfRouteHost
    {
        const Rendering::FrameTimingBoard&   timings;
        const Rendering::PreviewRenderStats& previewStats;
        Rendering::GpuTimer&                 gpuTimer;
        Graph::GraphModel&                  graph;               // render routes only
        Rendering::RenderThreadDispatcher&   renderDispatcher;
        std::atomic<bool>&                   forceContinuousRedraw;
        std::atomic<bool>&                   forceRender;
        // Switches GPU-resident analysis bindings; safe from any thread.
        std::function<void(bool)>            applyGpuBindings;
        // Shows a setting's new value on its flyout control. Posts to the UI
        // thread and returns at once.
        std::function<void(PerfSetting, int32_t)> showSetting;
    };

    void RegisterPerfRoutes(McpRouteTable& routes, PerfRouteHost host);
}
