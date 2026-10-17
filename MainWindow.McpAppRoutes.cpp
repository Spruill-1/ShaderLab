// MainWindow partial: the GUI's own MCP routes, those that need its UI or
// its render context: the canvas, the preview and the GPU adapter. Each is
// registered as a UI, render or any-thread route (Controls::McpRouteTable).

#include "pch.h"
#include "MainWindow.xaml.h"
#include "Mcp/McpArgs.h"
#include "Mcp/McpJsonWriter.h"
#include "Mcp/McpSerialize.h"
#include "Graph/GraphEdits.h"
#include "Controls/McpRouteTable.h"
#include "Rendering/ImageCapture.h"
#include "Rendering/PixelTrace.h"

namespace winrt::ShaderLab::implementation
{
    namespace
    {
        namespace WDJ = winrt::Windows::Data::Json;
        namespace Mcp = ::ShaderLab::Mcp;
        using Mcp::JsonWriter;
        using Mcp::Response;

        // An optional body; a missing or unparseable one reads as no arguments.
        WDJ::JsonObject OptionalBody(const std::string& body)
        {
            WDJ::JsonObject args{ nullptr };
            Mcp::ParseObjectBody(body, args);
            return args;
        }

        // The {path, size, width, height, mimeType[, base64]} answer for a PNG
        // written to %TEMP%.
        Response PngResponse(const std::wstring& path, const std::vector<uint8_t>& png,
                             uint32_t width, uint32_t height, bool wantInline)
        {
            JsonWriter json;
            json.BeginObject()
                .Key("path").String(path)
                .Key("size").Int(png.size())
                .Key("width").Int(width)
                .Key("height").Int(height)
                .Key("mimeType").String("image/png");
            if (wantInline)
                json.Key("base64").String(Mcp::Base64Encode(png));
            return Mcp::JsonResponse(200, json.EndObject().Take());
        }

        void WriteTraceNode(JsonWriter& json, const ::ShaderLab::Rendering::PixelTraceNode& node)
        {
            const auto& px = node.pixel;
            json.BeginObject()
                .Key("nodeId").Int(node.nodeId)
                .Key("name").String(node.nodeName)
                .Key("pin").String(node.pinName)
                .Key("pixel").BeginObject()
                    .Key("scRGB").BeginArray().Float(px.scR).Float(px.scG).Float(px.scB).Float(px.scA).EndArray()
                    .Key("sRGB").BeginArray().Int(px.sR).Int(px.sG).Int(px.sB).Int(px.sA).EndArray()
                    .Key("luminance").Float(px.luminanceNits)
                .EndObject()
                .Key("analysisFields").BeginArray();
            if (node.hasAnalysisOutput)
                for (const auto& field : node.analysisFields)
                    json.Raw(Mcp::AnalysisFieldToJson(field));
            json.EndArray().Key("inputs").BeginArray();
            for (const auto& input : node.inputs)
                WriteTraceNode(json, input);
            json.EndArray().EndObject();
        }
    }

