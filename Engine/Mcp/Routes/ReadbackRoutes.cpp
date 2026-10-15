#include "pch_engine.h"
#include "RouteSupport.h"
#include "../McpRouter.h"
#include "../McpSerialize.h"

#include "../../../Graph/GraphModel.h"
#include "../../../Rendering/GraphEvaluator.h"
#include "../../../Rendering/PixelReadback.h"
#include "../../../Rendering/CaptureNode.h"
#include "../../../Rendering/ImageCapture.h"
#include "../../../Graph/GraphEdits.h"

// Reading a node's output: FP32 pixels, analysis fields, image bounds and
// PNG captures. Each renders a frame that includes the node first, so what
// it reads is current.
namespace ShaderLab::Mcp::Routes
{
    namespace
    {
        constexpr uint32_t cMaxRegionAxis = 64;
        constexpr uint32_t cMaxRegionPixels = 1024;

        // POST /render/pixel-region { nodeId, x, y, w, h }: FP32 RGBA, at
        // most 64 per axis and 1024 pixels, so a thin strip (64x4) fits.
        // Non-finite samples read as null.
        void RegisterPixelRegion(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/render/pixel-region",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON body");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    const auto x = RequireIntArg(args, L"x", argError);
                    const auto y = RequireIntArg(args, L"y", argError);
                    const auto w = RequireUintArg(args, L"w", argError);
                    const auto h = RequireUintArg(args, L"h", argError);
                    if (!argError.empty())
                        return Error(400, argError);
                    if (*w == 0 || *h == 0)
                        return Error(400, "w and h must be > 0");
                    if (*w > cMaxRegionAxis || *h > cMaxRegionAxis || *w * *h > cMaxRegionPixels)
                        return Error(400, "Region too large (cap: each axis <= 64, total area <= 1024)");

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        // Headless renders the whole graph here, which a
                        // probe counting evaluations has to allow for.
                        RenderFrameFor(ctx, *nodeId);

                        const auto region = Rendering::ReadPixelRegion(*ctx.graph, *nodeId, *x, *y, *w, *h, ctx.dc);
                        using S = Rendering::ReadPixelRegionStatus;
                        if (region.status != S::Success && region.status != S::NotFound)
                            if (auto compiling = CompilingResponse(*ctx.graph, *nodeId))
                                return *compiling;
                        switch (region.status)
                        {
                        case S::NotFound:
                            return NodeNotFound(*nodeId);
                        case S::NotReady:
                            return Json(409, BeginErrorBody(409, std::format("Node {} is not yet evaluated", *nodeId))
                                .Key("notReady").Bool(true).EndObject().Take());
                        case S::InvalidRegion:
                            return Error(404, "Region is empty after clipping to image bounds");
                        case S::D2DError:
                            return Error(500, std::format("D2D readback error 0x{:08X}", static_cast<uint32_t>(region.hr)));
                        case S::Success:
                            break;
                        }

                        JsonWriter json;
                        json.BeginObject()
                            .Key("nodeId").Int(*nodeId)
                            .Key("requestedX").Int(*x).Key("requestedY").Int(*y)
                            .Key("requestedW").Int(*w).Key("requestedH").Int(*h)
                            .Key("actualW").Int(region.actualWidth).Key("actualH").Int(region.actualHeight)
                            .Key("channelOrder").BeginArray().String("r").String("g").String("b").String("a").EndArray()
                            .Key("pixels").BeginArray();
                        for (float value : region.pixels)
                            json.Float(value);
                        json.EndArray().EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        // GET /analysis/{id}: the node's analysis output fields. When CPU
        // readbacks are skipped the fields can be frames old, so the node is
        // dirtied and a frame that reads it back is rendered first; a clean
        // node would dispatch, and so copy back, nothing.
        void RegisterAnalysisOutput(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"GET", L"/analysis/",
                [&sink](const std::wstring& path, const std::wstring&, const std::string&) -> Response
                {
                    uint32_t nodeId = 0;
                    if (auto bad = ReadPathNodeId(path, L"/analysis/", nodeId))
                        return *bad;

                    return sink.Dispatch([nodeId](EngineContext& ctx) -> Response
                    {
                        auto* node = ctx.graph->FindNode(nodeId);
                        if (!node)
                            return Error(404, "Node not found");

                        if (ctx.evaluator && ctx.evaluator->Options().skipUnneededCpuReadback && ctx.renderFrame)
                        {
                            Graph::MarkNodeChanged(*node);
                            // Only a D3D11 compute dispatch spends the request.
                            if (node->customEffect.has_value() &&
                                node->customEffect->shaderType == Graph::CustomShaderType::D3D11ComputeShader)
                                ctx.evaluator->RequestFreshCpuReadback(nodeId);
                            RenderFrameFor(ctx, nodeId);
                            // The frame may have changed the graph.
                            node = ctx.graph->FindNode(nodeId);
                            if (!node)
                                return Error(404, "Node not found");
                        }

                        JsonWriter json;
                        json.BeginObject().Key("fields").BeginArray();
                        if (node->analysisOutput.type == Graph::AnalysisOutputType::Typed)
                            for (const auto& field : node->analysisOutput.fields)
                                json.Raw(AnalysisFieldToJson(field));
                        json.EndArray().EndObject();
                        return Json(200, json.Take());
                    });
                });
        }

