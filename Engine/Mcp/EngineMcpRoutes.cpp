#include "pch_engine.h"
#include "EngineMcpRoutes.h"
#include "McpRouter.h"

#include "../../Graph/EffectGraph.h"
#include "../../Rendering/GraphEvaluator.h"
#include "../../Rendering/DisplayMonitor.h"
#include "../../Rendering/PixelReadback.h"
#include "../../Rendering/CaptureNode.h"
#include "../../Rendering/DisplayProfile.h"
#include "../../Rendering/IccProfileParser.h"
#include "../../Rendering/WorkingSpaceSync.h"
#include "../../Rendering/EffectGraphFile.h"
#include "../../Effects/EffectRegistry.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Effects/SourceNodeFactory.h"
#include "../../Effects/ShaderCompiler.h"
#include "../../Effects/ShaderVariants.h"
#include "../../Effects/CustomComputeShaderEffect.h"
#include "../../Effects/CustomPixelShaderEffect.h"
#include "../../Effects/Performance.h"
#include "../../Version.h"

#include <winrt/Windows.Data.Json.h>
#include <filesystem>

namespace ShaderLab::Mcp
{
    namespace WDJ = winrt::Windows::Data::Json;

    namespace
    {
        // ---- Small response helpers --------------------------------------
        // Interpret a stored PropertyValue as a boolean, however it arrived.
        //
        // This exists because `isPlaying` is a RUNTIME field mirrored from a
        // property, and the mirror used to accept only a std::bool -- so a
        // client that sent `true` as a JSON string ("true") or a number (1)
        // set the property, got a 200 back, and the clock silently never
        // started. CLAUDE.md records that untyped MCP arguments arrive as
        // JSON STRINGS from Claude Code, so that was the common case, not the
        // exotic one: the toggle worked from some clients and not others,
        // with no error either way.
        //
        // Returns false only when the value is a type that cannot mean a
        // boolean at all, so the caller can report the failure instead of
        // silently dropping it.
        // The one place that decides what a STRING means as a boolean.
        // Returns false for a word that means neither, so callers can report
        // it instead of quietly picking one. Quietly picking one is how
        // `isPlaying: "banana"` used to read as "pause": a typo silently
        // stopped the clock and the route still answered 200.
        inline bool CoerceStringToBool(const std::wstring& w, bool& out)
        {
            std::wstring t;
            for (wchar_t c : w) t += static_cast<wchar_t>(::towlower(c));
            if (t == L"true"  || t == L"1" || t == L"on"  || t == L"yes") { out = true;  return true; }
            if (t == L"false" || t == L"0" || t == L"off" || t == L"no")  { out = false; return true; }
            return false;
        }

        inline bool CoercePropertyToBool(const ShaderLab::Graph::PropertyValue& v, bool& out)
        {
            if (auto* b = std::get_if<bool>(&v))        { out = *b;            return true; }
            if (auto* f = std::get_if<float>(&v))       { out = (*f != 0.0f);  return true; }
            if (auto* i = std::get_if<int32_t>(&v))     { out = (*i != 0);     return true; }
            if (auto* u = std::get_if<uint32_t>(&v))    { out = (*u != 0u);    return true; }
            if (auto* w = std::get_if<std::wstring>(&v))
                return CoerceStringToBool(*w, out);
            return false;
        }

        Response Json(uint16_t status, const std::string& body)
        {
            Response r;
            r.statusCode = status;
            r.body = body;
            r.contentType = "application/json";
            return r;
        }

        // Force a fresh frame that includes nodeId, for a readback of that node.
        void RenderFrameFor(EngineContext& ctx, uint32_t nodeId)
        {
            if (ctx.renderFrameFor)
                ctx.renderFrameFor(nodeId);
            else if (ctx.renderFrame)
                ctx.renderFrame();
        }

        // Names of the node and every node it depends on (through edges or
        // property bindings) whose shader is still compiling in the background.
        std::vector<std::wstring> CompilingDependencies(const Graph::EffectGraph& graph, uint32_t nodeId)
        {
            std::vector<std::wstring> compiling;
            std::set<uint32_t> visited;
            std::vector<uint32_t> pending{ nodeId };
            while (!pending.empty())
            {
                const uint32_t currentId = pending.back();
                pending.pop_back();
                if (!visited.insert(currentId).second)
                    continue;
                const auto* node = graph.FindNode(currentId);
                if (!node)
                    continue;
                if (node->compilePending)
                    compiling.push_back(node->name);

                for (const auto& edge : graph.Edges())
                {
                    if (edge.destNodeId == currentId)
                        pending.push_back(edge.sourceNodeId);
                }
                for (const auto& [property, binding] : node->propertyBindings)
                {
                    if (binding.wholeArray)
                        pending.push_back(binding.wholeArraySourceNodeId);
                    for (const auto& source : binding.sources)
                    {
                        if (source.has_value())
                            pending.push_back(source->sourceNodeId);
                    }
                }
            }
            return compiling;
        }

        // 409 for a readback that failed because shaders are still compiling,
        // or nullopt when nothing upstream is compiling.
        std::optional<Response> CompilingResponse(const Graph::EffectGraph& graph, uint32_t nodeId)
        {
            const auto compiling = CompilingDependencies(graph, nodeId);
            if (compiling.empty())
                return std::nullopt;

            std::string names;
            for (const auto& name : compiling)
            {
                if (!names.empty()) names += ",";
                names += "\"" + JsonEscape(WideToUtf8(name)) + "\"";
            }
            return Json(409, "{\"error\":\"Node " + std::to_string(nodeId)
                + " is waiting on shader compiles; retry in a few seconds\","
                + "\"notReady\":true,\"compiling\":[" + names + "]}");
        }

        Response Error(uint16_t status, const std::string& msg)
        {
            return Json(status, "{\"error\":\"" + msg + "\"}");
        }

        // WideToUtf8 + JsonEscape now come from McpTypes.h (the single
        // shared implementations — stdio-migration Step 3 unified the
        // previously-divergent escapers). This TU sits inside namespace
        // ShaderLab::Mcp, so unqualified calls resolve to them directly.

