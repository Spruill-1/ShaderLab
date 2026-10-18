#pragma once

#include <cstdint>

// The GUI host's render-loop policy. Engine evaluators never read these; the
// render worker, the UI present and the CPU-analysis hint builder do.
namespace ShaderLab::Rendering::HostRenderSettings
{
    // Benchmark mode. The render worker drains its queue without waiting for a
    // frame deadline and re-evaluates every tick whether or not anything is
    // dirty, and the UI presents with sync interval 0. That measures pipeline
    // throughput instead of the display's cadence, at the cost of tearing and
    // a pegged GPU. Off by default.
    bool IsUnthrottledRenderEnabled();
    void SetUnthrottledRenderEnabled(bool enabled);

    // Scope of the CPU-analysis hint set. Off (default): the selected node and
    // the nodes its bindings read. On: every compute node, so canvas value
    // labels on unselected nodes refresh at the evaluator's hint throttle.
    bool IsCpuAnalysisHintAllNodesEnabled();
    void SetCpuAnalysisHintAllNodesEnabled(bool enabled);

    // The flyout's "Refresh analysis readouts" presets, shared with the
    // perf_render_mode MCP route so the two cannot drift:
    //   0 = selected node (+ its binding sources), every 2 s   (default)
    //   1 = every compute node, every 2 s
    //   2 = every compute node, every 250 ms
    //   3 = every compute node, every frame (a Map() stall per frame)
    // Sets the hint scope above and the default evaluator hint throttle;
    // returns false for an unknown preset. AnalysisReadoutPreset reports the
    // current one, or -1 when the two settings match none.
    bool SetAnalysisReadoutPreset(int32_t preset);
    int32_t AnalysisReadoutPreset();
}
