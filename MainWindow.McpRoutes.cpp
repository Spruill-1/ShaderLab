#include "pch.h"
#include "MainWindow.xaml.h"
#include "Engine/Mcp/McpRouter.h"
#include "Engine/Mcp/McpJsonRpc.h"
#include "Engine/Mcp/McpTimeouts.h"
#include <appmodel.h>
#include <shlobj.h>
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/SourceNodeFactory.h"
#include "Rendering/IccProfileParser.h"
#include "Version.h"

// Helper: narrow string from wide string.
static std::string ToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), len, nullptr, nullptr);
    return s;
}

// Base64 (standard alphabet, '=' padding, no line wrapping).
static std::string Base64Encode(const uint8_t* data, size_t len)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    if (len == 0) return out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len)
    {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
        i += 3;
    }
    if (i < len)
    {
        uint32_t v = uint32_t(data[i]) << 16;
        bool two = (i + 1 < len);
        if (two) v |= uint32_t(data[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(two ? kAlphabet[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}


namespace winrt::ShaderLab::implementation
{
    // Dispatch a lambda to the UI thread and block until completion.
    // Returns the result from the lambda. Must NOT be called from the UI thread.
    //
    // Key correctness properties:
    //   * The state (result/exception/event) lives in a shared_ptr captured by
    //     the lambda, so if we time out and the caller returns, the lambda can
    //     still safely write to it without dangling references.
    //   * We always check the wait result before reading the result so a timeout
    //     won't deref an empty optional. On timeout we throw so the calling
    //     route returns a 500 instead of producing garbage.
    //   * Non-void return only -- DispatchSync<void> isn't supported.
    template<typename F>
    auto MainWindow::DispatchSync(F&& fn) -> decltype(fn())
    {
        using R = decltype(fn());
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
            throw std::runtime_error("DispatchSync: CreateEventW failed");

        // Move the lambda into a shared_ptr so it stays alive even if the
        // DispatcherQueue holds the callback longer than this scope.
        auto fnPtr = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
        // TryEnqueue returns false once the DispatcherQueue is shutting down
        // (window closing). The old code DISCARDED that bool, so the event
        // never fired and every in-flight request ate its full 30 s timeout
        // during shutdown. Fail fast instead — this is the DispatchSync rung
        // of the timeout ladder (Engine/Mcp/McpTimeouts.h).
        if (!DispatcherQueue().TryEnqueue([state, fnPtr]()
            {
                try { state->result = (*fnPtr)(); }
                catch (...) { state->ex = std::current_exception(); }
                SetEvent(state->event);
            }))
        {
            throw std::runtime_error("DispatchSync: UI dispatcher queue is shutting down");
        }

        DWORD wait = WaitForSingleObject(state->event,
            static_cast<DWORD>(::ShaderLab::Mcp::kDispatchSyncTimeout.count()));
        if (wait != WAIT_OBJECT_0)
            throw std::runtime_error("DispatchSync: UI thread did not respond in time");
        if (state->ex) std::rethrow_exception(state->ex);
        if (!state->result.has_value())
            throw std::runtime_error("DispatchSync: lambda completed without producing a result");
        return std::move(*state->result);
    }

    // stdio-migration Step 8: the hub's AUMID for the client's shim to
    // activate. Packaged: "<PackageFamilyName>!Hub". Unpackaged (dev): empty
    // — no packaged hub to activate, so the shim only works against a hub
    // that's already running (e.g. a deployed GUI, or a manual --hub).
    std::wstring MainWindow::HubAumid()
    {
        UINT32 len = 0;
        LONG rc = ::GetCurrentPackageFamilyName(&len, nullptr);
        if (rc != ERROR_INSUFFICIENT_BUFFER)
            return {};   // APPMODEL_ERROR_NO_PACKAGE -> unpackaged
        std::wstring pfn(len, L'\0');
        if (::GetCurrentPackageFamilyName(&len, pfn.data()) != ERROR_SUCCESS)
            return {};
        pfn.resize(len ? len - 1 : 0);   // drop the null terminator
        return pfn + L"!Hub";
    }

    // stdio-migration Step 8: copy the shim (ShaderLabMcpBroker.exe, next to
    // ShaderLab.exe in the package payload) to a STABLE UNPACKAGED path,
    // %LOCALAPPDATA%\ShaderLab\bin\. Being unpackaged, that copy is immune to
    // MSIX update / uninstall — the MCP client keeps talking to it across a
    // ShaderLab upgrade. Returns the target path (empty on failure).
    //
    // Rename-then-write: a running shim holds the file open, so overwrite-in-
    // place fails. Renaming the existing copy aside works even while it runs
    // (the process keeps its image), then the fresh binary lands at the
    // canonical path for the NEXT client launch. Stale .old files are reaped
    // best-effort (a still-running shim keeps its .old locked until it exits).
    std::wstring MainWindow::EnsureShimDistributed()
    {
        wchar_t exePath[MAX_PATH * 2]{};
        if (::GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) == 0)
            return {};
        std::wstring dir = exePath;
        auto slash = dir.find_last_of(L'\\');
        if (slash == std::wstring::npos) return {};
        std::wstring source = dir.substr(0, slash) + L"\\ShaderLabMcpBroker.exe";
        if (::GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES)
            return {};   // no broker payload (unexpected in a real build)

        PWSTR local = nullptr;
        if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            return {};
        std::wstring binDir = std::wstring(local) + L"\\ShaderLab\\bin";
        ::CoTaskMemFree(local);
        ::SHCreateDirectoryExW(nullptr, binDir.c_str(), nullptr);
        std::wstring target = binDir + L"\\ShaderLabMcpBroker.exe";

        if (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            std::wstring aside = target + L"." + std::to_wstring(::GetTickCount64()) + L".old";
            ::MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        ::CopyFileW(source.c_str(), target.c_str(), FALSE);

        // Reap stale .old copies whose shim has since exited.
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((target + L".*.old").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do { ::DeleteFileW((binDir + L"\\" + fd.cFileName).c_str()); }
            while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        return (::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
            ? target : std::wstring{};
    }

    // stdio-migration Step 7: register this window with the broker hub as a
    // session. The client serves each sealed request by routing through
    // m_mcpServer -- the same router/dispatcher/engine routes the HTTP
    // transport uses -- so tool calls marshal to the render worker and fire
    // the 8 event hooks exactly like an HTTP request. Idempotent.
    void MainWindow::StartMcpSession()
    {
        if (m_sessionClient || !m_mcpServer)
            return;

        // Refresh the on-disk shim (rename-then-write) so a client launching
        // it gets this build, while any shim an MCP client already has open
        // keeps running (update-immune by design, stdio-migration Step 8).
        EnsureShimDistributed();

        if (m_mcpSessionId.empty())
        {
            GUID g{};
            CoCreateGuid(&g);
            wchar_t buf[64]{};
            StringFromGUID2(g, buf, ARRAYSIZE(buf));
            m_mcpSessionId = buf;   // stable for this window's lifetime, not an ordinal
        }

        ::ShaderLab::Mcp::SessionClientOptions opts;
        opts.sessionId = m_mcpSessionId;
        opts.label = std::format(L"ShaderLab {} (pid {})",
            std::wstring(::ShaderLab::VersionString), GetCurrentProcessId());
        m_sessionClient = std::make_unique<::ShaderLab::Mcp::McpSessionClient>(
            *m_mcpServer, std::move(opts));
        m_sessionThread = std::thread([this] { m_sessionClient->Run(); });
    }

    // Stop the session BEFORE the render dispatcher shuts down: reject-new ->
    // (Run observes stop) -> close pipe (CancelIo equivalent) -> join. If we
    // instead joined after m_renderDispatcher.Shutdown(), an in-flight
    // session request could be mid-DispatchSync onto a worker that is already
    // gone -- the 30 s stall the plan warns about.
    void MainWindow::StopMcpSession()
    {
        if (m_sessionClient)
            m_sessionClient->Stop();
        if (m_sessionThread.joinable())
            m_sessionThread.join();
        m_sessionClient.reset();
    }

    // Toolbar label. The toggle means "expose this window to MCP"; the label
    // reflects whether this window is registered as a hub session. (Richer
    // "session N of M / no hub" wording would need a hub round-trip on the UI
    // tick; deferred.)
    void MainWindow::UpdateMcpStatusLabel()
    {
        if (!McpServerLabel())
            return;
        McpServerLabel().Text(m_sessionClient ? L"MCP: on" : L"MCP: off");
    }

    ::ShaderLab::Mcp::Response MainWindow::GuiEngineCommandSink::Dispatch(
        std::function<::ShaderLab::Mcp::Response(
            ::ShaderLab::Mcp::EngineContext&)> closure)
    {
        // While an adapter switch is in flight the render worker is joined
        // and the device stack is being torn down + rebuilt, so there is no
        // valid consumer or D2D context to marshal to. Fail with 503 rather
        // than dispatching into a half-dead engine. A user clicking the GPU
        // dropdown mid-request hits exactly this.
        if (window->m_adapterSwitchInProgress.load(std::memory_order_acquire))
        {
            ::ShaderLab::Mcp::Response busy;
            busy.statusCode = 503;
            busy.body = R"({"error":"GPU adapter switch in progress; retry shortly"})";
            return busy;
        }

        // Marshal the engine work to the render thread (single writer to
        // m_graph). Re-entrant calls from inside the consumer thread run
        // inline (RenderThreadDispatcher detects this). The render rung of
        // the timeout ladder (Engine/Mcp/McpTimeouts.h) bounds the wait so a
        // wedged closure surfaces before MainWindow::DispatchSync above it.
        ::ShaderLab::Mcp::Response resp;
        try
        {
            resp = window->m_renderDispatcher.DispatchSync(
                [this, &closure]() -> ::ShaderLab::Mcp::Response {
                    ::ShaderLab::Mcp::EngineContext ctx{};
                    ctx.graph = &window->m_graph;
                    ctx.evaluator = &window->m_graphEvaluator;
                    ctx.displayMonitor = &window->m_displayMonitor;
                    ctx.sourceFactory = &window->m_sourceFactory;
                    // Closure runs on render thread now -- give it the render
                    // D2D context so source-prep / evaluator ops it performs
                    // share state with the per-frame render path.
                    ctx.dc = window->m_renderEngine.RenderD2DContext();
                    ctx.d3dDevice = window->m_renderEngine.D3DDevice();
                    ctx.d3dContext = window->m_renderEngine.D3DContext();
                    ctx.renderFrame = [this]() { window->RenderFrameToOffscreen(0.0); };
                    ctx.getPreviewNodeId = [this]() -> uint32_t { return window->m_previewNodeId; };
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
        catch (const std::exception& e)
        {
            ::ShaderLab::Mcp::Response err;
            err.statusCode = 500;
            err.body = std::string(R"({"error":")") + e.what() + R"("})";
            err.contentType = "application/json";
            return err;
        }
        return resp;
    }

    // Event hooks fire from inside Dispatch closures, which run on the
    // render thread. Anything that touches XAML or UI-thread-only controllers
    // must be marshalled back to the UI thread via DispatcherQueue().TryEnqueue.
    //
    // P7 race protection: NodeGraphController::AutoLayout / RebuildLayout
    // iterate `m_graph.Nodes()` and each node's `properties` std::map. The
    // render worker mutates those maps every tick (analysisOutput updates,
    // clock advancement, source factory writes). To avoid use-after-free /
    // iterator invalidation in std::map traversal, we run those layout
    // calls THROUGH the render dispatcher with DispatchSync. The dispatcher
    // drains queued closures BEFORE the worker's per-tick body, so when our
    // closure runs the worker is implicitly paused -- m_graph is stable for
    // the duration of the layout, and m_visuals isn't being concurrently
    // read by the UI thread (which is blocked in DispatchSync).
    static void RunLayoutOnRenderThread(::ShaderLab::Rendering::RenderThreadDispatcher& dispatcher,
                                        std::function<void()> work)
    {
        dispatcher.DispatchSync([work = std::move(work)]() {
            work();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnNodeAdded(uint32_t nodeId)
    {
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        // Detect Output nodes added via /graph/apply or other engine-side
        // routes and auto-open a window for each. Engine-pure hosts don't
        // care about windows; this sink runs only in the GUI host.
        bool isOutput = false;
        if (auto* n = window->m_graph.FindNode(nodeId))
            isOutput = (n->type == ::ShaderLab::Graph::NodeType::Output);
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w, nodeId, isOutput]{
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
            w->PopulatePreviewNodeSelector();
            if (isOutput)
            {
                try { w->OpenOutputWindow(nodeId); } catch (...) {}
            }
        });
    }

    void MainWindow::GuiEngineCommandSink::OnNodeRemoved(uint32_t nodeId)
    {
        window->m_graphEvaluator.InvalidateNode(nodeId);
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w, nodeId]{
            w->CloseOutputWindow(nodeId);
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
            w->PopulatePreviewNodeSelector();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnNodeChanged(uint32_t nodeId)
    {
        window->m_forceRender = true;

        // If the changed node has any visibleWhen-conditional parameters,
        // a property change might flip a pin's visibility. Rebuild the
        // node-graph layout so new input pins materialize on the canvas.
        //
        // OnNodeChanged is already running inside a render-dispatcher
        // closure (the /graph/set-property route DispatchSync's into the
        // worker before invoking this hook), so it is safe to touch
        // m_graph and m_nodeGraphController here directly -- the worker
        // is paused for the duration. We just need to tell the controller
        // to repaint via m_needsRedraw (set by RebuildLayout itself).
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
        window->m_previewNodeId = 0;
        window->m_graph.MarkAllDirty();
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            w->m_outputWindows.clear();
            // Also drop sinks so the render worker doesn't keep churning on
            // closed windows. Without this the m_outputSinks vector grows
            // unbounded across graph clear/load cycles.
            {
                std::scoped_lock lock(w->m_outputSinksMutex);
                w->m_outputSinks.clear();
            }
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
            w->PopulatePreviewNodeSelector();
        });
    }

    void MainWindow::GuiEngineCommandSink::OnGraphLoaded()
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            w->ResetAfterGraphLoad(/*reopenOutputWindows=*/true);
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
        });
    }

    void MainWindow::GuiEngineCommandSink::OnGraphStructureChanged()
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.AutoLayout(); });
        });
    }

    void MainWindow::GuiEngineCommandSink::OnCustomEffectRecompiled(uint32_t /*nodeId*/)
    {
        window->m_forceRender = true;
        auto* w = window;
        w->DispatcherQueue().TryEnqueue([w]{
            RunLayoutOnRenderThread(w->m_renderDispatcher,
                [w]{ w->m_nodeGraphController.RebuildLayout(); });
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

        // Activity callback: fires on the listener thread once per HTTP request.
        // We update atomic state + a small mutexed string snapshot; the UI render
        // tick polls these and refreshes the indicator dot + tooltip.  Keeping
        // this side cheap and non-blocking ensures we don't introduce lock-step
        // between MCP responses and the UI thread.
        m_mcpServer->SetActivityCallback(
            [this](const std::string& method,
                   const std::wstring& path,
                   uint16_t statusCode,
                   const std::string& peerAddress)
            {
                using clk = std::chrono::system_clock;
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    clk::now().time_since_epoch()).count();
                m_mcpLastActivityMs.store(nowMs, std::memory_order_relaxed);
                m_mcpRequestCount.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard lock(m_mcpLastReqMutex);
                    m_mcpLastReqMethod = method;
                    m_mcpLastReqPath = ToUtf8(path);
                    m_mcpLastReqPeer = peerAddress;
                    m_mcpLastReqStatus = statusCode;
                    if (!peerAddress.empty())
                        m_mcpKnownPeers.insert(peerAddress);
                }
                m_mcpUiUpdateSeq.fetch_add(1, std::memory_order_release);
            });

        // Register engine-pure routes (Phase 7 migration). Currently
        // empty; routes are migrated in batches with each commit.
        // The GUI app then registers UI-coupled routes below
        // (graph_snapshot, preview/graph view tools, etc).
        ::ShaderLab::Mcp::RegisterEngineRoutes(*m_mcpServer, *m_engineSink);

        // =====================================================================
        // GET / (health) + POST / (JSON-RPC dispatcher) — engine-provided
        // (stdio-migration Step 3). RegisterJsonRpcEndpoint installs both on
        // this router; the dispatcher forwards tools/call into the routes
        // registered here via the declarative Engine/Mcp/McpToolCatalog.
        // =====================================================================
        {
            ::ShaderLab::Mcp::JsonRpcOptions rpcOptions;
            rpcOptions.hostKind = "gui";
            ::ShaderLab::Mcp::RegisterJsonRpcEndpoint(*m_mcpServer, std::move(rpcOptions));
        }

        // =====================================================================
        // GET /context  — System prompt / onboarding for calling agents
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/context", [](const std::wstring&, const std::wstring&, const std::string&)
            -> ::ShaderLab::Mcp::Response
        {
            std::string doc = R"JSON({
"name": "ShaderLab",
"description": "D2D shader effect development tool with node graph, HDR/WCG pipeline.",
"pipeline": {
    "format": "scRGB FP16 linear light",
    "whitePoint": "1.0 = 80 nits SDR white",
    "valuesAbove1": "HDR, super-white",
    "colorSpace": "Linear sRGB primaries, Rec.709"
},
"shaderConventions": {
    "texcoords": "D2D provides TEXCOORD0 in pixel/scene space, not normalized 0-1",
    "sampling": "Use Load int3 uv0.xy 0 for direct texel access; all inputs share TEXCOORD0",
    "filteredSampling": "Use SampleLevel with GetDimensions normalization for bilinear",
    "constantBuffer": "register b0, variables packed by D3DReflect offsets",
    "textures": "register t0..t7, one per input",
    "computeOutput": "RWTexture2D<float4> Output : register u0"
},
"analysisEffects": {
    "pattern": "Compute shaders can act as analysis effects that read an entire image and produce typed output fields",
    "outputTypes": "float, float2, float3, float4, floatarray, float2array, float3array, float4array",
    "outputConvention": "Write results to Output[int2(pixelOffset, 0)]. Each field occupies pixelCount() pixels sequentially",
    "readback": "The host reads the output row and unpacks typed fields based on analysisFields descriptors",
    "fieldDescriptors": "Define analysisFields array in custom effect definition with name, type, and length (for arrays)",
    "propertyBindings": "Analysis output fields can be bound to downstream node properties via propertyBindings",
    "builtInExample": "D2D Histogram effect: processes input, exposes 256-float histogram via GetValue",
    "customExample": "Gamut analysis: Output[0,0].x = maxLuminance (float), Output[1,0] = gamutBounds (float4), etc."
},
"nodeTypes": ["Source", "BuiltInEffect", "PixelShader", "ComputeShader", "Output"],
"outputNote": "PNG captures are tone-mapped SDR. Use POST /render/pixel-region for true scRGB float values. Values above 1.0 are HDR.",
"endpoints": {
    "GET /context": "This document",
    "GET /graph": "Full graph state with nodes, edges, properties, custom effects",
    "GET /graph/node/{id}": "Single node detail including custom effect definition",
    "GET /registry/effects": "All built-in D2D effects",
    "GET /custom-effects": "All custom effects in graph with HLSL source",
    "GET /render/capture": "Output as base64 PNG, SDR tone-mapped",
    "POST /render/pixel-region": "FP32 scRGB region readback, body: nodeId x y w h",
    "POST /graph/add-node": "Add a node, body: effectName string",
    "POST /graph/remove-node": "Remove node, body: nodeId number",
    "POST /graph/connect": "Connect pins, body: srcId srcPin dstId dstPin",
    "POST /graph/disconnect": "Disconnect, body: srcId srcPin dstId dstPin",
    "POST /graph/set-property": "Set property, body: nodeId key value",
    "POST /graph/load": "Load graph JSON, body: full graph JSON string",
    "GET /graph/save": "Get graph as JSON string",
    "POST /graph/clear": "Clear entire graph",
    "POST /effect/compile": "Compile HLSL, body: nodeId hlsl",
    "POST /render/preview-node": "Set preview node, body: nodeId number"
}
})JSON";
            return { 200, doc };
        });

        // =====================================================================
        // GET /graph, GET /graph/save, GET /graph/node/{id}
        // -- moved to Engine/Mcp/EngineMcpRoutes.cpp (Phase 7).
        // =====================================================================

        // =====================================================================
        // GET /registry -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Static D2D effect catalog, no Dispatch needed.
        // =====================================================================

        // =====================================================================
        // GET /custom-effects -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7).
        // =====================================================================

        // =====================================================================
        // POST /graph/add-node -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/remove-node -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/connect -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Same notes as /graph/disconnect.
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/disconnect -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). UI side effects (nodeLogs, AutoLayout) drop
        // out: the GUI render tick picks up dirty state and refreshes the
        // canvas next frame.
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/set-property -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Mutates m_graph through IEngineCommandSink.
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/load -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // =====================================================================
        // POST /graph/clear -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // POST /render/preview-node
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/render/preview-node", [this](const std::wstring&, const std::wstring&, const std::string& body)
            -> ::ShaderLab::Mcp::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                    m_previewNodeId = nodeId;
                    m_needsFitPreview = true;
                    m_forceRender = true;
                    m_graph.MarkAllDirty();
                    return { 200, R"({"ok":true})" };
                });
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // POST /effect/compile
        // =====================================================================
        // -- moved to Engine/Mcp/EngineMcpRoutes.cpp (Phase 7).
        //    Mirrors EffectDesignerWindow Update-in-Graph; fires
        //    OnCustomEffectRecompiled hook.
        // =====================================================================

        // =====================================================================
        // GET /render/pixel/{x}/{y} -- REMOVED (stdio-migration Step 1).
        // Was a "coming soon" stub that touched the UI D2D context on the
        // listener thread with no dispatch and std::stof'd unvalidated
        // input. True pixel readback is POST /render/pixel-region (engine
        // route); an unmatched GET path now 404s from the router.
        // =====================================================================

        // =====================================================================
        // POST /graph/rename-node
        // =====================================================================
        // Promoted from the inline `graph_rename_node` tools/call handler
        // (stdio-migration Step 1). Stays app-side: the rename must refresh
        // XAML surfaces (preview selector + Add Node flyout) and
        // IEngineCommandSink has no rename hook -- adding one is an engine
        // ABI change, deferred until Step 2 bumps the ABI anyway.
        // Threading: mutation + RebuildLayout run on the render thread
        // (layout's home per the NodeGraphController rules); the XAML
        // refresh is TryEnqueue'd to the UI thread fire-and-forget, so a
        // busy UI can no longer turn a committed rename into a 500.
        m_mcpServer->AddRoute(L"POST", L"/graph/rename-node", [this](const std::wstring&, const std::wstring&, const std::string& body)
            -> ::ShaderLab::Mcp::Response
        {
            uint32_t nodeId = 0;
            std::wstring newName;
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                newName = std::wstring(jobj.GetNamedString(L"name"));
            }
            catch (...) { return { 400, R"({"error":"Invalid request: need nodeId + name"})" }; }

            auto resp = m_renderDispatcher.DispatchSync(
                [this, nodeId, &newName]() -> ::ShaderLab::Mcp::Response {
                    auto* node = m_graph.FindNode(nodeId);
                    if (!node) return { 404, R"({"error":"Node not found"})" };
                    node->name = newName;
                    m_nodeGraphController.RebuildLayout();
                    return { 200, R"({"ok":true})" };
                });

            if (resp.statusCode == 200)
            {
                DispatcherQueue().TryEnqueue([this]() {
                    PopulatePreviewNodeSelector();
                    PopulateAddNodeFlyout();
                });
            }
            return resp;
        });

        // =====================================================================
        // =====================================================================
        // GET /render/capture -- Save output PNG to temp file, return path
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/render/capture", [this](const std::wstring&, const std::wstring&, const std::string&)
            -> ::ShaderLab::Mcp::Response
        {
            return m_renderDispatcher.DispatchSync(
                [this]() -> ::ShaderLab::Mcp::Response {
                // Run on render thread (single writer to graph + owns the
                // engine D2D context). Force a full re-evaluation so the
                // capture reflects current state.
                m_graph.MarkAllDirty();
                RenderFrameToOffscreen(0.0);
                auto pngData = CapturePreviewAsPng();
                if (pngData.empty())
                    return { 404, R"({"error":"No output image"})" };

                // Write to temp file.
                wchar_t tempPath[MAX_PATH]{};
                GetTempPathW(MAX_PATH, tempPath);
                std::wstring filePath = std::wstring(tempPath) + L"shaderlab_capture.png";
                HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile == INVALID_HANDLE_VALUE)
                    return { 500, R"({"error":"Failed to create temp file"})" };
                DWORD written = 0;
                WriteFile(hFile, pngData.data(), static_cast<DWORD>(pngData.size()), &written, nullptr);
                CloseHandle(hFile);

                auto escaped = ::ShaderLab::Mcp::JsonEscape(ToUtf8(filePath));

                return { 200, std::format("{{\"path\":\"{}\",\"size\":{}}}", escaped, pngData.size()) };
            });
        });

        // =====================================================================
        // GET /perf — Return per-frame performance timings
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/perf", [this](const std::wstring&, const std::wstring&, const std::string&)
            -> ::ShaderLab::Mcp::Response
        {
            auto& t = m_lastFrameTiming;
            if (t.framesSampled == 0)
                t = m_frameTiming;  // fallback to live if no snapshot yet
            double fps = (t.totalUs > 0) ? 1000000.0 / t.totalUs : 0;
            return { 200, std::format(
                "{{\"fps\":{:.1f},\"totalMs\":{:.2f},"
                "\"sourcesPrepMs\":{:.2f},\"evaluateMs\":{:.2f},"
                "\"deferredComputeMs\":{:.2f},\"drawMs\":{:.2f},"
                "\"endDrawFlushMs\":{:.2f},"
                "\"uiTickMs\":{:.2f},\"outputWindowsMs\":{:.2f},\"traceMs\":{:.2f},"
                "\"computeDispatches\":{},"
                "\"framesSampled\":{},\"endDrawFailed\":{}}}",
                fps, t.totalUs / 1000.0,
                t.sourcesPrepUs / 1000.0, t.evaluateUs / 1000.0,
                t.deferredComputeUs / 1000.0, t.drawUs / 1000.0,
                t.endDrawFlushUs / 1000.0,
                t.uiTickUs / 1000.0, t.outputWindowsUs / 1000.0, t.traceUs / 1000.0,
                t.computeDispatches,
                t.framesSampled, t.endDrawFailed) };
        });

        // =====================================================================
        // GET /display/info -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (stdio-migration Step 2). Step 1 parked it here because it needs
        // the pipeline-format name; EngineContext::getPipelineFormatName
        // (supplied by GuiEngineCommandSink::Dispatch from
        // RenderEngine::ActiveFormat()) closed that gap, so it is now
        // engine-pure and headless serves it too.
        // =====================================================================

        // =====================================================================
        // GET /node/{id}/logs — Return per-node log entries
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/node/", [this](const std::wstring& path, const std::wstring& query, const std::string&)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                // Parse nodeId and optional /logs suffix from path.
                // Expected: /node/{id}/logs or /node/{id}/logs?since={seq}
                auto stripped = path.substr(6); // remove "/node/"
                uint32_t nodeId = 0;
                try { nodeId = static_cast<uint32_t>(std::stoi(stripped)); } catch (...) {
                    return { 400, R"({"error":"Invalid node ID"})" };
                }

                auto it = m_nodeLogs.find(nodeId);
                if (it == m_nodeLogs.end())
                    return { 200, R"({"logs":[]})" };

                auto& log = it->second;
                // ?since= arrives via the query argument (stdio-migration
                // Step 2). Previously the HTTP listener stripped the query
                // before routing, so raw-HTTP polls silently returned the
                // whole log every time; only RouteRequest callers that
                // embedded "?since=" in the path string got filtering.
                uint64_t sinceSeq = 0;
                auto qPos = query.find(L"since=");
                if (qPos != std::wstring::npos)
                {
                    try { sinceSeq = std::stoull(query.substr(qPos + 6)); } catch (...) {}
                }

                std::string json = "{\"logs\":[";
                bool first = true;
                for (const auto& entry : log.Entries())
                {
                    if (entry.sequence <= sinceSeq) continue;
                    if (!first) json += ",";
                    first = false;

                    auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        entry.timestamp.time_since_epoch()).count() % 1000;
                    struct tm tm_buf{};
                    localtime_s(&tm_buf, &tt);
                    char timeBuf[32]{};
                    sprintf_s(timeBuf, "%02d:%02d:%02d.%03d",
                        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms));

                    const char* levelStr = "Info";
                    if (entry.level == ::ShaderLab::Controls::LogLevel::Warning) levelStr = "Warning";
                    else if (entry.level == ::ShaderLab::Controls::LogLevel::Error) levelStr = "Error";

                    // Shared escaper (stdio-migration Step 3 unified the
                    // previously-divergent copies).
                    std::string escaped = ::ShaderLab::Mcp::JsonEscape(ToUtf8(entry.message));

                    json += std::format("{{\"seq\":{},\"time\":\"{}\",\"level\":\"{}\",\"message\":\"{}\"}}",
                        entry.sequence, timeBuf, levelStr, escaped);
                }
                json += "]}";
                return { 200, json };
            });
        });

        // =====================================================================
        // POST /graph/bind-property -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // POST /graph/unbind-property -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================

        // =====================================================================
        // GET /analysis/{id} -- moved to Engine/Mcp/EngineMcpRoutes.cpp (Phase 7).
        // =====================================================================

        // =====================================================================
        // POST /render/pixel-trace — Run pixel trace at normalized coordinates
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/render/pixel-trace", [this](const std::wstring&, const std::wstring&, const std::string& body)
            -> ::ShaderLab::Mcp::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                float normX = static_cast<float>(jobj.GetNamedNumber(L"x"));
                float normY = static_cast<float>(jobj.GetNamedNumber(L"y"));

                return m_renderDispatcher.DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                    // P13: pixel-trace runs on the render thread. m_graph is
                    // single-writer there; the render-side D2D context shares
                    // the engine's multi-threaded D2D device with the worker's
                    // BeginDraw session. UI's PopulatePixelTraceTree also
                    // routes its ReTrace through the render dispatcher, so
                    // there's no concurrent access to m_pixelTrace state.
                    auto* dc = m_renderEngine.RenderD2DContext();
                    if (!dc) return { 500, R"({"error":"No device context"})" };

                    // Compute preview image bounds inline on render context
                    // (can't call MainWindow::GetPreviewImageBounds, which
                    // reads from the UI D2D context).
                    auto* previewNode = m_graph.FindNode(m_previewNodeId);
                    auto* previewImage = previewNode ? previewNode->cachedOutput : nullptr;
                    if (!previewImage) return { 404, R"({"error":"No preview image"})" };
                    float oldDpiX, oldDpiY;
                    dc->GetDpi(&oldDpiX, &oldDpiY);
                    dc->SetDpi(96.0f, 96.0f);
                    D2D1_RECT_F bounds{};
                    HRESULT bhr = dc->GetImageLocalBounds(previewImage, &bounds);
                    dc->SetDpi(oldDpiX, oldDpiY);
                    if (FAILED(bhr)) return { 500, R"({"error":"GetImageLocalBounds failed"})" };

                    uint32_t imageW = static_cast<uint32_t>(bounds.right - bounds.left);
                    uint32_t imageH = static_cast<uint32_t>(bounds.bottom - bounds.top);
                    if (imageW == 0 || imageH == 0)
                        return { 404, R"({"error":"No preview image"})" };

                    if (!m_pixelTrace.BuildTrace(dc, m_graph, nodeId, normX, normY, imageW, imageH))
                        return { 500, R"({"error":"Pixel trace failed"})" };

                    const auto& root = m_pixelTrace.Root();
                    uint32_t pixelX = static_cast<uint32_t>(normX * imageW);
                    uint32_t pixelY = static_cast<uint32_t>(normY * imageH);

                    // Recursive lambda to serialize the trace tree.
                    std::function<std::string(const ::ShaderLab::Controls::PixelTraceNode&)> serializeNode;
                    serializeNode = [&](const ::ShaderLab::Controls::PixelTraceNode& tn) -> std::string
                    {
                        std::string j = "{";
                        j += std::format("\"nodeId\":{},\"name\":\"{}\",\"pin\":\"{}\"",
                            tn.nodeId, ToUtf8(tn.nodeName), ToUtf8(tn.pinName));

                        // Pixel values.
                        const auto& px = tn.pixel;
                        j += std::format(",\"pixel\":{{\"scRGB\":[{:.6f},{:.6f},{:.6f},{:.6f}]",
                            px.scR, px.scG, px.scB, px.scA);
                        j += std::format(",\"sRGB\":[{},{},{},{}]",
                            px.sR, px.sG, px.sB, px.sA);
                        j += std::format(",\"luminance\":{:.2f}}}", px.luminanceNits);

                        // Analysis fields (for compute/analysis nodes).
                        if (tn.hasAnalysisOutput && !tn.analysisFields.empty())
                        {
                            j += ",\"analysisFields\":[";
                            bool first = true;
                            for (const auto& fv : tn.analysisFields)
                            {
                                if (!first) j += ",";
                                j += "{\"name\":\"" + ToUtf8(fv.name) + "\"";

                                std::string typeTag;
                                switch (fv.type)
                                {
                                case ::ShaderLab::Graph::AnalysisFieldType::Float:       typeTag = "float"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float2:      typeTag = "float2"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float3:      typeTag = "float3"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float4:      typeTag = "float4"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::FloatArray:   typeTag = "floatarray"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float2Array:  typeTag = "float2array"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float3Array:  typeTag = "float3array"; break;
                                case ::ShaderLab::Graph::AnalysisFieldType::Float4Array:  typeTag = "float4array"; break;
                                }
                                j += ",\"type\":\"" + typeTag + "\"";

                                if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                                {
                                    uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                                    j += ",\"value\":[";
                                    for (uint32_t c = 0; c < cc; ++c)
                                    {
                                        if (c > 0) j += ",";
                                        j += std::format("{:.6f}", fv.components[c]);
                                    }
                                    j += "]";
                                }
                                else
                                {
                                    uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                                    uint32_t count = stride > 0 ? static_cast<uint32_t>(fv.arrayData.size()) / stride : 0;
                                    j += ",\"count\":" + std::to_string(count);
                                    j += ",\"value\":[";
                                    for (size_t i = 0; i < fv.arrayData.size(); ++i)
                                    {
                                        if (i > 0) j += ",";
                                        j += std::format("{:.6f}", fv.arrayData[i]);
                                    }
                                    j += "]";
                                }
                                j += "}";
                                first = false;
                            }
                            j += "]";
                        }
                        else
                        {
                            j += ",\"analysisFields\":[]";
                        }

                        // Recurse into inputs.
                        j += ",\"inputs\":[";
                        for (size_t i = 0; i < tn.inputs.size(); ++i)
                        {
                            if (i > 0) j += ",";
                            j += serializeNode(tn.inputs[i]);
                        }
                        j += "]";

                        j += "}";
                        return j;
                    };

                    std::string json = "{";
                    json += std::format("\"position\":{{\"x\":{:.6f},\"y\":{:.6f},\"pixelX\":{},\"pixelY\":{}}}",
                        normX, normY, pixelX, pixelY);
                    json += ",\"nodes\":[" + serializeNode(root) + "]";
                    json += "}";
                    return { 200, json };
                });
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // Graph editor view (snapshot + pan/zoom)
        // =====================================================================

        // POST /graph/snapshot — body: { "inline": bool? }
        // Captures the live node-graph view at the swap-chain panel size.
        // Always writes PNG to a unique %TEMP% file. When inline=true, also
        // returns base64-encoded bytes in the response.
        m_mcpServer->AddRoute(L"POST", L"/graph/snapshot", [this](const std::wstring&, const std::wstring&, const std::string& body)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                bool wantInline = false;
                if (!body.empty())
                {
                    winrt::Windows::Data::Json::JsonObject jo{ nullptr };
                    if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(body), jo))
                    {
                        if (jo.HasKey(L"inline"))
                        {
                            auto v = jo.GetNamedValue(L"inline");
                            if (v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean)
                                wantInline = v.GetBoolean();
                        }
                    }
                }

                auto pngData = CaptureGraphAsPng();
                if (pngData.empty())
                    return { 500, R"({"error":"Snapshot failed"})" };

                // Unique temp filename to avoid concurrent-capture overwrites.
                static std::atomic<uint32_t> s_seq{ 0 };
                uint32_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
                wchar_t tempPath[MAX_PATH]{};
                GetTempPathW(MAX_PATH, tempPath);
                std::wstring filePath = std::format(L"{}shaderlab_graph_snapshot_{}_{}.png",
                    tempPath, GetCurrentProcessId(), seq);
                HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile == INVALID_HANDLE_VALUE)
                    return { 500, R"({"error":"Failed to create temp file"})" };
                DWORD written = 0;
                WriteFile(hFile, pngData.data(), static_cast<DWORD>(pngData.size()), &written, nullptr);
                CloseHandle(hFile);

                auto pathUtf8 = ToUtf8(filePath);
                std::string escapedPath;
                for (char c : pathUtf8) { if (c == '\\') escapedPath += "\\\\"; else escapedPath += c; }

                if (wantInline)
                {
                    auto b64 = Base64Encode(pngData.data(), pngData.size());
                    return { 200, std::format(
                        "{{\"path\":\"{}\",\"size\":{},\"width\":{},\"height\":{},"
                        "\"mimeType\":\"image/png\",\"base64\":\"{}\"}}",
                        escapedPath, pngData.size(),
                        m_graphPanelWidth, m_graphPanelHeight, b64) };
                }
                return { 200, std::format(
                    "{{\"path\":\"{}\",\"size\":{},\"width\":{},\"height\":{},\"mimeType\":\"image/png\"}}",
                    escapedPath, pngData.size(),
                    m_graphPanelWidth, m_graphPanelHeight) };
            });
        });

        // GET /graph/view — current pan/zoom + viewport + content bounds
        m_mcpServer->AddRoute(L"GET", L"/graph/view", [this](const std::wstring&, const std::wstring&, const std::string&)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                m_nodeGraphController.RebuildLayout();
                auto pan = m_nodeGraphController.PanOffset();
                float zoom = m_nodeGraphController.Zoom();
                auto b = m_nodeGraphController.ContentBounds();
                float cw = (std::max)(0.0f, b.right - b.left);
                float ch = (std::max)(0.0f, b.bottom - b.top);
                return { 200, std::format(
                    "{{\"zoom\":{:.6f},\"panX\":{:.6f},\"panY\":{:.6f}"
                    ",\"viewportW\":{},\"viewportH\":{}"
                    ",\"contentBounds\":{{\"x\":{:.6f},\"y\":{:.6f},\"w\":{:.6f},\"h\":{:.6f}}}"
                    ",\"zoomLimits\":{{\"min\":0.1,\"max\":5.0}}"
                    "}}",
                    zoom, pan.x, pan.y,
                    m_graphPanelWidth, m_graphPanelHeight,
                    b.left, b.top, cw, ch) };
            });
        });

        // POST /graph/view — body: { zoom?, panX?, panY? }
        m_mcpServer->AddRoute(L"POST", L"/graph/view", [this](const std::wstring&, const std::wstring&, const std::string& body)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                winrt::Windows::Data::Json::JsonObject jo{ nullptr };
                if (!winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(body), jo))
                    return { 400, R"({"error":"Invalid JSON body"})" };

                auto pan = m_nodeGraphController.PanOffset();
                float zoom = m_nodeGraphController.Zoom();
                bool changed = false;

                if (jo.HasKey(L"zoom"))
                {
                    auto v = jo.GetNamedValue(L"zoom");
                    if (v.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                        return { 400, R"({"error":"'zoom' must be a number"})" };
                    zoom = static_cast<float>(v.GetNumber());
                    m_nodeGraphController.SetZoom(zoom);
                    changed = true;
                }
                if (jo.HasKey(L"panX"))
                {
                    auto v = jo.GetNamedValue(L"panX");
                    if (v.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                        return { 400, R"({"error":"'panX' must be a number"})" };
                    pan.x = static_cast<float>(v.GetNumber());
                    changed = true;
                }
                if (jo.HasKey(L"panY"))
                {
                    auto v = jo.GetNamedValue(L"panY");
                    if (v.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                        return { 400, R"({"error":"'panY' must be a number"})" };
                    pan.y = static_cast<float>(v.GetNumber());
                    changed = true;
                }
                if (jo.HasKey(L"panX") || jo.HasKey(L"panY"))
                    m_nodeGraphController.SetPanOffset(pan.x, pan.y);

                // Re-read post-clamp.
                auto p2 = m_nodeGraphController.PanOffset();
                float z2 = m_nodeGraphController.Zoom();
                return { 200, std::format(
                    "{{\"ok\":true,\"changed\":{},\"zoom\":{:.6f},\"panX\":{:.6f},\"panY\":{:.6f}}}",
                    changed ? "true" : "false", z2, p2.x, p2.y) };
            });
        });

        // POST /graph/view/fit — body: { padding?:number (DIPs, default 40) }
        m_mcpServer->AddRoute(L"POST", L"/graph/view/fit", [this](const std::wstring&, const std::wstring&, const std::string& body)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                float padding = 40.0f;
                if (!body.empty())
                {
                    winrt::Windows::Data::Json::JsonObject jo{ nullptr };
                    if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(body), jo)
                        && jo.HasKey(L"padding"))
                    {
                        auto v = jo.GetNamedValue(L"padding");
                        if (v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
                            padding = static_cast<float>(v.GetNumber());
                    }
                }
                FitGraphView(padding);
                auto p = m_nodeGraphController.PanOffset();
                float z = m_nodeGraphController.Zoom();
                auto b = m_nodeGraphController.ContentBounds();
                bool empty = !(b.right > b.left && b.bottom > b.top);
                return { 200, std::format(
                    "{{\"ok\":true,\"empty\":{},\"zoom\":{:.6f},\"panX\":{:.6f},\"panY\":{:.6f}}}",
                    empty ? "true" : "false", z, p.x, p.y) };
            });
        });

        // =====================================================================
        // GET /gpu/list  — Enumerate GPU adapters + identify the active one
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/gpu/list", [this](const std::wstring&, const std::wstring&, const std::string&)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                auto adapters = ::ShaderLab::Rendering::RenderEngine::EnumerateAdapters();
                std::string json = "{\"active\":{";
                json += "\"name\":\"" + ToUtf8(m_renderEngine.AdapterName()) + "\"";
                json += ",\"isWarp\":" + std::string(m_renderEngine.IsWarp() ? "true" : "false");
                json += "},\"adapters\":[";
                for (size_t i = 0; i < adapters.size(); ++i)
                {
                    if (i > 0) json += ",";
                    const auto& a = adapters[i];
                    json += "{";
                    json += "\"name\":\"" + ToUtf8(a.name) + "\"";
                    json += ",\"vendorId\":" + std::to_string(a.vendorId);
                    json += ",\"deviceId\":" + std::to_string(a.deviceId);
                    json += ",\"dedicatedVideoMemoryMB\":" + std::to_string(a.dedicatedVideoMemoryMB);
                    json += ",\"isWarp\":" + std::string(a.isWarp ? "true" : "false");
                    json += ",\"luid\":{";
                    json += "\"low\":" + std::to_string(static_cast<uint32_t>(a.luid.LowPart));
                    json += ",\"high\":" + std::to_string(static_cast<int32_t>(a.luid.HighPart));
                    json += "}";
                    json += ",\"isActive\":" + std::string(
                        (a.name == m_renderEngine.AdapterName() ||
                         (a.isWarp && m_renderEngine.IsWarp())) ? "true" : "false");
                    json += "}";
                }
                json += "]}";
                return { 200, json };
            });
        });

        // =====================================================================
        // POST /gpu/switch  — Switch the active GPU adapter
        // Body: one of
        //   {"mode":"warp"}            -> WARP software adapter
        //   {"mode":"default"}         -> let driver pick (typical: integrated)
        //   {"mode":"adapter","name":"NVIDIA GeForce ..."}
        //   {"mode":"adapter","luid":{"low":NUMBER,"high":NUMBER}}
        // Returns the new active adapter (or falls back to default if the
        // requested one fails to initialize). Triggers full
        // SwitchAdapter sequence: graph save -> device teardown -> new
        // device init -> graph reload.
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/gpu/switch", [this](const std::wstring&, const std::wstring&, const std::string& body)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                using namespace ::ShaderLab::Rendering;
                namespace WDJ = winrt::Windows::Data::Json;
                WDJ::JsonObject jobj{ nullptr };
                if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jobj))
                    return { 400, R"({"error":"Invalid JSON body"})" };

                std::wstring mode = jobj.HasKey(L"mode")
                    ? std::wstring(jobj.GetNamedString(L"mode")) : L"default";

                LUID luid{};
                DevicePreference pref = DevicePreference::Default;

                if (mode == L"warp")
                {
                    pref = DevicePreference::Warp;
                }
                else if (mode == L"default")
                {
                    pref = DevicePreference::Default;
                }
                else if (mode == L"adapter")
                {
                    pref = DevicePreference::Adapter;
                    if (jobj.HasKey(L"luid"))
                    {
                        auto luidObj = jobj.GetNamedObject(L"luid");
                        luid.LowPart  = static_cast<DWORD>(luidObj.GetNamedNumber(L"low"));
                        luid.HighPart = static_cast<LONG>(luidObj.GetNamedNumber(L"high"));
                    }
                    else if (jobj.HasKey(L"name"))
                    {
                        auto wantedName = std::wstring(jobj.GetNamedString(L"name"));
                        auto adapters = RenderEngine::EnumerateAdapters();
                        bool found = false;
                        for (const auto& a : adapters)
                        {
                            // Case-insensitive substring match so the
                            // caller doesnt have to know exact GPU naming.
                            auto lower = a.name;
                            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                            auto wantedLower = wantedName;
                            std::transform(wantedLower.begin(), wantedLower.end(), wantedLower.begin(), ::towlower);
                            if (lower.find(wantedLower) != std::wstring::npos)
                            { luid = a.luid; found = true; break; }
                        }
                        if (!found)
                            return { 404, R"({"error":"No adapter matches the supplied name"})" };
                    }
                    else
                    {
                        return { 400, R"({"error":"adapter mode requires luid or name"})" };
                    }
                }
                else
                {
                    return { 400, R"({"error":"mode must be 'warp', 'default', or 'adapter'"})" };
                }

                SwitchAdapter(pref, luid);

                std::string json = "{\"ok\":true";
                json += ",\"active\":{";
                json += "\"name\":\"" + ToUtf8(m_renderEngine.AdapterName()) + "\"";
                json += ",\"isWarp\":" + std::string(m_renderEngine.IsWarp() ? "true" : "false");
                json += "}}";
                return { 200, json };
            });
        });

        // =====================================================================
        // /display/profiles, /display/profile, /display/profile/clear
        // -- moved to Engine/Mcp/EngineMcpRoutes.cpp (Phase 7).
        //    Uses Rendering::UpdateWorkingSpaceNodes engine helper and
        //    fires OnDisplayProfileChanged so the GUI keeps the status
        //    bar / profile selector / dirty state in sync.
        // =====================================================================

        // =====================================================================
        // POST /render/capture-node -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7). Uses Rendering::CaptureNodeAsPng so the GUI host and
        // headless host share the same render-and-encode pipeline.
        // =====================================================================

        // =====================================================================
        // POST /render/image-stats -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Engine-pure GPU reduction over a node's output.
        // =====================================================================

        // =====================================================================
        // POST /render/pixel-region — moved to Engine/Mcp/EngineMcpRoutes.cpp
        // (Phase 7 migration). Uses the existing Rendering::ReadPixelRegion
        // helper through IEngineCommandSink::Dispatch -- same UI-thread
        // serialization the MainWindow shim provided, but the route body
        // now lives engine-side and is registered for the headless host
        // too.
        // =====================================================================

        // =====================================================================
        // GET /preview/view  — Current preview pan/zoom + image bounds
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/preview/view", [this](const std::wstring&, const std::wstring&, const std::string&)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                auto bounds = GetPreviewImageBounds();
                float imgW = (std::max)(0.0f, bounds.right - bounds.left);
                float imgH = (std::max)(0.0f, bounds.bottom - bounds.top);
                return { 200, std::format(
                    R"({{"zoom":{:.6f},"panX":{:.6f},"panY":{:.6f})"
                    R"(,"previewNodeId":{},"imageBounds":{{"x":{:.4f},"y":{:.4f},"w":{:.4f},"h":{:.4f}}})"
                    R"(,"zoomLimits":{{"min":0.01,"max":100.0}}}})",
                    m_previewZoom, m_previewPanX, m_previewPanY,
                    m_previewNodeId, bounds.left, bounds.top, imgW, imgH) };
            });
        });

        // =====================================================================
        // POST /preview/view  — Set preview pan/zoom (any subset).
        // Body: { zoom?, panX?, panY? }   zoom clamped to [0.01, 100.0]
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/preview/view", [this](const std::wstring&, const std::wstring&, const std::string& body)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                namespace WDJ = winrt::Windows::Data::Json;
                WDJ::JsonObject jo{ nullptr };
                if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                    return { 400, R"({"error":"Invalid JSON body"})" };

                bool changed = false;
                if (jo.HasKey(L"zoom"))
                {
                    auto v = jo.GetNamedValue(L"zoom");
                    if (v.ValueType() != WDJ::JsonValueType::Number)
                        return { 400, R"({"error":"'zoom' must be a number"})" };
                    float z = static_cast<float>(v.GetNumber());
                    z = (std::clamp)(z, 0.01f, 100.0f);
                    m_previewZoom = z;
                    changed = true;
                }
                if (jo.HasKey(L"panX"))
                {
                    auto v = jo.GetNamedValue(L"panX");
                    if (v.ValueType() != WDJ::JsonValueType::Number)
                        return { 400, R"({"error":"'panX' must be a number"})" };
                    m_previewPanX = static_cast<float>(v.GetNumber());
                    changed = true;
                }
                if (jo.HasKey(L"panY"))
                {
                    auto v = jo.GetNamedValue(L"panY");
                    if (v.ValueType() != WDJ::JsonValueType::Number)
                        return { 400, R"({"error":"'panY' must be a number"})" };
                    m_previewPanY = static_cast<float>(v.GetNumber());
                    changed = true;
                }
                if (changed) m_forceRender = true;

                return { 200, std::format(
                    R"({{"ok":true,"changed":{},"zoom":{:.6f},"panX":{:.6f},"panY":{:.6f}}})",
                    changed ? "true" : "false",
                    m_previewZoom, m_previewPanX, m_previewPanY) };
            });
        });

        // =====================================================================
        // POST /preview/view/fit  — Fit preview image to viewport.
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/preview/view/fit", [this](const std::wstring&, const std::wstring&, const std::string&)
            -> ::ShaderLab::Mcp::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::Mcp::Response {
                FitPreviewToView();
                m_forceRender = true;
                return { 200, std::format(
                    R"({{"ok":true,"zoom":{:.6f},"panX":{:.6f},"panY":{:.6f}}})",
                    m_previewZoom, m_previewPanX, m_previewPanY) };
            });
        });

        // =====================================================================
        // GET /effect/hlsl/{nodeId} -- moved to Engine/Mcp/EngineMcpRoutes.cpp
        // =====================================================================
        // =====================================================================
        // POST / (JSON-RPC dispatcher) + GET / (health) -- moved to
        // Engine/Mcp/McpJsonRpc.cpp (stdio-migration Step 3). The GUI
        // registers the shared endpoint via RegisterJsonRpcEndpoint in
        // SetupMcpRoutes above; the tool catalog lives in
        // Engine/Mcp/McpToolCatalog.cpp.
        // =====================================================================
    }
}
