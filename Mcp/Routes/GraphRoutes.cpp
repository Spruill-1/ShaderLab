#include "pch_engine.h"
#include "RouteSupport.h"
#include "GraphPatch.h"
#include "../McpRouter.h"
#include "../McpSerialize.h"

#include "Graph/GraphModel.h"
#include "Graph/GraphEdits.h"
#include "Effects/EffectUpgrade.h"
#include "Effects/NodeCatalog.h"
#include "Sources/CaptureSources.h"
#include "Rendering/GraphEvaluator.h"

// Graph structure: reading the graph, adding, duplicating and removing nodes,
// edges, the /graph/apply batch, and the edits that change a node's pins
// (effect upgrade, Numeric Expression inputs). Every route that touches the
// graph runs inside sink.Dispatch, on the thread that owns it.
namespace ShaderLab::Mcp::Routes
{
    namespace
    {
        const char* GpuStateName(Graph::GpuNodeState state)
        {
            switch (state)
            {
            case Graph::GpuNodeState::Measured: return "measured";
            case Graph::GpuNodeState::Fused:    return "fused-downstream";
            case Graph::GpuNodeState::Cached:   return "cached";
            case Graph::GpuNodeState::Idle:     return "idle";
            case Graph::GpuNodeState::CpuOnly:  return "cpu-only";
            default:                            return "unmeasured";
            }
        }

        uint32_t PreviewNodeId(const EngineContext& ctx)
        {
            return ctx.getPreviewNodeId ? ctx.getPreviewNodeId() : 0;
        }

        // GET /graph/overview: nodes (with errors and GPU attribution), edges
        // as [src, srcPin, dst, dstPin], and the preview node.
        void RegisterOverview(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"GET", L"/graph/overview",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> Response
                    {
                        JsonWriter json;
                        json.BeginObject().Key("previewNodeId").Int(PreviewNodeId(ctx)).Key("nodes").BeginArray();
                        for (const auto& node : ctx.Model().Nodes())
                        {
                            json.BeginObject()
                                .Key("id").Int(node.id)
                                .Key("name").String(node.name)
                                .Key("type").String(NodeTypeName(node.type));
                            if (!node.runtimeError.empty())
                                json.Key("error").String(node.runtimeError);
                            // gpuMs alone is ambiguous (absent: free, or not
                            // attributable), so the state is always reported
                            // while timing is on.
                            if (node.gpuState != Graph::GpuNodeState::Unmeasured)
                                json.Key("gpuState").String(GpuStateName(node.gpuState));
                            if (node.lastGpuMs >= 0.0)
                                json.Key("gpuMs").Fixed(node.lastGpuMs, 4);
                            json.Key("inputs").Int(node.inputPins.size())
                                .Key("outputs").Int(node.outputPins.size())
                                .EndObject();
                        }
                        json.EndArray().Key("edges").BeginArray();
                        for (const auto& edge : ctx.Model().Edges())
                            json.BeginArray().Int(edge.sourceNodeId).Int(edge.sourcePin)
                                .Int(edge.destNodeId).Int(edge.destPin).EndArray();
                        json.EndArray().EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        // GET /graph (full state), GET /graph/save (the saved-graph JSON) and
        // GET /graph/node/{id} (one node).
        void RegisterGetGraph(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"GET", L"/graph",
                [&sink](const std::wstring& path, const std::wstring&, const std::string&) -> Response
                {
                    constexpr std::wstring_view cNodePrefix = L"/graph/node/";
                    uint32_t nodeId = 0;
                    const bool nodeRequest = path.starts_with(cNodePrefix);
                    if (nodeRequest)
                        if (auto bad = ReadPathNodeId(path, cNodePrefix, nodeId))
                            return *bad;

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        if (path == L"/graph/save")
                            return Json(200, WideToUtf8(std::wstring(ctx.Model().ToJson())));
                        if (nodeRequest)
                        {
                            const auto* node = ctx.Model().FindNode(nodeId);
                            return node ? Json(200, NodeToJson(*node)) : NodeNotFound(nodeId);
                        }

                        JsonWriter json;
                        json.BeginObject().Key("nodes").BeginArray();
                        for (const auto& node : ctx.Model().Nodes())
                            json.Raw(NodeToJson(node));
                        json.EndArray().Key("edges").BeginArray();
                        for (const auto& edge : ctx.Model().Edges())
                            json.BeginObject()
                                .Key("srcId").Int(edge.sourceNodeId).Key("srcPin").Int(edge.sourcePin)
                                .Key("dstId").Int(edge.destNodeId).Key("dstPin").Int(edge.destPin)
                                .EndObject();
                        json.EndArray().Key("previewNodeId").Int(PreviewNodeId(ctx)).EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        // POST /graph/add-node { effectName, filePath?, adapterIndex?, outputIndex? }.
        // OnNodeAdded then lets the host lay out the canvas and refresh its
        // pickers, as the toolbar's Add Node does.
        void RegisterAddNode(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/add-node",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    auto name = ReadStringArg(args, L"effectName", argError);
                    Effects::NodeCreateArgs createArgs;
                    createArgs.filePath     = ReadStringArg(args, L"filePath", argError).value_or(L"");
                    ReadUintArgs(args, createArgs, argError);
                    if (!argError.empty())
                        return Error(400, argError);
                    if (!name)
                        return Error(400, "Provide effectName");

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        auto created = Effects::CreateNodeByName(*name, createArgs, ctx.Model());
                        if (!created.node)
                            return Error(400, WideToUtf8(created.error));
                        const std::wstring nodeName = created.node->name;
                        const bool duplication = Sources::IsDesktopDuplication(*created.node);
                        const uint32_t id = AddCreatedNode(ctx, created);   // starts dirty
                        sink.OnNodeAdded(id);

                        JsonWriter json;
                        json.BeginObject().Key("nodeId").Int(id);
                        if (duplication)
                            json.Key("adapterIndex").Int(createArgs.uints[L"adapterIndex"])
                                .Key("outputIndex").Int(createArgs.uints[L"outputIndex"])
                                .Key("name").String(nodeName);
                        return Json(200, json.EndObject().Take());
                    });
                });
        }