    // The node-graph canvas: snapshot, view, renaming and node logs.
    void MainWindow::RegisterCanvasMcpRoutes(::ShaderLab::Controls::McpRouteTable& routes)
    {
        // POST /graph/snapshot { inline? }: the canvas as a PNG in %TEMP%.
        routes.AddUiRoute(L"POST", L"/graph/snapshot",
            [this](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                const auto args = OptionalBody(body);
                std::string argError;
                const bool wantInline = Mcp::ReadBoolArg(args, L"inline", argError).value_or(false);
                if (!argError.empty())
                    return Mcp::ErrorResponse(400, argError);

                const auto png = CaptureGraphAsPng();
                if (png.empty())
                    return Mcp::ErrorResponse(500, "Snapshot failed");
                const auto saved = Mcp::WriteTempPng(L"shaderlab_graph_snapshot", png);
                if (saved.path.empty())
                    return Mcp::ErrorResponse(500, "Snapshot could not be saved: " + saved.error);
                return PngResponse(saved.path, png, m_graphPanelWidth, m_graphPanelHeight, wantInline);
            });

        // GET /graph/view: pan, zoom, viewport and content bounds. Layout is
        // rebuilt on the render thread first, so the bounds are current.
        routes.AddUiRoute(L"GET", L"/graph/view",
            [this](const std::wstring&, const std::wstring&, const std::string&) -> Response
            {
                m_nodeGraphController.RequestRelayout();
                const auto pan = m_nodeGraphController.PanOffset();
                const auto bounds = m_nodeGraphController.ContentBounds();
                return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                    .Key("zoom").Fixed(m_nodeGraphController.Zoom(), 6)
                    .Key("panX").Fixed(pan.x, 6)
                    .Key("panY").Fixed(pan.y, 6)
                    .Key("viewportW").Int(m_graphPanelWidth)
                    .Key("viewportH").Int(m_graphPanelHeight)
                    .Key("contentBounds").BeginObject()
                        .Key("x").Fixed(bounds.left, 6)
                        .Key("y").Fixed(bounds.top, 6)
                        .Key("w").Fixed((std::max)(0.0f, bounds.right - bounds.left), 6)
                        .Key("h").Fixed((std::max)(0.0f, bounds.bottom - bounds.top), 6)
                    .EndObject()
                    .Key("zoomLimits").Raw(R"({"min":0.1,"max":5.0})")
                    .EndObject().Take());
            });

