#include "pch.h"
#include "MainWindow.xaml.h"
#include "ShaderLab/McpHttpServer.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Effects/ShaderLabEffects.h"
#include "Effects/SourceNodeFactory.h"
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
            def.shaderType == ::ShaderLab::Graph::CustomShaderType::PixelShader ? "PixelShader" :
            def.shaderType == ::ShaderLab::Graph::CustomShaderType::D3D11ComputeShader ? "D3D11ComputeShader" :
            "ComputeShader",
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

        // Analysis fields.
        if (!def.analysisFields.empty())
        {
            json += ",\"analysisFields\":[";
            for (size_t i = 0; i < def.analysisFields.size(); ++i)
            {
                if (i > 0) json += ",";
                const auto& fd = def.analysisFields[i];
                std::string typeTag;
                switch (fd.type)
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
                json += "{\"name\":\"" + ToUtf8(fd.name) + "\",\"type\":\"" + typeTag + "\"";
                if (::ShaderLab::Graph::AnalysisFieldIsArray(fd.type))
                    json += ",\"length\":" + std::to_string(fd.arrayLength);
                json += "}";
            }
            json += "]";
        }
        json += "}";
    }

    // Analysis output results (runtime data, not serialized in graph JSON).
    if (node.analysisOutput.type == ::ShaderLab::Graph::AnalysisOutputType::Typed &&
        !node.analysisOutput.fields.empty())
    {
        json += ",\"analysisResults\":[";
        bool first = true;
        for (const auto& fv : node.analysisOutput.fields)
        {
            if (!first) json += ",";
            json += "{\"name\":\"" + ToUtf8(fv.name) + "\"";
            if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
            {
                uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                json += ",\"value\":[";
                for (uint32_t c = 0; c < cc; ++c)
                {
                    if (c > 0) json += ",";
                    json += std::format("{:.6f}", fv.components[c]);
                }
                json += "]";
            }
            else
            {
                json += ",\"value\":[";
                for (size_t i = 0; i < fv.arrayData.size(); ++i)
                {
                    if (i > 0) json += ",";
                    json += std::format("{:.6f}", fv.arrayData[i]);
                }
                json += "]";
            }
            json += "}";
            first = false;
        }
        json += "]";
    }
    else if (node.analysisOutput.type == ::ShaderLab::Graph::AnalysisOutputType::Histogram &&
             !node.analysisOutput.data.empty())
    {
        json += std::format(",\"analysisResults\":{{\"type\":\"histogram\",\"channel\":{},\"bins\":{}}}",
            node.analysisOutput.channelIndex, node.analysisOutput.data.size());
    }

    json += "}";
    return json;
}

namespace winrt::ShaderLab::implementation
{
    // Helper: dispatch a lambda to the UI thread and block until completion.
    // Returns the result from the lambda. Must NOT be called from the UI thread.
    template<typename F>
    auto MainWindow::DispatchSync(F&& fn) -> decltype(fn())
    {
        using R = decltype(fn());
        std::optional<R> result;
        std::exception_ptr ex;
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DispatcherQueue().TryEnqueue([&]()
        {
            try { result = fn(); }
            catch (...) { ex = std::current_exception(); }
            SetEvent(event);
        });
        WaitForSingleObject(event, 10000); // 10s timeout
        CloseHandle(event);
        if (ex) std::rethrow_exception(ex);
        return *result;
    }

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

