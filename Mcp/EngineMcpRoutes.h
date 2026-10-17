#pragma once

// The engine's MCP routes, registered by both hosts through
// RegisterEngineRoutes. They live under Mcp/Routes/, one file per
// concern. Routes that need host UI (the canvas snapshot, preview and graph
// view, GPU switching) are the GUI's own, in MainWindow.McpRoutes.cpp.
//
// A route reads its arguments, then runs its engine work through
// IEngineCommandSink::Dispatch, which the host runs on the thread that owns
// the graph: the GUI's render worker, or headless's session thread.

#include "pch_engine.h"
#include "Engine/Api/EffectGraphInternal.h"
#include "Rendering/DeviceStack.h"
#include "Rendering/DisplayProfile.h"
#include "McpTypes.h"

#include <functional>
#include <optional>

namespace ShaderLab
{
    class McpRouter;
}
namespace ShaderLab::Graph
{
    class GraphModel;
}
namespace ShaderLab::Rendering
{
    class GraphEvaluator;
    class DisplayMonitor;
}
namespace ShaderLab::Effects
{
    class SourceNodeFactory;
}

namespace ShaderLab::Mcp
{
    // What a route's closure works on: the host's EffectGraph context, plus
    // the host hooks below. The host builds it for each Dispatch; the members
    // are non-owning and valid for the closure's lifetime.
    struct EngineContext
    {
        explicit EngineContext(IEffectGraphContextInternal& graphContext) : context(graphContext) {}

        IEffectGraphContextInternal&  context;
        Rendering::DisplayMonitor*    displayMonitor{ nullptr };

        Graph::GraphModel&            Model() const { return context.Model(); }
        Rendering::GraphEvaluator&    Evaluator() const { return context.Evaluator(); }
        Effects::SourceNodeFactory&   Sources() const { return context.Sources(); }
        ID2D1DeviceContext5*          Dc() const { return context.DeviceContext(); }
        ID3D11Device*                 D3DDevice() const { return context.Devices().d3dDevice.get(); }
        ID3D11DeviceContext*          D3DContext() const { return context.Devices().d3dContext.get(); }

        // Renders one frame, synchronously, in which nodeId is evaluated even
        // when nothing displayed needs it; routes call it before reading a
        // node back. It runs on the dispatch thread inside the closure, so it
        // must not dispatch again, and it may add or remove nodes (re-resolve
        // node pointers after it). The GUI renders an offscreen frame on the
        // render worker; headless evaluates the whole graph. Unset: no frame
        // is rendered.
        std::function<void(uint32_t nodeId)> renderFrame;

        // The preview node for GET /graph. Unset: 0 (headless has no preview).
        std::function<uint32_t()>     getPreviewNodeId;

        // The pipeline format's display name for /display/info. Unset: "unknown".
        std::function<std::wstring()> getPipelineFormatName;

        // The last ICC profile loaded, reported as "loadedIcc" by
        // /display/profiles and offered again by the GUI's profile picker.
        // The GUI installs both; headless neither.
        std::function<std::optional<Rendering::DisplayProfile>()> getLoadedIccProfile;
        std::function<void(const Rendering::DisplayProfile&)>     setLoadedIccProfile;
    };

    // Runs route closures on the host's graph thread and tells the host what
    // changed. The On* events fire inside Dispatch, on the dispatch thread,
    // right after the closure's mutation, so a host can bring its UI in line
    // the way its own controls would; the defaults do nothing.
    class IEngineCommandSink
    {
    public:
        virtual ~IEngineCommandSink() = default;

        // Runs `closure` on the host's graph thread with a fresh
        // EngineContext and blocks until it returns. Exceptions propagate.
        virtual Response Dispatch(
            std::function<Response(EngineContext&)> closure) = 0;

        virtual void OnNodeAdded(uint32_t /*nodeId*/) {}
        virtual void OnNodeRemoved(uint32_t /*nodeId*/) {}
        virtual void OnNodeChanged(uint32_t /*nodeId*/) {}
        virtual void OnGraphCleared() {}
        virtual void OnGraphLoaded() {}
        virtual void OnGraphStructureChanged() {}  // edges added/removed

        // The graph's source paths now point into extractDir, the media a
        // /graph/load-file package was extracted to; empty when the new graph
        // uses no extracted media (load-file of bare JSON, /graph/load,
        // /graph/clear). The host owns the directory from here: keep it while
        // the graph uses it, delete the previous one, and delete it at
        // shutdown. The default leaves it in the temp folder.
        virtual void OnGraphMediaDirChanged(const std::wstring& /*extractDir*/) {}

        // A custom effect was recompiled: its bytecode and parameters, and
        // so its pins, may have changed.
        virtual void OnCustomEffectRecompiled(uint32_t /*nodeId*/) {}

        // The active display profile changed (simulated, live or ICC).
        // Working Space nodes already follow it when this fires.
        virtual void OnDisplayProfileChanged() {}
    };

    // Registers the engine routes on `server`, once. The sink must outlive
    // the server: handlers hold it by reference.
    void RegisterEngineRoutes(
        McpRouter& server,
        IEngineCommandSink& sink);

    // Registers POST /render/video (Rendering/VideoExport). Separate from
    // RegisterEngineRoutes because the export runs inside one Dispatch for
    // its whole encode: only a host whose dispatch thread can block that
    // long (headless) registers it.
    void RegisterVideoExportRoute(
        McpRouter& server,
        IEngineCommandSink& sink);
}