        // POST /graph/view { zoom?, panX?, panY? }: answers the view after clamping.
        routes.AddUiRoute(L"POST", L"/graph/view",
            [this](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                WDJ::JsonObject args{ nullptr };
                if (!Mcp::ParseObjectBody(body, args))
                    return Mcp::ErrorResponse(400, "Invalid JSON body");
                std::string argError;
                const auto zoom = Mcp::ReadNumberArg(args, L"zoom", argError);
                const auto panX = Mcp::ReadNumberArg(args, L"panX", argError);
                const auto panY = Mcp::ReadNumberArg(args, L"panY", argError);
                if (!argError.empty())
                    return Mcp::ErrorResponse(400, argError);

                if (zoom)
                    m_nodeGraphController.SetZoom(static_cast<float>(*zoom));
                if (panX || panY)
                {
                    auto pan = m_nodeGraphController.PanOffset();
                    if (panX) pan.x = static_cast<float>(*panX);
                    if (panY) pan.y = static_cast<float>(*panY);
                    m_nodeGraphController.SetPanOffset(pan.x, pan.y);
                }
                const auto pan = m_nodeGraphController.PanOffset();
                return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                    .Key("ok").Bool(true)
                    .Key("changed").Bool(zoom || panX || panY)
                    .Key("zoom").Fixed(m_nodeGraphController.Zoom(), 6)
                    .Key("panX").Fixed(pan.x, 6)
                    .Key("panY").Fixed(pan.y, 6)
                    .EndObject().Take());
            });

        // POST /graph/view/fit { padding? }: fits the content, padding in DIPs (default 40).
        routes.AddUiRoute(L"POST", L"/graph/view/fit",
            [this](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                const auto args = OptionalBody(body);
                std::string argError;
                const double padding = Mcp::ReadNumberArg(args, L"padding", argError).value_or(40.0);
                if (!argError.empty())
                    return Mcp::ErrorResponse(400, argError);
                FitGraphView(static_cast<float>(padding));
                const auto pan = m_nodeGraphController.PanOffset();
                const auto bounds = m_nodeGraphController.ContentBounds();
                return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                    .Key("ok").Bool(true)
                    .Key("empty").Bool(!(bounds.right > bounds.left && bounds.bottom > bounds.top))
                    .Key("zoom").Fixed(m_nodeGraphController.Zoom(), 6)
                    .Key("panX").Fixed(pan.x, 6)
                    .Key("panY").Fixed(pan.y, 6)
                    .EndObject().Take());
            });

        // POST /graph/rename-node { nodeId, name }: renames on the render
        // thread and rebuilds the layout there; the pickers that show names
        // refresh on the UI thread afterwards, without holding the answer.
        routes.AddRenderRoute(L"POST", L"/graph/rename-node",
            [this](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                WDJ::JsonObject args{ nullptr };
                if (!Mcp::ParseObjectBody(body, args))
                    return Mcp::ErrorResponse(400, "Invalid request: need nodeId + name");
                std::string argError;
                const auto nodeId = Mcp::RequireUintArg(args, L"nodeId", argError);
                const auto name = Mcp::RequireStringArg(args, L"name", argError);
                if (!argError.empty())
                    return Mcp::ErrorResponse(400, argError);

                if (!::ShaderLab::Graph::RenameNode(Model(), *nodeId, *name))
                    return Mcp::ErrorResponse(404, "Node not found");
                m_nodeGraphController.RebuildLayout();
                DispatcherQueue().TryEnqueue([this]
                {
                    PopulatePreviewNodeSelector();
                    PopulateAddNodeFlyout();
                });
                return Mcp::JsonResponse(200, R"({"ok":true})");
            });

        // GET /node/{id}/logs?since={seq}: the node's log entries after seq.
        routes.AddUiRoute(L"GET", L"/node/",
            [this](const std::wstring& path, const std::wstring& query, const std::string&) -> Response
            {
                const std::wstring_view rest = std::wstring_view(path).substr(6);   // after "/node/"
                const auto nodeId = Mcp::ParseUintText(rest.substr(0, rest.find(L'/')));
                if (!nodeId)
                    return Mcp::ErrorResponse(400, "Invalid node ID");
                uint64_t sinceSeq = 0;
                if (const auto at = query.find(L"since="); at != std::wstring::npos)
                {
                    const std::wstring_view value = std::wstring_view(query).substr(at + 6);
                    const auto since = ::ShaderLab::Graph::ParseNumber(value.substr(0, value.find(L'&')));
                    if (!since || *since < 0.0)
                        return Mcp::ErrorResponse(400, "since must be a non-negative integer");
                    sinceSeq = static_cast<uint64_t>(*since);
                }

                JsonWriter json;
                json.BeginObject().Key("logs").BeginArray();
                if (auto it = m_nodeLogs.find(*nodeId); it != m_nodeLogs.end())
                {
                    for (const auto& entry : it->second.Entries())
                    {
                        if (entry.sequence <= sinceSeq) continue;
                        const auto time = std::chrono::system_clock::to_time_t(entry.timestamp);
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            entry.timestamp.time_since_epoch()).count() % 1000;
                        tm local{};
                        localtime_s(&local, &time);
                        const char* level = "Info";
                        if (entry.level == ::ShaderLab::Controls::LogLevel::Warning) level = "Warning";
                        else if (entry.level == ::ShaderLab::Controls::LogLevel::Error) level = "Error";
                        json.BeginObject()
                            .Key("seq").Int(entry.sequence)
                            .Key("time").String(std::format("{:02}:{:02}:{:02}.{:03}",
                                local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(ms)))
                            .Key("level").String(level)
                            .Key("message").String(entry.message)
                            .EndObject();
                    }
                }
                json.EndArray().EndObject();
                return Mcp::JsonResponse(200, json.Take());
            });
    }

    // The preview: which node it shows, its view, a capture of it and the
    // pixel trace through it.
    void MainWindow::RegisterPreviewMcpRoutes(::ShaderLab::Controls::McpRouteTable& routes)
    {
        // POST /render/preview-node { nodeId }. Nothing is dirtied: changes
        // made while a node was not needed stay pending in the evaluator.
        routes.AddUiRoute(L"POST", L"/render/preview-node",
            [this](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                WDJ::JsonObject args{ nullptr };
                if (!Mcp::ParseObjectBody(body, args))
                    return Mcp::ErrorResponse(400, "Invalid request");
                std::string argError;
                const auto nodeId = Mcp::RequireUintArg(args, L"nodeId", argError);
                if (!nodeId)
                    return Mcp::ErrorResponse(400, argError);
                SelectPreviewNode(*nodeId);
                m_forceRender = true;
                return Mcp::JsonResponse(200, R"({"ok":true})");
            });

        // GET /render/capture: the preview as a PNG in %TEMP%, after a frame
        // that evaluates whatever is dirty.
        routes.AddRenderRoute(L"GET", L"/render/capture",
            [this](const std::wstring&, const std::wstring&, const std::string&) -> Response
            {
                RenderFrameToOffscreen();
                const auto png = CapturePreviewAsPng();
                if (png.empty())
                    return Mcp::ErrorResponse(404, "No output image");
                const auto saved = Mcp::WriteTempPng(L"shaderlab_capture", png);
                if (saved.path.empty())
                    return Mcp::ErrorResponse(500, "Capture could not be saved: " + saved.error);
                return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                    .Key("path").String(saved.path)
                    .Key("size").Int(png.size())
                    .EndObject().Take());
            });

        // POST /render/pixel-trace { nodeId, x, y }: the pixel at normalized
        // (x, y) of nodeId's image (of the preview's when nodeId's is
        // unbounded), read at nodeId and every node upstream. On the render
        // thread, which owns the graph and the node outputs.
        routes.AddRenderRoute(L"POST", L"/render/pixel-trace",
            [this](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                WDJ::JsonObject args{ nullptr };
                if (!Mcp::ParseObjectBody(body, args))
                    return Mcp::ErrorResponse(400, "Invalid request");
                std::string argError;
                const auto nodeId = Mcp::RequireUintArg(args, L"nodeId", argError);
                const auto x = Mcp::RequireNumberArg(args, L"x", argError);
                const auto y = Mcp::RequireNumberArg(args, L"y", argError);
                if (!argError.empty())
                    return Mcp::ErrorResponse(400, argError);
                const float normX = static_cast<float>(*x);
                const float normY = static_cast<float>(*y);

                auto* dc = m_renderEngine.RenderD2DContext();
                if (!dc)
                    return Mcp::ErrorResponse(500, "No device context");
                if (!Model().FindNode(*nodeId))
                    return Mcp::ErrorResponse(404, "Node not found");
                // Measured on the render context, which owns the images.
                auto boundsOf = [&](uint32_t id) -> std::optional<D2D1_RECT_F>
                {
                    auto* image = ::ShaderLab::Rendering::ResolveNodeImage(Model(), id);
                    D2D1_RECT_F b{};
                    if (!image || FAILED(::ShaderLab::Rendering::GetImageBounds96Dpi(dc, image, b))) return std::nullopt;
                    const float w = b.right - b.left, h = b.bottom - b.top;
                    if (w < 1.0f || h < 1.0f || w > 100000.0f || h > 100000.0f) return std::nullopt;
                    return b;
                };
                auto bounds = boundsOf(*nodeId);
                if (!bounds) bounds = boundsOf(Preview().NodeId());
                if (!bounds)
                    return Mcp::ErrorResponse(404, "No image to trace: the node and the preview have no finite output");
                const auto imageW = static_cast<uint32_t>(bounds->right - bounds->left);
                const auto imageH = static_cast<uint32_t>(bounds->bottom - bounds->top);
                const int32_t pixelX = static_cast<int32_t>(bounds->left) +
                    static_cast<int32_t>((std::min)(normX * imageW, static_cast<float>(imageW - 1)));
                const int32_t pixelY = static_cast<int32_t>(bounds->top) +
                    static_cast<int32_t>((std::min)(normY * imageH, static_cast<float>(imageH - 1)));

                const auto traced = ::ShaderLab::Rendering::TracePixel(Model(), *nodeId, pixelX, pixelY, dc);
                if (!traced.root || FAILED(traced.hr))
                    return Mcp::ErrorResponse(500, std::format("Pixel trace failed 0x{:08X}", static_cast<uint32_t>(traced.hr)));
                const auto& trace = traced.root;

                JsonWriter json;
                json.BeginObject()
                    .Key("position").BeginObject()
                        .Key("x").Fixed(normX, 6)
                        .Key("y").Fixed(normY, 6)
                        .Key("pixelX").Int(pixelX)
                        .Key("pixelY").Int(pixelY)
                    .EndObject()
                    .Key("nodes").BeginArray();
                WriteTraceNode(json, *trace);
                json.EndArray().EndObject();
                return Mcp::JsonResponse(200, json.Take());
            });

        // GET /preview/view: pan, zoom and the preview image bounds.
        routes.AddUiRoute(L"GET", L"/preview/view",
            [this](const std::wstring&, const std::wstring&, const std::string&) -> Response
            {
                const auto bounds = GetPreviewImageBounds();
                const auto view = Preview().View();
                return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                    .Key("zoom").Fixed(view.zoom, 6)
                    .Key("panX").Fixed(view.panX, 6)
                    .Key("panY").Fixed(view.panY, 6)
                    .Key("previewNodeId").Int(Preview().NodeId())
                    .Key("imageBounds").BeginObject()
                        .Key("x").Fixed(bounds.left, 4)
                        .Key("y").Fixed(bounds.top, 4)
                        .Key("w").Fixed((std::max)(0.0f, bounds.right - bounds.left), 4)
                        .Key("h").Fixed((std::max)(0.0f, bounds.bottom - bounds.top), 4)
                    .EndObject()
                    .Key("zoomLimits").Raw(R"({"min":0.01,"max":100.0})")
                    .EndObject().Take());
            });

        // POST /preview/view { zoom?, panX?, panY? }: zoom is clamped to
        // [0.01, 100]. An explicit view ends auto-fit.
        routes.AddUiRoute(L"POST", L"/preview/view",
            [this](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                WDJ::JsonObject args{ nullptr };
                if (!Mcp::ParseObjectBody(body, args))
                    return Mcp::ErrorResponse(400, "Invalid JSON body");
                std::string argError;
                const auto zoom = Mcp::ReadNumberArg(args, L"zoom", argError);
                const auto panX = Mcp::ReadNumberArg(args, L"panX", argError);
                const auto panY = Mcp::ReadNumberArg(args, L"panY", argError);
                if (!argError.empty())
                    return Mcp::ErrorResponse(400, argError);

                auto view = Preview().View();
                if (zoom) view.zoom = (std::clamp)(static_cast<float>(*zoom), 0.01f, 100.0f);
                if (panX) view.panX = static_cast<float>(*panX);
                if (panY) view.panY = static_cast<float>(*panY);
                const bool changed = zoom || panX || panY;
                if (changed)
                {
                    Preview().SetView(view);
                    m_forceRender = true;
                }
                return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                    .Key("ok").Bool(true)
                    .Key("changed").Bool(changed)
                    .Key("zoom").Fixed(view.zoom, 6)
                    .Key("panX").Fixed(view.panX, 6)
                    .Key("panY").Fixed(view.panY, 6)
                    .EndObject().Take());
            });

        // POST /preview/view/fit: fits the image and keeps it fitted, as a
        // double-click does.
        routes.AddUiRoute(L"POST", L"/preview/view/fit",
            [this](const std::wstring&, const std::wstring&, const std::string&) -> Response
            {
                Preview().FitNow(GetPreviewImageBounds());
                m_forceRender = true;
                const auto view = Preview().View();
                return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                    .Key("ok").Bool(true)
                    .Key("zoom").Fixed(view.zoom, 6)
                    .Key("panX").Fixed(view.panX, 6)
                    .Key("panY").Fixed(view.panY, 6)
                    .EndObject().Take());
            });
    }

    // The GPU adapter: listing them and switching.
    void MainWindow::RegisterGpuMcpRoutes(::ShaderLab::Controls::McpRouteTable& routes)
    {
        // GET /gpu/list: every adapter and which one is active.
        routes.AddUiRoute(L"GET", L"/gpu/list",
            [this](const std::wstring&, const std::wstring&, const std::string&) -> Response
            {
                const auto adapters = ::ShaderLab::Rendering::RenderEngine::EnumerateAdapters();
                const std::wstring activeName = m_renderEngine.AdapterName();
                const bool activeIsWarp = m_renderEngine.IsWarp();
                JsonWriter json;
                json.BeginObject()
                    .Key("active").BeginObject().Key("name").String(activeName).Key("isWarp").Bool(activeIsWarp).EndObject()
                    .Key("adapters").BeginArray();
                for (const auto& adapter : adapters)
                {
                    json.BeginObject()
                        .Key("name").String(adapter.name)
                        .Key("vendorId").Int(adapter.vendorId)
                        .Key("deviceId").Int(adapter.deviceId)
                        .Key("dedicatedVideoMemoryMB").Int(adapter.dedicatedVideoMemoryMB)
                        .Key("isWarp").Bool(adapter.isWarp)
                        .Key("luid").BeginObject()
                            .Key("low").Int(static_cast<uint32_t>(adapter.luid.LowPart))
                            .Key("high").Int(static_cast<int32_t>(adapter.luid.HighPart))
                        .EndObject()
                        .Key("isActive").Bool(adapter.name == activeName || (adapter.isWarp && activeIsWarp))
                        .EndObject();
                }
                json.EndArray().EndObject();
                return Mcp::JsonResponse(200, json.Take());
            });

        // POST /gpu/switch { mode: "warp" | "default" | "adapter", name?, luid? }.
        // "adapter" takes a luid {low, high} or a case-insensitive part of
        // the adapter's name. Runs the whole SwitchAdapter sequence (save the
        // graph, rebuild the device stack, reload) and answers the adapter now
        // active, which is the default one if the requested one failed.
        routes.AddUiRoute(L"POST", L"/gpu/switch",
            [this](const std::wstring&, const std::wstring&, const std::string& body) -> Response
            {
                using ::ShaderLab::Rendering::DevicePreference;
                WDJ::JsonObject args{ nullptr };
                if (!Mcp::ParseObjectBody(body, args))
                    return Mcp::ErrorResponse(400, "Invalid JSON body");
                std::string argError;
                const std::wstring mode = Mcp::ReadStringArg(args, L"mode", argError).value_or(L"default");
                const auto luidArg = Mcp::ReadObjectArg(args, L"luid", argError);
                const auto nameArg = Mcp::ReadStringArg(args, L"name", argError);
                if (!argError.empty())
                    return Mcp::ErrorResponse(400, argError);

                LUID luid{};
                DevicePreference preference = DevicePreference::Default;
                if (mode == L"warp")
                {
                    preference = DevicePreference::Warp;
                }
                else if (mode == L"adapter")
                {
                    preference = DevicePreference::Adapter;
                    if (luidArg)
                    {
                        const auto low = Mcp::RequireNumberArg(*luidArg, L"low", argError);
                        const auto high = Mcp::RequireIntArg(*luidArg, L"high", argError);
                        if (!argError.empty())
                            return Mcp::ErrorResponse(400, "luid." + argError);
                        luid.LowPart = static_cast<DWORD>(*low);
                        luid.HighPart = static_cast<LONG>(*high);
                    }
                    else if (nameArg)
                    {
                        auto lower = [](std::wstring text)
                        {
                            std::transform(text.begin(), text.end(), text.begin(), ::towlower);
                            return text;
                        };
                        const std::wstring wanted = lower(*nameArg);
                        bool found = false;
                        for (const auto& adapter : ::ShaderLab::Rendering::RenderEngine::EnumerateAdapters())
                        {
                            if (lower(adapter.name).find(wanted) != std::wstring::npos)
                            {
                                luid = adapter.luid;
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                            return Mcp::ErrorResponse(404, "No adapter matches the supplied name");
                    }
                    else
                    {
                        return Mcp::ErrorResponse(400, "adapter mode requires luid or name");
                    }
                }
                else if (mode != L"default")
                {
                    return Mcp::ErrorResponse(400, "mode must be 'warp', 'default', or 'adapter'");
                }

                SwitchAdapter(preference, luid);

                return Mcp::JsonResponse(200, JsonWriter().BeginObject()
                    .Key("ok").Bool(true)
                    .Key("active").BeginObject()
                        .Key("name").String(m_renderEngine.AdapterName())
                        .Key("isWarp").Bool(m_renderEngine.IsWarp())
                    .EndObject()
                    .EndObject().Take());
            });
    }
}