                    // Support creating custom compute/pixel shader nodes directly.
                    if (name == L"Custom Compute Shader" || name == L"Custom Pixel Shader" ||
                        name == L"Custom D3D11 Compute Shader")
                    {
                        ::ShaderLab::Graph::EffectNode node;
                        bool isCompute = (name == L"Custom Compute Shader");
                        bool isD3D11 = (name == L"Custom D3D11 Compute Shader");
                        node.type = (isCompute || isD3D11)
                            ? ::ShaderLab::Graph::NodeType::ComputeShader
                            : ::ShaderLab::Graph::NodeType::PixelShader;
                        node.name = std::wstring(name);

                        if (isD3D11)
                        {
                            // D3D11 compute: no D2D effect, no output pin (data-only).
                            // No effectClsid needed.
                        }
                        else
                        {
                            node.effectClsid = isCompute
                                ? ::ShaderLab::Effects::CustomComputeShaderEffect::CLSID_CustomComputeShader
                                : ::ShaderLab::Effects::CustomPixelShaderEffect::CLSID_CustomPixelShader;
                            node.outputPins.push_back({ L"Output", 0 });
                        }

                        // Create a default custom effect definition.
                        ::ShaderLab::Graph::CustomEffectDefinition def;
                        def.shaderType = isD3D11
                            ? ::ShaderLab::Graph::CustomShaderType::D3D11ComputeShader
                            : isCompute
                                ? ::ShaderLab::Graph::CustomShaderType::ComputeShader
                                : ::ShaderLab::Graph::CustomShaderType::PixelShader;
                        CoCreateGuid(&def.shaderGuid);

                        // Default: 1 input named "Source".
                        def.inputNames.push_back(L"Source");
                        node.inputPins.push_back({ L"I0", 0 });

                        if (isCompute) { def.threadGroupX = 8; def.threadGroupY = 8; def.threadGroupZ = 1; }

                        if (isD3D11)
                        {
                            // Default analysis field so the node has output.
                            def.analysisOutputType = ::ShaderLab::Graph::AnalysisOutputType::Typed;
                            def.analysisFields.push_back(
                                { L"Result", ::ShaderLab::Graph::AnalysisFieldType::Float4 });
                        }

                        node.customEffect = std::move(def);

                        return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                            auto id = m_graph.AddNode(std::move(node));
                            m_graph.MarkAllDirty();
                            m_nodeGraphController.AutoLayout();
                            PopulatePreviewNodeSelector();
                            return { 200, std::format("{{\"nodeId\":{}}}", id) };
                        });
                    }

                    // Check ShaderLab effects registry.
                    auto* slDesc = ::ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(name);
                    if (slDesc)
                    {
                        auto node = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*slDesc);
                        return DispatchSync([&, wname = std::wstring(name)]() -> ::ShaderLab::McpHttpServer::Response {
                            auto id = m_graph.AddNode(std::move(node));
                            m_nodeLogs[id].Info(std::format(L"Node created: {}", wname));
                            m_graph.MarkAllDirty();
                            m_nodeGraphController.AutoLayout();
                            PopulatePreviewNodeSelector();
                            return { 200, std::format("{{\"nodeId\":{}}}", id) };
                        });
                    }

                    auto* desc = ::ShaderLab::Effects::EffectRegistry::Instance().FindByName(name);
                    if (!desc)
                    {
                        // Check for special source types.
                        std::wstring wname(name.begin(), name.end());
                        if (wname == L"Video Source")
                        {
                            // Video Source requires a file path — create empty placeholder.
                            auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(L"", L"Video Source");
                            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                                auto id = m_graph.AddNode(std::move(node));
                                m_graph.MarkAllDirty();
                                m_nodeGraphController.AutoLayout();
                                PopulatePreviewNodeSelector();
                                return { 200, std::format("{{\"nodeId\":{}}}", id) };
                            });
                        }
                        return { 400, R"({"error":"Unknown effect name"})" };
                    }
                    auto node = ::ShaderLab::Effects::EffectRegistry::CreateNode(*desc);
                    return DispatchSync([&, wname = std::wstring(name)]() -> ::ShaderLab::McpHttpServer::Response {
                        auto id = m_graph.AddNode(std::move(node));
                        m_nodeLogs[id].Info(std::format(L"Node created: {}", wname));
                        m_graph.MarkAllDirty();
                        m_nodeGraphController.AutoLayout();
                        PopulatePreviewNodeSelector();
                        return { 200, std::format("{{\"nodeId\":{}}}", id) };
                    });
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
                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    m_graph.RemoveNode(nodeId);
                    m_graphEvaluator.InvalidateNode(nodeId);
                    CloseOutputWindow(nodeId);
                    m_graph.MarkAllDirty();
                    m_nodeGraphController.AutoLayout();
                    PopulatePreviewNodeSelector();
                    return { 200, R"({"ok":true})" };
                });
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
                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    bool ok = m_graph.Connect(srcId, srcPin, dstId, dstPin);
                    m_graph.MarkAllDirty();
                    m_nodeGraphController.AutoLayout();
                    return { 200, std::format("{{\"connected\":{}}}", ok ? "true" : "false") };
                });
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
                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    bool ok = m_graph.Disconnect(srcId, srcPin, dstId, dstPin);
                    m_graph.MarkAllDirty();
                    m_nodeGraphController.AutoLayout();
                    return { 200, std::format("{{\"disconnected\":{}}}", ok ? "true" : "false") };
                });
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
                auto val = jobj.GetNamedValue(L"value");

                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    auto* node = m_graph.FindNode(nodeId);
                    if (!node) return { 404, R"({"error":"Node not found"})" };

                    switch (val.ValueType())
                    {
                    case winrt::Windows::Data::Json::JsonValueType::Number:
                    {
                        // Check if this parameter is declared as uint and store as uint32_t.
                        bool isUint = false;
                        if (node->customEffect.has_value())
                        {
                            for (const auto& p : node->customEffect->parameters)
                            {
                                if (p.name == key && p.typeName == L"uint")
                                { isUint = true; break; }
                            }
                        }
                        // Also check existing property type.
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

                    // Sync shaderPath property to the dedicated node field (for source/shader nodes).
                    if (key == L"shaderPath")
                    {
                        auto* sv = std::get_if<std::wstring>(&node->properties[key]);
                        if (sv) node->shaderPath = *sv;
                    }

                    return { 200, R"({"ok":true})" };
                });
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
                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    m_graphEvaluator.ReleaseCache();
                    m_graph = std::move(loaded);
                    ResetAfterGraphLoad();
                    m_nodeGraphController.AutoLayout();
                    return { 200, R"({"ok":true})" };
                });
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
            return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                m_graphEvaluator.ReleaseCache();
                m_graph.Clear();
                m_outputWindows.clear();
                m_previewNodeId = 0;
                m_graph.MarkAllDirty();
                m_nodeGraphController.AutoLayout();
                PopulatePreviewNodeSelector();
                return { 200, R"({"ok":true})" };
            });
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
                auto slashPos = stripped.find(L'/');
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
        // POST /graph/bind-property — Bind a property to an analysis output field
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/bind-property", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                auto propName = std::wstring(jobj.GetNamedString(L"propertyName"));
                uint32_t srcNodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"sourceNodeId"));
                auto srcFieldName = std::wstring(jobj.GetNamedString(L"sourceFieldName"));
                uint32_t srcComponent = jobj.HasKey(L"sourceComponent")
                    ? static_cast<uint32_t>(jobj.GetNamedNumber(L"sourceComponent")) : 0;

                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    auto err = m_graph.BindProperty(nodeId, propName, srcNodeId, srcFieldName, srcComponent);
                    if (!err.empty())
                    {
                        std::string errUtf8 = ToUtf8(err);
                        return { 400, "{\"error\":\"" + errUtf8 + "\"}" };
                    }
                    m_nodeGraphController.RebuildLayout();
                    return { 200, R"({"ok":true})" };
                });
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // POST /graph/unbind-property — Remove a property binding
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/graph/unbind-property", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                uint32_t nodeId = static_cast<uint32_t>(jobj.GetNamedNumber(L"nodeId"));
                auto propName = std::wstring(jobj.GetNamedString(L"propertyName"));

                return DispatchSync([&]() -> ::ShaderLab::McpHttpServer::Response {
                    if (!m_graph.UnbindProperty(nodeId, propName))
                        return { 404, R"({"error":"No binding for that property"})" };
                    m_nodeGraphController.RebuildLayout();
                    return { 200, R"({"ok":true})" };
                });
            }
            catch (...) { return { 400, R"({"error":"Invalid request"})" }; }
        });

        // =====================================================================
        // GET /analysis/{id} — Read analysis output fields
        // =====================================================================
        m_mcpServer->AddRoute(L"GET", L"/analysis/", [this](const std::wstring& path, const std::string&)
            -> ::ShaderLab::McpHttpServer::Response
        {
            // Parse path: /analysis/{id}
            auto rest = path.substr(10); // after "/analysis/"
            uint32_t nodeId = static_cast<uint32_t>(std::stoul(rest));
            auto* node = m_graph.FindNode(nodeId);
            if (!node) return { 404, R"({"error":"Node not found"})" };

            if (node->analysisOutput.type != ::ShaderLab::Graph::AnalysisOutputType::Typed ||
                node->analysisOutput.fields.empty())
                return { 200, R"({"fields":[]})" };

            std::string json = R"({"fields":[)";
            bool first = true;
            for (const auto& fv : node->analysisOutput.fields)
            {
                if (!first) json += ",";
                json += "{\"name\":\"" + ToUtf8(fv.name) + "\"";

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
                json += ",\"type\":\"" + typeTag + "\"";

                if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                {
                    uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    json += ",\"value\":[";
                    for (uint32_t c = 0; c < cc; ++c)
                    {
                        if (c > 0) json += ",";
                        json += std::format("{:.6f}", fv.components[c]);
                    }
                    json += "]";
                }
                else
                {
                    uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    uint32_t count = stride > 0 ? static_cast<uint32_t>(fv.arrayData.size()) / stride : 0;
                    json += ",\"count\":" + std::to_string(count);
                    json += ",\"value\":[";
                    for (size_t i = 0; i < fv.arrayData.size(); ++i)
                    {
                        if (i > 0) json += ",";
                        json += std::format("{:.6f}", fv.arrayData[i]);
                    }
                    json += "]";
                }
                json += "}";
                first = false;
            }
            json += "]}";
            return { 200, json };
        });

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
        // POST /  — MCP JSON-RPC 2.0 endpoint (Streamable HTTP transport)
        // =====================================================================
        m_mcpServer->AddRoute(L"POST", L"/", [this](const std::wstring&, const std::string& body)
            -> ::ShaderLab::McpHttpServer::Response
        {
            try
            {
                auto jobj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                auto method = ToUtf8(std::wstring(jobj.GetNamedString(L"method")));
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
                    return { 200, "" };
                }

                // ---- tools/list ----
                if (method == "tools/list")
                {
                    std::string tools = R"JSON({"tools":[
{"name":"graph_add_node","description":"Add a built-in D2D effect node by name, or a ShaderLab analysis/source effect (e.g. Luminance Heatmap, CIE Chromaticity Plot, Gamut Highlight, Waveform Monitor, Vectorscope, False Color, Delta E Comparator, Gamut Coverage, Gamut Source, Color Checker, Zone Plate, Gradient Generator, HDR Test Pattern)","inputSchema":{"type":"object","properties":{"effectName":{"type":"string","description":"Effect name e.g. Gaussian Blur"}},"required":["effectName"]}},
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
{"name":"graph_rename_node","description":"Rename a node","inputSchema":{"type":"object","properties":{"nodeId":{"type":"number"},"name":{"type":"string"}},"required":["nodeId","name"]}}
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