        // Base64 (standard alphabet, '=' padding, no line wrapping).
        // Used for /render/capture-node `inline` PNG payloads.
        std::string Base64Encode(const uint8_t* data, size_t len)
        {
            static constexpr char kAlphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            if (len == 0) return out;
            out.reserve(((len + 2) / 3) * 4);
            size_t i = 0;
            while (i + 2 < len)
            {
                uint32_t v = (uint32_t(data[i]) << 16)
                           | (uint32_t(data[i + 1]) << 8)
                           |  uint32_t(data[i + 2]);
                out.push_back(kAlphabet[(v >> 18) & 0x3F]);
                out.push_back(kAlphabet[(v >> 12) & 0x3F]);
                out.push_back(kAlphabet[(v >> 6)  & 0x3F]);
                out.push_back(kAlphabet[ v        & 0x3F]);
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

        // ---- Node serialization helpers ------------------------------------
        // Used by GET /graph, GET /graph/save, GET /graph/node/{id},
        // GET /custom-effects. Mirrors the byte-identical output the
        // MainWindow helpers produced before migration so existing MCP
        // consumers don't see schema drift.

        std::string GuidToString(const GUID& g)
        {
            wchar_t buf[64]{};
            ::StringFromGUID2(g, buf, 64);
            return WideToUtf8(buf);
        }

        std::string PropertyValueToJson(const ::ShaderLab::Graph::PropertyValue& pv)
        {
            return std::visit([](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                    return JsonFloat(v);
                else if constexpr (std::is_same_v<T, int32_t>)
                    return std::format("{}", v);
                else if constexpr (std::is_same_v<T, uint32_t>)
                    return std::format("{}", v);
                else if constexpr (std::is_same_v<T, bool>)
                    return v ? "true" : "false";
                else if constexpr (std::is_same_v<T, std::wstring>)
                    return "\"" + JsonEscape(WideToUtf8(v)) + "\"";
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                    return "[" + JsonFloat(v.x) + "," + JsonFloat(v.y) + "]";
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                    return "[" + JsonFloat(v.x) + "," + JsonFloat(v.y) + "," + JsonFloat(v.z) + "]";
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                    return "[" + JsonFloat(v.x) + "," + JsonFloat(v.y) + "," + JsonFloat(v.z) + "," + JsonFloat(v.w) + "]";
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                    return "\"<matrix>\"";
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                    return "\"<curve>\"";
                else
                    return "null";
            }, pv);
        }

        const char* NodeTypeStr(::ShaderLab::Graph::NodeType t)
        {
            using NT = ::ShaderLab::Graph::NodeType;
            switch (t)
            {
            case NT::Source:        return "Source";
            case NT::BuiltInEffect: return "BuiltInEffect";
            case NT::PixelShader:   return "PixelShader";
            case NT::ComputeShader: return "ComputeShader";
            case NT::Output:        return "Output";
            }
            return "Unknown";
        }

        const char* AnalysisFieldTypeStr(::ShaderLab::Graph::AnalysisFieldType t)
        {
            using AFT = ::ShaderLab::Graph::AnalysisFieldType;
            switch (t)
            {
            case AFT::Float:       return "float";
            case AFT::Float2:      return "float2";
            case AFT::Float3:      return "float3";
            case AFT::Float4:      return "float4";
            case AFT::FloatArray:  return "floatarray";
            case AFT::Float2Array: return "float2array";
            case AFT::Float3Array: return "float3array";
            case AFT::Float4Array: return "float4array";
            }
            return "unknown";
        }

        std::string NodeToJson(const ::ShaderLab::Graph::EffectNode& node)
        {
            std::string json = "{";
            json += std::format("\"id\":{},\"name\":\"{}\",\"type\":\"{}\"",
                node.id, JsonEscape(WideToUtf8(node.name)), NodeTypeStr(node.type));
            json += std::format(",\"position\":[{:.1f},{:.1f}]",
                node.position.x, node.position.y);

            // Properties.
            json += ",\"properties\":{";
            bool first = true;
            for (const auto& [key, val] : node.properties)
            {
                if (!first) json += ",";
                json += "\"" + JsonEscape(WideToUtf8(key)) + "\":" + PropertyValueToJson(val);
                first = false;
            }
            json += "}";

            // Property bindings. Mirrors the shape EffectGraph::ToJson writes so
            // an agent reading a node sees the same structure it would find in a
            // saved .effectgraph. Without this an agent can create a binding via
            // /graph/bind-property and observe its effect, but has no way to read
            // back which properties are already bound — e.g. whether a custom
            // gamut's primaries are wired to a Working Space node.
            if (!node.propertyBindings.empty())
            {
                json += ",\"propertyBindings\":{";
                bool firstBinding = true;
                for (const auto& [propName, binding] : node.propertyBindings)
                {
                    if (!firstBinding) json += ",";
                    json += "\"" + JsonEscape(WideToUtf8(propName)) + "\":{";
                    if (binding.wholeArray)
                    {
                        json += std::format(
                            "\"wholeArray\":true,\"sourceNodeId\":{},\"sourceFieldName\":\"{}\"",
                            binding.wholeArraySourceNodeId,
                            JsonEscape(WideToUtf8(binding.wholeArraySourceFieldName)));
                    }
                    else
                    {
                        json += "\"sources\":[";
                        for (size_t i = 0; i < binding.sources.size(); ++i)
                        {
                            if (i > 0) json += ",";
                            const auto& src = binding.sources[i];
                            if (src.has_value())
                            {
                                json += std::format(
                                    "{{\"nodeId\":{},\"field\":\"{}\",\"index\":{},\"comp\":{}}}",
                                    src->sourceNodeId,
                                    JsonEscape(WideToUtf8(src->sourceFieldName)),
                                    src->sourceIndex,
                                    src->sourceComponent);
                            }
                            else
                            {
                                json += "null";
                            }
                        }
                        json += "]";
                    }
                    json += "}";
                    firstBinding = false;
                }
                json += "}";
            }

            // Pins.
            json += ",\"inputPins\":[";
            for (size_t i = 0; i < node.inputPins.size(); ++i)
            {
                if (i > 0) json += ",";
                json += std::format("{{\"name\":\"{}\",\"index\":{}}}",
                    JsonEscape(WideToUtf8(node.inputPins[i].name)),
                    node.inputPins[i].index);
            }
            json += "],\"outputPins\":[";
            for (size_t i = 0; i < node.outputPins.size(); ++i)
            {
                if (i > 0) json += ",";
                json += std::format("{{\"name\":\"{}\",\"index\":{}}}",
                    JsonEscape(WideToUtf8(node.outputPins[i].name)),
                    node.outputPins[i].index);
            }
            json += "]";

            if (node.effectClsid.has_value())
                json += ",\"effectClsid\":\"" + GuidToString(node.effectClsid.value()) + "\"";
            if (!node.runtimeError.empty())
                json += ",\"runtimeError\":\"" + JsonEscape(WideToUtf8(node.runtimeError)) + "\"";

            // Custom effect definition.
            if (node.customEffect.has_value())
            {
                auto& def = node.customEffect.value();
                using CST = ::ShaderLab::Graph::CustomShaderType;
                const char* shaderTypeStr =
                    def.shaderType == CST::PixelShader ? "PixelShader" :
                    def.shaderType == CST::D3D11ComputeShader ? "D3D11ComputeShader" :
                    "ComputeShader";
                json += ",\"customEffect\":{";
                json += std::format("\"shaderType\":\"{}\",\"compiled\":{},\"bytecodeSize\":{}",
                    shaderTypeStr,
                    def.isCompiled() ? "true" : "false",
                    def.compiledBytecode.size());
                json += ",\"inputNames\":[";
                for (size_t i = 0; i < def.inputNames.size(); ++i)
                {
                    if (i > 0) json += ",";
                    json += "\"" + JsonEscape(WideToUtf8(def.inputNames[i])) + "\"";
                }
                json += "],\"parameters\":[";
                for (size_t i = 0; i < def.parameters.size(); ++i)
                {
                    if (i > 0) json += ",";
                    auto& p = def.parameters[i];
                    json += std::format(
                        "{{\"name\":\"{}\",\"type\":\"{}\",\"min\":{:.4f},\"max\":{:.4f},\"step\":{:.4f}",
                        JsonEscape(WideToUtf8(p.name)),
                        JsonEscape(WideToUtf8(p.typeName)),
                        p.minValue, p.maxValue, p.step);
                    if (!p.enumLabels.empty())
                    {
                        json += ",\"options\":[";
                        for (size_t j = 0; j < p.enumLabels.size(); ++j)
                            json += (j ? ",\"" : "\"") + JsonEscape(WideToUtf8(p.enumLabels[j])) + "\"";
                        json += "]";
                    }
                    if (p.gpuBindable) json += ",\"gpuBindable\":true";
                    if (p.specialize)  json += ",\"specialize\":true";
                    json += "}";
                }
                json += "]";
                // How many option variants the definition prebuilds, and how many are still compiling.
                if (const size_t combinations = ::ShaderLab::Effects::OptionCombinationCount(def))
                    json += std::format(",\"optionVariants\":{},\"variantsCompiling\":{}",
                        combinations, node.variantsCompiling);

                json += ",\"hlslSource\":\"" + JsonEscape(WideToUtf8(def.hlslSource)) + "\"";

                if (!def.analysisFields.empty())
                {
                    json += ",\"analysisFields\":[";
                    for (size_t i = 0; i < def.analysisFields.size(); ++i)
                    {
                        if (i > 0) json += ",";
                        const auto& fd = def.analysisFields[i];
                        json += "{\"name\":\"" + JsonEscape(WideToUtf8(fd.name))
                              + "\",\"type\":\"" + AnalysisFieldTypeStr(fd.type) + "\"";
                        if (::ShaderLab::Graph::AnalysisFieldIsArray(fd.type))
                            json += ",\"length\":" + std::to_string(fd.arrayLength);
                        json += "}";
                    }
                    json += "]";
                }
                json += "}";
            }

            // Analysis output results (runtime data).
            using AOT = ::ShaderLab::Graph::AnalysisOutputType;
            if (node.analysisOutput.type == AOT::Typed && !node.analysisOutput.fields.empty())
            {
                json += ",\"analysisResults\":[";
                bool firstField = true;
                for (const auto& fv : node.analysisOutput.fields)
                {
                    if (!firstField) json += ",";
                    firstField = false;
                    json += "{\"name\":\"" + JsonEscape(WideToUtf8(fv.name)) + "\"";
                    if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                    {
                        uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                        json += ",\"value\":[";
                        for (uint32_t c = 0; c < cc; ++c)
                        {
                            if (c > 0) json += ",";
                            json += JsonFloat(fv.components[c]);
                        }
                        json += "]";
                    }
                    else
                    {
                        json += ",\"value\":[";
                        for (size_t i = 0; i < fv.arrayData.size(); ++i)
                        {
                            if (i > 0) json += ",";
                            json += JsonFloat(fv.arrayData[i]);
                        }
                        json += "]";
                    }
                    json += "}";
                }
                json += "]";
            }
            else if (node.analysisOutput.type == AOT::Histogram &&
                     !node.analysisOutput.data.empty())
            {
                json += std::format(
                    ",\"analysisResults\":{{\"type\":\"histogram\",\"channel\":{},\"bins\":{}}}",
                    node.analysisOutput.channelIndex,
                    node.analysisOutput.data.size());
            }

            json += "}";
            return json;
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
        void RegisterRegistry(McpRouter& server)
        {
            server.AddRoute(L"GET", L"/registry",
                [](const std::wstring& path, const std::wstring&, const std::string&) -> Response {
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

        // ---- GET /effects — all effects grouped by category (static) ------
        // Promoted from the GUI's inline `list_effects` tools/call handler
        // (stdio-migration Step 1) so the headless host serves it too. Both
        // catalogs are immutable after startup, so no Dispatch is needed —
        // same reasoning as /registry. The built-in D2D "Analysis" category
        // is deliberately skipped: the ShaderLab analysis effects supersede
        // those wrappers in the catalog agents should pick from.
        void RegisterListEffects(McpRouter& server)
        {
            server.AddRoute(L"GET", L"/effects",
                [](const std::wstring&, const std::wstring&, const std::string&) -> Response {
                    std::string json = "{\"builtIn\":{";
                    auto& reg = ::ShaderLab::Effects::EffectRegistry::Instance();
                    bool firstCat = true;
                    for (const auto& cat : reg.Categories())
                    {
                        if (cat == L"Analysis") continue;
                        if (!firstCat) json += ",";
                        json += "\"" + JsonEscape(WideToUtf8(cat)) + "\":[";
                        bool firstFx = true;
                        for (const auto* e : reg.ByCategory(cat))
                        {
                            if (!firstFx) json += ",";
                            json += "\"" + JsonEscape(WideToUtf8(e->name)) + "\"";
                            firstFx = false;
                        }
                        json += "]";
                        firstCat = false;
                    }
                    json += "},\"shaderLab\":{";
                    auto& sl = ::ShaderLab::Effects::ShaderLabEffects::Instance();
                    firstCat = true;
                    for (const auto& cat : sl.Categories())
                    {
                        if (!firstCat) json += ",";
                        json += "\"" + JsonEscape(WideToUtf8(cat)) + "\":[";
                        bool firstFx = true;
                        for (const auto* e : sl.ByCategory(cat))
                        {
                            if (!firstFx) json += ",";
                            json += "\"" + JsonEscape(WideToUtf8(e->name)) + "\"";
                            firstFx = false;
                        }
                        json += "]";
                        firstCat = false;
                    }
                    // User effects are listed in their categories above; this adds
                    // where they were loaded from and what failed to load.
                    auto list = [](const std::vector<std::wstring>& values)
                    {
                        std::string out = "[";
                        for (size_t i = 0; i < values.size(); ++i)
                            out += (i ? ",\"" : "\"") + JsonEscape(WideToUtf8(values[i])) + "\"";
                        return out + "]";
                    };
                    const auto& user = sl.UserEffectReport();
                    json += "},\"user\":{\"directories\":" + list(user.directories) +
                            ",\"loaded\":" + list(user.loaded) +
                            ",\"errors\":" + list(user.errors) + "}}";
                    return Json(200, json);
                });
        }

        // ---- GET /graph/overview — compact summary (nodes, edges, preview) -
        // Promoted from the GUI's inline `graph_overview` tools/call handler
        // (stdio-migration Step 1). Reads the live graph, so it runs through
        // sink.Dispatch; previously it read m_graph on the UI thread while
        // the render worker mutated it. Longest-prefix routing sends
        // /graph/overview here rather than to the shorter GET /graph route.
        void RegisterGraphOverview(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/graph/overview",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> Response {
                        uint32_t previewId = ctx.getPreviewNodeId ? ctx.getPreviewNodeId() : 0;
                        std::string json = "{\"previewNodeId\":" + std::to_string(previewId) + ",\"nodes\":[";
                        bool first = true;
                        for (const auto& n : ctx.graph->Nodes())
                        {
                            if (!first) json += ",";
                            std::string typeStr;
                            switch (n.type)
                            {
                            case Graph::NodeType::Source:        typeStr = "Source"; break;
                            case Graph::NodeType::BuiltInEffect: typeStr = "BuiltIn"; break;
                            case Graph::NodeType::PixelShader:   typeStr = "PixelShader"; break;
                            case Graph::NodeType::ComputeShader: typeStr = "ComputeShader"; break;
                            case Graph::NodeType::Output:        typeStr = "Output"; break;
                            }
                            json += std::format("{{\"id\":{},\"name\":\"{}\",\"type\":\"{}\"",
                                n.id, JsonEscape(WideToUtf8(n.name)), typeStr);
                            if (!n.runtimeError.empty())
                                json += ",\"error\":\"" + JsonEscape(WideToUtf8(n.runtimeError)) + "\"";
                            // GPU milliseconds attributed to this node on the
                            // last measured frame, plus WHY it reads that way.
                            // gpuMs alone is ambiguous -- absent could mean
                            // "cost zero" or "cost unattributable" -- so the
                            // state is always emitted when timing is on and a
                            // reader should branch on it, not on gpuMs.
                            if (n.gpuState != Graph::GpuNodeState::Unmeasured)
                            {
                                const char* st = "unmeasured";
                                switch (n.gpuState)
                                {
                                case Graph::GpuNodeState::Measured: st = "measured"; break;
                                case Graph::GpuNodeState::Fused:    st = "fused-downstream"; break;
                                case Graph::GpuNodeState::Cached:   st = "cached"; break;
                                case Graph::GpuNodeState::Idle:     st = "idle"; break;
                                case Graph::GpuNodeState::CpuOnly:  st = "cpu-only"; break;
                                default: break;
                                }
                                json += std::format(",\"gpuState\":\"{}\"", st);
                            }
                            if (n.lastGpuMs >= 0.0)
                                json += std::format(",\"gpuMs\":{:.4f}", n.lastGpuMs);
                            json += std::format(",\"inputs\":{},\"outputs\":{}}}",
                                n.inputPins.size(), n.outputPins.size());
                            first = false;
                        }
                        json += "],\"edges\":[";
                        first = true;
                        for (const auto& e : ctx.graph->Edges())
                        {
                            if (!first) json += ",";
                            json += std::format("[{},{},{},{}]",
                                e.sourceNodeId, e.sourcePin, e.destNodeId, e.destPin);
                            first = false;
                        }
                        json += "]}";
                        return Json(200, json);
                    });
                });
        }

        // ---- GET /display/info — caps + active profile + pipeline ---------
        // Moved from MainWindow.McpRoutes.cpp in stdio-migration Step 2.
        // Step 1 left it app-side because it needs the pipeline-format
        // name and EngineContext had no way to supply one; the
        // getPipelineFormatName shim (added with this step's ABI bump)
        // closes that gap, so both hosts serve it now.
        void RegisterDisplayInfo(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/display/info",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> Response {
                        auto profile = ctx.displayMonitor->ActiveProfile();
                        auto live = ctx.displayMonitor->LiveProfile();
                        auto caps = ctx.displayMonitor->CachedCapabilities();
                        auto verStr = WideToUtf8(std::wstring(::ShaderLab::VersionString));
                        std::wstring fmtName = ctx.getPipelineFormatName
                            ? ctx.getPipelineFormatName() : std::wstring(L"unknown");
                        // Non-empty when the WinRT display binding or last
                        // query failed — the caps below are struct defaults,
                        // not measurements. Emitted so clients can tell.
                        auto monitorErr = ctx.displayMonitor->LastError();
                        std::string statusField = monitorErr.empty()
                            ? std::string{}
                            : std::format(",\"monitorStatus\":\"{}\"",
                                  JsonEscape(WideToUtf8(monitorErr)));
                        std::string json = std::format(
                            "{{\"appVersion\":\"{}\",\"graphFormatVersion\":{}"
                            ",\"pipeline\":\"{}\""
                            ",\"display\":{{\"hdr\":{},\"maxNits\":{:.0f},\"sdrWhiteNits\":{:.0f}"
                            ",\"simulated\":{},\"profileName\":\"{}\""
                            ",\"activeGamut\":{{\"red\":[{:.4f},{:.4f}],\"green\":[{:.4f},{:.4f}],\"blue\":[{:.4f},{:.4f}]}}"
                            ",\"monitorGamut\":{{\"red\":[{:.4f},{:.4f}],\"green\":[{:.4f},{:.4f}],\"blue\":[{:.4f},{:.4f}]}}"
                            "{}"
                            "}}}}",
                            verStr, ::ShaderLab::GraphFormatVersion,
                            JsonEscape(WideToUtf8(fmtName)),
                            caps.hdrEnabled ? "true" : "false",
                            caps.maxLuminanceNits, caps.sdrWhiteLevelNits,
                            profile.isSimulated ? "true" : "false",
                            JsonEscape(WideToUtf8(profile.profileName)),
                            profile.primaryRed.x, profile.primaryRed.y,
                            profile.primaryGreen.x, profile.primaryGreen.y,
                            profile.primaryBlue.x, profile.primaryBlue.y,
                            live.primaryRed.x, live.primaryRed.y,
                            live.primaryGreen.x, live.primaryGreen.y,
                            live.primaryBlue.x, live.primaryBlue.y,
                            statusField);
                        return Json(200, json);
                    });
                });
        }

        // ---- POST /graph/connect — wire output pin -> input pin -----------
        // Note: the GUI app also runs m_nodeGraphController.AutoLayout()
        // and adds NodeLog entries. Those are UI side effects; the engine
        // route just does the graph mutation. The GUI's render tick will
        // pick up the dirty state and refresh the canvas next frame.
        void RegisterConnect(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/connect",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t srcId = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcId"));
                            uint32_t srcPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcPin"));
                            uint32_t dstId = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstId"));
                            uint32_t dstPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstPin"));
                            bool ok = ctx.graph->Connect(srcId, srcPin, dstId, dstPin);   // dirties dst
                            if (ok) sink.OnGraphStructureChanged();
                            return Json(200,
                                std::string("{\"connected\":") + (ok ? "true" : "false") + "}");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/disconnect — remove a single edge ----------------
        void RegisterDisconnect(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/disconnect",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t srcId = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcId"));
                            uint32_t srcPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcPin"));
                            uint32_t dstId = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstId"));
                            uint32_t dstPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstPin"));
                            bool ok = ctx.graph->Disconnect(srcId, srcPin, dstId, dstPin);   // dirties dst
                            if (ok) sink.OnGraphStructureChanged();
                            return Json(200,
                                std::string("{\"disconnected\":") + (ok ? "true" : "false") + "}");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/bind-property — bind property to analysis output -
        // Note: the GUI's m_nodeGraphController.RebuildLayout() drops out;
        // the render tick will pick up the dirty state and rebuild
        // automatically. Headless host has no canvas anyway.
        void RegisterBindProperty(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/bind-property",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto propName = std::wstring(jobj.GetNamedString(L"propertyName"));
                            uint32_t srcNodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"sourceNodeId"));
                            auto srcFieldName = std::wstring(jobj.GetNamedString(L"sourceFieldName"));
                            uint32_t srcComponent = jobj.HasKey(L"sourceComponent")
                                ? static_cast<uint32_t>(jobj.GetNamedNumber(L"sourceComponent")) : 0;

                            auto err = ctx.graph->BindProperty(nodeId, propName, srcNodeId, srcFieldName, srcComponent);
                            if (!err.empty())
                                return Json(400, "{\"error\":\"" + WideToUtf8(err) + "\"}");
                            sink.OnGraphStructureChanged();
                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/unbind-property -----------------------------------
        void RegisterUnbindProperty(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/unbind-property",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto propName = std::wstring(jobj.GetNamedString(L"propertyName"));
                            if (!ctx.graph->UnbindProperty(nodeId, propName))
                                return Json(404, R"({"error":"No binding for that property"})");
                            sink.OnGraphStructureChanged();
                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/add-node ------------------------------------------
        // Big route with multiple node-type branches:
        //   * "Custom Compute Shader" / "Custom Pixel Shader" / "Custom D3D11
        //     Compute Shader" -- create a fresh user-authored shader node.
        //   * Any ShaderLab effect name -- look up in ShaderLabEffects registry.
        //   * "Video Source" / "Image Source" -- create + PrepareSourceNode.
        //   * Any built-in D2D effect name -- look up in EffectRegistry.
        //
        // After AddNode the OnNodeAdded event fires so the host (if any)
        // can run AutoLayout + PopulatePreviewNodeSelector + log entry.
        // Same UI path the toolbar AddNode flyout takes.
        void RegisterAddNode(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/add-node",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            if (!jobj.HasKey(L"effectName"))
                                return Json(400, R"({"error":"Provide effectName"})");
                            auto name = jobj.GetNamedString(L"effectName");

                            auto addAndReply = [&](Graph::EffectNode&& node) -> Response {
                                auto id = ctx.graph->AddNode(std::move(node));   // starts dirty
                                sink.OnNodeAdded(id);
                                return Json(200, "{\"nodeId\":" + std::to_string(id) + "}");
                            };

                            // Custom shader node creation.
                            if (name == L"Custom Compute Shader" || name == L"Custom Pixel Shader" ||
                                name == L"Custom D3D11 Compute Shader")
                            {
                                Graph::EffectNode node;
                                bool isCompute = (name == L"Custom Compute Shader");
                                bool isD3D11 = (name == L"Custom D3D11 Compute Shader");
                                node.type = (isCompute || isD3D11)
                                    ? Graph::NodeType::ComputeShader
                                    : Graph::NodeType::PixelShader;
                                node.name = std::wstring(name);

                                if (!isD3D11)
                                {
                                    node.effectClsid = isCompute
                                        ? ::ShaderLab::Effects::CustomComputeShaderEffect::CLSID_CustomComputeShader
                                        : ::ShaderLab::Effects::CustomPixelShaderEffect::CLSID_CustomPixelShader;
                                    node.outputPins.push_back({ L"Output", 0 });
                                }

                                Graph::CustomEffectDefinition def;
                                def.shaderType = isD3D11
                                    ? Graph::CustomShaderType::D3D11ComputeShader
                                    : isCompute
                                        ? Graph::CustomShaderType::ComputeShader
                                        : Graph::CustomShaderType::PixelShader;
                                CoCreateGuid(&def.shaderGuid);
                                def.inputNames.push_back(L"Source");
                                node.inputPins.push_back({ L"I0", 0 });
                                if (isCompute) { def.threadGroupX = 8; def.threadGroupY = 8; def.threadGroupZ = 1; }
                                if (isD3D11)
                                {
                                    def.analysisOutputType = Graph::AnalysisOutputType::Typed;
                                    def.analysisFields.push_back(
                                        { L"Result", Graph::AnalysisFieldType::Float4 });
                                }
                                node.customEffect = std::move(def);
                                return addAndReply(std::move(node));
                            }

                            // ShaderLab effects registry lookup.
                            if (auto* slDesc = ::ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(name))
                            {
                                auto node = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*slDesc);
                                return addAndReply(std::move(node));
                            }

                            // Built-in D2D effects registry lookup.
                            if (auto* desc = ::ShaderLab::Effects::EffectRegistry::Instance().FindByName(name))
                            {
                                auto node = ::ShaderLab::Effects::EffectRegistry::CreateNode(*desc);
                                return addAndReply(std::move(node));
                            }

                            // Special source types: Video / Image. Optional
                            // filePath; if present, PrepareSourceNode kicks in.
                            std::wstring wname(name.begin(), name.end());
                            std::wstring filePath;
                            if (jobj.HasKey(L"filePath"))
                                filePath = std::wstring(jobj.GetNamedString(L"filePath"));

                            auto displayName = [&](const wchar_t* fallback) {
                                return filePath.empty() ? std::wstring(fallback)
                                    : filePath.substr(filePath.find_last_of(L"\\/") + 1);
                            };

                            // Output node. Not in the effect registry -- it is a
                            // node TYPE, not an effect -- so it needs its own branch.
                            // In the GUI host OnNodeAdded opens a window for it, which
                            // is the only way to get a second surface on screen without
                            // a human clicking: useful for putting real HDR pixels on
                            // the desktop while the main preview shows something else.
                            if (wname == L"Output")
                            {
                                auto node = ::ShaderLab::Effects::EffectRegistry::CreateOutputNode();
                                return addAndReply(std::move(node));
                            }

                            // Desktop duplication. The node is pure data
                            // (adapter/output index + RawFP16), so unlike
                            // Windows Graphics Capture -- which needs a
                            // GraphicsCaptureItem from the OS picker -- it can
                            // be created headlessly and over MCP. That matters
                            // here: it is what makes the whole HDR capture ->
                            // tone map -> 8-bit handback path scriptable end to
                            // end instead of requiring a human to click a menu.
                            //
                            // RawFP16 is set by the factory: on an HDR display
                            // the duplicated surface is FP16 scRGB, which is the
                            // only form that preserves values above 1.0 and the
                            // negative components carrying wide-gamut chroma.
                            if (wname == L"Desktop Duplication" || wname == L"DXGI Duplicate Output" ||
                                wname == L"Display Capture")
                            {
                                auto readIndex = [&](const wchar_t* key) -> uint32_t {
                                    if (!jobj.HasKey(key)) return 0;
                                    auto v = jobj.GetNamedValue(key);
                                    // Untyped MCP args arrive as JSON strings
                                    // ("0"), the same coercion /graph/set-property
                                    // does -- see CLAUDE.md.
                                    if (v.ValueType() == WDJ::JsonValueType::String)
                                        return static_cast<uint32_t>(_wtoi(v.GetString().c_str()));
                                    if (v.ValueType() == WDJ::JsonValueType::Number)
                                        return static_cast<uint32_t>(v.GetNumber());
                                    return 0;
                                };
                                const uint32_t adapterIndex = readIndex(L"adapterIndex");
                                const uint32_t outputIndex  = readIndex(L"outputIndex");

                                auto outputs = ::ShaderLab::Effects::DxgiDuplicationSourceProvider::EnumerateOutputs();
                                auto match = std::find_if(outputs.begin(), outputs.end(),
                                    [&](const auto& o) {
                                        return o.adapterIndex == adapterIndex && o.outputIndex == outputIndex;
                                    });
                                if (match == outputs.end())
                                {
                                    // Fail loudly with the list rather than
                                    // creating a node that renders black: a
                                    // silently dead capture source is very hard
                                    // to tell from a correctly-captured black
                                    // screen.
                                    std::string avail;
                                    for (const auto& o : outputs)
                                    {
                                        if (!avail.empty()) avail += ", ";
                                        avail += std::to_string(o.adapterIndex) + ":" +
                                                 std::to_string(o.outputIndex);
                                    }
                                    if (avail.empty()) avail = "(none)";
                                    return Json(400, std::format(
                                        R"({{"error":"No DXGI output {}:{}. Available adapter:output pairs: {}"}})",
                                        adapterIndex, outputIndex, JsonEscape(avail)));
                                }

                                std::wstring label = match->deviceName.empty()
                                    ? std::format(L"DXGI {}:{}", adapterIndex, outputIndex)
                                    : std::format(L"DXGI {} ({},{})", match->deviceName,
                                                  adapterIndex, outputIndex);
                                auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateDxgiDuplicateOutputSourceNode(
                                    adapterIndex, outputIndex, label);
                                auto id = ctx.graph->AddNode(std::move(node));
                                if (ctx.sourceFactory && ctx.dc)
                                {
                                    if (auto* graphNode = ctx.graph->FindNode(id))
                                        ctx.sourceFactory->PrepareSourceNode(*graphNode,
                                            static_cast<ID2D1DeviceContext5*>(ctx.dc), 0.0,
                                            ctx.d3dDevice, ctx.d3dContext);
                                }
                                sink.OnNodeAdded(id);
                                return Json(200, std::format(
                                    R"({{"nodeId":{},"adapterIndex":{},"outputIndex":{},"name":"{}"}})",
                                    id, adapterIndex, outputIndex,
                                    JsonEscape(WideToUtf8(label))));
                            }

                            if (wname == L"Video Source" || wname == L"Video")
                            {
                                auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(
                                    filePath, displayName(L"Video Source"));
                                auto id = ctx.graph->AddNode(std::move(node));
                                if (!filePath.empty() && ctx.sourceFactory && ctx.dc)
                                {
                                    if (auto* graphNode = ctx.graph->FindNode(id))
                                        ctx.sourceFactory->PrepareSourceNode(*graphNode,
                                            static_cast<ID2D1DeviceContext5*>(ctx.dc), 0.0,
                                            ctx.d3dDevice, ctx.d3dContext);
                                }
                                sink.OnNodeAdded(id);
                                return Json(200, "{\"nodeId\":" + std::to_string(id) + "}");
                            }
                            if (wname == L"Image Source" || wname == L"Image")
                            {
                                auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(
                                    filePath, displayName(L"Image Source"));
                                auto id = ctx.graph->AddNode(std::move(node));
                                if (!filePath.empty() && ctx.sourceFactory && ctx.dc)
                                {
                                    if (auto* graphNode = ctx.graph->FindNode(id))
                                        ctx.sourceFactory->PrepareSourceNode(*graphNode,
                                            static_cast<ID2D1DeviceContext5*>(ctx.dc), 0.0,
                                            ctx.d3dDevice, ctx.d3dContext);
                                }
                                sink.OnNodeAdded(id);
                                return Json(200, "{\"nodeId\":" + std::to_string(id) + "}");
                            }

                            return Json(400, R"({"error":"Unknown effect name"})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid JSON"})"); }
                    });
                });
        }

        // ---- GET /effect/hlsl/<nodeId> -- read HLSL of a custom effect -----
        // Read-only; no Dispatch needed. Library effects (ShaderLab built-in)
        // are reported with isLibraryEffect=true so agents know they're
        // read-only-shipped and shouldn't try to recompile them.
        void RegisterEffectHlsl(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/effect/hlsl/",
                [&sink](const std::wstring& path, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([path](EngineContext& ctx) -> Response {
                        if (path.size() <= 13)
                            return Json(400, R"({"error":"Missing nodeId in URL"})");
                        uint32_t nodeId = 0;
                        try { nodeId = static_cast<uint32_t>(std::stoul(path.substr(13))); }
                        catch (...) { return Json(400, R"({"error":"Invalid nodeId"})"); }

                        auto* node = ctx.graph->FindNode(nodeId);
                        if (!node)
                            return Json(404, "{\"error\":\"Node " + std::to_string(nodeId) + " not found\"}");

                        std::string runtimeErr = JsonEscape(WideToUtf8(node->runtimeError));
                        std::string nameEsc = JsonEscape(WideToUtf8(node->name));

                        if (!node->customEffect.has_value())
                        {
                            std::string j = "{\"nodeId\":" + std::to_string(nodeId)
                                + ",\"hasCustomEffect\":false,\"name\":\"" + nameEsc
                                + "\",\"runtimeError\":\"" + runtimeErr + "\"}";
                            return Json(200, j);
                        }

                        const auto& def = node->customEffect.value();
                        const char* shaderTypeStr =
                            (def.shaderType == Graph::CustomShaderType::PixelShader)
                                ? "PixelShader" : "ComputeShader";

                        std::string inputsJson = "[";
                        for (size_t i = 0; i < def.inputNames.size(); ++i)
                        {
                            if (i) inputsJson += ",";
                            inputsJson += "\"" + JsonEscape(WideToUtf8(def.inputNames[i])) + "\"";
                        }
                        inputsJson += "]";

                        std::string paramsJson = "[";
                        for (size_t i = 0; i < def.parameters.size(); ++i)
                        {
                            if (i) paramsJson += ",";
                            paramsJson += "{\"name\":\""
                                + JsonEscape(WideToUtf8(def.parameters[i].name)) + "\"}";
                        }
                        paramsJson += "]";

                        std::string libBlock;
                        if (!def.shaderLabEffectId.empty())
                        {
                            libBlock = std::string(",\"isLibraryEffect\":true,\"shaderLabEffectId\":\"")
                                + JsonEscape(WideToUtf8(def.shaderLabEffectId))
                                + "\",\"shaderLabEffectVersion\":"
                                + std::to_string(def.shaderLabEffectVersion);
                        }
                        else
                        {
                            libBlock = ",\"isLibraryEffect\":false";
                        }

                        std::string j = "{\"nodeId\":" + std::to_string(nodeId)
                            + ",\"hasCustomEffect\":true,\"name\":\"" + nameEsc + "\""
                            + ",\"shaderType\":\"" + shaderTypeStr + "\""
                            + ",\"hlslSource\":\"" + JsonEscape(WideToUtf8(def.hlslSource)) + "\""
                            + ",\"inputNames\":" + inputsJson
                            + ",\"parameters\":" + paramsJson
                            + ",\"bytecodeSize\":" + std::to_string(def.compiledBytecode.size())
                            + ",\"isCompiled\":" + (def.isCompiled() ? "true" : "false")
                            + ",\"runtimeError\":\"" + runtimeErr + "\""
                            + libBlock + "}";
                        return Json(200, j);
                    });
                });
        }

        // ---- Graph load / save helpers ----------------------------------------

        // Parse graph JSON and restore the runtime flags the file does not carry.
        std::optional<Graph::EffectGraph> ParseGraph(const std::wstring& json, std::string& error)
        {
            try
            {
                auto graph = Graph::EffectGraph::FromJson(winrt::hstring(json));
                Effects::ShaderLabEffects::RestoreRuntimeFlags(graph);
                return graph;
            }
            catch (const winrt::hresult_error& ex) { error = WideToUtf8(std::wstring(ex.message())); }
            catch (const std::exception& ex)       { error = ex.what(); }
            if (error.empty()) error = "invalid graph JSON";
            return std::nullopt;
        }

        // Replace the graph the way File > Open does and prepare its sources.
        // Runs inside Dispatch.
        void AdoptLoadedGraph(EngineContext& ctx, IEngineCommandSink& sink,
            Graph::EffectGraph&& loaded, const std::wstring& mediaDir)
        {
            if (ctx.evaluator) ctx.evaluator->ReleaseCache();
            *ctx.graph = std::move(loaded);
            ctx.graph->MarkAllDirty();
            if (ctx.sourceFactory)
            {
                // Close the old graph's providers before its media directory is deleted.
                ctx.sourceFactory->PruneOrphans(ctx.graph->Nodes());
                if (ctx.dc)
                {
                    for (auto& node : const_cast<std::vector<Graph::EffectNode>&>(ctx.graph->Nodes()))
                    {
                        if (node.type != Graph::NodeType::Source) continue;
                        try
                        {
                            ctx.sourceFactory->PrepareSourceNode(node,
                                static_cast<ID2D1DeviceContext5*>(ctx.dc), 0.0,
                                ctx.d3dDevice, ctx.d3dContext);
                        }
                        catch (...)
                        {
                            node.runtimeError = L"Source preparation failed";
                        }
                    }
                }
            }
            sink.OnGraphMediaDirChanged(mediaDir);
            sink.OnGraphLoaded();
        }

        // Parse a { "path": "<absolute path>" } body. Returns the 400 to send on failure.
        std::optional<Response> ReadPathArgument(const std::string& body, std::wstring& path, WDJ::JsonObject& request)
        {
            if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), request))
                return Error(400, "body must be a JSON object with a path");
            if (!request.HasKey(L"path") || request.GetNamedValue(L"path").ValueType() != WDJ::JsonValueType::String)
                return Error(400, "path (an absolute file path) is required");
            path = std::wstring(request.GetNamedString(L"path"));
            if (path.empty() || !std::filesystem::path(path).is_absolute())
                return Error(400, JsonEscape(WideToUtf8(L"path must be absolute: " + path)));
            path = std::filesystem::path(path).lexically_normal().wstring();
            return std::nullopt;
        }

        std::wstring TempRoot()
        {
            wchar_t buffer[MAX_PATH + 1]{};
            const DWORD length = ::GetTempPathW(MAX_PATH + 1, buffer);
            return std::wstring(buffer, length);
        }

        void RemoveExtractDir(const std::wstring& dir)
        {
            std::error_code ec;
            if (!dir.empty()) std::filesystem::remove_all(dir, ec);   // best effort: temp dir
        }

        // ---- POST /graph/clear ---------------------------------------------
        // Engine drops graph state and the evaluator cache. The OnGraphCleared
        // event runs the host's UI cleanup (output windows, preview selector
        // reset). Same path /graph/clear via UI button takes.
        void RegisterClear(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/clear",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([&sink](EngineContext& ctx) -> Response {
                        ctx.evaluator->ReleaseCache();
                        ctx.graph->Clear();
                        ctx.graph->MarkAllDirty();
                        if (ctx.sourceFactory) ctx.sourceFactory->PruneOrphans(ctx.graph->Nodes());
                        sink.OnGraphMediaDirChanged({});
                        sink.OnGraphCleared();
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // ---- POST /graph/load ----------------------------------------------
        // Body is the full graph JSON. Engine deserializes + assigns; the
        // OnGraphLoaded event runs the host's per-load setup (heartbeats,
        // re-opens output windows for nodes that had them, preview selector
        // refresh). Same path the file-open dialog takes.
        void RegisterLoad(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/load",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    // Parse on the listener thread; assignment requires the
                    // dispatch thread (it owns m_graph).
                    std::string parseError;
                    auto loaded = ParseGraph(std::wstring(winrt::to_hstring(body)), parseError);
                    if (!loaded)
                        return Error(400, JsonEscape(parseError));
                    return sink.Dispatch([&loaded, &sink](EngineContext& ctx) -> Response {
                        AdoptLoadedGraph(ctx, sink, std::move(*loaded), {});
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // ---- POST /graph/load-file -----------------------------------------
        // Body { path }. Reads a .effectgraph package or bare graph JSON, told
        // apart by content. The file is read and its media extracted on the
        // listener thread; only the swap runs inside Dispatch, so a large
        // package does not hold the render thread.
        void RegisterLoadFile(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/load-file",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    std::wstring path;
                    WDJ::JsonObject request{ nullptr };
                    if (auto failure = ReadPathArgument(body, path, request))
                        return *failure;
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(path, ec))
                        return Error(400, JsonEscape(WideToUtf8(L"file not found: " + path)));

                    std::wstring loadError;
                    auto file = Rendering::EffectGraphFile::LoadAny(path, TempRoot(), loadError);
                    if (!file)
                        return Error(400, JsonEscape(WideToUtf8(loadError)));

                    std::string parseError;
                    auto loaded = ParseGraph(file->graphJson, parseError);
                    if (!loaded)
                    {
                        RemoveExtractDir(file->extractDir);
                        return Error(400, JsonEscape(WideToUtf8(L"'" + path + L"' is not a valid graph: ")) + JsonEscape(parseError));
                    }
                    Rendering::EffectGraphFile::ResolveMediaTokens(*loaded, file->mediaMap);
                    // Load names a directory even when there was no media to put in it.
                    const std::wstring mediaDir = file->mediaMap.empty() ? std::wstring() : file->extractDir;

                    // If Dispatch throws (a timeout), the closure may still run
                    // and adopt the directory, so it is not deleted here.
                    return sink.Dispatch([&](EngineContext& ctx) -> Response {
                        AdoptLoadedGraph(ctx, sink, std::move(*loaded), mediaDir);

                        std::string nodes;
                        std::string errors;
                        for (const auto& node : ctx.graph->Nodes())
                        {
                            if (!nodes.empty()) nodes += ",";
                            nodes += std::format(R"({{"id":{},"name":"{}","type":"{}"}})",
                                node.id, JsonEscape(WideToUtf8(node.name)), NodeTypeStr(node.type));

                            std::wstring nodeError = node.runtimeError;
                            if (nodeError.empty() && node.type == Graph::NodeType::Source
                                && node.shaderPath.has_value() && !node.shaderPath->empty())
                            {
                                std::error_code existsError;
                                if (!std::filesystem::exists(*node.shaderPath, existsError))
                                    nodeError = L"source file not found: " + *node.shaderPath;
                            }
                            if (nodeError.empty()) continue;
                            if (!errors.empty()) errors += ",";
                            errors += std::format(R"({{"nodeId":{},"name":"{}","error":"{}"}})",
                                node.id, JsonEscape(WideToUtf8(node.name)), JsonEscape(WideToUtf8(nodeError)));
                        }
                        return Json(200, std::format(
                            R"({{"ok":true,"path":"{}","format":"{}","nodeCount":{},"nodes":[{}],"mediaFiles":{},"extractDir":"{}","errors":[{}]}})",
                            JsonEscape(WideToUtf8(path)), file->package ? "package" : "json",
                            ctx.graph->Nodes().size(), nodes, file->mediaMap.size(),
                            JsonEscape(WideToUtf8(mediaDir)), errors));
                    });
                });
        }

        // ---- POST /graph/save-file -----------------------------------------
        // Body { path, embedMedia? }. A path ending in .json is written as bare
        // graph JSON, anything else as a package; embedMedia defaults to true
        // for a package. The graph is serialized inside Dispatch and written
        // after it returns, so the write does not hold the render thread.
        void RegisterSaveFile(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/save-file",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    std::wstring path;
                    WDJ::JsonObject request{ nullptr };
                    if (auto failure = ReadPathArgument(body, path, request))
                        return *failure;

                    std::wstring extension = std::filesystem::path(path).extension().wstring();
                    for (auto& c : extension) c = static_cast<wchar_t>(::towlower(c));
                    const bool package = extension != L".json";

                    bool embedMedia = package;
                    if (request.HasKey(L"embedMedia"))
                    {
                        const auto value = request.GetNamedValue(L"embedMedia");
                        bool parsed = false;
                        switch (value.ValueType())
                        {
                        case WDJ::JsonValueType::Boolean: embedMedia = value.GetBoolean(); parsed = true; break;
                        case WDJ::JsonValueType::Number:  embedMedia = value.GetNumber() != 0.0; parsed = true; break;
                        case WDJ::JsonValueType::String:  parsed = CoerceStringToBool(std::wstring(value.GetString()), embedMedia); break;
                        case WDJ::JsonValueType::Null:    parsed = true; break;
                        default: break;
                        }
                        if (!parsed)
                            return Error(400, "embedMedia must be a boolean");
                    }
                    if (embedMedia && !package)
                        return Error(400, "embedMedia needs a package; use a .effectgraph path");

                    std::error_code ec;
                    const auto parent = std::filesystem::path(path).parent_path();
                    if (!std::filesystem::is_directory(parent, ec))
                        return Error(400, JsonEscape(WideToUtf8(L"folder not found: " + parent.wstring())));
                    if (std::filesystem::is_directory(path, ec))
                        return Error(400, JsonEscape(WideToUtf8(L"path is a folder: " + path)));

                    // Serialize on the dispatch thread, the graph's single writer.
                    std::wstring graphJson;
                    std::vector<Rendering::EffectGraphFile::MediaEntry> media;
                    std::vector<std::wstring> warnings;
                    size_t nodeCount = 0;
                    Response serialized = sink.Dispatch([&](EngineContext& ctx) -> Response {
                        nodeCount = ctx.graph->Nodes().size();
                        graphJson = Rendering::EffectGraphFile::SerializeForSave(*ctx.graph, embedMedia, media);
                        const std::wstring extractedPrefix = TempRoot() + L"ShaderLab-";
                        for (const auto& node : ctx.graph->Nodes())
                        {
                            if (node.type != Graph::NodeType::Source || !node.shaderPath.has_value()) continue;
                            const std::wstring& sourcePath = *node.shaderPath;
                            if (sourcePath.empty() || sourcePath.starts_with(L"media://")) continue;
                            std::error_code existsError;
                            if (!std::filesystem::exists(sourcePath, existsError))
                                warnings.push_back(std::format(L"node {}: source file not found, saved as a path: {}", node.id, sourcePath));
                            else if (!embedMedia && sourcePath.starts_with(extractedPrefix))
                                warnings.push_back(std::format(L"node {}: source is extracted package media in the temp folder, which is deleted when this graph is replaced; embed it to keep it: {}", node.id, sourcePath));
                        }
                        return Json(200, "{}");
                    });
                    if (serialized.statusCode != 200)
                        return serialized;

                    Rendering::EffectGraphFile::SaveStats stats;
                    bool saved = false;
                    if (package)
                    {
                        saved = Rendering::EffectGraphFile::Save(path, graphJson, media, {}, &stats);
                    }
                    else
                    {
                        saved = Rendering::EffectGraphFile::SaveJson(path, graphJson, &stats.bytesWritten);
                    }
                    if (!saved)
                        return Error(500, JsonEscape(WideToUtf8(std::format(L"could not write '{}' (Win32 error {})", path, ::GetLastError()))));

                    std::string warningsJson;
                    for (const auto& warning : warnings)
                    {
                        if (!warningsJson.empty()) warningsJson += ",";
                        warningsJson += "\"" + JsonEscape(WideToUtf8(warning)) + "\"";
                    }
                    const uint64_t fileSize = std::filesystem::file_size(path, ec);
                    return Json(200, std::format(
                        R"({{"ok":true,"path":"{}","format":"{}","embedMedia":{},"nodeCount":{},"mediaEmbedded":{},"mediaUnchanged":{},"mediaWritten":{},"inPlace":{},"bytesWritten":{},"fileSize":{},"warnings":[{}]}})",
                        JsonEscape(WideToUtf8(path)), package ? "package" : "json", embedMedia ? "true" : "false",
                        nodeCount, media.size(), stats.mediaUnchanged, stats.mediaWritten,
                        stats.inPlace ? "true" : "false", stats.bytesWritten, ec ? 0 : fileSize, warningsJson));
                });
        }

        // ---- POST /graph/remove-node ---------------------------------------
        void RegisterRemoveNode(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/remove-node",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            ctx.graph->RemoveNode(nodeId);   // dirties its consumers
                            sink.OnNodeRemoved(nodeId);
                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/set-property — mutates m_graph -------------------
        void RegisterSetProperty(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/set-property",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto key = std::wstring(jobj.GetNamedString(L"key"));
                            auto val = jobj.GetNamedValue(L"value");

                            auto* node = ctx.graph->FindNode(nodeId);
                            if (!node) return Json(404, R"({"error":"Node not found"})");

                            // NOTE: a key guard was added here and reverted. It
                            // rejected every un-seeded D2D built-in property
                            // (Gaussian Blur Optimization / BorderMode), Clock's
                            // IsPlaying mirror, shaderPath and numeric index keys,
                            // because those are legitimately absent from both
                            // node->properties and customEffect->parameters until
                            // first written -- so it broke working calls, and
                            // Tests/RunTests.ps1 Graph.SetProperty asserts one of
                            // them. Meanwhile /graph/apply writes property maps
                            // unchecked, so the invalid case it was meant to close
                            // had a one-call bypass. Accepting an unknown key is a
                            // real hole (a typo, or a parameter removed by an
                            // effectVersion bump, lands in the map and serialises
                            // into the document doing nothing) but it is
                            // pre-existing and strictly less harmful than
                            // rejecting valid ones. Closing it properly means
                            // resolving a key the way GraphEvaluator::ApplyProperties
                            // does -- built-in GetPropertyIndex included -- and
                            // guarding /graph/apply on the same predicate.

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
                            {
                                // Some MCP clients stringify untyped (schema {})
                                // argument values, so a numeric/bool param can
                                // arrive as a JSON string ("203", "true"). Coerce
                                // to the target's real type -- a bogus wstring
                                // otherwise both starves the shader (param reads 0)
                                // and breaks bindability (IsBindablePropertyType
                                // rejects non-float/bool variants).
                                std::wstring sval(val.GetString());
                                std::wstring want;   // float | uint | int | bool | ""
                                if (node->customEffect.has_value())
                                    for (const auto& p : node->customEffect->parameters)
                                        if (p.name == key) { want = p.typeName; break; }
                                if (want.empty())
                                {
                                    auto it = node->properties.find(key);
                                    if (it != node->properties.end())
                                    {
                                        if (std::holds_alternative<float>(it->second)) want = L"float";
                                        else if (std::holds_alternative<uint32_t>(it->second)) want = L"uint";
                                        else if (std::holds_alternative<int32_t>(it->second)) want = L"int";
                                        else if (std::holds_alternative<bool>(it->second)) want = L"bool";
                                    }
                                }
                                if (want.empty() && (key == L"IsPlaying" || key == L"isPlaying"))
                                    want = L"bool";
                                try
                                {
                                    if (want == L"float")     node->properties[key] = std::stof(sval);
                                    else if (want == L"uint") node->properties[key] = static_cast<uint32_t>(std::stoul(sval));
                                    else if (want == L"int")  node->properties[key] = static_cast<int32_t>(std::stol(sval));
                                    else if (want == L"bool")
                                    {
                                        bool bv = false;
                                        if (!CoerceStringToBool(sval, bv))
                                            // Custom delimiter: the message itself ends in `)"`,
                                            // which closes a bare R"(...)" literal early.
                                            return Json(400, R"JSON({"error":"expected a boolean-valued string (true/false, 1/0, on/off, yes/no)"})JSON");
                                        node->properties[key] = bv;
                                    }
                                    else                      node->properties[key] = sval;
                                }
                                // Not-a-number: keep the raw string rather than
                                // dropping the write. Note this lands back in
                                // the broken state the coercion above exists to
                                // prevent (a wstring in a numeric slot), so it
                                // should only ever be reached for a genuinely
                                // non-numeric value the caller sent by mistake.
                                catch (...) { node->properties[key] = sval; }
                                break;
                            }
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
                            // Only this node changed; the evaluator pulls the change
                            // downstream. (MarkAllDirty here made every MCP sweep
                            // step a worst-case full-graph frame.)
                            node->dirty = true;

                            // Special-cased properties that mirror to dedicated node fields.
                            // Match both casings: graph storage uses `IsPlaying`
                            // (PascalCase, matching the Clock + Video effect
                            // descriptors and graph_get_node output), but legacy
                            // callers used lowercase. Without this mirror, the
                            // runtime `node.isPlaying` bool stays false and
                            // Clock/Video never tick (RenderTick gates on
                            // `node.isPlaying`, not on the property map).
                            if (key == L"isPlaying" || key == L"IsPlaying")
                            {
                                bool play = false;
                                if (!CoercePropertyToBool(node->properties[key], play))
                                    return Json(400, R"({"error":"isPlaying must be a boolean, number or true/false string"})");
                                // Play from the end means REWIND, the way every
                                // transport control works. Without this the
                                // tick clamps clockTime to duration and stops
                                // again on the same frame, so "play" on a
                                // finished non-looping clock is a silent no-op
                                // -- which reads as the toggle being broken.
                                if (play && !node->isPlaying)
                                {
                                    auto getF = [&](const wchar_t* k, float dflt) {
                                        auto it = node->properties.find(k);
                                        if (it != node->properties.end())
                                            if (auto* f = std::get_if<float>(&it->second)) return *f;
                                        return dflt;
                                    };
                                    const bool loop = getF(L"Loop", 1.0f) > 0.5f;
                                    double duration = getF(L"StopTime", 10.0f) - getF(L"StartTime", 0.0f);
                                    if (duration <= 0.0) duration = 1.0;
                                    if (!loop && node->clockTime >= duration)
                                        node->clockTime = 0.0;
                                }
                                node->isPlaying = play;
                            }
                            if (key == L"shaderPath")
                            {
                                if (auto* sv = std::get_if<std::wstring>(&node->properties[key]))
                                    node->shaderPath = *sv;
                            }

                            sink.OnNodeChanged(nodeId);
                            return Json(200, R"({"ok":true})");
                        }
                        catch (...) { return Json(400, R"({"error":"Invalid request"})"); }
                    });
                });
        }

        // ---- POST /graph/apply ---------------------------------------------
        // Bulk graph-construction primitive. Body shape:
        //   {
        //     "clear": true,                                   // optional; default false
        //     "nodes":    [ { "ref": "video", "effect": "Video", "properties": {...} }, ... ],
        //     "edges":    [ { "from": "video", "to": "scale", "fromPin": 0, "toPin": 0 }, ... ],
        //     "bindings": [ { "node": "tonemap", "property": "InputMaxLuminance",
        //                     "from": "lum.Max", "component": 0 }, ... ],
        //     "previewNode": "split"                           // optional
        //   }
        // `ref` is a client-supplied symbolic name (string). The route resolves
        // refs to server-assigned node IDs as it goes, so the caller never has
        // to round-trip through add-node responses to wire the rest of the graph.
        // Edges/bindings can also use raw numeric IDs to mix with hand-managed
        // nodes; refs and numeric IDs are interchangeable in those positions.
        // The whole closure runs as a single render-thread dispatch -- atomic
        // either succeeds or fails as one unit.
        void RegisterApply(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/graph/apply",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        WDJ::JsonObject root;
                        try { root = WDJ::JsonObject::Parse(winrt::to_hstring(body)); }
                        catch (...) { return Json(400, R"({"error":"Invalid JSON"})"); }

                        // Note: an in-patch `clear` flag was removed because
                        // re-entering ctx.graph->Clear() from inside an
                        // already-dispatched closure proved fragile (D2D
                        // teardown races subsequent AddNode calls). Callers
                        // wanting a fresh graph should call /graph/clear
                        // first, then /graph/apply.

                        std::map<std::wstring, uint32_t> refToId;
                        std::vector<uint32_t> orderedIds;

                        // Resolve a JSON value to a node id: numeric -> id directly,
                        // string -> lookup in refToId. Returns 0 + error on miss.
                        auto resolveNode = [&](WDJ::IJsonValue const& v, const char* what,
                                               uint32_t* outId, std::string* err) -> bool
                        {
                            if (v.ValueType() == WDJ::JsonValueType::Number)
                            {
                                *outId = static_cast<uint32_t>(v.GetNumber());
                                return true;
                            }
                            if (v.ValueType() == WDJ::JsonValueType::String)
                            {
                                auto s = std::wstring(v.GetString());
                                auto it = refToId.find(s);
                                if (it == refToId.end())
                                {
                                    *err = std::string("Unknown ") + what + " ref: " +
                                           WideToUtf8(s);
                                    return false;
                                }
                                *outId = it->second;
                                return true;
                            }
                            *err = std::string("Expected number or string for ") + what;
                            return false;
                        };

                        // Helper: write a JSON value into an EffectNode property,
                        // mirroring /graph/set-property's number/bool/string/array
                        // type-fanout. Used for both the per-node `properties`
                        // block at create time and any later patch. Returns the
                        // error response for a value it cannot store.
                        auto writeProperty = [&](Graph::EffectNode& node,
                            const std::wstring& key, WDJ::IJsonValue const& val) -> std::optional<Response>
                        {
                            switch (val.ValueType())
                            {
                            case WDJ::JsonValueType::Number:
                            {
                                bool isUint = false;
                                if (node.customEffect.has_value())
                                {
                                    for (const auto& p : node.customEffect->parameters)
                                    {
                                        if (p.name == key && p.typeName == L"uint")
                                        { isUint = true; break; }
                                    }
                                }
                                auto existIt = node.properties.find(key);
                                if (existIt != node.properties.end() &&
                                    std::holds_alternative<uint32_t>(existIt->second))
                                    isUint = true;
                                if (isUint)
                                    node.properties[key] = static_cast<uint32_t>(val.GetNumber());
                                else
                                    node.properties[key] = static_cast<float>(val.GetNumber());
                                break;
                            }
                            case WDJ::JsonValueType::Boolean:
                                node.properties[key] = val.GetBoolean();
                                break;
                            case WDJ::JsonValueType::String:
                            {
                                // Same coercion as /graph/set-property: untyped MCP
                                // arg values may arrive stringified, so map "203" /
                                // "true" to the target param's real type instead of
                                // storing a bindability-breaking, shader-starving wstring.
                                std::wstring sval(val.GetString());
                                std::wstring want;
                                if (node.customEffect.has_value())
                                    for (const auto& p : node.customEffect->parameters)
                                        if (p.name == key) { want = p.typeName; break; }
                                if (want.empty())
                                {
                                    auto it = node.properties.find(key);
                                    if (it != node.properties.end())
                                    {
                                        if (std::holds_alternative<float>(it->second)) want = L"float";
                                        else if (std::holds_alternative<uint32_t>(it->second)) want = L"uint";
                                        else if (std::holds_alternative<int32_t>(it->second)) want = L"int";
                                        else if (std::holds_alternative<bool>(it->second)) want = L"bool";
                                    }
                                }
                                if (want.empty() && (key == L"IsPlaying" || key == L"isPlaying"))
                                    want = L"bool";
                                try
                                {
                                    if (want == L"float")     node.properties[key] = std::stof(sval);
                                    else if (want == L"uint") node.properties[key] = static_cast<uint32_t>(std::stoul(sval));
                                    else if (want == L"int")  node.properties[key] = static_cast<int32_t>(std::stol(sval));
                                    else if (want == L"bool")
                                    {
                                        bool bv = false;
                                        if (!CoerceStringToBool(sval, bv))
                                            // Custom delimiter: the message itself ends in `)"`,
                                            // which closes a bare R"(...)" literal early.
                                            return Json(400, R"JSON({"error":"expected a boolean-valued string (true/false, 1/0, on/off, yes/no)"})JSON");
                                        node.properties[key] = bv;
                                    }
                                    else                      node.properties[key] = sval;
                                }
                                // See the matching note in /graph/set-property:
                                // keeping the raw string preserves the write but
                                // lands back in the state the coercion prevents.
                                catch (...) { node.properties[key] = sval; }
                                break;
                            }
                            case WDJ::JsonValueType::Array:
                            {
                                auto arr = val.GetArray();
                                if (arr.Size() == 2)
                                    node.properties[key] = winrt::Windows::Foundation::Numerics::float2{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()) };
                                else if (arr.Size() == 3)
                                    node.properties[key] = winrt::Windows::Foundation::Numerics::float3{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()),
                                        static_cast<float>(arr.GetAt(2).GetNumber()) };
                                else if (arr.Size() == 4)
                                    node.properties[key] = winrt::Windows::Foundation::Numerics::float4{
                                        static_cast<float>(arr.GetAt(0).GetNumber()),
                                        static_cast<float>(arr.GetAt(1).GetNumber()),
                                        static_cast<float>(arr.GetAt(2).GetNumber()),
                                        static_cast<float>(arr.GetAt(3).GetNumber()) };
                                break;
                            }
                            default: break;
                            }

                            // Mirror of the special-case property handling in
                            // /graph/set-property -- preserve runtime fields.
                            if (key == L"isPlaying" || key == L"IsPlaying")
                            {
                                bool play = false;
                                if (CoercePropertyToBool(node.properties[key], play))
                                    node.isPlaying = play;
                            }
                            if (key == L"shaderPath")
                            {
                                if (auto* sv = std::get_if<std::wstring>(&node.properties[key]))
                                    node.shaderPath = *sv;
                            }
                            return std::nullopt;
                        };

                        // ---- nodes -----------------------------------------------
                        if (root.HasKey(L"nodes"))
                        {
                            auto nodesArr = root.GetNamedArray(L"nodes");
                            for (uint32_t i = 0; i < nodesArr.Size(); ++i)
                            {
                                auto entry = nodesArr.GetObjectAt(i);
                                if (!entry.HasKey(L"effect"))
                                    return Json(400,
                                        std::string("{\"error\":\"nodes[") + std::to_string(i) +
                                        "] missing 'effect'\"}");
                                auto effectName = entry.GetNamedString(L"effect");

                                Graph::EffectNode node;
                                bool created = false;

                                if (auto* slDesc = ::ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(effectName))
                                {
                                    node = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*slDesc);
                                    created = true;
                                }
                                else if (auto* eDesc = ::ShaderLab::Effects::EffectRegistry::Instance().FindByName(effectName))
                                {
                                    node = ::ShaderLab::Effects::EffectRegistry::CreateNode(*eDesc);
                                    created = true;
                                }
                                else if (effectName == L"Video Source" || effectName == L"Video")
                                {
                                    std::wstring filePath;
                                    if (entry.HasKey(L"filePath"))
                                        filePath = std::wstring(entry.GetNamedString(L"filePath"));
                                    auto displayName = filePath.empty()
                                        ? std::wstring(L"Video Source")
                                        : filePath.substr(filePath.find_last_of(L"\\/") + 1);
                                    node = ::ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(
                                        filePath, displayName);
                                    created = true;
                                }
                                else if (effectName == L"Image Source" || effectName == L"Image")
                                {
                                    std::wstring filePath;
                                    if (entry.HasKey(L"filePath"))
                                        filePath = std::wstring(entry.GetNamedString(L"filePath"));
                                    auto displayName = filePath.empty()
                                        ? std::wstring(L"Image Source")
                                        : filePath.substr(filePath.find_last_of(L"\\/") + 1);
                                    node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(
                                        filePath, displayName);
                                    created = true;
                                }
                                else if (effectName == L"Output")
                                {
                                    // Output node: graph terminator + sink for an
                                    // OutputWindow on hosts that have one. Engine-pure
                                    // hosts (headless / tests) just keep it as graph
                                    // metadata; the GUI host's OnNodeAdded hook detects
                                    // the new Output and opens a SwapChainPanel-bound
                                    // window for it.
                                    node = ::ShaderLab::Effects::EffectRegistry::CreateOutputNode();
                                    auto outputIds = ctx.graph->GetOutputNodeIds();
                                    node.name = outputIds.empty()
                                        ? std::wstring(L"Output")
                                        : (L"Output " + std::to_wstring(outputIds.size() + 1));
                                    created = true;
                                }

                                if (!created)
                                {
                                    return Json(400,
                                        std::string("{\"error\":\"nodes[") + std::to_string(i) +
                                        "] unknown effect: " + WideToUtf8(effectName) + "\"}");
                                }

                                // Apply per-node properties (after default-construction
                                // so user values override descriptor defaults).
                                if (entry.HasKey(L"properties") &&
                                    entry.GetNamedValue(L"properties").ValueType() == WDJ::JsonValueType::Object)
                                {
                                    auto propsObj = entry.GetNamedObject(L"properties");
                                    for (auto kv : propsObj)
                                    {
                                        if (auto error = writeProperty(node, std::wstring(kv.Key()), kv.Value()))
                                            return *error;
                                    }
                                }

                                auto id = ctx.graph->AddNode(std::move(node));
                                orderedIds.push_back(id);

                                // For Video/Image with filePath, kick off PrepareSourceNode
                                // exactly as /graph/add-node does. (PrepareSourceNode is a
                                // no-op without a context; harmless on headless.)
                                if ((effectName == L"Video Source" || effectName == L"Video" ||
                                     effectName == L"Image Source" || effectName == L"Image") &&
                                    entry.HasKey(L"filePath") && ctx.sourceFactory && ctx.dc)
                                {
                                    if (auto* graphNode = ctx.graph->FindNode(id))
                                        ctx.sourceFactory->PrepareSourceNode(*graphNode,
                                            static_cast<ID2D1DeviceContext5*>(ctx.dc), 0.0,
                                            ctx.d3dDevice, ctx.d3dContext);
                                }

                                if (entry.HasKey(L"ref") &&
                                    entry.GetNamedValue(L"ref").ValueType() == WDJ::JsonValueType::String)
                                {
                                    auto refKey = std::wstring(entry.GetNamedString(L"ref"));
                                    refToId[refKey] = id;
                                }

                                sink.OnNodeAdded(id);
                            }
                        }

                        // ---- edges -----------------------------------------------
                        if (root.HasKey(L"edges"))
                        {
                            auto edgesArr = root.GetNamedArray(L"edges");
                            for (uint32_t i = 0; i < edgesArr.Size(); ++i)
                            {
                                auto e = edgesArr.GetObjectAt(i);
                                if (!e.HasKey(L"from") || !e.HasKey(L"to"))
                                    return Json(400,
                                        std::string("{\"error\":\"edges[") + std::to_string(i) +
                                        "] missing 'from' or 'to'\"}");
                                uint32_t srcId = 0, dstId = 0;
                                std::string err;
                                if (!resolveNode(e.GetNamedValue(L"from"), "edge source", &srcId, &err))
                                    return Json(400, "{\"error\":\"" + err + "\"}");
                                if (!resolveNode(e.GetNamedValue(L"to"), "edge dest", &dstId, &err))
                                    return Json(400, "{\"error\":\"" + err + "\"}");
                                uint32_t srcPin = e.HasKey(L"fromPin")
                                    ? static_cast<uint32_t>(e.GetNamedNumber(L"fromPin")) : 0;
                                uint32_t dstPin = e.HasKey(L"toPin")
                                    ? static_cast<uint32_t>(e.GetNamedNumber(L"toPin")) : 0;
                                if (!ctx.graph->Connect(srcId, srcPin, dstId, dstPin))
                                    return Json(400,
                                        std::string("{\"error\":\"edges[") + std::to_string(i) +
                                        "] Connect failed (cycle or invalid pin)\"}");
                            }
                        }

                        // ---- bindings --------------------------------------------
                        if (root.HasKey(L"bindings"))
                        {
                            auto bArr = root.GetNamedArray(L"bindings");
                            for (uint32_t i = 0; i < bArr.Size(); ++i)
                            {
                                auto b = bArr.GetObjectAt(i);
                                if (!b.HasKey(L"node") || !b.HasKey(L"property") || !b.HasKey(L"from"))
                                    return Json(400,
                                        std::string("{\"error\":\"bindings[") + std::to_string(i) +
                                        "] missing one of node/property/from\"}");
                                uint32_t dstId = 0;
                                std::string err;
                                if (!resolveNode(b.GetNamedValue(L"node"), "binding node", &dstId, &err))
                                    return Json(400, "{\"error\":\"" + err + "\"}");
                                auto prop = std::wstring(b.GetNamedString(L"property"));

                                // `from` is "ref.fieldName" (string) or { "node": ..., "field": ... }.
                                uint32_t srcId = 0;
                                std::wstring fieldName;
                                auto fromVal = b.GetNamedValue(L"from");
                                if (fromVal.ValueType() == WDJ::JsonValueType::String)
                                {
                                    auto s = std::wstring(fromVal.GetString());
                                    auto dot = s.find(L'.');
                                    if (dot == std::wstring::npos)
                                        return Json(400,
                                            std::string("{\"error\":\"bindings[") + std::to_string(i) +
                                            "] 'from' must be 'ref.fieldName'\"}");
                                    auto refPart = s.substr(0, dot);
                                    fieldName = s.substr(dot + 1);
                                    auto it = refToId.find(refPart);
                                    if (it == refToId.end())
                                    {
                                        // Maybe it's a numeric ID baked in.
                                        try { srcId = static_cast<uint32_t>(std::stoul(WideToUtf8(refPart))); }
                                        catch (...)
                                        {
                                            return Json(400,
                                                "{\"error\":\"Unknown binding source ref: " +
                                                WideToUtf8(refPart) + "\"}");
                                        }
                                    }
                                    else
                                    {
                                        srcId = it->second;
                                    }
                                }
                                else if (fromVal.ValueType() == WDJ::JsonValueType::Object)
                                {
                                    auto fromObj = fromVal.GetObject();
                                    if (!fromObj.HasKey(L"node") || !fromObj.HasKey(L"field"))
                                        return Json(400,
                                            std::string("{\"error\":\"bindings[") + std::to_string(i) +
                                            "] 'from' object missing node/field\"}");
                                    if (!resolveNode(fromObj.GetNamedValue(L"node"),
                                                     "binding source", &srcId, &err))
                                        return Json(400, "{\"error\":\"" + err + "\"}");
                                    fieldName = std::wstring(fromObj.GetNamedString(L"field"));
                                }
                                else
                                {
                                    return Json(400,
                                        std::string("{\"error\":\"bindings[") + std::to_string(i) +
                                        "] 'from' must be string or object\"}");
                                }

                                uint32_t component = b.HasKey(L"component")
                                    ? static_cast<uint32_t>(b.GetNamedNumber(L"component")) : 0;

                                auto bindErr = ctx.graph->BindProperty(
                                    dstId, prop, srcId, fieldName, component);
                                if (!bindErr.empty())
                                    return Json(400,
                                        std::string("{\"error\":\"bindings[") + std::to_string(i) +
                                        "]: " + WideToUtf8(bindErr) + "\"}");
                            }
                        }

                        // Every write above dirtied what it touched: new nodes start
                        // dirty, Connect and BindProperty dirty their consumer.
                        sink.OnGraphStructureChanged();

                        // ---- previewNode (optional) ------------------------------
                        // We deliberately don't poke a preview-node id from inside
                        // this engine route -- preview is a host-side UI concept
                        // (see /render/preview-node in the gui host's MCP routes).
                        // Callers wanting a preview should make a follow-up call.

                        // Build response: { ok, refToId: {...}, nodeIds: [...] }
                        std::string out = "{\"ok\":true,\"nodeIds\":[";
                        for (size_t i = 0; i < orderedIds.size(); ++i)
                        {
                            if (i) out += ",";
                            out += std::to_string(orderedIds[i]);
                        }
                        out += "],\"refToId\":{";
                        bool first = true;
                        for (const auto& [k, v] : refToId)
                        {
                            if (!first) out += ",";
                            out += "\"" + JsonEscape(WideToUtf8(k)) + "\":" + std::to_string(v);
                            first = false;
                        }
                        out += "}}";
                        return Json(200, out);
                    });
                });
        }


        void RegisterPixelRegion(McpRouter& server, IEngineCommandSink& sink)
        {
            // POST /render/pixel-region -- Read FP32 RGBA pixel grid.
            // Body: { nodeId, x, y, w, h }   (capped at 32x32 = 1024 pixels)
            server.AddRoute(L"POST", L"/render/pixel-region",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
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

                        // Cap region area at 64 per axis / 1024 total (1024 pixels). Per-axis cap of
                        // 64 lets the agent ask for a thin strip (e.g. 64x4) but
                        // never more than 1024 total samples.
                        if (w == 0 || h == 0)
                            return Json(400, R"({"error":"w and h must be > 0"})");
                        if (w > 64 || h > 64 || (w * h) > 1024)
                            return Json(400,
                                "{\"error\":\"Region too large (cap: each axis <= 64, total area <= 1024)\"}");

                        // Force a fresh frame so dirty nodes evaluate before
                        // readback. Headless sets renderFrame to runEval, so
                        // this is a full evaluation, not a no-op -- load-bearing
                        // for anyone counting evaluations in a probe script.
                        RenderFrameFor(ctx, nodeId);

                        auto rr = Rendering::ReadPixelRegion(*ctx.graph, nodeId, x, y, w, h, ctx.dc);
                        if (rr.status != Rendering::ReadPixelRegionStatus::Success &&
                            rr.status != Rendering::ReadPixelRegionStatus::NotFound)
                        {
                            if (auto compiling = CompilingResponse(*ctx.graph, nodeId))
                                return *compiling;
                        }
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
                        char buf[64];
                        for (size_t i = 0; i < rr.pixels.size(); ++i)
                        {
                            if (i) json += ",";
                            const float v = rr.pixels[i];
                            if (!std::isfinite(v))
                            {
                                // JSON has no Infinity/NaN literals, and "%.6f"
                                // emits the bare C tokens nan / inf / -inf. The
                                // route still returned 200, so a caller got a
                                // body that parsed as a STRING (or threw) while
                                // looking like a successful read -- measurement
                                // degraded silently, which is exactly how a
                                // whole run of plausible wrong numbers happens.
                                // null is the JSON-legal signal for "no value".
                                json += "null";
                                continue;
                            }
                            int n = std::snprintf(buf, sizeof(buf), "%.6f", v);
                            // snprintf returns what it WOULD have written, not
                            // what it did. Appending that count after a
                            // truncated write reads past the buffer.
                            if (n < 0) n = 0;
                            else if (n >= static_cast<int>(sizeof(buf)))
                                n = static_cast<int>(sizeof(buf)) - 1;
                            json.append(buf, static_cast<size_t>(n));
                        }
                        json += "]}";
                        return Json(200, json);
                    });
                });
        }
        // ---- GET /graph — full graph state, /graph/save, /graph/node/{id} -
        // The host that wants /graph to surface a "previewNodeId" provides
        // ctx.getPreviewNodeId. Headless leaves it null and we emit 0,
        // which matches "no preview pane" semantics.
        void RegisterGetGraph(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/graph",
                [&sink](const std::wstring& path, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([&path](EngineContext& ctx) -> Response {
                        // /graph/save -> raw graph JSON via EffectGraph::ToJson.
                        if (path == L"/graph/save")
                        {
                            auto json = ctx.graph->ToJson();
                            return Json(200, WideToUtf8(std::wstring(json)));
                        }
                        // /graph/node/{id} -> single node detail.
                        if (path.starts_with(L"/graph/node/"))
                        {
                            auto idStr = path.substr(12);
                            uint32_t nodeId = 0;
                            try { nodeId = static_cast<uint32_t>(std::stoul(idStr)); }
                            catch (...) { return Json(400, R"({"error":"Invalid node ID"})"); }
                            auto* node = ctx.graph->FindNode(nodeId);
                            if (!node) return Json(404, R"({"error":"Node not found"})");
                            return Json(200, NodeToJson(*node));
                        }

                        // /graph -> full graph state.
                        std::string json = "{\"nodes\":[";
                        bool first = true;
                        for (const auto& node : ctx.graph->Nodes())
                        {
                            if (!first) json += ",";
                            json += NodeToJson(node);
                            first = false;
                        }
                        json += "],\"edges\":[";
                        first = true;
                        for (const auto& edge : ctx.graph->Edges())
                        {
                            if (!first) json += ",";
                            json += std::format(
                                "{{\"srcId\":{},\"srcPin\":{},\"dstId\":{},\"dstPin\":{}}}",
                                edge.sourceNodeId, edge.sourcePin,
                                edge.destNodeId, edge.destPin);
                            first = false;
                        }
                        uint32_t previewId = ctx.getPreviewNodeId ? ctx.getPreviewNodeId() : 0;
                        json += std::format("],\"previewNodeId\":{}}}", previewId);
                        return Json(200, json);
                    });
                });
        }

        // ---- GET /custom-effects — all nodes with a customEffect def -----
        void RegisterCustomEffects(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/custom-effects",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> Response {
                        std::string json = "[";
                        bool first = true;
                        for (const auto& node : ctx.graph->Nodes())
                        {
                            if (!node.customEffect.has_value()) continue;
                            if (!first) json += ",";
                            json += NodeToJson(node);
                            first = false;
                        }
                        json += "]";
                        return Json(200, json);
                    });
                });
        }

        // ---- GET /analysis/{id} — analysis output fields ------------------
        void RegisterAnalysisOutput(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/analysis/",
                [&sink](const std::wstring& path, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([&path](EngineContext& ctx) -> Response {
                        auto rest = path.substr(10); // after "/analysis/"
                        uint32_t nodeId = 0;
                        try { nodeId = static_cast<uint32_t>(std::stoul(rest)); }
                        catch (...) { return Json(400, R"({"error":"Invalid node ID"})"); }
                        auto* node = ctx.graph->FindNode(nodeId);
                        if (!node) return Json(404, R"({"error":"Node not found"})");

                        // Phase 8c: ensure freshness. If the host has the
                        // skip-readback flag enabled, the node's
                        // analysisOutput.fields may be stale by 1+ frames.
                        // Temporarily disable the flag, force a render,
                        // then restore. Cost: one frame of full readback
                        // per MCP analysis read (acceptable given MCP
                        // calls are out-of-band and infrequent).
                        //
                        // Clearing the skip flag is necessary but NOT
                        // sufficient: the readback happens inside the node's
                        // DISPATCH, so a forced frame in which the node is
                        // clean re-renders, dispatches nothing, and copies
                        // nothing back. The route then returned the values
                        // from whatever frame the node last dispatched with
                        // readback on -- the first one -- at 200 OK, forever.
                        //
                        // It looked convincing: every field present, every
                        // number plausible. Measured tell: changing the SOURCE
                        // node's PatternSize from 512 to 2048 left `Samples`
                        // pinned at 1048576, which is arithmetically
                        // impossible for a live reduction.
                        //
                        // So dirty the node too, and make the forced frame
                        // actually recompute what it is about to report.
                        const bool prevSkip =
                            ::ShaderLab::Performance::IsSkipUnneededCpuReadbackEnabled();
                        if (prevSkip && ctx.renderFrame)
                        {
                            ::ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
                            node->dirty = true;
                            ctx.renderFrame();
                            ::ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(prevSkip);
                            // Re-resolve: the graph could have changed.
                            node = ctx.graph->FindNode(nodeId);
                            if (!node) return Json(404, R"({"error":"Node not found"})");
                        }

                        using AOT = ::ShaderLab::Graph::AnalysisOutputType;
                        if (node->analysisOutput.type != AOT::Typed ||
                            node->analysisOutput.fields.empty())
                            return Json(200, R"({"fields":[]})");

                        std::string json = R"({"fields":[)";
                        bool first = true;
                        for (const auto& fv : node->analysisOutput.fields)
                        {
                            if (!first) json += ",";
                            first = false;
                            json += "{\"name\":\"" + JsonEscape(WideToUtf8(fv.name)) + "\"";
                            json += ",\"type\":\"" + std::string(AnalysisFieldTypeStr(fv.type)) + "\"";
                            if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                            {
                                uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                                json += ",\"value\":[";
                                for (uint32_t c = 0; c < cc; ++c)
                                {
                                    if (c > 0) json += ",";
                                    json += JsonFloat(fv.components[c]);
                                }
                                json += "]";
                            }
                            else
                            {
                                uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                                uint32_t count = stride > 0
                                    ? static_cast<uint32_t>(fv.arrayData.size()) / stride
                                    : 0;
                                json += ",\"count\":" + std::to_string(count);
                                json += ",\"value\":[";
                                for (size_t i = 0; i < fv.arrayData.size(); ++i)
                                {
                                    if (i > 0) json += ",";
                                    json += JsonFloat(fv.arrayData[i]);
                                }
                                json += "]";
                            }
                            json += "}";
                        }
                        json += "]}";
                        return Json(200, json);
                    });
                });
        }
        // ---- POST /render/image-bounds — return raw GetImageLocalBounds -----
        // Body: { nodeId }. Returns { width, height } at 96 DPI in pixels.
        // Useful for diagnosing rect-bloat issues that the capture-node
        // route hides via its maxDim clamp.
        void RegisterImageBounds(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/render/image-bounds",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body](EngineContext& ctx) -> Response {
                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");
                        if (!jo.HasKey(L"nodeId"))
                            return Json(400, R"({"error":"'nodeId' is required"})");
                        uint32_t nodeId = static_cast<uint32_t>(jo.GetNamedNumber(L"nodeId"));
                        RenderFrameFor(ctx, nodeId);
                        auto* node = ctx.graph->FindNode(nodeId);
                        if (!node || !node->cachedOutput)
                            return Json(404, R"({"error":"Node not ready"})");
                        float oldDpiX = 0, oldDpiY = 0;
                        ctx.dc->GetDpi(&oldDpiX, &oldDpiY);
                        ctx.dc->SetDpi(96.0f, 96.0f);
                        D2D1_RECT_F bounds{};
                        ctx.dc->GetImageLocalBounds(node->cachedOutput, &bounds);
                        ctx.dc->SetDpi(oldDpiX, oldDpiY);
                        return Json(200, std::format(
                            R"({{"left":{},"top":{},"right":{},"bottom":{},"width":{},"height":{}}})",
                            bounds.left, bounds.top, bounds.right, bounds.bottom,
                            bounds.right - bounds.left, bounds.bottom - bounds.top));
                    });
                });
        }

        // ---- POST /render/capture-node — render any node to PNG ------------
        // Body: { nodeId, inline?:bool }. Saves PNG to %TEMP%; returns
        // path + size. If inline=true, also returns a base64 PNG payload.
        // Uses Rendering::CaptureNodeAsPng so this route is identical
        // between GUI and headless hosts.
        void RegisterCaptureNode(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/render/capture-node",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body](EngineContext& ctx) -> Response {
                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");
                        if (!jo.HasKey(L"nodeId"))
                            return Json(400, R"({"error":"'nodeId' is required"})");
                        uint32_t nodeId = static_cast<uint32_t>(jo.GetNamedNumber(L"nodeId"));
                        bool wantInline = jo.HasKey(L"inline")
                            && jo.GetNamedValue(L"inline").ValueType() == WDJ::JsonValueType::Boolean
                            && jo.GetNamedBoolean(L"inline");
                        // Optional maxDim: fit the longer edge to this many px
                        // (aspect preserved). Smaller = lower-res preview =
                        // fewer inline tokens; omit for the 2048 default.
                        uint32_t maxDim = 2048;
                        if (jo.HasKey(L"maxDim")
                            && jo.GetNamedValue(L"maxDim").ValueType() == WDJ::JsonValueType::Number)
                            maxDim = std::clamp(
                                static_cast<uint32_t>(jo.GetNamedNumber(L"maxDim")), 32u, 8192u);

                        // Force a fresh frame so dirty nodes evaluate before
                        // capture. Headless sets renderFrame to runEval: a full evaluation.
                        RenderFrameFor(ctx, nodeId);

                        auto cap = ::ShaderLab::Rendering::CaptureNodeAsPng(
                            *ctx.graph, nodeId, ctx.dc, maxDim);
                        using S = ::ShaderLab::Rendering::CaptureNodeStatus;
                        if (cap.status != S::Success && cap.status != S::NotFound)
                        {
                            if (auto compiling = CompilingResponse(*ctx.graph, nodeId))
                                return *compiling;
                        }
                        switch (cap.status)
                        {
                        case S::NotFound:
                            return Json(404, std::format(
                                R"({{"error":"Node {} not found"}})", nodeId));
                        case S::NotReady:
                            return Json(409, std::format(
                                R"({{"error":"Node {} is not yet evaluated","notReady":true}})", nodeId));
                        case S::EmptyImage:
                            return Json(500, R"({"error":"Capture failed: empty image"})");
                        case S::D2DError:
                            return Json(500, R"({"error":"Capture failed"})");
                        case S::Success:
                            break;
                        }

                        // Persist to %TEMP% with PID + nodeId + monotonic seq
                        // so concurrent captures don't collide.
                        static std::atomic<uint32_t> s_seq{ 0 };
                        uint32_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
                        wchar_t tempPath[MAX_PATH]{};
                        ::GetTempPathW(MAX_PATH, tempPath);
                        std::wstring filePath = std::format(
                            L"{}shaderlab_node_{}_{}_{}.png",
                            tempPath, ::GetCurrentProcessId(), nodeId, seq);
                        HANDLE hFile = ::CreateFileW(filePath.c_str(),
                            GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hFile == INVALID_HANDLE_VALUE)
                            return Json(500, R"({"error":"Failed to create temp file"})");
                        DWORD written = 0;
                        ::WriteFile(hFile, cap.png.data(),
                            static_cast<DWORD>(cap.png.size()), &written, nullptr);
                        ::CloseHandle(hFile);

                        std::string escapedPath = JsonEscape(WideToUtf8(filePath));
                        if (wantInline)
                        {
                            auto b64 = Base64Encode(cap.png.data(), cap.png.size());
                            return Json(200, std::format(
                                R"({{"path":"{}","size":{},"nodeId":{},"width":{},"height":{},"mimeType":"image/png","base64":"{}"}})",
                                escapedPath, cap.png.size(), nodeId,
                                cap.width, cap.height, b64));
                        }
                        return Json(200, std::format(
                            R"({{"path":"{}","size":{},"nodeId":{},"width":{},"height":{},"mimeType":"image/png"}})",
                            escapedPath, cap.png.size(), nodeId,
                            cap.width, cap.height));
                    });
                });
        }
        // ---- POST /effect/compile — recompile a custom-effect node's HLSL -
        // Body: { nodeId, hlsl, analysisFields?:[...] }
        // Compiles HLSL via D3DCompile (ps_5_0 or cs_5_0 based on the
        // node's existing shaderType), then applies the new bytecode +
        // generates a new shaderGuid + updates analysis fields. Fires
        // OnCustomEffectRecompiled so the GUI rebuilds the canvas
        // layout (parameter pins may have changed) and Add Node flyout.
        // Mirrors EffectDesignerWindow's "Update in Graph" path.
        void RegisterCompileEffect(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/effect/compile",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        try
                        {
                            auto jobj = WDJ::JsonObject::Parse(winrt::to_hstring(body));
                            uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                            auto hlsl = std::wstring(jobj.GetNamedString(L"hlsl"));

                            // Normalize line endings before D3DCompile.
                            std::string hlslUtf8 = WideToUtf8(hlsl);
                            for (auto& ch : hlslUtf8) { if (ch == '\r') ch = '\n'; }

                            auto* node = ctx.graph->FindNode(nodeId);
                            if (!node || !node->customEffect.has_value())
                                return Json(404,
                                    R"({"error":"Custom effect node not found"})");

                            using CST = ::ShaderLab::Graph::CustomShaderType;
                            std::string target =
                                (node->customEffect->shaderType == CST::PixelShader)
                                ? "ps_5_0" : "cs_5_0";
                            // Compile the generic build only; the evaluator
                            // builds the variants from this definition afterwards.
                            const auto macroDefs =
                                ::ShaderLab::Effects::GenericVariantMacros(*node->customEffect);
                            std::vector<::ShaderLab::Effects::ShaderCompiler::MacroDef> macros;
                            for (const auto& [name, value] : macroDefs) macros.push_back({ name.c_str(), value.c_str() });
                            auto result = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
                                hlslUtf8, "McpCompile", "main", target, macros);

                            if (!result.succeeded)
                            {
                                std::string err = JsonEscape(WideToUtf8(result.ErrorMessage()));
                                return Json(200, std::format(
                                    "{{\"compiled\":false,\"error\":\"{}\"}}", err));
                            }

                            auto* blob = result.bytecode.get();
                            std::vector<uint8_t> bytecode(blob->GetBufferSize());
                            std::memcpy(bytecode.data(), blob->GetBufferPointer(),
                                blob->GetBufferSize());

                            // Optional analysisFields update.
                            using AFT = ::ShaderLab::Graph::AnalysisFieldType;
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
                                    if (typeTag == L"float")        fd.type = AFT::Float;
                                    else if (typeTag == L"float2")  fd.type = AFT::Float2;
                                    else if (typeTag == L"float3")  fd.type = AFT::Float3;
                                    else if (typeTag == L"float4")  fd.type = AFT::Float4;
                                    else if (typeTag == L"floatarray")  fd.type = AFT::FloatArray;
                                    else if (typeTag == L"float2array") fd.type = AFT::Float2Array;
                                    else if (typeTag == L"float3array") fd.type = AFT::Float3Array;
                                    else if (typeTag == L"float4array") fd.type = AFT::Float4Array;
                                    if (fobj.HasKey(L"length"))
                                        fd.arrayLength = static_cast<uint32_t>(
                                            fobj.GetNamedNumber(L"length"));
                                    newFields.push_back(std::move(fd));
                                }
                            }

                            // Apply: bytecode + fresh shaderGuid + optional fields.
                            auto& def = node->customEffect.value();
                            def.hlslSource = hlsl;
                            def.compiledBytecode = std::move(bytecode);
                            ::CoCreateGuid(&def.shaderGuid);
                            if (hasAnalysisFields)
                            {
                                def.analysisFields = std::move(newFields);
                                def.analysisOutputType = def.analysisFields.empty()
                                    ? ::ShaderLab::Graph::AnalysisOutputType::None
                                    : ::ShaderLab::Graph::AnalysisOutputType::Typed;
                            }

                            node->dirty = true;
                            ctx.evaluator->UpdateNodeShader(nodeId, *node);

                            // Auto-rename if another custom-effect node has the
                            // same display name but different HLSL. Same
                            // policy MainWindow::EnforceCustomEffectNameUniqueness
                            // applies to the EffectDesigner "Update in Graph"
                            // path: append " (N)" suffix until unique.
                            const auto& modHlsl = def.hlslSource;
                            const auto& modName = node->name;
                            bool conflict = false;
                            for (const auto& other : ctx.graph->Nodes())
                            {
                                if (other.id == nodeId) continue;
                                if (other.name != modName) continue;
                                if (!other.customEffect.has_value()) continue;
                                if (other.customEffect->hlslSource != modHlsl)
                                {
                                    conflict = true;
                                    break;
                                }
                            }
                            if (conflict)
                            {
                                std::wstring baseName = modName;
                                auto parenPos = baseName.rfind(L" (");
                                if (parenPos != std::wstring::npos && baseName.back() == L')')
                                    baseName = baseName.substr(0, parenPos);
                                for (int suffix = 2; suffix < 100; ++suffix)
                                {
                                    std::wstring candidate = baseName + L" ("
                                        + std::to_wstring(suffix) + L")";
                                    bool taken = false;
                                    for (const auto& other : ctx.graph->Nodes())
                                    {
                                        if (other.id == nodeId) continue;
                                        if (other.name == candidate &&
                                            other.customEffect.has_value() &&
                                            other.customEffect->hlslSource != modHlsl)
                                        {
                                            taken = true;
                                            break;
                                        }
                                    }
                                    if (!taken)
                                    {
                                        node->name = candidate;
                                        break;
                                    }
                                }
                            }

                            sink.OnCustomEffectRecompiled(nodeId);

                            return Json(200, std::format(
                                "{{\"compiled\":true,\"bytecodeSize\":{}}}",
                                def.compiledBytecode.size()));
                        }
                        catch (...)
                        {
                            return Json(400, R"({"error":"Invalid request"})");
                        }
                    });
                });
        }
        // ---- Display profile routes ---------------------------------------
        // Shared serializer matching the byte format the MainWindow
        // routes used to emit (so MCP consumers don't see schema drift).
        std::string SerializeProfile(const ::ShaderLab::Rendering::DisplayProfile& p)
        {
            return std::format(
                "{{\"name\":\"{}\",\"hdrEnabled\":{},\"bitsPerColor\":{}"
                ",\"sdrWhiteNits\":{:.2f},\"peakNits\":{:.2f}"
                ",\"minNits\":{:.4f},\"maxFullFrameNits\":{:.2f}"
                ",\"gamut\":\"{}\",\"isSimulated\":{}"
                ",\"primaryRed\":[{:.4f},{:.4f}]"
                ",\"primaryGreen\":[{:.4f},{:.4f}]"
                ",\"primaryBlue\":[{:.4f},{:.4f}]"
                ",\"whitePoint\":[{:.4f},{:.4f}]}}",
                JsonEscape(WideToUtf8(p.profileName)),
                p.caps.hdrEnabled ? "true" : "false",
                p.caps.bitsPerColor,
                p.caps.sdrWhiteLevelNits, p.caps.maxLuminanceNits,
                p.caps.minLuminanceNits, p.caps.maxFullFrameLuminanceNits,
                JsonEscape(WideToUtf8(::ShaderLab::Rendering::GamutIdToString(p.gamut))),
                p.isSimulated ? "true" : "false",
                p.primaryRed.x, p.primaryRed.y,
                p.primaryGreen.x, p.primaryGreen.y,
                p.primaryBlue.x, p.primaryBlue.y,
                p.whitePoint.x, p.whitePoint.y);
        }

        // GET /display/profiles  — All built-in presets + active + live.
        void RegisterGetDisplayProfiles(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"GET", L"/display/profiles",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([](EngineContext& ctx) -> Response {
                        auto presets = ::ShaderLab::Rendering::AllPresets();
                        std::string json = "{\"presets\":[";
                        for (size_t i = 0; i < presets.size(); ++i)
                        {
                            if (i) json += ",";
                            json += "{\"index\":" + std::to_string(i) + ",\"profile\":";
                            json += SerializeProfile(presets[i]);
                            json += "}";
                        }
                        json += "],\"active\":" + SerializeProfile(ctx.displayMonitor->ActiveProfile());
                        json += ",\"live\":"  + SerializeProfile(ctx.displayMonitor->LiveProfile());
                        json += ",\"isSimulated\":";
                        json += (ctx.displayMonitor->IsSimulated() ? "true" : "false");
                        if (ctx.getLoadedIccProfile)
                        {
                            auto icc = ctx.getLoadedIccProfile();
                            if (icc.has_value())
                                json += ",\"loadedIcc\":" + SerializeProfile(icc.value());
                        }
                        json += "}";
                        return Json(200, json);
                    });
                });
        }

        // POST /display/profile — apply a simulated profile.
        // Body: exactly one of {preset, presetIndex, iccPath, custom}.
        void RegisterSetDisplayProfile(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/display/profile",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    return sink.Dispatch([&body, &sink](EngineContext& ctx) -> Response {
                        using namespace ::ShaderLab::Rendering;

                        WDJ::JsonObject jo{ nullptr };
                        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jo))
                            return Json(400, R"({"error":"Invalid JSON body"})");

                        int count = 0;
                        if (jo.HasKey(L"preset"))      ++count;
                        if (jo.HasKey(L"presetIndex")) ++count;
                        if (jo.HasKey(L"iccPath"))     ++count;
                        if (jo.HasKey(L"custom"))      ++count;
                        if (count != 1)
                            return Json(400, R"({"error":"Specify exactly one of: preset, presetIndex, iccPath, custom"})");

                        std::optional<DisplayProfile> chosen;

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
                                return Json(400, std::format(
                                    R"({{"error":"Unknown preset: {}"}})",
                                    JsonEscape(WideToUtf8(name))));
                            chosen = p;
                        }
                        else if (jo.HasKey(L"presetIndex"))
                        {
                            auto idx = static_cast<size_t>(jo.GetNamedNumber(L"presetIndex"));
                            auto presets = AllPresets();
                            if (idx >= presets.size())
                                return Json(400, std::format(
                                    "{{\"error\":\"presetIndex out of range (0-{})\"}}",
                                    presets.size() - 1));
                            chosen = presets[idx];
                        }
                        else if (jo.HasKey(L"iccPath"))
                        {
                            auto path = std::wstring(jo.GetNamedString(L"iccPath"));
                            if (!std::filesystem::exists(path))
                                return Json(400, std::format(
                                    R"({{"error":"ICC file not found: {}"}})",
                                    JsonEscape(WideToUtf8(path))));
                            auto parsed = IccProfileParser::LoadFromFile(path);
                            if (!parsed.has_value() || !parsed->valid)
                                return Json(400, std::format(
                                    R"({{"error":"Failed to parse ICC profile: {}"}})",
                                    JsonEscape(WideToUtf8(path))));
                            chosen = DisplayProfileFromIcc(parsed.value());
                            if (ctx.setLoadedIccProfile)
                                ctx.setLoadedIccProfile(chosen.value());
                        }
                        else  // custom
                        {
                            auto co = jo.GetNamedObject(L"custom");
                            DisplayProfile p{};
                            p.isSimulated = true;

                            if (co.HasKey(L"name"))
                                p.profileName = std::wstring(co.GetNamedString(L"name"));
                            else
                                p.profileName = L"Custom MCP profile";

                            p.caps.hdrEnabled = co.HasKey(L"hdrEnabled") && co.GetNamedBoolean(L"hdrEnabled");
                            p.caps.sdrWhiteLevelNits = co.HasKey(L"sdrWhiteNits")
                                ? static_cast<float>(co.GetNamedNumber(L"sdrWhiteNits"))
                                : (p.caps.hdrEnabled ? 203.0f : 80.0f);
                            if (!co.HasKey(L"peakNits"))
                                return Json(400, R"({"error":"custom profile requires 'peakNits'"})");
                            p.caps.maxLuminanceNits = static_cast<float>(co.GetNamedNumber(L"peakNits"));
                            p.caps.minLuminanceNits = co.HasKey(L"minNits")
                                ? static_cast<float>(co.GetNamedNumber(L"minNits")) : 0.5f;
                            p.caps.maxFullFrameLuminanceNits = co.HasKey(L"maxFullFrameNits")
                                ? static_cast<float>(co.GetNamedNumber(L"maxFullFrameNits"))
                                : p.caps.maxLuminanceNits;

                            auto readChroma = [&](const wchar_t* key, ChromaticityXY& dst) -> bool {
                                if (!co.HasKey(key)) return true;
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
                                return Json(400, R"({"error":"primaries / whitePoint must be 2-element arrays"})");

                            // Default the gamut from the primaries (sRGB
                            // struct defaults classify as sRGB) so the
                            // stamp below doesn't misread a plain-sRGB
                            // custom as wide-gamut; an explicit "gamut"
                            // key still overrides. Mirrors the ICC path.
                            p.gamut = DetectGamut(p.primaryRed, p.primaryGreen, p.primaryBlue);
                            if (co.HasKey(L"gamut"))
                            {
                                auto gn = std::wstring(co.GetNamedString(L"gamut"));
                                if      (gn == L"sRGB")    p.gamut = GamutId::sRGB;
                                else if (gn == L"DCI-P3"  || gn == L"P3" || gn == L"DCI_P3")    p.gamut = GamutId::DCI_P3;
                                else if (gn == L"BT.2020" || gn == L"BT2020" || gn == L"Rec2020") p.gamut = GamutId::BT2020;
                                else                       p.gamut = GamutId::Custom;
                            }
                            // Stamp coherent activeColorMode / *Supported /
                            // *UserEnabled / bitsPerColor — without this a
                            // custom HDR profile reported ActiveColorMode=0
                            // (SDR) through the Working Space node while
                            // get_display_info said hdr:true.
                            StampSimulatedColorMode(p);
                            chosen = p;
                        }

                        ctx.displayMonitor->SetSimulatedProfile(chosen.value());
                        ::ShaderLab::Rendering::UpdateWorkingSpaceNodes(*ctx.graph, *ctx.displayMonitor);
                        sink.OnDisplayProfileChanged();

                        auto active = ctx.displayMonitor->ActiveProfile();
                        return Json(200, std::format(
                            R"({{"ok":true,"applied":"{}","hdrEnabled":{},"peakNits":{:.2f}}})",
                            JsonEscape(WideToUtf8(active.profileName)),
                            active.caps.hdrEnabled ? "true" : "false",
                            active.caps.maxLuminanceNits));
                    });
                });
        }

        // POST /display/profile/clear — revert to the live OS profile.
        void RegisterClearDisplayProfile(McpRouter& server, IEngineCommandSink& sink)
        {
            server.AddRoute(L"POST", L"/display/profile/clear",
                [&sink](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    return sink.Dispatch([&sink](EngineContext& ctx) -> Response {
                        ctx.displayMonitor->ClearSimulatedProfile();
                        ::ShaderLab::Rendering::UpdateWorkingSpaceNodes(*ctx.graph, *ctx.displayMonitor);
                        sink.OnDisplayProfileChanged();
                        return Json(200, R"({"ok":true,"isSimulated":false})");
                    });
                });
        }
    }

    void RegisterEngineRoutes(McpRouter& server, IEngineCommandSink& sink)
    {
        RegisterRegistry(server);
        RegisterListEffects(server);
        RegisterGraphOverview(server, sink);
        RegisterDisplayInfo(server, sink);
        RegisterEffectHlsl(server, sink);
        RegisterAddNode(server, sink);
        RegisterRemoveNode(server, sink);
        RegisterConnect(server, sink);
        RegisterDisconnect(server, sink);
        RegisterBindProperty(server, sink);
        RegisterUnbindProperty(server, sink);
        RegisterClear(server, sink);
        RegisterLoad(server, sink);
        RegisterLoadFile(server, sink);
        RegisterSaveFile(server, sink);
        RegisterSetProperty(server, sink);
        RegisterApply(server, sink);
        RegisterPixelRegion(server, sink);
        RegisterGetGraph(server, sink);
        RegisterCustomEffects(server, sink);
        RegisterAnalysisOutput(server, sink);
        RegisterImageBounds(server, sink);
        RegisterCaptureNode(server, sink);
        RegisterCompileEffect(server, sink);
        RegisterGetDisplayProfiles(server, sink);
        RegisterSetDisplayProfile(server, sink);
        RegisterClearDisplayProfile(server, sink);
    }
}
