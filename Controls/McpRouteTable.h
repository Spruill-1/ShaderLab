#pragma once

// Registers the GUI's own MCP routes with the thread each one runs on, so a
// route says where it runs instead of choosing a dispatch mechanism inline.
//
//   UI route:         runs on the UI thread (XAML, view state, NodeLog,
//                     UI-thread controllers). It may run layout on the render
//                     thread through RunLayoutOnRenderThread, never the
//                     reverse.
//   Render route:     runs on the render worker, the graph's single writer
//                     (m_graph, the render D2D context, pixel trace).
//   Any-thread route: runs on the MCP session thread and may touch only
//                     atomics and state published for any thread.
//
// Engine routes are not registered here: they go through
// IEngineCommandSink::Dispatch, which runs them on the render worker. The
// table may be discarded once the routes are added; each route keeps what
// it needs.

#include "../Mcp/McpRouter.h"
#include "../Rendering/RenderThreadDispatcher.h"

#include <functional>

namespace ShaderLab::Controls
{
    class McpRouteTable
    {
    public:
        using Handler = ::ShaderLab::McpRouter::Handler;
        // Runs a closure on the UI thread and returns its response; blocks.
        using UiInvoker = std::function<Mcp::Response(std::function<Mcp::Response()>)>;
        // True while the render worker cannot take work (an adapter switch).
        using BusyCheck = std::function<bool()>;

        McpRouteTable(McpRouter& router, Rendering::RenderThreadDispatcher& renderDispatcher,
                      UiInvoker invokeOnUi, BusyCheck renderBusy);

        void AddUiRoute(const std::wstring& method, const std::wstring& path, Handler handler);
        void AddRenderRoute(const std::wstring& method, const std::wstring& path, Handler handler);
        void AddAnyThreadRoute(const std::wstring& method, const std::wstring& path, Handler handler);

    private:
        McpRouter&                          m_router;
        Rendering::RenderThreadDispatcher&  m_renderDispatcher;
        UiInvoker                           m_invokeOnUi;
        BusyCheck                           m_renderBusy;
    };
}
