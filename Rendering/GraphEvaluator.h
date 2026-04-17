#pragma once

#include "pch.h"
#include "../Graph/EffectGraph.h"

namespace ShaderLab::Rendering
{
    // Evaluates an EffectGraph by walking nodes in topological order,
    // creating / caching D2D effects, wiring inputs from upstream outputs,
    // and returning the final ID2D1Image* for presentation.
    //
    // The evaluator keeps a per-node effect cache so that effects are only
    // recreated when the node type or CLSID changes (not every frame).
    // Properties are re-applied each frame on dirty nodes.
    //
    // Usage:
    //   auto* finalImage = evaluator.Evaluate(graph, deviceContext);
    //   if (finalImage) { dc->DrawImage(finalImage); }
    class GraphEvaluator
    {
    public:
        GraphEvaluator() = default;

        // Walk the graph in topological order and produce the final output image.
        // Returns nullptr if the graph is empty or has no Output node.
        ID2D1Image* Evaluate(Graph::EffectGraph& graph, ID2D1DeviceContext5* dc);

        // Release all cached D2D effects (e.g., on device lost or graph clear).
        void ReleaseCache();

        // Invalidate the cached effect for a specific node (e.g., CLSID changed).
        void InvalidateNode(uint32_t nodeId);

    private:
        // Create or retrieve the cached D2D effect for a built-in effect node.
        ID2D1Effect* GetOrCreateEffect(
            ID2D1DeviceContext5* dc,
            const Graph::EffectNode& node);

        // Apply the node's property map to its D2D effect.
        void ApplyProperties(ID2D1Effect* effect, const Graph::EffectNode& node);

        // Wire input edges: for each input pin on destNode, find the upstream
        // node's cachedOutput and call effect->SetInput(pin, image).
        void WireInputs(
            ID2D1Effect* effect,
            const Graph::EffectNode& destNode,
            const Graph::EffectGraph& graph);

        // Per-node effect cache: nodeId → D2D effect.
        // Effects are reused across frames; only properties are updated.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Effect>> m_effectCache;

        // Force D2D to compute the histogram and read output data.
        void ReadHistogramOutput(
            ID2D1DeviceContext5* dc,
            ID2D1Effect* effect,
            Graph::EffectNode& node);

        // Temp target for forcing effect computation.
        winrt::com_ptr<ID2D1Bitmap1> m_analysisTarget;
        uint32_t m_analysisTargetW{ 0 };
        uint32_t m_analysisTargetH{ 0 };
    };
}