        // POST /graph/remove-node { nodeId }.
        void RegisterRemoveNode(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/remove-node",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    if (!nodeId)
                        return Error(400, argError);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        ctx.Model().RemoveNode(*nodeId);   // dirties its consumers
                        sink.OnNodeRemoved(*nodeId);
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // POST /graph/duplicate-nodes { nodeIds:[...] }: Graph::DuplicateNodes.
        // Answers { nodeIds:[new ids], idMap:{ "old": new } }.
        void RegisterDuplicateNodes(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/duplicate-nodes",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    const auto list = RequireArg(ReadArrayArg(args, L"nodeIds", argError), args, L"nodeIds", argError);
                    if (!list)
                        return Error(400, argError);
                    std::vector<uint32_t> nodeIds;
                    for (const auto& item : *list)
                    {
                        std::optional<uint32_t> id;
                        if (item.ValueType() == WDJ::JsonValueType::Number)
                        {
                            const double number = item.GetNumber();
                            if (number >= 0.0 && number <= static_cast<double>(UINT32_MAX) && std::floor(number) == number)
                                id = static_cast<uint32_t>(number);
                        }
                        else if (item.ValueType() == WDJ::JsonValueType::String)
                        {
                            id = ParseUintText(item.GetString());
                        }
                        if (!id)
                            return Error(400, "nodeIds must hold node ids");
                        nodeIds.push_back(*id);
                    }
                    if (nodeIds.empty())
                        return Error(400, "nodeIds is empty");

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        const auto added = Graph::DuplicateNodes(ctx.Model(), nodeIds);
                        if (added.empty())
                            return Error(404, "A node in nodeIds was not found");
                        for (const auto& [oldId, newId] : added)
                            sink.OnNodeAdded(newId);
                        sink.OnGraphStructureChanged();

                        JsonWriter json;
                        json.BeginObject().Key("nodeIds").BeginArray();
                        for (const auto& [oldId, newId] : added)
                            json.Int(newId);
                        json.EndArray().Key("idMap").BeginObject();
                        for (const auto& [oldId, newId] : added)
                            json.Key(std::to_string(oldId)).Int(newId);
                        json.EndObject().EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        // POST /graph/upgrade-nodes { nodeId? }: brings one outdated ShaderLab
        // effect node, or every one when nodeId is absent, up to the
        // registry's version, as the Properties panel's Update button and the
        // toolbar's Update all do. Answers { upgraded:[ids] }; a node that is
        // already current is not an error.
        void RegisterUpgradeNodes(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/upgrade-nodes",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    const auto nodeId = ReadUintArg(args, L"nodeId", argError);
                    if (!argError.empty())
                        return Error(400, argError);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        std::vector<uint32_t> upgraded;
                        if (nodeId)
                        {
                            auto* node = ctx.Model().FindNode(*nodeId);
                            if (!node)
                                return NodeNotFound(*nodeId);
                            const auto* desc = Effects::NewerDescriptorFor(*node);
                            if (desc && Effects::UpgradeNode(ctx.Model(), *node, *desc))
                                upgraded.push_back(*nodeId);
                        }
                        else
                        {
                            upgraded = Effects::UpgradeOutdatedNodes(ctx.Model());
                        }
                        for (uint32_t id : upgraded)
                            ctx.Evaluator().InvalidateNode(id);
                        if (!upgraded.empty())
                            sink.OnGraphStructureChanged();

                        JsonWriter json;
                        json.BeginObject().Key("upgraded").BeginArray();
                        for (uint32_t id : upgraded)
                            json.Int(id);
                        json.EndArray().EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        // POST /graph/expression-input { nodeId, action: "add"|"remove", name? }:
        // adds the next free input (A to Z) to a Numeric Expression, or
        // removes `name` with its binding. Answers { name }.
        void RegisterExpressionInput(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/expression-input",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    const auto action = RequireStringArg(args, L"action", argError);
                    const auto name = ReadStringArg(args, L"name", argError);
                    if (!argError.empty())
                        return Error(400, argError);
                    const bool add = *action == L"add";
                    if (!add && *action != L"remove")
                        return Error(400, "action must be 'add' or 'remove'");
                    if (!add && !name)
                        return Error(400, "name is required to remove an input");

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        const auto* node = ctx.Model().FindNode(*nodeId);
                        if (!node)
                            return NodeNotFound(*nodeId);
                        std::wstring changed;
                        if (add)
                        {
                            changed = Graph::AddExpressionInput(ctx.Model(), *nodeId);
                            if (changed.empty())
                                return Error(400, "Not a Numeric Expression, or all 26 inputs are in use");
                        }
                        else
                        {
                            if (!Graph::RemoveExpressionInput(ctx.Model(), *nodeId, *name))
                                return Error(400, "Not a Numeric Expression input that can be removed "
                                                  "(the last input and Expression itself stay)");
                            changed = *name;
                        }
                        sink.OnGraphStructureChanged();
                        return Json(200, JsonWriter().BeginObject().Key("name").String(changed).EndObject().Take());
                    });
                });
        }

