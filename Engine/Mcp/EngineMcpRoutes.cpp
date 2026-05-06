#include "pch_engine.h"
#include "EngineMcpRoutes.h"

#include "../../Graph/EffectGraph.h"
#include "../../Rendering/GraphEvaluator.h"
#include "../../Rendering/DisplayMonitor.h"
#include "../../Rendering/PixelReadback.h"
#include "../../Effects/EffectRegistry.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Version.h"

#include <winrt/Windows.Data.Json.h>

namespace ShaderLab::Mcp
{
    namespace WDJ = winrt::Windows::Data::Json;

    namespace
    {
        // ---- Small response helpers --------------------------------------
        McpHttpServer::Response Json(uint16_t status, const std::string& body)
        {
            McpHttpServer::Response r;
            r.statusCode = status;
            r.body = body;
            r.contentType = "application/json";
            return r;
        }

        McpHttpServer::Response Error(uint16_t status, const std::string& msg)
        {
            return Json(status, "{\"error\":\"" + msg + "\"}");
        }

        std::string WideToUtf8(std::wstring_view ws)
        {
            if (ws.empty()) return {};
            int len = ::WideCharToMultiByte(CP_UTF8, 0,
                ws.data(), static_cast<int>(ws.size()),
                nullptr, 0, nullptr, nullptr);
            std::string out(len, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0,
                ws.data(), static_cast<int>(ws.size()),
                out.data(), len, nullptr, nullptr);
            return out;
        }

        // ---- Phase 7 incremental migration ---------------------------------
        // The 30 engine-pure routes (graph mutation, render capture, image
        // stats, pixel region readback, etc) live in MainWindow.McpRoutes
        // today. They get migrated to this TU one batch at a time as Phase 7
        // proceeds. Each migration: extract the route body into a free
        // function here, swap the registration to use sink.Dispatch where
        // the route mutates engine state, delete the original from
        // MainWindow.McpRoutes.cpp, build + verify, commit.
        //
        // Migrated so far: /render/pixel-region (the FP16 readback path
        // MCP-driven full-accuracy sampling depends on; uses the existing
        // Rendering::ReadPixelRegion helper).

        void RegisterPixelRegion(McpHttpServer& server, IEngineCommandSink& sink)
        {
            // POST /render/pixel-region -- Read FP32 RGBA pixel grid.
            // Body: { nodeId, x, y, w, h }   (capped at 32x32 = 1024 pixels)
            server.AddRoute(L"POST", L"/render/pixel-region",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body](EngineContext& ctx) -> McpHttpServer::Response {
                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");
                        for (auto k : { L"nodeId", L"x", L"y", L"w", L"h" })
                            if (!jo.HasKey(k))
                                return Json(400,
                                    "{\"error\":\"Missing required field: " + WideToUtf8(k) + "\"}");

                        uint32_t nodeId = static_cast<uint32_t>(jo.GetNamedNumber(L"nodeId"));
                        int32_t  x = static_cast<int32_t>(jo.GetNamedNumber(L"x"));
                        int32_t  y = static_cast<int32_t>(jo.GetNamedNumber(L"y"));
                        uint32_t w = static_cast<uint32_t>(jo.GetNamedNumber(L"w"));
                        uint32_t h = static_cast<uint32_t>(jo.GetNamedNumber(L"h"));

                        // Cap region area at 32x32 (1024 pixels). Per-axis cap of
                        // 64 lets the agent ask for a thin strip (e.g. 64x4) but
                        // never more than 1024 total samples.
                        if (w == 0 || h == 0)
                            return Json(400, R"({"error":"w and h must be > 0"})");
                        if (w > 64 || h > 64 || (w * h) > 1024)
                            return Json(400,
                                "{\"error\":\"Region too large (cap: each axis <= 64, total area <= 1024)\"}");

                        // Force a fresh frame so dirty nodes evaluate before
                        // readback. Headless host's renderFrame is a no-op
                        // (caller is expected to have evaluated the graph).
                        if (ctx.renderFrame) ctx.renderFrame();

                        auto rr = Rendering::ReadPixelRegion(*ctx.graph, nodeId, x, y, w, h, ctx.dc);
                        switch (rr.status)
                        {
                        case Rendering::ReadPixelRegionStatus::NotFound:
                            return Json(404,
                                "{\"error\":\"Node " + std::to_string(nodeId) + " not found\"}");
                        case Rendering::ReadPixelRegionStatus::NotReady:
                            return Json(409,
                                "{\"error\":\"Node " + std::to_string(nodeId)
                                + " is not yet evaluated\",\"notReady\":true}");
                        case Rendering::ReadPixelRegionStatus::InvalidRegion:
                            return Json(404, R"({"error":"Region is empty after clipping to image bounds"})");
                        case Rendering::ReadPixelRegionStatus::D2DError:
                            return Json(500, R"({"error":"D2D readback error"})");
                        case Rendering::ReadPixelRegionStatus::Success: break;
                        }

                        // Build the JSON response. Float formatting matches the
                        // pre-migration MainWindow body so MCP clients see the
                        // same representation.
                        std::string json = "{\"nodeId\":" + std::to_string(nodeId)
                            + ",\"requestedX\":" + std::to_string(x)
                            + ",\"requestedY\":" + std::to_string(y)
                            + ",\"requestedW\":" + std::to_string(w)
                            + ",\"requestedH\":" + std::to_string(h)
                            + ",\"actualW\":" + std::to_string(rr.actualWidth)
                            + ",\"actualH\":" + std::to_string(rr.actualHeight)
                            + ",\"channelOrder\":[\"r\",\"g\",\"b\",\"a\"],\"pixels\":[";
                        char buf[32];
                        for (size_t i = 0; i < rr.pixels.size(); ++i)
                        {
                            if (i) json += ",";
                            int n = std::snprintf(buf, sizeof(buf), "%.6f", rr.pixels[i]);
                            json.append(buf, n);
                        }
                        json += "]}";
                        return Json(200, json);
                    });
                });
        }
    }

    void RegisterEngineRoutes(McpHttpServer& server, IEngineCommandSink& sink)
    {
        RegisterPixelRegion(server, sink);
    }
}
