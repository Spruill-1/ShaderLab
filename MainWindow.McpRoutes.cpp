// MainWindow partial: the MCP router. The engine routes run through
// GuiEngineCommandSink on the render worker; the GUI's own routes are
// registered through a Controls::McpRouteTable, which states each route's
// thread (MainWindow.McpAppRoutes.cpp, Controls/PerfMcpRoutes).

#include "pch.h"
#include "MainWindow.xaml.h"
#include "Engine/Mcp/McpRouter.h"
#include "Engine/Mcp/McpJsonRpc.h"
#include "Engine/Mcp/McpJsonWriter.h"
#include "Engine/Mcp/McpTimeouts.h"
#include "Controls/McpRouteTable.h"
#include "Controls/PerfMcpRoutes.h"

namespace winrt::ShaderLab::implementation
{
    // Runs fn on the UI thread and blocks until it returns, rethrowing what
    // it throws. Never call it from the UI thread, which would wait on
    // itself. The state lives in a shared_ptr so a closure that runs after a
    // timeout still writes safely; a timeout or a closing window throws,
    // which the router answers with 500.
    template<typename F>
    auto MainWindow::RunOnUiThreadSync(F&& fn) -> decltype(fn())
    {
        using R = decltype(fn());
        if (DispatcherQueue().HasThreadAccess())
        {
            assert(!"RunOnUiThreadSync called on the UI thread");
            throw std::logic_error("RunOnUiThreadSync: called on the UI thread");
        }
        struct State
        {
            std::optional<R>      result;
            std::exception_ptr    ex;
            HANDLE                event{ nullptr };
            ~State() { if (event) CloseHandle(event); }
        };
        auto state = std::make_shared<State>();
        state->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!state->event)
            throw std::runtime_error("RunOnUiThreadSync: CreateEventW failed");

        auto fnPtr = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
        // TryEnqueue fails once the queue is shutting down; fail fast rather
        // than wait out the timeout (the DispatchSync rung of McpTimeouts.h).
        if (!DispatcherQueue().TryEnqueue([state, fnPtr]()
            {
                try { state->result = (*fnPtr)(); }
                catch (...) { state->ex = std::current_exception(); }
                SetEvent(state->event);
            }))
        {
            throw std::runtime_error("RunOnUiThreadSync: UI dispatcher queue is shutting down");
        }

