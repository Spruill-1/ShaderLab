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
        // Each route here used to live in MainWindow.McpRoutes.cpp.
        // Migration pattern:
        //   1. Copy route body into a Register* free function here.
        //   2. Wrap in sink.Dispatch where the route mutates engine state
        //      (anything touching m_graph, properties, etc).
        //   3. Replace MainWindow.McpRoutes.cpp body with a "moved to
        //      EngineMcpRoutes" comment marker.
        //   4. Build + verify.
        //
        // UI-coupled routes stay in MainWindow.McpRoutes.cpp:
        // graph_snapshot, preview/graph view tools, render/preview-node.

        // ---- GET /registry — D2D + ShaderLab effect catalog (static) -------
        void RegisterRegistry(McpHttpServer& server)
        {
            server.AddRoute(L"GET", L"/registry",
                [](const std::wstring& path, const std::string&) -> McpHttpServer::Response {
                    auto& reg = ::ShaderLab::Effects::EffectRegistry::Instance();

                    // /registry/effect/<name> — detailed effect info.
                    if (path.starts_with(L"/registry/effect/"))
                    {
                        auto name = path.substr(17);
                        auto* desc = reg.FindByName(name);
                        if (!desc) return Json(404, R"({"error":"Effect not found"})");

                        WDJ::JsonObject o;
                        o.Insert(L"name", WDJ::JsonValue::CreateStringValue(desc->name));
                        o.Insert(L"category", WDJ::JsonValue::CreateStringValue(desc->category));
                        o.Insert(L"inputCount", WDJ::JsonValue::CreateNumberValue(
                            static_cast<double>(desc->inputPins.size())));
                        WDJ::JsonObject props;
                        for (const auto& [key, meta] : desc->propertyMetadata)
                        {
                            WDJ::JsonObject m;
                            m.Insert(L"min", WDJ::JsonValue::CreateNumberValue(meta.minValue));
                            m.Insert(L"max", WDJ::JsonValue::CreateNumberValue(meta.maxValue));
                            m.Insert(L"step", WDJ::JsonValue::CreateNumberValue(meta.step));
                            if (!meta.enumLabels.empty())
                            {
                                WDJ::JsonArray labels;
                                for (const auto& lab : meta.enumLabels)
                                    labels.Append(WDJ::JsonValue::CreateStringValue(lab));
                                m.Insert(L"enumLabels", labels);
                            }
                            props.Insert(key, m);
                        }
                        o.Insert(L"properties", props);
                        return Json(200, WideToUtf8(o.Stringify()));
                    }

                    // /registry — list all effects.
                    WDJ::JsonArray arr;
                    for (const auto& d : reg.All())
                    {
                        WDJ::JsonObject o;
                        o.Insert(L"name", WDJ::JsonValue::CreateStringValue(d.name));
                        o.Insert(L"category", WDJ::JsonValue::CreateStringValue(d.category));
                        o.Insert(L"inputCount", WDJ::JsonValue::CreateNumberValue(
                            static_cast<double>(d.inputPins.size())));
                        arr.Append(o);
                    }
                    return Json(200, WideToUtf8(arr.Stringify()));
                });
        }

        // ---- POST /graph/set-property — mutates m_graph -------------------
        void RegisterSetProperty(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/set-property",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body](EngineContext& ctx) -> McpHttpServer::Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto key = std::wstring(jobj.GetNamedString(L"key"));
                            auto val = jobj.GetNamedValue(L"value");

                            auto* node = ctx.graph->FindNode(nodeId);
                            if (!node) return Json(404, R"({"error":"Node not found"})");

                            switch (val.ValueType())
                            {
                            case WDJ::JsonValueType::Number:
                            {
                                bool isUint = false;
                                if (node->customEffect.has_value())
                                {
                                    for (const auto& p : node->customEffect->parameters)
                                    {
                                        if (p.name == key && p.typeName == L"uint")
                                        { isUint = true; break; }
                                    }
                                }
                                auto existIt = node->properties.find(key);
                                if (existIt != node->properties.end() &&
                                    std::holds_alternative<uint32_t>(existIt->second))
                                    isUint = true;

                                if (isUint)
                                    node->properties[key] = static_cast<uint32_t>(val.GetNumber());
                                else
                                    node->properties[key] = static_cast<float>(val.GetNumber());
                                break;
                            }
                            case WDJ::JsonValueType::Boolean:
                                node->properties[key] = val.GetBoolean();
                                break;
                            case WDJ::JsonValueType::String:
                                node->properties[key] = std::wstring(val.GetString());
                                break;
                            case WDJ::JsonValueType::Array:
                            {
                                auto arr = val.GetArray();
                                if (arr.Size() == 2)
                                    node->properties[key] = winrt::Windows::Foundation::Numerics::float2{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()) };
                                else if (arr.Size() == 3)
                                    node->properties[key] = winrt::Windows::Foundation::Numerics::float3{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()),
                                        static_cast<float>(arr.GetAt(2).GetNumber()) };
                                else if (arr.Size() == 4)
                                    node->properties[key] = winrt::Windows::Foundation::Numerics::float4{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()),
                                        static_cast<float>(arr.GetAt(2).GetNumber()),
                                        static_cast<float>(arr.GetAt(3).GetNumber()) };
                                break;
                            }
                            default:
                                return Json(400, R"({"error":"Unsupported value type"})");
                            }
                            node->dirty = true;
                            ctx.graph->MarkAllDirty();

                            // Special-cased properties that mirror to dedicated node fields.
                            if (key == L"isPlaying")
                            {
                                if (auto* bv = std::get_if<bool>(&node->properties[key]))
                                    node->isPlaying = *bv;
                            }
                            if (key == L"shaderPath")
                            {
                                if (auto* sv = std::get_if<std::wstring>(&node->properties[key]))
                                    node->shaderPath = *sv;
                            }

                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /render/image-stats — GPU reduction over a node ----------
        void RegisterImageStats(McpHttpServer& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/render/image-stats",
                [&sink](const std::wstring&, const std::string& body) -> McpHttpServer::Response
                {
                    return sink.Dispatch([&body](EngineContext& ctx) -> McpHttpServer::Response {
                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");
                        if (!jo.HasKey(L"nodeId"))
                            return Json(400, R"({"error":"Missing required field: nodeId"})");

                        uint32_t nodeId = static_cast<uint32_t>(jo.GetNamedNumber(L"nodeId"));
                        bool nonzeroOnly = jo.HasKey(L"nonzeroOnly") && jo.GetNamedBoolean(L"nonzeroOnly");

                        // Parse channel list (default: all five). The engine
                        // takes uint32_t channel codes: 0=luminance, 1=r, 2=g,
                        // 3=b, 4=a.
                        std::vector<uint32_t>    chans;
                        std::vector<std::string> chanNames;
                        auto addChan = [&](const std::string& name, uint32_t code) {
                            chans.push_back(code);
                            chanNames.push_back(name);
                        };
                        if (jo.HasKey(L"channels"))
                        {
                            auto arr = jo.GetNamedArray(L"channels");
                            for (uint32_t i = 0; i < arr.Size(); ++i)
                            {
                                auto n = WideToUtf8(arr.GetStringAt(i));
                                if      (n == "luminance" || n == "y") addChan("luminance", 0);
                                else if (n == "r")                     addChan("r", 1);
                                else if (n == "g")                     addChan("g", 2);
                                else if (n == "b")                     addChan("b", 3);
                                else if (n == "a")                     addChan("a", 4);
                                else
                                    return Json(400, "{\"error\":\"Unknown channel: " + n + "\"}");
                            }
                        }
                        if (chans.empty())
                        {
                            addChan("luminance", 0);
                            addChan("r", 1);
                            addChan("g", 2);
                            addChan("b", 3);
                            addChan("a", 4);
                        }

                        // Force a fresh frame, then resolve the node's image.
                        if (ctx.renderFrame) ctx.renderFrame();
                        auto* node = ctx.graph->FindNode(nodeId);
                        if (!node)
                            return Json(404, "{\"error\":\"Node " + std::to_string(nodeId) + " not found\"}");
                        if (!node->cachedOutput || node->dirty)
                            return Json(409, "{\"error\":\"Node " + std::to_string(nodeId)
                                + " not yet evaluated\",\"notReady\":true}");

                        // GraphEvaluator's DC type is ID2D1DeviceContext5; ours is
                        // ID2D1DeviceContext. Cast through Q/I-style wrapper.
                        auto* dc5 = static_cast<ID2D1DeviceContext5*>(ctx.dc);
                        auto stats = ctx.evaluator->ComputeStandaloneStats(
                            dc5, node->cachedOutput, chans, nonzeroOnly);
                        if (stats.size() != chans.size())
                            return Json(500, R"({"error":"GPU reduction failed"})");

                        // Build response (matches pre-migration JSON shape exactly).
                        std::string json = "{\"nodeId\":" + std::to_string(nodeId)
                            + ",\"nonzeroOnly\":" + (nonzeroOnly ? "true" : "false")
                            + ",\"channels\":[";
                        char buf[512];
                        for (size_t i = 0; i < stats.size(); ++i)
                        {
                            if (i) json += ",";
                            const auto& s = stats[i];
                            float nzPct = (s.totalPixels > 0)
                                ? static_cast<float>(s.nonzeroPixels) / static_cast<float>(s.totalPixels) : 0.0f;
                            int n = std::snprintf(buf, sizeof(buf),
                                "{\"channel\":\"%s\",\"min\":%.6f,\"max\":%.6f,\"mean\":%.6f"
                                ",\"median\":%.6f,\"p95\":%.6f,\"sum\":%.6f"
                                ",\"samples\":%u,\"totalPixels\":%u,\"nonzeroPixels\":%u,\"nonzeroFraction\":%.6f}",
                                chanNames[i].c_str(), s.min, s.max, s.mean, s.median, s.p95, s.sum,
                                static_cast<unsigned>(s.samples),
                                static_cast<unsigned>(s.totalPixels),
                                static_cast<unsigned>(s.nonzeroPixels),
                                nzPct);
                            json.append(buf, n);
                        }
                        json += "]}";
                        return Json(200, json);
                    });
                });
        }

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
        RegisterRegistry(server);
        RegisterSetProperty(server, sink);
        RegisterImageStats(server, sink);
        RegisterPixelRegion(server, sink);
    }
}
