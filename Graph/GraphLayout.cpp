#include "pch_engine.h"
#include "GraphLayout.h"

namespace ShaderLab::Graph
{
    std::optional<std::unordered_map<uint32_t, winrt::Windows::Foundation::Numerics::float2>>
        ComputeAutoLayout(const GraphModel& graph,
                          const std::function<float(const EffectNode&)>& nodeHeight,
                          const AutoLayoutSpacing& spacing)
    {
        using Positions = std::unordered_map<uint32_t, winrt::Windows::Foundation::Numerics::float2>;
        if (graph.IsEmpty()) return Positions{};

        std::vector<uint32_t> order;
        try { order = graph.TopologicalSort(); }
        catch (const std::logic_error&) { return std::nullopt; }   // a cycle has no left-to-right order

        // Upstream nodes of every node, through image edges and bindings.
        std::unordered_map<uint32_t, std::vector<uint32_t>> parents;
        graph.ForEachDependency(DependencyKind::Any, [&](uint32_t sourceId, uint32_t dependentId)
        {
            auto& list = parents[dependentId];
            if (std::find(list.begin(), list.end(), sourceId) == list.end())
                list.push_back(sourceId);
        });

        // Depth: the longest path from a node without inputs.
        std::unordered_map<uint32_t, int> depth;
        int maxDepth = 0;
        for (uint32_t id : order)
        {
            int parentDepth = -1;
            for (uint32_t parentId : parents[id])
                if (auto it = depth.find(parentId); it != depth.end())
                    parentDepth = (std::max)(parentDepth, it->second);
            depth[id] = parentDepth + 1;
            maxDepth = (std::max)(maxDepth, parentDepth + 1);
        }

        std::vector<std::vector<uint32_t>> columns(static_cast<size_t>(maxDepth) + 1);
        for (uint32_t id : order)
            columns[depth[id]].push_back(id);

        // Sort key and height, computed once per node.
        std::unordered_map<uint32_t, float> meanParentY;
        std::unordered_map<uint32_t, float> height;
        for (const auto& node : graph.Nodes())
        {
            float sum = 0.0f;
            int count = 0;
            for (uint32_t parentId : parents[node.id])
                if (const auto* parent = graph.FindNode(parentId))
                {
                    sum += parent->position.y;
                    ++count;
                }
            meanParentY[node.id] = count > 0 ? sum / count : 0.0f;
            height[node.id] = nodeHeight(node);
        }

        // Ordering by upstream position reduces edge crossings.
        for (size_t col = 1; col < columns.size(); ++col)
            std::sort(columns[col].begin(), columns[col].end(),
                [&](uint32_t a, uint32_t b) { return meanParentY[a] < meanParentY[b]; });

        std::vector<float> columnHeight(columns.size(), 0.0f);
        float tallest = 0.0f;
        for (size_t col = 0; col < columns.size(); ++col)
        {
            for (uint32_t id : columns[col])
                columnHeight[col] += height[id] + spacing.rowGap;
            tallest = (std::max)(tallest, columnHeight[col]);
        }

        Positions positions;
        for (size_t col = 0; col < columns.size(); ++col)
        {
            const float x = spacing.startX + static_cast<float>(col) * spacing.columnSpacing;
            float y = spacing.startY + (tallest - columnHeight[col]) * 0.5f;
            for (uint32_t id : columns[col])
            {
                positions[id] = { x, y };
                y += height[id] + spacing.rowGap;
            }
        }
        return positions;
    }
}