        DWORD wait = WaitForSingleObject(state->event,
            static_cast<DWORD>(::ShaderLab::Mcp::kDispatchSyncTimeout.count()));
        if (wait != WAIT_OBJECT_0)
            throw std::runtime_error("RunOnUiThreadSync: UI thread did not respond in time");
        if (state->ex) std::rethrow_exception(state->ex);
        if (!state->result.has_value())
            throw std::runtime_error("RunOnUiThreadSync: lambda completed without producing a result");
        return std::move(*state->result);
    }

    ::ShaderLab::Mcp::Response MainWindow::GuiEngineCommandSink::Dispatch(
        std::function<::ShaderLab::Mcp::Response(
            ::ShaderLab::Mcp::EngineContext&)> closure)
    {
        // During an adapter switch the worker is joined and the device stack
        // is being rebuilt, so there is nothing to marshal to.
        if (window->m_adapterSwitchInProgress.load(std::memory_order_acquire))
            return ::ShaderLab::Mcp::ErrorResponse(503, "GPU adapter switch in progress; retry shortly");

        // The render worker is the graph's single writer; a call from the
        // worker itself runs inline. Exceptions reach the router, which
        // answers 500 with their message.
        return window->m_renderDispatcher.DispatchSync(
            [this, &closure]() -> ::ShaderLab::Mcp::Response
            {
                ::ShaderLab::Mcp::EngineContext ctx{};
                ctx.graph = &window->m_graph;
                ctx.evaluator = &window->m_graphEvaluator;
                ctx.displayMonitor = &window->m_displayMonitor;
                ctx.sourceFactory = &window->m_sourceFactory;
                // The render D2D context, shared with the per-frame render path.
                ctx.dc = window->m_renderEngine.RenderD2DContext();
                ctx.d3dDevice = window->m_renderEngine.D3DDevice();
                ctx.d3dContext = window->m_renderEngine.D3DContext();
                // m_readbackNodeId makes the frame evaluate nodeId even when
                // no preview or output window needs it.
                ctx.renderFrame = [this](uint32_t nodeId)
                {
                    window->m_readbackNodeId = nodeId;
                    try
                    {
                        window->RenderFrameToOffscreen();
                    }
                    catch (...)
                    {
                        window->m_readbackNodeId = 0;
                        throw;
                    }
                    window->m_readbackNodeId = 0;
                };
                ctx.getPreviewNodeId = [this]() -> uint32_t { return window->Preview().NodeId(); };
                ctx.getPipelineFormatName = [this]() -> std::wstring {
                    return std::wstring(window->m_renderEngine.ActiveFormat().name);
                };
                ctx.getLoadedIccProfile = [this]() -> std::optional<::ShaderLab::Rendering::DisplayProfile> {
                    return window->m_loadedIccProfile;
                };
                ctx.setLoadedIccProfile = [this](const ::ShaderLab::Rendering::DisplayProfile& p) {
                    window->m_loadedIccProfile = p;
                };
                return closure(ctx);
            },
            ::ShaderLab::Mcp::kRenderClosureTimeout);
    }

    // The events below fire inside Dispatch, on the render worker. Anything
    // that touches XAML or a UI-thread controller is posted to the UI thread.
    // Canvas layout reads the live graph, so from there it goes back to the
    // render thread (NodeGraphController::RequestAutoLayout / RequestRelayout).

    void MainWindow::GuiEngineCommandSink::OnNodeAdded(uint32_t nodeId)
    {
        // The new node starts dirty and has no consumers yet.
        window->m_forceRender = true;
        // An Output node gets its window, as when it is added from the toolbar.
        bool isOutput = false;
        if (auto* n = window->m_graph.FindNode(nodeId))
            isOutput = (n->type == ::ShaderLab::Graph::NodeType::Output);
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w, nodeId, isOutput]{
            w->m_nodeGraphController.RequestAutoLayout();
            w->PopulatePreviewNodeSelector();
            if (isOutput)
            {
                try { w->OpenOutputWindow(nodeId); }
                catch (...)
                {
                    // The node stays without a window, as after the user
                    // closes one; its log says why.
                    w->GetNodeLog(nodeId).Error(L"Its output window could not be opened");
                }
            }
        });
    }

    void MainWindow::GuiEngineCommandSink::OnNodeRemoved(uint32_t nodeId)
    {
        window->m_graphEvaluator.InvalidateNode(nodeId);
        // RemoveNode already dirtied the removed node's consumers.
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w, nodeId]{
            w->CloseOutputWindow(nodeId);
            w->m_nodeGraphController.RequestAutoLayout();
            w->PopulatePreviewNodeSelector();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnNodeChanged(uint32_t nodeId)
    {
        window->m_forceRender = true;

        // A visibleWhen parameter can show or hide a pin when a property
        // changes, so such a node's layout is rebuilt. This already runs on
        // the render worker, layout's home.
        if (auto* n = window->m_graph.FindNode(nodeId))
        {
            if (n->customEffect.has_value())
            {
                for (const auto& p : n->customEffect->parameters)
                {
                    if (!p.visibleWhen.empty())
                    {
                        window->m_nodeGraphController.RebuildLayout();
                        break;
                    }
                }
            }
        }
    }

    void MainWindow::GuiEngineCommandSink::OnGraphCleared()
    {
        window->m_graphEvaluator.ReleaseCache();
        window->Preview().ClearNode();
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            w->m_outputWindows.clear();
            // The worker would otherwise keep rendering into closed windows.
            {
                std::scoped_lock lock(w->m_outputSinksMutex);
                w->m_outputSinks.clear();
            }
            w->m_nodeGraphController.RequestAutoLayout();
            w->PopulatePreviewNodeSelector();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnGraphLoaded()
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            w->ResetAfterGraphLoad(/*reopenOutputWindows=*/true);
            w->m_nodeGraphController.RequestAutoLayout();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnGraphStructureChanged()
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            w->m_nodeGraphController.RequestAutoLayout();
            // Bindings, an upgrade or an expression-input edit change what
            // the panel shows and whether anything is still outdated.
            w->m_propertiesPanel.Rebuild();
            w->UpdateOutdatedEffectsButton();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnGraphMediaDirChanged(const std::wstring& extractDir)
    {
        // m_extractedMediaDirs and the heartbeat timer belong to the UI thread.
        // If the queue is shutting down the directory is left for the startup reaper.
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w, extractDir]
        {
            const std::wstring previous = w->m_mcpMediaDir;
            w->m_mcpMediaDir = extractDir;
            if (!previous.empty() && previous != extractDir)
            {
                // A file still open keeps the directory; it is retried at shutdown.
                std::error_code ec;
                std::filesystem::remove_all(previous, ec);
                if (!std::filesystem::exists(previous, ec))
                    std::erase(w->m_extractedMediaDirs, previous);
            }
            if (!extractDir.empty())
            {
                w->m_extractedMediaDirs.push_back(extractDir);
                w->TouchHeartbeats();
                w->StartHeartbeatTimer();
            }
        });
    }

    void MainWindow::GuiEngineCommandSink::OnCustomEffectRecompiled(uint32_t /*nodeId*/)
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            w->m_nodeGraphController.RequestRelayout();
            w->PopulateAddNodeFlyout();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnDisplayProfileChanged()
    {
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{ w->UpdateStatusBar(); });
    }

    void MainWindow::SetupMcpRoutes()
    {
        if (!m_mcpServer)
            m_mcpServer = std::make_unique<::ShaderLab::McpRouter>();
        if (!m_engineSink)
            m_engineSink = std::make_unique<GuiEngineCommandSink>(this);

        // Once per request, on the session thread; the UI tick paints it.
        m_mcpServer->SetActivityCallback(
            [this](const std::string& method,
                   const std::wstring& path,
                   uint16_t statusCode,
                   const std::string& peerAddress)
            {
                m_mcpIndicator.RecordRequest(method, path, statusCode, peerAddress);
            });

        ::ShaderLab::Mcp::RegisterEngineRoutes(*m_mcpServer, *m_engineSink);

        // POST /: the JSON-RPC dispatcher, which forwards tools/call to the
        // routes through Engine/Mcp/McpToolCatalog.
        ::ShaderLab::Mcp::JsonRpcOptions rpcOptions;
        rpcOptions.hostKind = "gui";
        ::ShaderLab::Mcp::RegisterJsonRpcEndpoint(*m_mcpServer, std::move(rpcOptions));

        ::ShaderLab::Controls::McpRouteTable routes(*m_mcpServer, m_renderDispatcher,
            [this](std::function<::ShaderLab::Mcp::Response()> fn) { return RunOnUiThreadSync(std::move(fn)); },
            [this] { return m_adapterSwitchInProgress.load(std::memory_order_acquire); });
        RegisterCanvasMcpRoutes(routes);
        RegisterPreviewMcpRoutes(routes);
        RegisterGpuMcpRoutes(routes);

        using ::ShaderLab::Controls::PerfSetting;
        ::ShaderLab::Controls::RegisterPerfRoutes(routes, {
            .timings = m_frameTimings,
            .previewStats = m_previewRenderStats,
            .gpuTimer = m_renderEngine.Timer(),
            .graph = m_graph,
            .renderDispatcher = m_renderDispatcher,
            .forceContinuousRedraw = m_forceContinuousRedraw,
            .forceRender = m_forceRender,
            .applyGpuBindings = [this](bool on) { ApplyGpuBindingsMode(on); },
            .showSetting = [this](PerfSetting setting, int32_t value)
            {
                DispatcherQueue().TryEnqueue([this, setting, value]
                {
                    switch (setting)
                    {
                    case PerfSetting::Unthrottled:
                        if (auto box = UnthrottledCheck()) box.IsChecked(value != 0);
                        break;
                    case PerfSetting::ForceRedraw:
                        if (auto box = ForceRedrawCheck()) box.IsChecked(value != 0);
                        break;
                    case PerfSetting::GpuBindings:
                        if (auto box = GpuBindingsCheck()) box.IsChecked(value != 0);
                        break;
                    case PerfSetting::AnalysisRefresh:
                        if (auto combo = AnalysisRefreshCombo()) combo.SelectedIndex(value);
                        break;
                    }
                });
            },
        });
    }
}