        struct EdgeArgs
        {
            uint32_t srcId{}, srcPin{}, dstId{}, dstPin{};
        };

        std::optional<EdgeArgs> ReadEdgeArgs(const std::string& body, std::string& error)
        {
            WDJ::JsonObject args{ nullptr };
            if (!ParseObjectBody(body, args))
            {
                error = "Invalid JSON";
                return std::nullopt;
            }
            auto srcId = RequireUintArg(args, L"srcId", error);
            auto srcPin = RequireUintArg(args, L"srcPin", error);
            auto dstId = RequireUintArg(args, L"dstId", error);
            auto dstPin = RequireUintArg(args, L"dstPin", error);
            if (!srcId || !srcPin || !dstId || !dstPin)
                return std::nullopt;
            return EdgeArgs{ *srcId, *srcPin, *dstId, *dstPin };
        }

        // POST /graph/connect and /graph/disconnect { srcId, srcPin, dstId, dstPin }.
        // Both dirty the destination; the host's next frame redraws the canvas.
        void RegisterEdgeRoutes(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/connect",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    std::string argError;
                    const auto edge = ReadEdgeArgs(body, argError);
                    if (!edge)
                        return Error(400, argError);
                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        const bool connected = ctx.Model().Connect(edge->srcId, edge->srcPin, edge->dstId, edge->dstPin);
                        if (connected) sink.OnGraphStructureChanged();
                        return Json(200, JsonWriter().BeginObject().Key("connected").Bool(connected).EndObject().Take());
                    });
                });

            router.AddRoute(L"POST", L"/graph/disconnect",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    std::string argError;
                    const auto edge = ReadEdgeArgs(body, argError);
                    if (!edge)
                        return Error(400, argError);
                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        const bool disconnected = ctx.Model().Disconnect(edge->srcId, edge->srcPin, edge->dstId, edge->dstPin);
                        if (disconnected) sink.OnGraphStructureChanged();
                        return Json(200, JsonWriter().BeginObject().Key("disconnected").Bool(disconnected).EndObject().Take());
                    });
                });
        }

        // POST /graph/apply: { nodes:[{ref, effect, properties, ...}],
        // edges:[{from, to, fromPin, toPin}], bindings:[{node, property, from,
        // component}] }. A ref names a node for later entries of the same
        // patch; a node id works wherever a ref does. Answers
        // { ok, nodeIds, refToId }. On any failure nothing is left applied.
        // Preview selection stays a host matter (/render/preview-node).
        void RegisterApply(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/apply",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject root{ nullptr };
                    if (!ParseObjectBody(body, root))
                        return Error(400, "Invalid JSON");
                    std::string parseError;
                    const auto patch = ParseGraphPatch(root, parseError);
                    if (!patch)
                        return Error(400, parseError);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        const auto result = ApplyGraphPatch(ctx, *patch);
                        if (!result.error.empty())
                            return Error(400, result.error);

                        // New nodes start dirty; Connect and BindProperty dirty their consumers.
                        for (uint32_t id : result.nodeIds)
                            sink.OnNodeAdded(id);
                        sink.OnGraphStructureChanged();

                        JsonWriter json;
                        json.BeginObject().Key("ok").Bool(true).Key("nodeIds").BeginArray();
                        for (uint32_t id : result.nodeIds)
                            json.Int(id);
                        json.EndArray().Key("refToId").BeginObject();
                        for (const auto& [ref, id] : result.refToId)
                            json.Key(WideToUtf8(ref)).Int(id);
                        json.EndObject().EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        // GET /custom-effects: every node with a custom-effect definition.
        void RegisterCustomEffects(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"GET", L"/custom-effects",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> Response
                    {
                        JsonWriter json;
                        json.BeginArray();
                        for (const auto& node : ctx.Model().Nodes())
                            if (node.customEffect.has_value())
                                json.Raw(NodeToJson(node));
                        json.EndArray();
                        return Json(200, json.Take());
                    });
                });
        }
    }

    void RegisterGraphRoutes(McpRouter& router, IEngineCommandSink& sink)
    {
        RegisterOverview(router, sink);
        RegisterGetGraph(router, sink);
        RegisterAddNode(router, sink);
        RegisterRemoveNode(router, sink);
        RegisterDuplicateNodes(router, sink);
        RegisterUpgradeNodes(router, sink);
        RegisterExpressionInput(router, sink);
        RegisterEdgeRoutes(router, sink);
        RegisterApply(router, sink);
        RegisterCustomEffects(router, sink);
    }
}
