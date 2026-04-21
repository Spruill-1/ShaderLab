#include "pch.h"
#include "MainWindow.xaml.h"
#include "ShaderLab/McpHttpServer.h"

// Helper: narrow string from wide string.
static std::string ToUtf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), len, nullptr, nullptr);
    return s;
}

static std::string GuidToString(const GUID& g)
{
    wchar_t buf[64]{};
    StringFromGUID2(g, buf, 64);
    return ToUtf8(buf);
}

// Helper: serialize a PropertyValue to a JSON fragment string.
static std::string PropertyValueToJson(const ::ShaderLab::Graph::PropertyValue& pv)
{
    return std::visit([](const auto& v) -> std::string
    {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, float>)
            return std::format("{:.6f}", v);
        else if constexpr (std::is_same_v<T, int32_t>)
            return std::format("{}", v);
        else if constexpr (std::is_same_v<T, uint32_t>)
            return std::format("{}", v);
        else if constexpr (std::is_same_v<T, bool>)
            return v ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::wstring>)
        {
            auto s = ToUtf8(v);
            // Escape quotes.
            std::string escaped;
            for (char c : s) { if (c == '"') escaped += "\\\""; else escaped += c; }
            return "\"" + escaped + "\"";
        }
        else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
            return std::format("[{:.6f},{:.6f}]", v.x, v.y);
        else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
            return std::format("[{:.6f},{:.6f},{:.6f}]", v.x, v.y, v.z);
        else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
            return std::format("[{:.6f},{:.6f},{:.6f},{:.6f}]", v.x, v.y, v.z, v.w);
        else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
            return "\"<matrix>\"";
        else if constexpr (std::is_same_v<T, std::vector<float>>)
            return "\"<curve>\"";
        else
            return "null";
    }, pv);
}

static std::string McpNodeTypeStr(::ShaderLab::Graph::NodeType t)
{
    switch (t)
    {
    case ::ShaderLab::Graph::NodeType::Source:        return "Source";
    case ::ShaderLab::Graph::NodeType::BuiltInEffect: return "BuiltInEffect";
    case ::ShaderLab::Graph::NodeType::PixelShader:   return "PixelShader";
    case ::ShaderLab::Graph::NodeType::ComputeShader: return "ComputeShader";
    case ::ShaderLab::Graph::NodeType::Output:        return "Output";
    default: return "Unknown";
    }
}

