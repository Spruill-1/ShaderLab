#include "pch_engine.h"
#include "RouteSupport.h"
#include "../McpRouter.h"
#include "../McpSerialize.h"

#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/SourceRuntime.h"
#include "Effects/SourceNodeFactory.h"

#include <filesystem>

// Replacing the whole graph: clear, and load from JSON. Files on disk
// (.effectgraph packages and graph JSON) are ShaderLab's: its programs add
// /graph/load-file and /graph/save-file (Support/PackageRoutes.cpp).
namespace ShaderLab::Mcp::Routes
{
    std::optional<Graph::GraphModel> ParseGraph(const std::wstring& json, std::string& error)
    {
        try
        {
            return Graph::GraphModel::FromJson(winrt::hstring(json));
        }
        catch (const winrt::hresult_error& ex) { error = WideToUtf8(std::wstring(ex.message())); }
        catch (const std::exception& ex)       { error = ex.what(); }
        if (error.empty()) error = "invalid graph JSON";
        return std::nullopt;
    }

    // Replaces the graph the way File > Open does and prepares its
    // sources. Runs inside Dispatch.
    void AdoptLoadedGraph(EngineContext& ctx, IEngineCommandSink& sink,
        Graph::GraphModel&& loaded, const std::wstring& mediaDir)
    {
        ctx.context.ReplaceGraph(std::move(loaded));
        sink.OnGraphMediaDirChanged(mediaDir);
        sink.OnGraphLoaded();
    }

    namespace
    {
        // POST /graph/clear: drops the graph and the evaluator cache.
        // OnGraphCleared lets the host close output windows and reset its
        // preview, as the toolbar's clear does.
        void RegisterClear(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/clear",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([&sink](EngineContext& ctx) -> Response
                    {
                        ctx.context.ReplaceGraph(Graph::GraphModel{});
                        sink.OnGraphMediaDirChanged({});
                        sink.OnGraphCleared();
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // POST /graph/load: the body is the graph JSON. Parsed on the calling
        // thread; OnGraphLoaded runs the host's per-load setup.
        void RegisterLoad(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/load",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    std::string parseError;
                    auto loaded = ParseGraph(std::wstring(winrt::to_hstring(body)), parseError);
                    if (!loaded)
                        return Error(400, parseError);
                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        AdoptLoadedGraph(ctx, sink, std::move(*loaded), {});
                        return Json(200, R"({"ok":true})");
                    });
                });
        }
    }

    void RegisterFileRoutes(McpRouter& router, IEngineCommandSink& sink)
    {
        RegisterClear(router, sink);
        RegisterLoad(router, sink);
    }
}
