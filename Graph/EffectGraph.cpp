#include "pch.h"
#include "EffectGraph.h"
#include "../Effects/ShaderCompiler.h"

#include <winrt/Windows.Data.Json.h>

namespace WDJ = winrt::Windows::Data::Json;

namespace ShaderLab::Graph
{
    // -----------------------------------------------------------------------
    // Node management
    // -----------------------------------------------------------------------
     
    uint32_t EffectGraph::AddNode(EffectNode node)
    {
        node.id = m_nextId++;
        node.dirty = true;
        m_nodes.push_back(std::move(node));
        return m_nodes.back().id;
    }

    void EffectGraph::RemoveNode(uint32_t nodeId)
    {
        // Remove all edges referencing this node.
        std::erase_if(m_edges, [nodeId](const EffectEdge& e)
        {
            return e.sourceNodeId == nodeId || e.destNodeId == nodeId;
        });

        // Remove the node itself.
        std::erase_if(m_nodes, [nodeId](const EffectNode& n)
        {
            return n.id == nodeId;
        });
    }

    EffectNode* EffectGraph::FindNode(uint32_t nodeId)
    {
        auto it = std::ranges::find_if(m_nodes, [nodeId](const EffectNode& n) { return n.id == nodeId; });
        return it != m_nodes.end() ? &(*it) : nullptr;
    }

    const EffectNode* EffectGraph::FindNode(uint32_t nodeId) const
    {
        auto it = std::ranges::find_if(m_nodes, [nodeId](const EffectNode& n) { return n.id == nodeId; });
        return it != m_nodes.end() ? &(*it) : nullptr;
    }

    // -----------------------------------------------------------------------
    // Edge management
    // -----------------------------------------------------------------------

