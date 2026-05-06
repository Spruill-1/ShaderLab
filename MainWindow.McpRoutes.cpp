#include "pch.h"
#include "MainWindow.xaml.h"
#include "Engine/Mcp/McpHttpServer.h"
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

// Helper: escape a string for embedding into a JSON string literal.
// Escapes backslash, quote, and control characters per RFC 8259.
// Use everywhere we splice user/host strings (HLSL source, paths, error
// messages, profile names) into a JSON response body.
static std::string JsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s)
    {
        unsigned char uc = static_cast<unsigned char>(ch);
        switch (ch)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (uc < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", uc);
                    out += buf;
                }
                else
                {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

static std::string JsonEscape(const std::wstring& ws)
{
    return JsonEscape(ToUtf8(ws));
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
        DispatcherQueue().TryEnqueue([state, fnPtr]()
        {
            try { state->result = (*fnPtr)(); }
            catch (...) { state->ex = std::current_exception(); }
            SetEvent(state->event);
        });

        // 30s timeout -- generous for stats/readback paths but still a
        // backstop against a wedged UI thread.
        DWORD wait = WaitForSingleObject(state->event, 30000);
        if (wait != WAIT_OBJECT_0)
            throw std::runtime_error("DispatchSync: UI thread did not respond within 30s");
        if (state->ex) std::rethrow_exception(state->ex);
        if (!state->result.has_value())
            throw std::runtime_error("DispatchSync: lambda completed without producing a result");
        return std::move(*state->result);
    }

    ::ShaderLab::McpHttpServer::Response MainWindow::GuiEngineCommandSink::Dispatch(
        std::function<::ShaderLab::McpHttpServer::Response(
            ::ShaderLab::Mcp::EngineContext&)> closure)
    {
        // Marshal the engine work to the UI thread so the closure
        // doesn't race with the render tick mutating the same state.
        // (Headless host's sink runs the closure synchronously on the
        // listener thread; for that host the McpHttpServer's listener
        // thread is the only consumer of the engine state.)
        return window->DispatchSync([this, &closure]() -> ::ShaderLab::McpHttpServer::Response {
            ::ShaderLab::Mcp::EngineContext ctx{};
            ctx.graph = &window->m_graph;
            ctx.evaluator = &window->m_graphEvaluator;
            ctx.displayMonitor = &window->m_displayMonitor;
            ctx.sourceFactory = &window->m_sourceFactory;
            ctx.dc = window->m_renderEngine.D2DDeviceContext();
            ctx.d3dDevice = window->m_renderEngine.D3DDevice();
            ctx.d3dContext = window->m_renderEngine.D3DContext();
            ctx.renderFrame = [this]() { window->RenderFrame(); };
            ctx.getPreviewNodeId = [this]() -> uint32_t { return window->m_previewNodeId; };
            return closure(ctx);
        });
    }

    // Event hooks. All of these run on the UI thread (the engine route's
    // Dispatch closure already marshaled there). The implementations call
    // the same UI methods MainWindow uses on native user interactions, so
    // MCP-driven mutations take the same UI code path the user does.

    void MainWindow::GuiEngineCommandSink::OnNodeAdded(uint32_t /*nodeId*/)
    {
        window->m_graph.MarkAllDirty();
        window->m_nodeGraphController.AutoLayout();
        window->PopulatePreviewNodeSelector();
        window->m_forceRender = true;
    }

    void MainWindow::GuiEngineCommandSink::OnNodeRemoved(uint32_t nodeId)
    {
        window->m_graphEvaluator.InvalidateNode(nodeId);
        window->CloseOutputWindow(nodeId);
        window->m_graph.MarkAllDirty();
        window->m_nodeGraphController.AutoLayout();
        window->PopulatePreviewNodeSelector();
        window->m_forceRender = true;
    }

    void MainWindow::GuiEngineCommandSink::OnNodeChanged(uint32_t /*nodeId*/)
    {
        window->m_forceRender = true;
    }

    void MainWindow::GuiEngineCommandSink::OnGraphCleared()
    {
        window->m_graphEvaluator.ReleaseCache();
        window->m_outputWindows.clear();
        window->m_previewNodeId = 0;
        window->m_graph.MarkAllDirty();
        window->m_nodeGraphController.AutoLayout();
        window->PopulatePreviewNodeSelector();
        window->m_forceRender = true;
    }

    void MainWindow::GuiEngineCommandSink::OnGraphLoaded()
    {
        // ResetAfterGraphLoad rebuilds the per-load UI state (heartbeats,
        // output windows, preview selector). Same path the file-open dialog
        // takes.
        window->ResetAfterGraphLoad(/*reopenOutputWindows=*/true);
        window->m_nodeGraphController.AutoLayout();
        window->m_forceRender = true;
    }

    void MainWindow::GuiEngineCommandSink::OnGraphStructureChanged()
    {
        // Edges or property bindings changed -- rebuild the canvas layout
        // so any new connections render correctly. AutoLayout is the same
        // call user-driven connect/disconnect uses.
        window->m_nodeGraphController.AutoLayout();
        window->m_forceRender = true;
    }

    void MainWindow::SetupMcpRoutes()
    {
        if (!m_mcpServer)
            m_mcpServer = std::make_unique<::ShaderLab::McpHttpServer>();
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
        // GET /  — Health check / probe (some MCP clients GET / before POST).
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/", [](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            // Only match exact "/" — longer GET paths fall through to other routes.
            if (path != L"/")
                return { 404, R"({"error":"Not found"})" };
            return { 200, R"({"name":"shaderlab","transport":"streamable-http","endpoint":"POST /"})" };
        });

        // =====================================================================
        // GET /context  — System prompt / onboarding for calling agents
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/context", [](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
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
"outputNote": "PNG captures are tone-mapped SDR. Use /render/pixel/X/Y for true scRGB float values. Values above 1.0 are HDR.",
"endpoints": {
    "GET /context": "This document",
    "GET /graph": "Full graph state with nodes, edges, properties, custom effects",
    "GET /graph/node/{id}": "Single node detail including custom effect definition",
    "GET /registry/effects": "All built-in D2D effects",
    "GET /custom-effects": "All custom effects in graph with HLSL source",
    "GET /render/capture": "Output as base64 PNG, SDR tone-mapped",
    "GET /render/pixel/{x}/{y}": "scRGB float4 plus luminance at coordinates",
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
        m_mcpServer->AddRoute(L"POST", L"/render/preview-node", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
        m_mcpServer->AddRoute(L"POST", L"/effect/compile", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                auto hlsl = std::wstring(jobj.GetNamedString(L"hlsl"));

                // Compilation itself is thread-safe. Do it here.
                std::string hlslUtf8 = ToUtf8(hlsl);
                for (auto& ch : hlslUtf8) { if (ch == '\r') ch = '\n'; }

                // Need to check node type before compiling.
                auto* checkNode = m_graph.FindNode(nodeId);
                if (!checkNode || !checkNode->customEffect.has_value())
                    return { 404, R"({"error":"Custom effect node not found"})" };

                std::string target = (checkNode->customEffect->shaderType == ::ShaderLab::Graph::CustomShaderType::PixelShader)
                    ? "ps_5_0" : "cs_5_0";
                auto result = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
                    hlslUtf8, "McpCompile", "main", target);

                if (!result.succeeded)
                {
                    auto errMsg = ToUtf8(result.ErrorMessage());
                    std::string escaped;
                    for (char c : errMsg) { if (c == '"') escaped += "\\\""; else if (c == '\n') escaped += "\\n"; else escaped += c; }
                    return { 200, std::format("{{\"compiled\":false,\"error\":\"{}\"}}", escaped) };
                }

                auto* blob = result.bytecode.get();
                std::vector<uint8_t> bytecode(blob->GetBufferSize());
                memcpy(bytecode.data(), blob->GetBufferPointer(), blob->GetBufferSize());

                // Apply to node on UI thread.
                // Also update analysis fields if provided.
                std::vector<::ShaderLab::Graph::AnalysisFieldDescriptor> newFields;
                bool hasAnalysisFields = jobj.HasKey(L"analysisFields");
                if (hasAnalysisFields)
                {
                    auto fieldsArr = jobj.GetNamedArray(L"analysisFields");
                    for (uint32_t fi = 0; fi < fieldsArr.Size(); ++fi)
                    {
                        auto fobj = fieldsArr.GetObjectAt(fi);
                        ::ShaderLab::Graph::AnalysisFieldDescriptor fd;
                        fd.name = std::wstring(fobj.GetNamedString(L"name"));
                        auto typeTag = std::wstring(fobj.GetNamedString(L"type"));
                        if (typeTag == L"float")         fd.type = ::ShaderLab::Graph::AnalysisFieldType::Float;
                        else if (typeTag == L"float2")    fd.type = ::ShaderLab::Graph::AnalysisFieldType::Float2;
                        else if (typeTag == L"float3")    fd.type = ::ShaderLab::Graph::AnalysisFieldType::Float3;
                        else if (typeTag == L"float4")    fd.type = ::ShaderLab::Graph::AnalysisFieldType::Float4;
                        else if (typeTag == L"floatarray")  fd.type = ::ShaderLab::Graph::AnalysisFieldType::FloatArray;
                        else if (typeTag == L"float2array") fd.type = ::ShaderLab::Graph::AnalysisFieldType::Float2Array;
                        else if (typeTag == L"float3array") fd.type = ::ShaderLab::Graph::AnalysisFieldType::Float3Array;
                        else if (typeTag == L"float4array") fd.type = ::ShaderLab::Graph::AnalysisFieldType::Float4Array;
                        if (fobj.HasKey(L"length"))
                            fd.arrayLength = static_cast<uint32_t>(fobj.GetNamedNumber(L"length"));
                        newFields.push_back(std::move(fd));
                    }
                }

                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    auto* node = m_graph.FindNode(nodeId);
                    if (!node || !node->customEffect.has_value())
                        return { 404, R"({"error":"Node not found"})" };
                    auto& def = node->customEffect.value();
                    def.hlslSource = hlsl;
                    def.compiledBytecode = std::move(bytecode);
                    // Always generate a new GUID on recompile so D2D registers
                    // the updated shader under a fresh CLSID.
                    CoCreateGuid(&def.shaderGuid);

                    // Update analysis fields if provided.
                    if (hasAnalysisFields)
                    {
                        def.analysisFields = std::move(newFields);
                        def.analysisOutputType = def.analysisFields.empty()
                            ? ::ShaderLab::Graph::AnalysisOutputType::None
                            : ::ShaderLab::Graph::AnalysisOutputType::Typed;
                    }

                    node->dirty = true;
                    m_graph.MarkAllDirty();
                    m_graphEvaluator.UpdateNodeShader(nodeId, *node);
                    EnforceCustomEffectNameUniqueness(nodeId);
                    m_nodeGraphController.RebuildLayout();
                    PopulateAddNodeFlyout();
                    return { 200, std::format("{{\"compiled\":true,\"bytecodeSize\":{}}}", def.compiledBytecode.size()) };
                });
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // GET /render/pixel/{x}/{y}
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/render/pixel/", [this](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            // Parse /render/pixel/{x}/{y}
            auto rest = path.substr(14); // after "/render/pixel/"
            auto slash = rest.find(L'/');
            if (slash == std::wstring::npos)
                return { 400, R"({"error":"Format: /render/pixel/{x}/{y}"})" };

            float x = std::stof(rest.substr(0, slash));
            float y = std::stof(rest.substr(slash + 1));

            auto* dc = m_renderEngine.D2DDeviceContext();
            if (!dc) return { 500, R"({"error":"No device context"})" };

            auto* image = ResolveDisplayImage(m_previewNodeId);
            if (!image) return { 404, R"({"error":"No output image"})" };

            // Read pixel value using a 1x1 bitmap copy.
            D2D1_POINT_2U srcPoint = { static_cast<UINT32>(x), static_cast<UINT32>(y) };
            D2D1_BITMAP_PROPERTIES1 bmpProps = {};
            bmpProps.pixelFormat = { DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED };
            bmpProps.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

            winrt::com_ptr<ID2D1Bitmap1> readBitmap;
            HRESULT hr = dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, bmpProps, readBitmap.put());
            if (FAILED(hr)) return { 500, R"({"error":"Failed to create read bitmap"})" };

            D2D1_RECT_U srcRect = { srcPoint.x, srcPoint.y, srcPoint.x + 1, srcPoint.y + 1 };
            // Need to render the image to a target first, then copy.
            // For simplicity, report from the cached pixel inspector logic.
            // TODO: implement proper pixel readback

            return { 200, std::format("{{\"x\":{:.0f},\"y\":{:.0f},\"note\":\"Pixel readback via MCP coming soon\"}}", x, y) };
        });

        // =====================================================================
        // =====================================================================
        // GET /render/capture -- Save output PNG to temp file, return path
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/render/capture", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                // Force a full re-evaluation so the capture reflects current state.
                m_graph.MarkAllDirty();
                RenderFrame();
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

                auto pathUtf8 = ToUtf8(filePath);
                // Escape backslashes for JSON.
                std::string escaped;
                for (char c : pathUtf8) { if (c == '\\') escaped += "\\\\"; else escaped += c; }

                return { 200, std::format("{{\"path\":\"{}\",\"size\":{}}}", escaped, pngData.size()) };
            });
        });

        // =====================================================================
        // GET /perf — Return per-frame performance timings
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/perf", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            auto& t = m_lastFrameTiming;
            if (t.framesSampled == 0)
                t = m_frameTiming;  // fallback to live if no snapshot yet
            double fps = (t.totalUs > 0) ? 1000000.0 / t.totalUs : 0;
            return { 200, std::format(
                "{{\"fps\":{:.1f},\"totalMs\":{:.2f},"
                "\"sourcesPrepMs\":{:.2f},\"evaluateMs\":{:.2f},"
                "\"deferredComputeMs\":{:.2f},\"drawMs\":{:.2f},"
                "\"presentMs\":{:.2f},\"computeDispatches\":{},"
                "\"framesSampled\":{}}}",
                fps, t.totalUs / 1000.0,
                t.sourcesPrepUs / 1000.0, t.evaluateUs / 1000.0,
                t.deferredComputeUs / 1000.0, t.drawUs / 1000.0,
                t.presentUs / 1000.0, t.computeDispatches,
                t.framesSampled) };
        });

        // =====================================================================
        // GET /node/{id}/logs — Return per-node log entries
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/node/", [this](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
                // Check for ?since= parameter.
                uint64_t sinceSeq = 0;
                auto qPos = path.find(L"since=");
                if (qPos != std::wstring::npos)
                {
                    try { sinceSeq = std::stoull(path.substr(qPos + 6)); } catch (...) {}
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

                    // JSON-escape the message.
                    std::string msg = ToUtf8(entry.message);
                    std::string escaped;
                    for (char c : msg) {
                        if (c == '"') escaped += "\\\"";
                        else if (c == '\\') escaped += "\\\\";
                        else if (c == '\n') escaped += "\\n";
                        else if (c == '\r') escaped += "\\r";
                        else if (c == '\t') escaped += "\\t";
                        else if (static_cast<unsigned char>(c) < 0x20)
                            escaped += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                        else escaped += c;
                    }

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
        m_mcpServer->AddRoute(L"POST", L"/render/pixel-trace", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                float normX = static_cast<float>(jobj.GetNamedNumber(L"x"));
                float normY = static_cast<float>(jobj.GetNamedNumber(L"y"));

                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    auto* dc = m_renderEngine.D2DDeviceContext();
                    if (!dc) return { 500, R"({"error":"No device context"})" };

                    auto bounds = GetPreviewImageBounds();
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
        m_mcpServer->AddRoute(L"POST", L"/graph/snapshot", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
        m_mcpServer->AddRoute(L"GET", L"/graph/view", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
        m_mcpServer->AddRoute(L"POST", L"/graph/view", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
        m_mcpServer->AddRoute(L"POST", L"/graph/view/fit", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
        // GET /display/profiles  — All built-in presets + the active profile
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/display/profiles", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                using namespace ::ShaderLab::Rendering;

                auto serializeProfile = [](const DisplayProfile& p) {
                    return std::format(
                        "{{\"name\":\"{}\",\"hdrEnabled\":{},\"bitsPerColor\":{}"
                        ",\"sdrWhiteNits\":{:.2f},\"peakNits\":{:.2f}"
                        ",\"minNits\":{:.4f},\"maxFullFrameNits\":{:.2f}"
                        ",\"gamut\":\"{}\",\"isSimulated\":{}"
                        ",\"primaryRed\":[{:.4f},{:.4f}]"
                        ",\"primaryGreen\":[{:.4f},{:.4f}]"
                        ",\"primaryBlue\":[{:.4f},{:.4f}]"
                        ",\"whitePoint\":[{:.4f},{:.4f}]}}",
                        JsonEscape(p.profileName),
                        p.caps.hdrEnabled ? "true" : "false",
                        p.caps.bitsPerColor,
                        p.caps.sdrWhiteLevelNits, p.caps.maxLuminanceNits,
                        p.caps.minLuminanceNits, p.caps.maxFullFrameLuminanceNits,
                        JsonEscape(GamutIdToString(p.gamut)),
                        p.isSimulated ? "true" : "false",
                        p.primaryRed.x, p.primaryRed.y,
                        p.primaryGreen.x, p.primaryGreen.y,
                        p.primaryBlue.x, p.primaryBlue.y,
                        p.whitePoint.x, p.whitePoint.y);
                };

                std::string json = "{\"presets\":[";
                for (size_t i = 0; i < m_displayPresets.size(); ++i)
                {
                    if (i) json += ",";
                    json += "{\"index\":" + std::to_string(i) + ",\"profile\":";
                    json += serializeProfile(m_displayPresets[i]);
                    json += "}";
                }
                json += "],\"active\":" + serializeProfile(m_displayMonitor.ActiveProfile());
                json += ",\"live\":" + serializeProfile(m_displayMonitor.LiveProfile());
                json += ",\"isSimulated\":";
                json += (m_displayMonitor.IsSimulated() ? "true" : "false");
                if (m_loadedIccProfile.has_value())
                {
                    json += ",\"loadedIcc\":" + serializeProfile(m_loadedIccProfile.value());
                }
                json += "}";
                return { 200, json };
            });
        });

        // =====================================================================
        // POST /display/profile  — Apply a simulated profile
        // Body: exactly one of:
        //   {"preset":"PresetP3_1000"}
        //   {"presetIndex":3}
        //   {"iccPath":"C:\\path\\to\\file.icc"}
        //   {"custom":{name?, hdrEnabled?, sdrWhiteNits?, peakNits, minNits?,
        //              maxFullFrameNits?, primaryRed[2], primaryGreen[2],
        //              primaryBlue[2], whitePoint[2], gamut?}}
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/display/profile", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                using namespace ::ShaderLab::Rendering;
                namespace WDJ = winrt::Windows::Data::Json;

                WDJ::JsonObject jo{ nullptr };
                if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                    return { 400, R"({"error":"Invalid JSON body"})" };

                // Mutex check: exactly one source.
                int count = 0;
                if (jo.HasKey(L"preset"))      ++count;
                if (jo.HasKey(L"presetIndex")) ++count;
                if (jo.HasKey(L"iccPath"))     ++count;
                if (jo.HasKey(L"custom"))      ++count;
                if (count != 1)
                    return { 400, R"({"error":"Specify exactly one of: preset, presetIndex, iccPath, custom"})" };

                std::optional<DisplayProfile> chosen;

                // ---- preset by name (factory function name) ----
                if (jo.HasKey(L"preset"))
                {
                    auto name = std::wstring(jo.GetNamedString(L"preset"));
                    DisplayProfile p{};
                    if      (name == L"PresetSrgbSdr"     || name == L"sRGB SDR (80 nits)")              p = PresetSrgbSdr();
                    else if (name == L"PresetSrgb270"     || name == L"sRGB SDR (270 nits, typical laptop)") p = PresetSrgb270();
                    else if (name == L"PresetAdobeRGB"    || name == L"Adobe RGB (1998)")                p = PresetAdobeRGB();
                    else if (name == L"PresetP3_600"      || name == L"DCI-P3 HDR (600 nits, MacBook Pro-class)") p = PresetP3_600();
                    else if (name == L"PresetP3_1000"     || name == L"DCI-P3 HDR (1000 nits, reference monitor)") p = PresetP3_1000();
                    else if (name == L"PresetBT2020_1000" || name == L"BT.2020 HDR (1000 nits, HDR TV)")  p = PresetBT2020_1000();
                    else if (name == L"PresetBT2020_4000" || name == L"BT.2020 HDR (4000 nits, mastering)") p = PresetBT2020_4000();
                    else
                        return { 400, std::format(R"({{"error":"Unknown preset: {}"}})", JsonEscape(name)) };
                    chosen = p;
                }
                else if (jo.HasKey(L"presetIndex"))
                {
                    auto idx = static_cast<size_t>(jo.GetNamedNumber(L"presetIndex"));
                    if (idx >= m_displayPresets.size())
                        return { 400, std::format("{{\"error\":\"presetIndex out of range (0-{})\"}}",
                            m_displayPresets.size() - 1) };
                    chosen = m_displayPresets[idx];
                }
                else if (jo.HasKey(L"iccPath"))
                {
                    auto path = std::wstring(jo.GetNamedString(L"iccPath"));
                    if (!std::filesystem::exists(path))
                        return { 400, std::format(R"({{"error":"ICC file not found: {}"}})", JsonEscape(path)) };
                    auto parsed = IccProfileParser::LoadFromFile(path);
                    if (!parsed.has_value() || !parsed->valid)
                        return { 400, std::format(R"({{"error":"Failed to parse ICC profile: {}"}})", JsonEscape(path)) };
                    chosen = DisplayProfileFromIcc(parsed.value());
                    m_loadedIccProfile = chosen;
                }
                else // custom
                {
                    auto co = jo.GetNamedObject(L"custom");
                    DisplayProfile p{};
                    p.isSimulated = true;

                    if (co.HasKey(L"name"))
                        p.profileName = std::wstring(co.GetNamedString(L"name"));
                    else
                        p.profileName = L"Custom MCP profile";

                    p.caps.hdrEnabled = co.HasKey(L"hdrEnabled") && co.GetNamedBoolean(L"hdrEnabled");
                    p.caps.colorSpace = p.caps.hdrEnabled
                        ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
                    p.caps.bitsPerColor = p.caps.hdrEnabled ? 10 : 8;
                    p.caps.sdrWhiteLevelNits     = co.HasKey(L"sdrWhiteNits")     ? static_cast<float>(co.GetNamedNumber(L"sdrWhiteNits"))     : (p.caps.hdrEnabled ? 203.0f : 80.0f);
                    if (!co.HasKey(L"peakNits"))
                        return { 400, R"({"error":"custom profile requires 'peakNits'"})" };
                    p.caps.maxLuminanceNits      = static_cast<float>(co.GetNamedNumber(L"peakNits"));
                    p.caps.minLuminanceNits      = co.HasKey(L"minNits")          ? static_cast<float>(co.GetNamedNumber(L"minNits"))          : 0.5f;
                    p.caps.maxFullFrameLuminanceNits = co.HasKey(L"maxFullFrameNits") ? static_cast<float>(co.GetNamedNumber(L"maxFullFrameNits")) : p.caps.maxLuminanceNits;

                    auto readChroma = [&](const wchar_t* key, ChromaticityXY& dst) -> bool {
                        if (!co.HasKey(key)) return true; // default already set
                        auto arr = co.GetNamedArray(key);
                        if (arr.Size() != 2) return false;
                        dst.x = static_cast<float>(arr.GetNumberAt(0));
                        dst.y = static_cast<float>(arr.GetNumberAt(1));
                        return true;
                    };
                    if (!readChroma(L"primaryRed",   p.primaryRed)   ||
                        !readChroma(L"primaryGreen", p.primaryGreen) ||
                        !readChroma(L"primaryBlue",  p.primaryBlue)  ||
                        !readChroma(L"whitePoint",   p.whitePoint))
                        return { 400, R"({"error":"primaries / whitePoint must be 2-element arrays"})" };

                    p.gamut = GamutId::Custom;
                    if (co.HasKey(L"gamut"))
                    {
                        auto gn = std::wstring(co.GetNamedString(L"gamut"));
                        if      (gn == L"sRGB")    p.gamut = GamutId::sRGB;
                        else if (gn == L"DCI-P3"  || gn == L"P3" || gn == L"DCI_P3")   p.gamut = GamutId::DCI_P3;
                        else if (gn == L"BT.2020" || gn == L"BT2020" || gn == L"Rec2020") p.gamut = GamutId::BT2020;
                        else                       p.gamut = GamutId::Custom;
                    }
                    chosen = p;
                }

                ApplyDisplayProfile(chosen.value());
                auto active = m_displayMonitor.ActiveProfile();
                return { 200, std::format(
                    R"({{"ok":true,"applied":"{}","hdrEnabled":{},"peakNits":{:.2f}}})",
                    JsonEscape(active.profileName),
                    active.caps.hdrEnabled ? "true" : "false",
                    active.caps.maxLuminanceNits) };
            });
        });

        // =====================================================================
        // POST /display/profile/clear  — Revert to the live OS-reported profile
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/display/profile/clear", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                RevertToLiveDisplay();
                return { 200, R"({"ok":true,"isSimulated":false})" };
            });
        });

        // =====================================================================
        // POST /render/capture-node  — Capture any node's output as PNG.
        // Body: { nodeId, inline?:bool }
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/render/capture-node", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                namespace WDJ = winrt::Windows::Data::Json;
                WDJ::JsonObject jo{ nullptr };
                if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                    return { 400, R"({"error":"Invalid JSON body"})" };
                if (!jo.HasKey(L"nodeId"))
                    return { 400, R"({"error":"'nodeId' is required"})" };
                uint32_t nodeId = static_cast<uint32_t>(jo.GetNamedNumber(L"nodeId"));
                bool wantInline = jo.HasKey(L"inline")
                    && jo.GetNamedValue(L"inline").ValueType() == WDJ::JsonValueType::Boolean
                    && jo.GetNamedBoolean(L"inline");

                bool notFound = false, notReady = false;
                auto pngData = CaptureNodeAsPng(nodeId, notFound, notReady);
                if (notFound)
                    return { 404, std::format(R"({{"error":"Node {} not found"}})", nodeId) };
                if (notReady)
                    return { 409, std::format(R"({{"error":"Node {} is not yet evaluated","notReady":true}})", nodeId) };
                if (pngData.empty())
                    return { 500, R"({"error":"Capture failed"})" };

                static std::atomic<uint32_t> s_seq{ 0 };
                uint32_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
                wchar_t tempPath[MAX_PATH]{};
                GetTempPathW(MAX_PATH, tempPath);
                std::wstring filePath = std::format(L"{}shaderlab_node_{}_{}_{}.png",
                    tempPath, GetCurrentProcessId(), nodeId, seq);
                HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile == INVALID_HANDLE_VALUE)
                    return { 500, R"({"error":"Failed to create temp file"})" };
                DWORD written = 0;
                WriteFile(hFile, pngData.data(), static_cast<DWORD>(pngData.size()), &written, nullptr);
                CloseHandle(hFile);

                std::string escapedPath = JsonEscape(ToUtf8(filePath));
                if (wantInline)
                {
                    auto b64 = Base64Encode(pngData.data(), pngData.size());
                    return { 200, std::format(
                        R"({{"path":"{}","size":{},"nodeId":{},"mimeType":"image/png","base64":"{}"}})",
                        escapedPath, pngData.size(), nodeId, b64) };
                }
                return { 200, std::format(
                    R"({{"path":"{}","size":{},"nodeId":{},"mimeType":"image/png"}})",
                    escapedPath, pngData.size(), nodeId) };
            });
        });

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
        m_mcpServer->AddRoute(L"GET", L"/preview/view", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
        m_mcpServer->AddRoute(L"POST", L"/preview/view", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
        m_mcpServer->AddRoute(L"POST", L"/preview/view/fit", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
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
        // POST /  — MCP JSON-RPC 2.0 endpoint (Streamable HTTP transport)
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                {
                    std::string preview = body.size() > 512 ? body.substr(0, 512) + "..." : body;
                    OutputDebugStringA(("[MCP] POST / body: " + preview + "\n").c_str());
                }
                winrt::Windows::Data::Json::JsonObject jobj{ nullptr };
                if (!winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(body), jobj))
                {
                    OutputDebugStringA(("[MCP] POST / parse error. bodySize=" + std::to_string(body.size())
                        + " raw=[" + body + "]\n").c_str());
                    return { 200, R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}})" };
                }
                if (!jobj.HasKey(L"method"))
                {
                    OutputDebugStringA("[MCP] POST / missing method field\n");
                    return { 200, R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Invalid Request"}})" };
                }
                auto method = ToUtf8(std::wstring(jobj.GetNamedString(L"method")));
                OutputDebugStringA(("[MCP] method=" + method + "\n").c_str());
                auto id = jobj.HasKey(L"id") ? jobj.GetNamedValue(L"id") : winrt::Windows::Data::Json::JsonValue::CreateNullValue();
                std::string idStr;
                if (id.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
                    idStr = std::format("{}", static_cast<int64_t>(id.GetNumber()));
                else if (id.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                    idStr = "\"" + ToUtf8(std::wstring(id.GetString())) + "\"";
                else
                    idStr = "null";

                auto wrapResult = [&](const std::string& result) -> std::string {
                    return std::format(R"JSON({{"jsonrpc":"2.0","id":{},"result":{}}})JSON", idStr, result);
                };

                // ---- initialize ----
                if (method == "initialize")
                {
                    auto verStr = ToUtf8(std::wstring(::ShaderLab::VersionString));
                    std::string result = R"JSON({
"protocolVersion": "2024-11-05",
"capabilities": {
    "tools": {},
    "resources": {}
},
"serverInfo": {
    "name": "shaderlab",
    "version": ")JSON" + verStr + R"JSON("
}
})JSON";
                    return { 200, wrapResult(result) };
                }

                // ---- notifications/initialized (no response needed but we ack) ----
                if (method == "notifications/initialized")
                {
                    return { 202, "" };
                }
                // Any other notification (no id, method starts with "notifications/")
                if (method.rfind("notifications/", 0) == 0)
                {
                    return { 202, "" };
                }

                // ---- tools/list ----
                if (method == "tools/list")
                {
                    std::string tools = R"JSON({"tools":[
{"name":"graph_add_node","description":"Add a node. Use effectName for built-in/ShaderLab effects. For sources use effectName='Video' or 'Image' with optional filePath.","inputSchema":{"type":"object","properties":{"effectName":{"type":"string","description":"Effect name, or 'Video'/'Image' for source nodes"},"filePath":{"type":"string","description":"File path for Video/Image source nodes (optional)"}},"required":["effectName"]}},
{"name":"graph_remove_node","description":"Remove a node by ID","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"graph_connect","description":"Connect output pin to input pin","inputSchema":{"type":"object","properties":{"srcId":{"type":"number"},"srcPin":{"type":"number"},"dstId":{"type":"number"},"dstPin":{"type":"number"}},"required":["srcId","srcPin","dstId","dstPin"]}},
{"name":"graph_disconnect","description":"Disconnect an edge","inputSchema":{"type":"object","properties":{"srcId":{"type":"number"},"srcPin":{"type":"number"},"dstId":{"type":"number"},"dstPin":{"type":"number"}},"required":["srcId","srcPin","dstId","dstPin"]}},
{"name":"graph_set_property","description":"Set a node property. Value can be number, bool, string, or array for vectors.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"key":{"type":"string"},"value":{}},"required":["nodeId","key","value"]}},
{"name":"graph_get_node","description":"Get detailed info about a node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"graph_save_json","description":"Serialize graph to JSON","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_load_json","description":"Load graph from JSON string","inputSchema":{"type":"object","properties":{"json":{"type":"string"}},"required":["json"]}},
{"name":"graph_clear","description":"Clear the graph","inputSchema":{"type":"object","properties":{}}},
{"name":"effect_compile","description":"Compile HLSL for a custom effect node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"hlsl":{"type":"string"}},"required":["nodeId","hlsl"]}},
{"name":"set_preview_node","description":"Set which node is previewed","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"render_capture","description":"Capture preview as PNG. Note: HDR values clipped to SDR.","inputSchema":{"type":"object","properties":{}}},
{"name":"perf_timings","description":"Get per-frame performance timings (ms) for render pipeline phases","inputSchema":{"type":"object","properties":{}}},
{"name":"node_logs","description":"Get per-node log entries (timestamped info/warning/error). Use sinceSeq for incremental reads.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"sinceSeq":{"type":"number","description":"Only return entries after this sequence number"}},"required":["nodeId"]}},
{"name":"registry_get_effect","description":"Get metadata for a built-in effect","inputSchema":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}},
{"name":"graph_bind_property","description":"Bind a node property to an upstream analysis output field","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"propertyName":{"type":"string"},"sourceNodeId":{"type":"number"},"sourceFieldName":{"type":"string"},"sourceComponent":{"type":"number","description":"0-3 for .xyzw component (scalar dest only)"}},"required":["nodeId","propertyName","sourceNodeId","sourceFieldName"]}},
{"name":"graph_unbind_property","description":"Remove a property binding","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"propertyName":{"type":"string"}},"required":["nodeId","propertyName"]}},
{"name":"read_analysis_output","description":"Read typed analysis output fields from a compute/analysis node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}},
{"name":"read_pixel_trace","description":"Run pixel trace at normalized coordinates, returns per-node pixel values and analysis outputs","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"x":{"type":"number","description":"Normalized X (0-1)"},"y":{"type":"number","description":"Normalized Y (0-1)"}},"required":["nodeId","x","y"]}},
{"name":"list_effects","description":"List all available effects (Built-in D2D + ShaderLab) with categories","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_overview","description":"Compact graph summary: nodes (id, name, type, error), edges, preview node","inputSchema":{"type":"object","properties":{}}},
{"name":"get_display_info","description":"Current display capabilities, active profile, pipeline format, app version","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_rename_node","description":"Rename a node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"name":{"type":"string"}},"required":["nodeId","name"]}},
{"name":"graph_snapshot","description":"Capture a PNG snapshot of the live node-graph editor view at the current pan/zoom and panel size. With inline=true returns the image as MCP image content (base64). Without inline, returns the temp file path only.","inputSchema":{"type":"object","properties":{"inline":{"type":"boolean","description":"If true, return the PNG bytes inline as MCP image content"}}}},
{"name":"graph_get_view","description":"Get the node-graph view's current zoom, pan offset, viewport size, and the bounding box of all nodes (in canvas space).","inputSchema":{"type":"object","properties":{}}},
{"name":"graph_set_view","description":"Pan and/or zoom the node-graph editor view. Any subset of {zoom, panX, panY} may be supplied. Changes apply immediately to the live UI. zoom is clamped to [0.1, 5.0]; pan has no clamp. Coordinate convention: screen = zoom * canvas + pan.","inputSchema":{"type":"object","properties":{"zoom":{"type":"number"},"panX":{"type":"number"},"panY":{"type":"number"}}}},
{"name":"graph_fit_view","description":"Fit the node-graph view to show all nodes with the given viewport-space padding (DIPs, default 40). No-op when the graph is empty.","inputSchema":{"type":"object","properties":{"padding":{"type":"number"}}}},
{"name":"list_display_profiles","description":"List all built-in display profile presets and the currently active simulated/live profile. Returns full caps (HDR, peak nits, SDR white) and CIE primaries.","inputSchema":{"type":"object","properties":{}}},
{"name":"set_display_profile","description":"Apply a simulated display profile (overrides OS-reported caps until cleared). Specify exactly ONE of: preset (factory or display name), presetIndex (0-based), iccPath (.icc/.icm file), custom (full chroma + nits spec).","inputSchema":{"type":"object","properties":{"preset":{"type":"string"},"presetIndex":{"type":"number"},"iccPath":{"type":"string"},"custom":{"type":"object","properties":{"name":{"type":"string"},"hdrEnabled":{"type":"boolean"},"sdrWhiteNits":{"type":"number"},"peakNits":{"type":"number"},"minNits":{"type":"number"},"maxFullFrameNits":{"type":"number"},"primaryRed":{"type":"array","items":{"type":"number"}},"primaryGreen":{"type":"array","items":{"type":"number"}},"primaryBlue":{"type":"array","items":{"type":"number"}},"whitePoint":{"type":"array","items":{"type":"number"}},"gamut":{"type":"string"}},"required":["peakNits"]}}}},
{"name":"clear_simulated_profile","description":"Revert to the live OS-reported display profile (clears any simulated/preset/ICC override).","inputSchema":{"type":"object","properties":{}}},
{"name":"render_capture_node","description":"Capture any node's resolved output as PNG (FORCES a render frame so dirty nodes evaluate). With inline=true returns the image as MCP image content (base64). 404 if node missing; 409 with notReady=true if the node is dirty / has unconnected inputs.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"inline":{"type":"boolean"}},"required":["nodeId"]}},
{"name":"preview_get_view","description":"Get the preview pane's current zoom + pan + image bounds + zoom limits.","inputSchema":{"type":"object","properties":{}}},
{"name":"preview_set_view","description":"Set the preview pane's zoom and/or pan. zoom clamped to [0.01, 100.0]. Returns post-clamp values.","inputSchema":{"type":"object","properties":{"zoom":{"type":"number"},"panX":{"type":"number"},"panY":{"type":"number"}}}},
{"name":"preview_fit_view","description":"Fit the preview image to the preview viewport (auto zoom + center).","inputSchema":{"type":"object","properties":{}}},
{"name":"image_stats","description":"GPU-accelerated per-channel image statistics (min/max/mean/median/p95/sum + nonzero counts). Forces a render frame first so the target node is fresh. Channels default to luminance+R+G+B+A; pass channels:[\"luminance\"] to skip the others. nonzeroOnly excludes zero pixels from min/max/mean/sum.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"nonzeroOnly":{"type":"boolean"},"channels":{"type":"array","items":{"type":"string","enum":["luminance","r","g","b","a"]}}},"required":["nodeId"]}},
{"name":"read_pixel_region","description":"Read a small w x h region of FP32 RGBA pixels from a node's output (scRGB linear-light). Region is capped at 32x32 (1024 pixels) and per-axis at 64. Pixels are returned row-major as a flat float array (RGBARGBA...).","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},"w":{"type":"number"},"h":{"type":"number"}},"required":["nodeId","x","y","w","h"]}},
{"name":"effect_get_hlsl","description":"Read a node's custom-effect HLSL source, parameter list, compile state, and last runtime error. For non-custom nodes returns hasCustomEffect=false (200, not 404). For ShaderLab library effects, also includes isLibraryEffect=true + shaderLabEffectId/Version.","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"}},"required":["nodeId"]}}
]})JSON";
                    return { 200, wrapResult(tools) };
                }

                // ---- tools/call ----
                if (method == "tools/call")
                {
                    auto params = jobj.GetNamedObject(L"params");
                    auto toolName = ToUtf8(std::wstring(params.GetNamedString(L"name")));
                    auto args = params.HasKey(L"arguments") ? params.GetNamedObject(L"arguments") : winrt::Windows::Data::Json::JsonObject();
                    auto argsStr = ToUtf8(std::wstring(args.Stringify()));

                    // Route to existing REST handlers.
                    ::ShaderLab::McpHttpServer::Response restResp = { 404, "" };

                    if (toolName == "graph_add_node")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/add-node", argsStr);
                    else if (toolName == "graph_remove_node")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/remove-node", argsStr);
                    else if (toolName == "graph_connect")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/connect", argsStr);
                    else if (toolName == "graph_disconnect")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/disconnect", argsStr);
                    else if (toolName == "graph_set_property")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/set-property", argsStr);
                    else if (toolName == "graph_get_node")
                    {
                        auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                        restResp = m_mcpServer->RouteRequest(L"GET", std::format(L"/graph/node/{}", nodeId), "");
                    }
                    else if (toolName == "graph_save_json")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/graph/save", "");
                    else if (toolName == "graph_load_json")
                    {
                        auto jsonStr = ToUtf8(std::wstring(args.GetNamedString(L"json")));
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/load", jsonStr);
                    }
                    else if (toolName == "graph_clear")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/clear", "");
                    else if (toolName == "effect_compile")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/effect/compile", argsStr);
                    else if (toolName == "set_preview_node")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/preview-node", argsStr);
                    else if (toolName == "render_capture")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/render/capture", "");
                    else if (toolName == "perf_timings")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/perf", "");
                    else if (toolName == "node_logs")
                    {
                        auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                        uint64_t sinceSeq = 0;
                        if (args.HasKey(L"sinceSeq"))
                            sinceSeq = static_cast<uint64_t>(args.GetNamedNumber(L"sinceSeq"));
                        restResp = m_mcpServer->RouteRequest(L"GET",
                            std::format(L"/node/{}/logs?since={}", nodeId, sinceSeq), "");
                    }
                    else if (toolName == "registry_get_effect")
                    {
                        auto name = std::wstring(args.GetNamedString(L"name"));
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/registry/effect/" + name, "");
                    }
                    else if (toolName == "graph_bind_property")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/bind-property", argsStr);
                    else if (toolName == "graph_unbind_property")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/unbind-property", argsStr);
                    else if (toolName == "read_analysis_output")
                    {
                        auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                        restResp = m_mcpServer->RouteRequest(L"GET", std::format(L"/analysis/{}", nodeId), "");
                    }
                    else if (toolName == "read_pixel_trace")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/pixel-trace", argsStr);
                    else if (toolName == "list_effects")
                    {
                        restResp = DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            std::string json = "{\"builtIn\":{";
                            auto& reg = ::ShaderLab::Effects::EffectRegistry::Instance();
                            auto cats = reg.Categories();
                            bool firstCat = true;
                            for (const auto& cat : cats)
                            {
                                if (cat == L"Analysis") continue;
                                if (!firstCat) json += ",";
                                json += "\"" + ToUtf8(cat) + "\":[";
                                auto effects = reg.ByCategory(cat);
                                bool firstFx = true;
                                for (const auto* e : effects)
                                {
                                    if (!firstFx) json += ",";
                                    json += "\"" + ToUtf8(e->name) + "\"";
                                    firstFx = false;
                                }
                                json += "]";
                                firstCat = false;
                            }
                            json += "},\"shaderLab\":{";
                            auto& sl = ::ShaderLab::Effects::ShaderLabEffects::Instance();
                            auto slCats = sl.Categories();
                            firstCat = true;
                            for (const auto& cat : slCats)
                            {
                                if (!firstCat) json += ",";
                                json += "\"" + ToUtf8(cat) + "\":[";
                                auto effects = sl.ByCategory(cat);
                                bool firstFx = true;
                                for (const auto* e : effects)
                                {
                                    if (!firstFx) json += ",";
                                    json += "\"" + ToUtf8(e->name) + "\"";
                                    firstFx = false;
                                }
                                json += "]";
                                firstCat = false;
                            }
                            json += "}}";
                            return { 200, json };
                        });
                    }
                    else if (toolName == "graph_overview")
                    {
                        restResp = DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            std::string json = "{\"previewNodeId\":" + std::to_string(m_previewNodeId) + ",\"nodes\":[";
                            bool first = true;
                            for (const auto& n : m_graph.Nodes())
                            {
                                if (!first) json += ",";
                                std::string typeStr;
                                switch (n.type)
                                {
                                case ::ShaderLab::Graph::NodeType::Source:        typeStr = "Source"; break;
                                case ::ShaderLab::Graph::NodeType::BuiltInEffect: typeStr = "BuiltIn"; break;
                                case ::ShaderLab::Graph::NodeType::PixelShader:   typeStr = "PixelShader"; break;
                                case ::ShaderLab::Graph::NodeType::ComputeShader: typeStr = "ComputeShader"; break;
                                case ::ShaderLab::Graph::NodeType::Output:        typeStr = "Output"; break;
                                }
                                json += std::format("{{\"id\":{},\"name\":\"{}\",\"type\":\"{}\"",
                                    n.id, ToUtf8(n.name), typeStr);
                                if (!n.runtimeError.empty())
                                    json += ",\"error\":\"" + ToUtf8(n.runtimeError) + "\"";
                                json += std::format(",\"inputs\":{},\"outputs\":{}}}", n.inputPins.size(), n.outputPins.size());
                                first = false;
                            }
                            json += "],\"edges\":[";
                            first = true;
                            for (const auto& e : m_graph.Edges())
                            {
                                if (!first) json += ",";
                                json += std::format("[{},{},{},{}]", e.sourceNodeId, e.sourcePin, e.destNodeId, e.destPin);
                                first = false;
                            }
                            json += "]}";
                            return { 200, json };
                        });
                    }
                    else if (toolName == "get_display_info")
                    {
                        restResp = DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            auto profile = m_displayMonitor.ActiveProfile();
                            auto live = m_displayMonitor.LiveProfile();
                            auto caps = m_displayMonitor.CachedCapabilities();
                            auto verStr = ToUtf8(std::wstring(::ShaderLab::VersionString));
                            std::string json = std::format(
                                "{{\"appVersion\":\"{}\",\"graphFormatVersion\":{}"
                                ",\"pipeline\":\"{}\""
                                ",\"display\":{{\"hdr\":{},\"maxNits\":{:.0f},\"sdrWhiteNits\":{:.0f}"
                                ",\"simulated\":{},\"profileName\":\"{}\""
                                ",\"activeGamut\":{{\"red\":[{:.4f},{:.4f}],\"green\":[{:.4f},{:.4f}],\"blue\":[{:.4f},{:.4f}]}}"
                                ",\"monitorGamut\":{{\"red\":[{:.4f},{:.4f}],\"green\":[{:.4f},{:.4f}],\"blue\":[{:.4f},{:.4f}]}}"
                                "}}}}",
                                verStr, ::ShaderLab::GraphFormatVersion,
                                ToUtf8(std::wstring(m_renderEngine.ActiveFormat().name)),
                                caps.hdrEnabled ? "true" : "false",
                                caps.maxLuminanceNits, caps.sdrWhiteLevelNits,
                                profile.isSimulated ? "true" : "false",
                                ToUtf8(profile.profileName),
                                profile.primaryRed.x, profile.primaryRed.y,
                                profile.primaryGreen.x, profile.primaryGreen.y,
                                profile.primaryBlue.x, profile.primaryBlue.y,
                                live.primaryRed.x, live.primaryRed.y,
                                live.primaryGreen.x, live.primaryGreen.y,
                                live.primaryBlue.x, live.primaryBlue.y);
                            return { 200, json };
                        });
                    }
                    else if (toolName == "graph_rename_node")
                    {
                        restResp = DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                            auto newName = std::wstring(args.GetNamedString(L"name"));
                            auto* node = m_graph.FindNode(nodeId);
                            if (!node) return { 404, R"({"error":"Node not found"})" };
                            node->name = newName;
                            m_nodeGraphController.RebuildLayout();
                            PopulatePreviewNodeSelector();
                            PopulateAddNodeFlyout();
                            return { 200, R"({"ok":true})" };
                        });
                    }
                    else if (toolName == "graph_snapshot")
                    {
                        // Forward to REST handler. Re-serialize args to JSON so
                        // the route gets a proper body containing {inline:bool}.
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/snapshot", argsStr);

                        // When the agent requested inline image bytes, repack
                        // the response as MCP-native image content (not text).
                        bool wantInline = false;
                        if (args.HasKey(L"inline"))
                        {
                            auto v = args.GetNamedValue(L"inline");
                            if (v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean)
                                wantInline = v.GetBoolean();
                        }
                        if (wantInline && restResp.statusCode == 200)
                        {
                            // Parse base64 + mimeType out of the REST response
                            // and emit MCP image content directly so we skip
                            // the text-escape wrapping below.
                            winrt::Windows::Data::Json::JsonObject ro{ nullptr };
                            if (winrt::Windows::Data::Json::JsonObject::TryParse(
                                    winrt::to_hstring(restResp.body), ro)
                                && ro.HasKey(L"base64") && ro.HasKey(L"mimeType"))
                            {
                                auto b64 = ToUtf8(std::wstring(ro.GetNamedString(L"base64")));
                                auto mime = ToUtf8(std::wstring(ro.GetNamedString(L"mimeType")));
                                std::string content = std::format(
                                    R"JSON({{"content":[{{"type":"image","data":"{}","mimeType":"{}"}}],"isError":false}})JSON",
                                    b64, mime);
                                return { 200, wrapResult(content) };
                            }
                        }
                    }
                    else if (toolName == "graph_get_view")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/graph/view", "");
                    else if (toolName == "graph_set_view")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/view", argsStr);
                    else if (toolName == "graph_fit_view")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/graph/view/fit", argsStr);
                    else if (toolName == "list_display_profiles")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/display/profiles", "");
                    else if (toolName == "set_display_profile")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/display/profile", argsStr);
                    else if (toolName == "clear_simulated_profile")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/display/profile/clear", "");
                    else if (toolName == "preview_get_view")
                        restResp = m_mcpServer->RouteRequest(L"GET", L"/preview/view", "");
                    else if (toolName == "preview_set_view")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/preview/view", argsStr);
                    else if (toolName == "preview_fit_view")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/preview/view/fit", "");
                    else if (toolName == "image_stats")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/image-stats", argsStr);
                    else if (toolName == "read_pixel_region")
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/pixel-region", argsStr);
                    else if (toolName == "effect_get_hlsl")
                    {
                        auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(L"nodeId"));
                        restResp = m_mcpServer->RouteRequest(L"GET",
                            std::format(L"/effect/hlsl/{}", nodeId), "");
                    }
                    else if (toolName == "render_capture_node")
                    {
                        // Forward to REST handler; if inline=true was requested
                        // and we got a successful PNG back, repack as MCP-native
                        // image content (mirroring the graph_snapshot flow).
                        restResp = m_mcpServer->RouteRequest(L"POST", L"/render/capture-node", argsStr);
                        bool wantInline = args.HasKey(L"inline")
                            && args.GetNamedValue(L"inline").ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean
                            && args.GetNamedBoolean(L"inline");
                        if (wantInline && restResp.statusCode == 200)
                        {
                            winrt::Windows::Data::Json::JsonObject ro{ nullptr };
                            if (winrt::Windows::Data::Json::JsonObject::TryParse(
                                    winrt::to_hstring(restResp.body), ro)
                                && ro.HasKey(L"base64") && ro.HasKey(L"mimeType"))
                            {
                                auto b64 = ToUtf8(std::wstring(ro.GetNamedString(L"base64")));
                                auto mime = ToUtf8(std::wstring(ro.GetNamedString(L"mimeType")));
                                std::string content = std::format(
                                    R"JSON({{"content":[{{"type":"image","data":"{}","mimeType":"{}"}}],"isError":false}})JSON",
                                    b64, mime);
                                return { 200, wrapResult(content) };
                            }
                        }
                    }

                    bool isError = restResp.statusCode >= 400;

                    // MCP requires content[].text to be a STRING, not raw JSON.
                    // Escape the body for embedding in a JSON string value.
                    std::string escaped;
                    for (char c : restResp.body)
                    {
                        if (c == '"') escaped += "\\\"";
                        else if (c == '\\') escaped += "\\\\";
                        else if (c == '\n') escaped += "\\n";
                        else if (c == '\r') escaped += "\\r";
                        else if (c == '\t') escaped += "\\t";
                        else escaped += c;
                    }

                    std::string content = std::format(
                        R"JSON({{"content":[{{"type":"text","text":"{}"}}],"isError":{}}})JSON",
                        escaped.empty() ? "" : escaped,
                        isError ? "true" : "false");

                    return { 200, wrapResult(content) };
                }

                // ---- resources/list ----
                if (method == "resources/list")
                {
                    std::string resources = R"JSON({"resources":[
{"uri":"shaderlab://context","name":"ShaderLab Context","description":"System prompt: pipeline format, shader conventions, API reference","mimeType":"application/json"},
{"uri":"shaderlab://graph","name":"Effect Graph","description":"Full graph state with nodes, edges, properties, custom effect definitions","mimeType":"application/json"},
{"uri":"shaderlab://registry/effects","name":"Built-in Effects","description":"All 48+ built-in D2D effects with property metadata","mimeType":"application/json"},
{"uri":"shaderlab://custom-effects","name":"Custom Effects","description":"Custom effects in graph with HLSL source and compile status","mimeType":"application/json"}
]})JSON";
                    return { 200, wrapResult(resources) };
                }

                // ---- resources/read ----
                if (method == "resources/read")
                {
                    auto params2 = jobj.GetNamedObject(L"params");
                    auto uri = ToUtf8(std::wstring(params2.GetNamedString(L"uri")));

                    std::string restPath;
                    if (uri == "shaderlab://context") restPath = "/context";
                    else if (uri == "shaderlab://graph") restPath = "/graph";
                    else if (uri == "shaderlab://registry/effects") restPath = "/registry/effects";
                    else if (uri == "shaderlab://custom-effects") restPath = "/custom-effects";
                    else
                        return { 200, wrapResult(R"JSON({"contents":[]})JSON") };

                    auto restResp = m_mcpServer->RouteRequest(L"GET", std::wstring(restPath.begin(), restPath.end()), "");

                    // Escape the JSON body for embedding in the text field.
                    std::string escaped;
                    for (char c : restResp.body)
                    {
                        if (c == '"') escaped += "\\\"";
                        else if (c == '\\') escaped += "\\\\";
                        else if (c == '\n') escaped += "\\n";
                        else if (c == '\r') escaped += "\\r";
                        else escaped += c;
                    }

                    std::string result = std::format(
                        R"JSON({{"contents":[{{"uri":"{}","mimeType":"application/json","text":"{}"}}]}})JSON",
                        uri, escaped);
                    return { 200, wrapResult(result) };
                }

                // ---- ping ----
                if (method == "ping")
                    return { 200, wrapResult("{}") };

                // Unknown method.
                return { 200, std::format(
                    R"JSON({{"jsonrpc":"2.0","id":{},"error":{{"code":-32601,"message":"Method not found: {}"}}}})JSON",
                    idStr, method) };
            }
            catch (const std::exception& ex)
            {
                return { 200, std::format(
                    R"JSON({{"jsonrpc":"2.0","id":null,"error":{{"code":-32700,"message":"Parse error: {}"}}}})JSON",
                    ex.what()) };
            }
        });
    }
}
