#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/GraphModel.h"

namespace ShaderLab::Rendering
{
    class GraphEvaluator;
    class GpuTimer;

    // Which nodes a frame evaluates (EffectNode::needed).
    enum class FrameDemand
    {
        // Every node: for hosts that may read any node afterwards (a headless
        // MCP session, tests).
        All,
        // Output nodes, dirty Typed-analysis nodes, FrameRequest::roots, and
        // everything they depend on (GraphModel::ComputeNeeded).
        Roots,
    };

    // Whether a second Evaluate pass runs before the draw session.
    enum class SecondPass
    {
        // When the first pass left needed nodes dirty: new custom effects need
        // two passes before their output is valid.
        IfDirty,
        // Never: for a caller that already settled every effect and re-dirties
        // the whole graph per frame, where a second pass only re-queues work.
        Never,
    };

    struct FrameRequest
    {
        // Bound for the draw session and for `draw`; may be null.
        ID2D1Image* target{ nullptr };
        bool markAllDirty{ false };
        FrameDemand demand{ FrameDemand::All };
        std::vector<uint32_t> roots;
        SecondPass secondPass{ SecondPass::IfDirty };
        // Nodes whose analysis values the host wants on the CPU this frame
        // (GraphEvaluator::SetCpuAnalysisInterest); unset leaves it as is.
        std::optional<std::unordered_set<uint32_t>> analysisInterest;
        // Brackets the Evaluate, DeferredCompute and Draw spans when set. The
        // host owns BeginFrame/EndFrame and the Frame span.
        GpuTimer* timer{ nullptr };
        // Runs inside the draw session after compute, with `target` bound.
        std::function<void(ID2D1DeviceContext5*)> draw;
    };

    struct FrameResult
    {
        HRESULT endDraw{ S_OK };
        // Evaluate passes, the frozen post-dispatch sweep included.
        uint32_t passes{ 0 };
        // ProcessDeferredCompute ran a non-empty queue.
        bool computeRan{ false };
        // Compute dispatches issued this frame.
        uint32_t dispatches{ 0 };
        // Phase boundaries: drawEnd is before EndDraw, end after it.
        std::chrono::steady_clock::time_point start, evaluateEnd, computeEnd, drawEnd, end;
    };

    // Runs one frame of a graph through the evaluator in the only order that
    // works:
    //
    //   1. Outside any draw session: dirty propagation, demand, then Evaluate
    //      (twice when new effects need it). The D2D Histogram and custom
    //      analysis readbacks open their OWN BeginDraw, and nesting that in a
    //      host's session fails with D2DERR_WRONG_STATE (0x88990001).
    //   2. Inside one BeginDraw/EndDraw: ProcessDeferredCompute, which calls
    //      dc->DrawImage to pre-render compute inputs and silently does nothing
    //      outside a session (compute then reads black, with no error).
    //   3. Still inside: a frozen Evaluate when anything needed is still dirty,
    //      to re-apply the D2D nodes downstream of this frame's dispatches.
    //      Frozen, so it queues no compute and opens no readback session; a
    //      Histogram it touches reads on the next frame.
    //   4. Still inside: the host's draw callback, then EndDraw.
    //
    // The context's target and transform are restored on return.
    class SHADERLAB_API FrameExecutor
    {
    public:
        static FrameResult Run(Graph::GraphModel& graph, GraphEvaluator& evaluator,
                               ID2D1DeviceContext5* dc, const FrameRequest& request);
    };
}
