#pragma once

#include "pch_engine.h"
#include "Graph/GraphModel.h"

// Automatic placement of a graph's nodes on the editor canvas. Pure: it reads
// the graph and returns positions, so any host (or a test) can run it.
namespace ShaderLab::Graph
{
    struct AutoLayoutSpacing
    {
        float columnSpacing{ 250.0f };   // left edge to left edge
        float rowGap{ 30.0f };           // between nodes in a column
        float startX{ 60.0f };
        float startY{ 60.0f };
    };

    // Columns by dependency depth (image edges and bindings), left to right.
    // Within a column, nodes are ordered by the mean current y of their
    // upstream nodes and stacked with their heights; every column is then
    // centred on the tallest. Returns nothing when the graph has a cycle.
    std::optional<std::unordered_map<uint32_t, winrt::Windows::Foundation::Numerics::float2>>
        ComputeAutoLayout(const GraphModel& graph,
                          const std::function<float(const EffectNode&)>& nodeHeight,
                          const AutoLayoutSpacing& spacing = {});
}
