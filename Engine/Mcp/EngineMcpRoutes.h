#pragma once

// Engine MCP routes — engine-side route handlers for the MCP server.
//
// Phase 7 architecture: the McpHttpServer runs in the engine DLL.
// The GUI app and headless host both instantiate it and register a
// mix of routes:
//
//   * Engine-pure routes (graph mutation, render capture, pixel
//     readback, image stats, etc) live in this TU and are registered
//     by both apps via `RegisterEngineRoutes`.
//
//   * UI-coupled routes (graph_snapshot of the canvas, preview pan/zoom,
//     graph view tools) stay in `MainWindow.McpRoutes.cpp` because
//     they touch WinUI controls that don't exist headless.
//
// Routes execute through `IEngineCommandSink::Dispatch`, which marshals
// the closure to the right thread for the host:
//   * GUI app: dispatches to the UI thread via DispatcherQueue (so
//     concurrent UI tick and MCP requests don't race the graph).
//   * Headless host: synchronous direct call (single-threaded MCP
//     access; the listener thread serializes requests).

#include "pch_engine.h"
#include "../../EngineExport.h"
#include "McpHttpServer.h"

#include <functional>

namespace ShaderLab::Graph
{
    class EffectGraph;
}
namespace ShaderLab::Rendering
{
    class GraphEvaluator;
    class DisplayMonitor;
}

namespace ShaderLab::Mcp
{
    // Bag of references the engine routes need. Built fresh by the host
    // each time Dispatch fires so the references are valid for the
    // closure's lifetime. Members are non-owning.
    struct EngineContext
    {
        Graph::EffectGraph*           graph{ nullptr };
        Rendering::GraphEvaluator*    evaluator{ nullptr };
        Rendering::DisplayMonitor*    displayMonitor{ nullptr };
        ID2D1DeviceContext*           dc{ nullptr };
        ID3D11Device*                 d3dDevice{ nullptr };
        ID3D11DeviceContext*          d3dContext{ nullptr };

        // Force a fresh evaluation of the graph if the host has any
        // dirty propagation / tick logic. Headless: no-op (caller did
        // this already). GUI: calls MainWindow::RenderFrame so dirty
        // nodes are repopulated before readback / capture.
        // Returning void; cannot fail.
        std::function<void()>         renderFrame;
    };

    // Functional / closure-based command sink (Q4 architecture choice).
    // The route handler hands a closure to Dispatch; the host runs it
    // on the right thread and returns the McpHttpServer::Response back
    // to the listener thread that the route returns on.
    class SHADERLAB_API IEngineCommandSink
    {
    public:
        virtual ~IEngineCommandSink() = default;

        // Run `closure` on whichever thread is appropriate for this
        // host. Closure receives a freshly-built EngineContext.
        // Synchronous: the calling thread blocks until the closure
        // completes. Closure exceptions propagate.
        virtual McpHttpServer::Response Dispatch(
            std::function<McpHttpServer::Response(EngineContext&)> closure) = 0;
    };

    // Register all engine-pure routes on the given server. Idempotent:
    // call once per process. The sink must outlive the server (handlers
    // capture it by reference).
    SHADERLAB_API void RegisterEngineRoutes(
        McpHttpServer& server,
        IEngineCommandSink& sink);
}
