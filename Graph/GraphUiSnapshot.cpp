#include "pch_engine.h"
#include "GraphUiSnapshot.h"

namespace ShaderLab::Graph
{
    std::shared_ptr<const GraphUiSnapshot> BuildGraphUiSnapshot(
        const EffectGraph& graph,
        std::uint32_t previewNodeId,
        std::uint64_t graphGeneration,
        std::uint64_t frameGeneration)
    {
        auto snap = std::make_shared<GraphUiSnapshot>();
        snap->graphGeneration = graphGeneration;
        snap->frameGeneration = frameGeneration;
        snap->previewNodeId = previewNodeId;

        // Copy nodes; clear cachedOutput so raw render-thread pointers can't
        // escape via the snapshot.
        const auto& srcNodes = graph.Nodes();
        snap->nodes.reserve(srcNodes.size());
        snap->nodeIndexById.reserve(srcNodes.size());
        for (const auto& node : srcNodes)
        {
            snap->nodeIndexById.emplace(node.id, snap->nodes.size());
            EffectNode copy = node;
            copy.cachedOutput = nullptr;
            snap->nodes.push_back(std::move(copy));
        }

        // Edges are value types already.
        const auto& srcEdges = graph.Edges();
        snap->edges.reserve(srcEdges.size());
        for (const auto& e : srcEdges) snap->edges.push_back(e);

        try
        {
            snap->topologicalOrder = graph.TopologicalSort();
        }
        catch (const std::logic_error&)
        {
            // A cycle has no order; the empty vector says so.
        }

        return snap;
    }
}