static std::string NodeToJson(const ::ShaderLab::Graph::EffectNode& node)
{
    std::string json = "{";
    json += std::format("\"id\":{},\"name\":\"{}\",\"type\":\"{}\"",
        node.id, ToUtf8(node.name), McpNodeTypeStr(node.type));
    json += std::format(",\"position\":[{:.1f},{:.1f}]", node.position.x, node.position.y);

    // Properties.
    json += ",\"properties\":{";
    bool first = true;
    for (const auto& [key, val] : node.properties)
    {
        if (!first) json += ",";
        json += "\"" + ToUtf8(key) + "\":" + PropertyValueToJson(val);
        first = false;
    }
    json += "}";

    // Pins.
    json += ",\"inputPins\":[";
    for (size_t i = 0; i < node.inputPins.size(); ++i)
    {
        if (i > 0) json += ",";
        json += std::format("{{\"name\":\"{}\",\"index\":{}}}", ToUtf8(node.inputPins[i].name), node.inputPins[i].index);
    }
    json += "],\"outputPins\":[";
    for (size_t i = 0; i < node.outputPins.size(); ++i)
    {
        if (i > 0) json += ",";
        json += std::format("{{\"name\":\"{}\",\"index\":{}}}", ToUtf8(node.outputPins[i].name), node.outputPins[i].index);
    }
    json += "]";

    if (node.effectClsid.has_value())
        json += ",\"effectClsid\":\"" + GuidToString(node.effectClsid.value()) + "\"";
    if (!node.runtimeError.empty())
        json += ",\"runtimeError\":\"" + ToUtf8(node.runtimeError) + "\"";

    // Custom effect definition.
    if (node.customEffect.has_value())
    {
        auto& def = node.customEffect.value();
        json += ",\"customEffect\":{";
        json += std::format("\"shaderType\":\"{}\",\"compiled\":{},\"bytecodeSize\":{}",
            def.shaderType == ::ShaderLab::Graph::CustomShaderType::PixelShader ? "PixelShader" : "ComputeShader",
            def.isCompiled() ? "true" : "false",
            def.compiledBytecode.size());
        json += ",\"inputNames\":[";
        for (size_t i = 0; i < def.inputNames.size(); ++i)
        {
            if (i > 0) json += ",";
            json += "\"" + ToUtf8(def.inputNames[i]) + "\"";
        }
        json += "],\"parameters\":[";
        for (size_t i = 0; i < def.parameters.size(); ++i)
        {
            if (i > 0) json += ",";
            auto& p = def.parameters[i];
            json += std::format("{{\"name\":\"{}\",\"type\":\"{}\",\"min\":{:.4f},\"max\":{:.4f},\"step\":{:.4f}}}",
                ToUtf8(p.name), ToUtf8(p.typeName), p.minValue, p.maxValue, p.step);
        }
        json += "]";

        // Include HLSL source.
        std::string hlsl = ToUtf8(def.hlslSource);
        std::string escapedHlsl;
        for (char c : hlsl)
        {
            if (c == '"') escapedHlsl += "\\\"";
            else if (c == '\\') escapedHlsl += "\\\\";
            else if (c == '\n') escapedHlsl += "\\n";
            else if (c == '\r') escapedHlsl += "\\r";
            else if (c == '\t') escapedHlsl += "\\t";
            else escapedHlsl += c;
        }
        json += ",\"hlslSource\":\"" + escapedHlsl + "\"";
        json += "}";
    }

    json += "}";
    return json;
}