    bool EffectGraph::Connect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin)
    {
        if (srcId == dstId)
            return false;

        if (!FindNode(srcId) || !FindNode(dstId))
            return false;

        if (WouldCreateCycle(srcId, dstId))
            return false;

        // Ensure a dest input pin has at most one incoming edge.
        DisconnectInput(dstId, dstPin);

        m_edges.push_back({ srcId, srcPin, dstId, dstPin });
        FindNode(dstId)->dirty = true;
        return true;
    }

    bool EffectGraph::Disconnect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin)
    {
        EffectEdge target{ srcId, srcPin, dstId, dstPin };
        auto it = std::ranges::find(m_edges, target);
        if (it != m_edges.end())
        {
            m_edges.erase(it);
            if (auto* dst = FindNode(dstId))
                dst->dirty = true;
            return true;
        }
        return false;
    }

    void EffectGraph::DisconnectInput(uint32_t dstId, uint32_t dstPin)
    {
        std::erase_if(m_edges, [dstId, dstPin](const EffectEdge& e)
        {
            return e.destNodeId == dstId && e.destPin == dstPin;
        });
    }

    std::vector<const EffectEdge*> EffectGraph::GetInputEdges(uint32_t nodeId) const
    {
        std::vector<const EffectEdge*> result;
        for (const auto& e : m_edges)
        {
            if (e.destNodeId == nodeId)
                result.push_back(&e);
        }
        return result;
    }

    std::vector<const EffectEdge*> EffectGraph::GetOutputEdges(uint32_t nodeId) const
    {
        std::vector<const EffectEdge*> result;
        for (const auto& e : m_edges)
        {
            if (e.sourceNodeId == nodeId)
                result.push_back(&e);
        }
        return result;
    }

    // -----------------------------------------------------------------------
    // Topological sort (Kahn's algorithm)
    // -----------------------------------------------------------------------

    std::vector<uint32_t> EffectGraph::TopologicalSort() const
    {
        // Build adjacency list and in-degree map.
        std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
        std::unordered_map<uint32_t, uint32_t> inDegree;

        for (const auto& node : m_nodes)
        {
            adj[node.id];            // ensure entry exists
            inDegree[node.id] = 0;
        }

        for (const auto& edge : m_edges)
        {
            adj[edge.sourceNodeId].push_back(edge.destNodeId);
            inDegree[edge.destNodeId]++;
        }

        // Seed queue with zero-in-degree nodes.
        std::queue<uint32_t> q;
        for (const auto& [id, deg] : inDegree)
        {
            if (deg == 0)
                q.push(id);
        }

        std::vector<uint32_t> sorted;
        sorted.reserve(m_nodes.size());

        while (!q.empty())
        {
            uint32_t current = q.front();
            q.pop();
            sorted.push_back(current);

            for (uint32_t neighbor : adj[current])
            {
                if (--inDegree[neighbor] == 0)
                    q.push(neighbor);
            }
        }

        if (sorted.size() != m_nodes.size())
            throw std::logic_error("EffectGraph contains a cycle");

        return sorted;
    }

    // -----------------------------------------------------------------------
    // Cycle detection (DFS reachability check)
    // -----------------------------------------------------------------------

    bool EffectGraph::WouldCreateCycle(uint32_t srcId, uint32_t dstId) const
    {
        // If dstId can already reach srcId, adding src->dst creates a cycle.
        std::unordered_set<uint32_t> visited;
        std::queue<uint32_t> work;
        work.push(dstId);

        while (!work.empty())
        {
            uint32_t current = work.front();
            work.pop();

            if (current == srcId)
                return true;

            if (!visited.insert(current).second)
                continue;

            for (const auto& edge : m_edges)
            {
                if (edge.sourceNodeId == current)
                    work.push(edge.destNodeId);
            }
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Dirty / cache helpers
    // -----------------------------------------------------------------------

    void EffectGraph::MarkAllDirty()
    {
        for (auto& node : m_nodes)
            node.dirty = true;
    }

    void EffectGraph::ClearCachedOutputs()
    {
        for (auto& node : m_nodes)
            node.cachedOutput = nullptr;
    }

    void EffectGraph::Clear()
    {
        m_nodes.clear();
        m_edges.clear();
        m_nextId = 1;
    }

    // -----------------------------------------------------------------------
    // JSON serialization helpers (anonymous namespace)
    // -----------------------------------------------------------------------
    namespace
    {
        WDJ::JsonObject PropertyValueToJson(const std::wstring& key, const PropertyValue& value)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(key));
            obj.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(PropertyValueTypeTag(value)));

            std::visit([&obj](auto&& v)
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(v));
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(static_cast<double>(v)));
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(static_cast<double>(v)));
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateBooleanValue(v));
                }
                else if constexpr (std::is_same_v<T, std::wstring>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateStringValue(v));
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.z));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.z));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.w));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                {
                    // Serialize as flat 20-element array (row-major: 5 rows × 4 cols).
                    WDJ::JsonArray arr;
                    const float* p = &v._11;
                    for (int i = 0; i < 20; ++i)
                        arr.Append(WDJ::JsonValue::CreateNumberValue(p[i]));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                {
                    WDJ::JsonArray arr;
                    for (float f : v)
                        arr.Append(WDJ::JsonValue::CreateNumberValue(f));
                    obj.SetNamedValue(L"value", arr);
                }
            }, value);

            return obj;
        }

        PropertyValue PropertyValueFromJson(const WDJ::JsonObject& obj)
        {
            auto type = std::wstring(obj.GetNamedString(L"type"));

            if (type == L"float")
                return static_cast<float>(obj.GetNamedNumber(L"value"));
            if (type == L"int")
                return static_cast<int32_t>(obj.GetNamedNumber(L"value"));
            if (type == L"uint")
                return static_cast<uint32_t>(obj.GetNamedNumber(L"value"));
            if (type == L"bool")
                return obj.GetNamedBoolean(L"value");
            if (type == L"string")
                return std::wstring(obj.GetNamedString(L"value"));

            auto arr = obj.GetNamedArray(L"value");
            if (type == L"float2")
            {
                return winrt::Windows::Foundation::Numerics::float2{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1))
                };
            }
            if (type == L"float3")
            {
                return winrt::Windows::Foundation::Numerics::float3{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1)),
                    static_cast<float>(arr.GetNumberAt(2))
                };
            }
            if (type == L"float4")
            {
                return winrt::Windows::Foundation::Numerics::float4{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1)),
                    static_cast<float>(arr.GetNumberAt(2)),
                    static_cast<float>(arr.GetNumberAt(3))
                };
            }
            if (type == L"matrix5x4")
            {
                D2D1_MATRIX_5X4_F m{};
                float* p = &m._11;
                uint32_t count = (std::min)(arr.Size(), 20u);
                for (uint32_t i = 0; i < count; ++i)
                    p[i] = static_cast<float>(arr.GetNumberAt(i));
                return m;
            }
            if (type == L"floatarray")
            {
                std::vector<float> v;
                v.reserve(arr.Size());
                for (uint32_t i = 0; i < arr.Size(); ++i)
                    v.push_back(static_cast<float>(arr.GetNumberAt(i)));
                return v;
            }

            throw std::invalid_argument("Unknown property type in JSON");
        }

        WDJ::JsonObject NodeToJson(const EffectNode& node)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"id", WDJ::JsonValue::CreateNumberValue(node.id));
            obj.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(node.name));
            obj.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(NodeTypeToString(node.type)));

            // Position
            WDJ::JsonArray pos;
            pos.Append(WDJ::JsonValue::CreateNumberValue(node.position.x));
            pos.Append(WDJ::JsonValue::CreateNumberValue(node.position.y));
            obj.SetNamedValue(L"position", pos);

            // Properties
            WDJ::JsonArray props;
            for (const auto& [key, value] : node.properties)
            {
                props.Append(PropertyValueToJson(key, value));
            }
            obj.SetNamedValue(L"properties", props);

            // Effect CLSID (stored as string for portability)
            if (node.effectClsid.has_value())
            {
                wchar_t guidStr[64]{};
                StringFromGUID2(node.effectClsid.value(), guidStr, 64);
                obj.SetNamedValue(L"effectClsid", WDJ::JsonValue::CreateStringValue(guidStr));
            }

            // Shader path
            if (node.shaderPath.has_value())
            {
                obj.SetNamedValue(L"shaderPath", WDJ::JsonValue::CreateStringValue(node.shaderPath.value()));
            }

            // Pins
            WDJ::JsonArray inPins;
            for (const auto& pin : node.inputPins)
            {
                WDJ::JsonObject p;
                p.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(pin.name));
                p.SetNamedValue(L"index", WDJ::JsonValue::CreateNumberValue(pin.index));
                inPins.Append(p);
            }
            obj.SetNamedValue(L"inputPins", inPins);

            WDJ::JsonArray outPins;
            for (const auto& pin : node.outputPins)
            {
                WDJ::JsonObject p;
                p.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(pin.name));
                p.SetNamedValue(L"index", WDJ::JsonValue::CreateNumberValue(pin.index));
                outPins.Append(p);
            }
            obj.SetNamedValue(L"outputPins", outPins);

            // Custom effect definition.
            if (node.customEffect.has_value())
            {
                WDJ::JsonObject ced;
                auto& def = node.customEffect.value();
                ced.SetNamedValue(L"shaderType", WDJ::JsonValue::CreateNumberValue(
                    static_cast<double>(def.shaderType)));
                ced.SetNamedValue(L"hlslSource", WDJ::JsonValue::CreateStringValue(def.hlslSource));

                WDJ::JsonArray inputs;
                for (const auto& name : def.inputNames)
                    inputs.Append(WDJ::JsonValue::CreateStringValue(name));
                ced.SetNamedValue(L"inputNames", inputs);

                WDJ::JsonArray params;
                for (const auto& p : def.parameters)
                {
                    WDJ::JsonObject po;
                    po.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(p.name));
                    po.SetNamedValue(L"typeName", WDJ::JsonValue::CreateStringValue(p.typeName));
                    po.SetNamedValue(L"minValue", WDJ::JsonValue::CreateNumberValue(p.minValue));
                    po.SetNamedValue(L"maxValue", WDJ::JsonValue::CreateNumberValue(p.maxValue));
                    po.SetNamedValue(L"step", WDJ::JsonValue::CreateNumberValue(p.step));
                    po.SetNamedValue(L"default", PropertyValueToJson(p.name, p.defaultValue));
                    params.Append(po);
                }
                ced.SetNamedValue(L"parameters", params);

                ced.SetNamedValue(L"threadGroupX", WDJ::JsonValue::CreateNumberValue(def.threadGroupX));
                ced.SetNamedValue(L"threadGroupY", WDJ::JsonValue::CreateNumberValue(def.threadGroupY));
                ced.SetNamedValue(L"threadGroupZ", WDJ::JsonValue::CreateNumberValue(def.threadGroupZ));
                ced.SetNamedValue(L"analysisOutputType", WDJ::JsonValue::CreateNumberValue(
                    static_cast<double>(def.analysisOutputType)));
                ced.SetNamedValue(L"analysisOutputSize", WDJ::JsonValue::CreateNumberValue(def.analysisOutputSize));

                obj.SetNamedValue(L"customEffect", ced);
            }

            return obj;
        }

        EffectNode NodeFromJson(const WDJ::JsonObject& obj)
        {
            EffectNode node;
            node.id = static_cast<uint32_t>(obj.GetNamedNumber(L"id"));
            node.name = std::wstring(obj.GetNamedString(L"name"));
            node.type = NodeTypeFromString(std::wstring(obj.GetNamedString(L"type")));

            auto pos = obj.GetNamedArray(L"position");
            node.position = {
                static_cast<float>(pos.GetNumberAt(0)),
                static_cast<float>(pos.GetNumberAt(1))
            };

            auto props = obj.GetNamedArray(L"properties");
            for (uint32_t i = 0; i < props.Size(); ++i)
            {
                auto propObj = props.GetObjectAt(i);
                auto key = std::wstring(propObj.GetNamedString(L"name"));
                node.properties[key] = PropertyValueFromJson(propObj);
            }

            if (obj.HasKey(L"effectClsid"))
            {
                GUID guid{};
                CLSIDFromString(std::wstring(obj.GetNamedString(L"effectClsid")).c_str(), &guid);
                node.effectClsid = guid;
            }

            if (obj.HasKey(L"shaderPath"))
            {
                node.shaderPath = std::wstring(obj.GetNamedString(L"shaderPath"));
            }

            // Pins
            {
                auto arr = obj.GetNamedArray(L"inputPins");
                for (uint32_t i = 0; i < arr.Size(); ++i)
                {
                    auto p = arr.GetObjectAt(i);
                    node.inputPins.push_back({
                        std::wstring(p.GetNamedString(L"name")),
                        static_cast<uint32_t>(p.GetNamedNumber(L"index"))
                    });
                }
            }
            {
                auto arr = obj.GetNamedArray(L"outputPins");
                for (uint32_t i = 0; i < arr.Size(); ++i)
                {
                    auto p = arr.GetObjectAt(i);
                    node.outputPins.push_back({
                        std::wstring(p.GetNamedString(L"name")),
                        static_cast<uint32_t>(p.GetNamedNumber(L"index"))
                    });
                }
            }

            // Custom effect definition.
            if (obj.HasKey(L"customEffect"))
            {
                auto ced = obj.GetNamedObject(L"customEffect");
                CustomEffectDefinition def;
                def.shaderType = static_cast<CustomShaderType>(
                    static_cast<int>(ced.GetNamedNumber(L"shaderType")));
                def.hlslSource = std::wstring(ced.GetNamedString(L"hlslSource"));

                auto inputs = ced.GetNamedArray(L"inputNames");
                for (uint32_t i = 0; i < inputs.Size(); ++i)
                    def.inputNames.push_back(std::wstring(inputs.GetStringAt(i)));

                auto params = ced.GetNamedArray(L"parameters");
                for (uint32_t i = 0; i < params.Size(); ++i)
                {
                    auto po = params.GetObjectAt(i);
                    ParameterDefinition pd;
                    pd.name = std::wstring(po.GetNamedString(L"name"));
                    pd.typeName = std::wstring(po.GetNamedString(L"typeName"));
                    pd.minValue = static_cast<float>(po.GetNamedNumber(L"minValue"));
                    pd.maxValue = static_cast<float>(po.GetNamedNumber(L"maxValue"));
                    pd.step = static_cast<float>(po.GetNamedNumber(L"step"));
                    if (po.HasKey(L"default"))
                        pd.defaultValue = PropertyValueFromJson(po.GetNamedObject(L"default"));
                    def.parameters.push_back(std::move(pd));
                }

                def.threadGroupX = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupX"));
                def.threadGroupY = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupY"));
                def.threadGroupZ = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupZ"));
                def.analysisOutputType = static_cast<AnalysisOutputType>(
                    static_cast<int>(ced.GetNamedNumber(L"analysisOutputType")));
                def.analysisOutputSize = static_cast<uint32_t>(ced.GetNamedNumber(L"analysisOutputSize"));

                // Recompile from source on load.
                CoCreateGuid(&def.shaderGuid);
                std::string target = (def.shaderType == CustomShaderType::PixelShader)
                    ? "ps_5_0" : "cs_5_0";
                auto compileResult = ::ShaderLab::Effects::ShaderCompiler::CompileFromString(
                    std::string(def.hlslSource.begin(), def.hlslSource.end()),
                    "GraphLoad", "main", target);
                if (compileResult.succeeded && compileResult.bytecode)
                {
                    auto* blob = compileResult.bytecode.get();
                    def.compiledBytecode.resize(blob->GetBufferSize());
                    memcpy(def.compiledBytecode.data(), blob->GetBufferPointer(), blob->GetBufferSize());
                }

                node.customEffect = std::move(def);
            }

            node.dirty = true;
            return node;
        }

        WDJ::JsonObject EdgeToJson(const EffectEdge& edge)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"sourceNodeId", WDJ::JsonValue::CreateNumberValue(edge.sourceNodeId));
            obj.SetNamedValue(L"sourcePin", WDJ::JsonValue::CreateNumberValue(edge.sourcePin));
            obj.SetNamedValue(L"destNodeId", WDJ::JsonValue::CreateNumberValue(edge.destNodeId));
            obj.SetNamedValue(L"destPin", WDJ::JsonValue::CreateNumberValue(edge.destPin));
            return obj;
        }

        EffectEdge EdgeFromJson(const WDJ::JsonObject& obj)
        {
            return EffectEdge{
                .sourceNodeId = static_cast<uint32_t>(obj.GetNamedNumber(L"sourceNodeId")),
                .sourcePin    = static_cast<uint32_t>(obj.GetNamedNumber(L"sourcePin")),
                .destNodeId   = static_cast<uint32_t>(obj.GetNamedNumber(L"destNodeId")),
                .destPin      = static_cast<uint32_t>(obj.GetNamedNumber(L"destPin")),
            };
        }
    }

    // -----------------------------------------------------------------------
    // JSON round-trip
    // -----------------------------------------------------------------------

    winrt::hstring EffectGraph::ToJson() const
    {
        WDJ::JsonObject root;

        // Nodes
        WDJ::JsonArray nodesArr;
        for (const auto& node : m_nodes)
            nodesArr.Append(NodeToJson(node));
        root.SetNamedValue(L"nodes", nodesArr);

        // Edges
        WDJ::JsonArray edgesArr;
        for (const auto& edge : m_edges)
            edgesArr.Append(EdgeToJson(edge));
        root.SetNamedValue(L"edges", edgesArr);

        // Next ID (so deserialized graphs can continue adding nodes)
        root.SetNamedValue(L"nextId", WDJ::JsonValue::CreateNumberValue(m_nextId));

        return root.Stringify();
    }

    EffectGraph EffectGraph::FromJson(winrt::hstring const& json)
    {
        auto root = WDJ::JsonObject::Parse(json);
        EffectGraph graph;

        auto nodesArr = root.GetNamedArray(L"nodes");
        for (uint32_t i = 0; i < nodesArr.Size(); ++i)
        {
            graph.m_nodes.push_back(NodeFromJson(nodesArr.GetObjectAt(i)));
        }

        auto edgesArr = root.GetNamedArray(L"edges");
        for (uint32_t i = 0; i < edgesArr.Size(); ++i)
        {
            graph.m_edges.push_back(EdgeFromJson(edgesArr.GetObjectAt(i)));
        }

        graph.m_nextId = static_cast<uint32_t>(root.GetNamedNumber(L"nextId"));

        return graph;
    }
}
