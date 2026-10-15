#include "pch_engine.h"
#include "RouteSupport.h"

#include "../../../Graph/GraphModel.h"
#include "../../../Effects/NodeCatalog.h"
#include "../../../Effects/SourceNodeFactory.h"

#include <set>

namespace ShaderLab::Mcp::Routes
{
    void RenderFrameFor(EngineContext& ctx, uint32_t nodeId)
    {
        if (ctx.renderFrame)
            ctx.renderFrame(nodeId);
    }

    uint32_t AddCreatedNode(EngineContext& ctx, Effects::NodeCreateResult& created)
    {
        const uint32_t id = ctx.graph->AddNode(std::move(*created.node));
        if (created.needsPrepare && ctx.sourceFactory && ctx.dc)
            if (auto* node = ctx.graph->FindNode(id))
                ctx.sourceFactory->PrepareSourceNode(*node, ctx.dc, ctx.d3dDevice, ctx.d3dContext);
        return id;
    }

    namespace
    {
        // Names of the node and every node it depends on (through edges or
        // property bindings) whose shader is still compiling in the background.
        std::vector<std::wstring> CompilingDependencies(const Graph::GraphModel& graph, uint32_t nodeId)
        {
            std::vector<std::wstring> compiling;
            std::set<uint32_t> visited;
            std::vector<uint32_t> pending{ nodeId };
            while (!pending.empty())
            {
                const uint32_t currentId = pending.back();
                pending.pop_back();
                if (!visited.insert(currentId).second)
                    continue;
                const auto* node = graph.FindNode(currentId);
                if (!node)
                    continue;
                if (node->compilePending)
                    compiling.push_back(node->name);

                for (const auto& edge : graph.Edges())
                {
                    if (edge.destNodeId == currentId)
                        pending.push_back(edge.sourceNodeId);
                }
                Graph::ForEachBindingSource(*node, [&](uint32_t sourceNodeId) { pending.push_back(sourceNodeId); });
            }
            return compiling;
        }
    }

    std::optional<Response> CompilingResponse(const Graph::GraphModel& graph, uint32_t nodeId)
    {
        const auto compiling = CompilingDependencies(graph, nodeId);
        if (compiling.empty())
            return std::nullopt;

        auto json = BeginErrorBody(409, std::format(
            "Node {} is waiting on shader compiles; retry in a few seconds", nodeId));
        json.Key("notReady").Bool(true).Key("compiling").BeginArray();
        for (const auto& name : compiling)
            json.String(name);
        json.EndArray().EndObject();
        return Json(409, json.Take());
    }

    std::optional<Response> ReadPathNodeId(const std::wstring& path, std::wstring_view prefix, uint32_t& nodeId)
    {
        if (path.size() <= prefix.size())
            return Error(400, "Missing node id in the path");
        auto parsed = ParseUintText(std::wstring_view(path).substr(prefix.size()));
        if (!parsed)
            return Error(400, "Invalid node id: " + WideToUtf8(path.substr(prefix.size())));
        nodeId = *parsed;
        return std::nullopt;
    }
}