namespace winrt::ShaderLab::implementation
{
    void MainWindow::SetupMcpRoutes()
    {
        if (!m_mcpServer)
            m_mcpServer = std::make_unique<::ShaderLab::McpHttpServer>();

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
    "textures": "register t0..t7, one per input"
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
        // GET /graph  — Full graph state
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/graph", [this](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            // Check for /graph/save or /graph/node/{id}
            if (path == L"/graph/save")
            {
                auto json = m_graph.ToJson();
                return { 200, ToUtf8(std::wstring(json)) };
            }
            if (path.starts_with(L"/graph/node/"))
            {
                auto idStr = path.substr(12);
                uint32_t nodeId = static_cast<uint32_t>(std::stoul(idStr));
                auto* node = m_graph.FindNode(nodeId);
                if (!node) return { 404, R"({"error":"Node not found"})" };
                return { 200, NodeToJson(*node) };
            }

            // Full graph.
            std::string json = "{\"nodes\":[";
            bool first = true;
            for (const auto& node : m_graph.Nodes())
            {
                if (!first) json += ",";
                json += NodeToJson(node);
                first = false;
            }
            json += "],\"edges\":[";
            first = true;
            for (const auto& edge : m_graph.Edges())
            {
                if (!first) json += ",";
                json += std::format("{{\"srcId\":{},\"srcPin\":{},\"dstId\":{},\"dstPin\":{}}}",
                    edge.sourceNodeId, edge.sourcePin, edge.destNodeId, edge.destPin);
                first = false;
            }
            json += std::format("],\"previewNodeId\":{}}}", m_previewNodeId);
            return { 200, json };
        });

        // =====================================================================
        // GET /registry/effects  — Built-in D2D effects
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/registry", [](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            auto& reg = ::ShaderLab::Effects::EffectRegistry::Instance();

            if (path.starts_with(L"/registry/effect/"))
            {
                auto name = path.substr(17);
                auto* desc = reg.FindByName(name);
                if (!desc) return { 404, R"({"error":"Effect not found"})" };

                std::string json = "{";
                json += "\"name\":\"" + ToUtf8(std::wstring(desc->name)) + "\"";
                json += ",\"category\":\"" + ToUtf8(std::wstring(desc->category)) + "\"";
                json += ",\"clsid\":\"" + GuidToString(desc->clsid) + "\"";
                json += std::format(",\"inputCount\":{}", desc->inputPins.size());

                json += ",\"properties\":{";
                bool first = true;
                for (const auto& [key, meta] : desc->propertyMetadata)
                {
                    if (!first) json += ",";
                    json += "\"" + ToUtf8(key) + "\":{";
                    json += std::format("\"min\":{:.4f},\"max\":{:.4f},\"step\":{:.4f}",
                        meta.minValue, meta.maxValue, meta.step);
                    if (!meta.enumLabels.empty())
                    {
                        json += ",\"enumLabels\":[";
                        for (size_t i = 0; i < meta.enumLabels.size(); ++i)
                        {
                            if (i > 0) json += ",";
                            json += "\"" + ToUtf8(meta.enumLabels[i]) + "\"";
                        }
                        json += "]";
                    }
                    json += "}";
                    first = false;
                }
                json += "}}";
                return { 200, json };
            }

            // List all effects.
            std::string json = "[";
            bool first = true;
            for (const auto& desc : reg.All())
            {
                if (!first) json += ",";
                json += "{\"name\":\"" + ToUtf8(std::wstring(desc.name)) + "\"";
                json += ",\"category\":\"" + ToUtf8(std::wstring(desc.category)) + "\"";
                json += std::format(",\"inputCount\":{}}}", desc.inputPins.size());
                first = false;
            }
            json += "]";
            return { 200, json };
        });

        // =====================================================================
        // GET /custom-effects  — Custom effects in graph
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/custom-effects", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            std::string json = "[";
            bool first = true;
            for (const auto& node : m_graph.Nodes())
            {
                if (!node.customEffect.has_value()) continue;
                if (!first) json += ",";
                json += NodeToJson(node);
                first = false;
            }
            json += "]";
            return { 200, json };
        });

        // =====================================================================
        // POST /graph/add-node
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/add-node", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                if (jobj.HasKey(L"effectName"))
                {
                    auto name = jobj.GetNamedString(L"effectName");
                    auto* desc = ::ShaderLab::Effects::EffectRegistry::Instance().FindByName(name);
                    if (!desc) return { 400, R"({"error":"Unknown effect name"})" };
                    auto node = ::ShaderLab::Effects::EffectRegistry::CreateNode(*desc);
                    auto id = m_graph.AddNode(std::move(node));
                    m_graph.MarkAllDirty();
                    m_nodeGraphController.RebuildLayout();
                    PopulatePreviewNodeSelector();
                    return { 200, std::format("{{\"nodeId\":{}}}", id) };
                }
                return { 400, R"({"error":"Provide effectName"})" };
            }
            catch (...) { return { 400, R"({"error":"Invalid JSON"})" }; }
        });

        // =====================================================================
        // POST /graph/remove-node
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/remove-node", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                m_graph.RemoveNode(nodeId);
                m_graphEvaluator.InvalidateNode(nodeId);
                m_graph.MarkAllDirty();
                m_nodeGraphController.RebuildLayout();
                PopulatePreviewNodeSelector();
                return { 200, R"({"ok":true})" };
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // POST /graph/connect
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/connect", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t srcId = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcId"));
                uint32_t srcPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcPin"));
                uint32_t dstId = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstId"));
                uint32_t dstPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstPin"));
                bool ok = m_graph.Connect(srcId, srcPin, dstId, dstPin);
                m_graph.MarkAllDirty();
                return { 200, std::format("{{\"connected\":{}}}", ok ? "true" : "false") };
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // POST /graph/disconnect
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/disconnect", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t srcId = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcId"));
                uint32_t srcPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"srcPin"));
                uint32_t dstId = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstId"));
                uint32_t dstPin = static_cast<uint32_t>(jobj.GetNamedNumber(L"dstPin"));
                bool ok = m_graph.Disconnect(srcId, srcPin, dstId, dstPin);
                m_graph.MarkAllDirty();
                return { 200, std::format("{{\"disconnected\":{}}}", ok ? "true" : "false") };
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // POST /graph/set-property
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/set-property", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                auto key = std::wstring(jobj.GetNamedString(L"key"));

                auto* node = m_graph.FindNode(nodeId);
                if (!node) return { 404, R"({"error":"Node not found"})" };

                auto val = jobj.GetNamedValue(L"value");
                switch (val.ValueType())
                {
                case winrt::Windows::Data::Json::JsonValueType::Number:
                    node->properties[key] = static_cast<float>(val.GetNumber());
                    break;
                case winrt::Windows::Data::Json::JsonValueType::Boolean:
                    node->properties[key] = val.GetBoolean();
                    break;
                case winrt::Windows::Data::Json::JsonValueType::String:
                    node->properties[key] = std::wstring(val.GetString());
                    break;
                case winrt::Windows::Data::Json::JsonValueType::Array:
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
                    return { 400, R"({"error":"Unsupported value type"})" };
                }
                node->dirty = true;
                m_graph.MarkAllDirty();
                return { 200, R"({"ok":true})" };
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // POST /graph/load
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/load", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto loaded = ::ShaderLab::Graph::EffectGraph::FromJson(winrt::to_hstring(body));
                m_graphEvaluator.ReleaseCache();
                m_graph = std::move(loaded);
                // Dispatch UI reset to the UI thread.
                DispatcherQueue().TryEnqueue([this]() { ResetAfterGraphLoad(); });
                return { 200, R"({"ok":true})" };
            }
            catch (const std::exception& ex)
            {
                return { 400, std::string(R"({"error":")") + ex.what() + R"("})" };
            }
        });

        // =====================================================================
        // POST /graph/clear
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/clear", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            m_graphEvaluator.ReleaseCache();
            m_graph.Clear();
            m_nodeGraphController.EnsureOutputNode();
            m_graph.MarkAllDirty();
            m_nodeGraphController.RebuildLayout();
            PopulatePreviewNodeSelector();
            return { 200, R"({"ok":true})" };
        });

        // =====================================================================
        // POST /render/preview-node
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/render/preview-node", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                m_previewNodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                return { 200, R"({"ok":true})" };
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

                auto* node = m_graph.FindNode(nodeId);
                if (!node || !node->customEffect.has_value())
                    return { 404, R"({"error":"Custom effect node not found"})" };

                auto& def = node->customEffect.value();
                def.hlslSource = hlsl;

                // Convert \r -> \n for D3DCompile.
                std::string hlslUtf8 = ToUtf8(hlsl);
                for (auto& ch : hlslUtf8) { if (ch == '\r') ch = '\n'; }

                std::string target = (def.shaderType == ::ShaderLab::Graph::CustomShaderType::PixelShader)
                    ? "ps_5_0" : "cs_5_0";
                auto result = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
                    hlslUtf8, "McpCompile", "main", target);

                if (!result.succeeded)
                {
                    def.compiledBytecode.clear();
                    auto errMsg = ToUtf8(result.ErrorMessage());
                    // Escape for JSON.
                    std::string escaped;
                    for (char c : errMsg) { if (c == '"') escaped += "\\\""; else if (c == '\n') escaped += "\\n"; else escaped += c; }
                    return { 200, std::format("{{\"compiled\":false,\"error\":\"{}\"}}", escaped) };
                }

                auto* blob = result.bytecode.get();
                def.compiledBytecode.resize(blob->GetBufferSize());
                memcpy(def.compiledBytecode.data(), blob->GetBufferPointer(), blob->GetBufferSize());
                if (def.shaderGuid == GUID{})
                    CoCreateGuid(&def.shaderGuid);

                node->dirty = true;
                m_graph.MarkAllDirty();
                m_graphEvaluator.InvalidateNode(nodeId);

                return { 200, std::format("{{\"compiled\":true,\"bytecodeSize\":{}}}", def.compiledBytecode.size()) };
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
        // GET /render/capture — Output as base64 PNG (placeholder)
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/render/capture", [this](const std::wstring&, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            // TODO: render to bitmap, WIC encode to PNG, base64 encode
            return { 200, R"({"note":"Image capture via MCP coming soon","previewNodeId":)" +
                std::format("{}}}", m_previewNodeId) };
        });
    }
}