        // POST /render/image-bounds { nodeId }: the node's GetImageLocalBounds
        // at 96 DPI, unclamped, for diagnosing rect bloat the capture route's
        // maxDim hides.
        void RegisterImageBounds(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/render/image-bounds",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON body");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    if (!nodeId)
                        return Error(400, argError);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        RenderFrameFor(ctx, *nodeId);
                        const auto* node = ctx.graph->FindNode(*nodeId);
                        if (!node || !node->cachedOutput)
                            return Error(404, "Node not ready");
                        D2D1_RECT_F bounds{};
                        if (const HRESULT hr = Rendering::GetImageBounds96Dpi(ctx.dc, node->cachedOutput, bounds); FAILED(hr))
                            return Error(500, std::format("GetImageLocalBounds failed 0x{:08X}", static_cast<uint32_t>(hr)));
                        return Json(200, JsonWriter().BeginObject()
                            .Key("left").Shortest(bounds.left).Key("top").Shortest(bounds.top)
                            .Key("right").Shortest(bounds.right).Key("bottom").Shortest(bounds.bottom)
                            .Key("width").Shortest(bounds.right - bounds.left)
                            .Key("height").Shortest(bounds.bottom - bounds.top)
                            .EndObject().Take());
                    });
                });
        }

        // POST /render/capture-node { nodeId, inline?, maxDim? }: renders the
        // node to a PNG in %TEMP% (longer edge fitted to maxDim, default 2048)
        // and answers its path, plus the base64 bytes when inline is true.
        void RegisterCaptureNode(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/render/capture-node",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON body");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    const bool wantInline = ReadBoolArg(args, L"inline", argError).value_or(false);
                    const double maxDimArg = ReadNumberArg(args, L"maxDim", argError).value_or(2048.0);
                    if (!argError.empty())
                        return Error(400, argError);
                    const auto maxDim = static_cast<uint32_t>(std::clamp(maxDimArg, 32.0, 8192.0));

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        RenderFrameFor(ctx, *nodeId);

                        const auto capture = Rendering::CaptureNodeAsPng(*ctx.graph, *nodeId, ctx.dc, maxDim);
                        using S = Rendering::CaptureNodeStatus;
                        if (capture.status != S::Success && capture.status != S::NotFound)
                            if (auto compiling = CompilingResponse(*ctx.graph, *nodeId))
                                return *compiling;
                        switch (capture.status)
                        {
                        case S::NotFound:
                            return NodeNotFound(*nodeId);
                        case S::NotReady:
                            return Json(409, BeginErrorBody(409, std::format("Node {} is not yet evaluated", *nodeId))
                                .Key("notReady").Bool(true).EndObject().Take());
                        case S::EmptyImage:
                            return Error(500, "Capture failed: empty image");
                        case S::D2DError:
                            return Error(500, std::format("Capture failed 0x{:08X}", static_cast<uint32_t>(capture.hr)));
                        case S::Success:
                            break;
                        }

                        const auto saved = WriteTempPng(std::format(L"shaderlab_node_{}", *nodeId), capture.png);
                        if (saved.path.empty())
                            return Error(500, "Capture could not be saved: " + saved.error);

                        JsonWriter json;
                        json.BeginObject()
                            .Key("path").String(saved.path)
                            .Key("size").Int(capture.png.size())
                            .Key("nodeId").Int(*nodeId)
                            .Key("width").Int(capture.width)
                            .Key("height").Int(capture.height)
                            .Key("mimeType").String("image/png");
                        if (wantInline)
                            json.Key("base64").String(Base64Encode(capture.png));
                        return Json(200, json.EndObject().Take());
                    });
                });
        }
    }

    void RegisterReadbackRoutes(McpRouter& router, IEngineCommandSink& sink)
    {
        RegisterPixelRegion(router, sink);
        RegisterAnalysisOutput(router, sink);
        RegisterImageBounds(router, sink);
        RegisterCaptureNode(router, sink);
    }
}
