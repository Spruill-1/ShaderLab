#pragma once

// Helpers shared by the engine's MCP route files, and the Register*
// function each file exposes. Internal to the engine DLL.
//
// Route shape: read the arguments (a bad one answers 400 and touches
// nothing), call the engine, serialise. Nothing after argument parsing is
// wrapped in a catch, so a server fault reaches McpRouter's handler and
// answers 500 with its message instead of passing as a bad request.

#include "../EngineMcpRoutes.h"
#include "Graph/GraphModel.h"
#include "../McpArgs.h"
#include "../McpJsonWriter.h"
#include "Effects/NodeCatalog.h"

#include <optional>
#include <string>

namespace ShaderLab
{
    class McpRouter;
}
namespace ShaderLab::Mcp::Routes
{
    namespace WDJ = winrt::Windows::Data::Json;

    inline Response Json(uint16_t status, std::string body) { return JsonResponse(status, std::move(body)); }
    inline Response Error(uint16_t status, std::string_view message) { return ErrorResponse(status, message); }
    inline Response NodeNotFound(uint32_t nodeId) { return Error(404, std::format("Node {} not found", nodeId)); }

    // Renders a frame in which nodeId is evaluated; a no-op for a host
    // without a renderer.
    void RenderFrameFor(EngineContext& ctx, uint32_t nodeId);

    // The named arguments ShaderLab's node makers read into `createArgs`:
    // adapterIndex and outputIndex for Desktop Duplication.
    inline void ReadUintArgs(const WDJ::JsonObject& args, Effects::NodeCreateArgs& createArgs, std::string& error)
    {
        for (const wchar_t* key : { L"adapterIndex", L"outputIndex" })
            if (auto value = ReadUintArg(args, key, error))
                createArgs.uints[key] = *value;
    }

    // Adds a node CreateNodeByName made and, when it is a Source that needs
    // it, prepares it on the host's device. Returns the new id.
    uint32_t AddCreatedNode(EngineContext& ctx, Effects::NodeCreateResult& created);

    // 409 for a readback that failed while the node or anything it depends
    // on is still compiling, naming what is; nullopt when nothing is.
    std::optional<Response> CompilingResponse(const Graph::GraphModel& graph, uint32_t nodeId);

    // A graph from its JSON, or nullopt with `error` saying why.
    std::optional<Graph::GraphModel> ParseGraph(const std::wstring& json, std::string& error);

    // Replaces the graph the way File > Open does and prepares its sources:
    // ReplaceGraph, then the host's media and graph-loaded events. Runs
    // inside Dispatch. `mediaDir` is the folder a package's media was
    // extracted to, or empty.
    void AdoptLoadedGraph(EngineContext& ctx, IEngineCommandSink& sink,
        Graph::GraphModel&& loaded, const std::wstring& mediaDir);

    // The id after `prefix` in a path such as /graph/node/{id}, or the 400 to send.
    std::optional<Response> ReadPathNodeId(const std::wstring& path, std::wstring_view prefix, uint32_t& nodeId);

    void RegisterCatalogRoutes(McpRouter& router);
    void RegisterGraphRoutes(McpRouter& router, IEngineCommandSink& sink);
    void RegisterPropertyRoutes(McpRouter& router, IEngineCommandSink& sink);
    void RegisterEffectRoutes(McpRouter& router, IEngineCommandSink& sink);
    void RegisterReadbackRoutes(McpRouter& router, IEngineCommandSink& sink);
    void RegisterFileRoutes(McpRouter& router, IEngineCommandSink& sink);
    void RegisterDisplayRoutes(McpRouter& router, IEngineCommandSink& sink);
}
