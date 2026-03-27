#include "pch.h"
#include "GraphEvaluator.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering
{
    // -----------------------------------------------------------------------
    // Main evaluation entry point
    // -----------------------------------------------------------------------

    ID2D1Image* GraphEvaluator::Evaluate(EffectGraph& graph, ID2D1DeviceContext5* dc)
    {
        if (graph.IsEmpty() || !dc)
            return nullptr;

        // Get topological ordering (sources first → output last).
        std::vector<uint32_t> order;
        try
        {
            order = graph.TopologicalSort();
        }
        catch (const std::logic_error&)
        {
            // Graph has a cycle — cannot evaluate.
            return nullptr;
        }

        ID2D1Image* finalOutput = nullptr;

        for (uint32_t nodeId : order)
        {
            EffectNode* node = graph.FindNode(nodeId);
            if (!node)
                continue;

            switch (node->type)
            {
            case NodeType::Source:
            {
                // Source nodes have their cachedOutput set externally
                // (by WIC image loading or Flood effect in Step 8).
                // Nothing to do here — just ensure it's marked clean.
                node->dirty = false;
                break;
            }

            case NodeType::BuiltInEffect:
            {
                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (effect)
                {
                    WireInputs(effect, *node, graph);

                    if (node->dirty)
                    {
                        ApplyProperties(effect, *node);
                        node->dirty = false;
                    }

                    // The effect's output is an ID2D1Image.
                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    node->cachedOutput = output.get();
                }
                break;
            }

            case NodeType::PixelShader:
            case NodeType::ComputeShader:
            {
                // Custom shader effects — placeholder for Steps 10-11.
                // When implemented, these will use ID2D1EffectImpl +
                // ID2D1DrawTransform / ID2D1ComputeTransform and follow
                // the same GetOrCreateEffect / WireInputs / ApplyProperties
                // pattern as built-in effects.
                ID2D1Effect* effect = GetOrCreateEffect(dc, *node);
                if (effect)
                {
                    WireInputs(effect, *node, graph);
                    if (node->dirty)
                    {
                        ApplyProperties(effect, *node);
                        node->dirty = false;
                    }
                    winrt::com_ptr<ID2D1Image> output;
                    effect->GetOutput(output.put());
                    node->cachedOutput = output.get();
                }
                break;
            }

            case NodeType::Output:
            {
                // The output node simply passes through the first input.
                auto inputs = graph.GetInputEdges(nodeId);
                if (!inputs.empty())
                {
                    const EffectNode* srcNode = graph.FindNode(inputs[0]->sourceNodeId);
                    if (srcNode && srcNode->cachedOutput)
                    {
                        node->cachedOutput = srcNode->cachedOutput;
                        finalOutput = node->cachedOutput;
                    }
                }
                node->dirty = false;
                break;
            }
            }
        }

        return finalOutput;
    }

    // -----------------------------------------------------------------------
    // Effect cache management
    // -----------------------------------------------------------------------

    void GraphEvaluator::ReleaseCache()
    {
        m_effectCache.clear();
    }

    void GraphEvaluator::InvalidateNode(uint32_t nodeId)
    {
        m_effectCache.erase(nodeId);
    }

    // -----------------------------------------------------------------------
    // D2D effect creation / retrieval
    // -----------------------------------------------------------------------

    ID2D1Effect* GraphEvaluator::GetOrCreateEffect(
        ID2D1DeviceContext5* dc,
        const EffectNode& node)
    {
        // Check the cache first.
        auto it = m_effectCache.find(node.id);
        if (it != m_effectCache.end())
            return it->second.get();

        // Need a CLSID to create a built-in effect.
        if (!node.effectClsid.has_value())
            return nullptr;

        winrt::com_ptr<ID2D1Effect> effect;
        HRESULT hr = dc->CreateEffect(node.effectClsid.value(), effect.put());
        if (FAILED(hr))
            return nullptr;

        auto* raw = effect.get();
        m_effectCache[node.id] = std::move(effect);
        return raw;
    }

    // -----------------------------------------------------------------------
    // Property application
    // -----------------------------------------------------------------------

    void GraphEvaluator::ApplyProperties(ID2D1Effect* effect, const EffectNode& node)
    {
        if (!effect)
            return;

        // D2D built-in effects use indexed properties (0, 1, 2, ...).
        // The property map in EffectNode uses string keys. We map numeric
        // string keys ("0", "1", ...) directly to D2D property indices.
        // Named keys are resolved through the effect's property name table.
        for (const auto& [key, value] : node.properties)
        {
            // Try to parse the key as a numeric index first.
            uint32_t index = UINT32_MAX;
            try
            {
                size_t pos = 0;
                unsigned long parsed = std::stoul(key, &pos);
                if (pos == key.size())
                    index = static_cast<uint32_t>(parsed);
            }
            catch (...) {}

            // If not numeric, look up the property by name.
            if (index == UINT32_MAX)
            {
                index = effect->GetPropertyIndex(key.c_str());
                if (index == UINT32_MAX)
                    continue;
            }

            // Set the property value based on its variant type.
            std::visit([effect, index](auto&& v)
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    effect->SetValue(index, v);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    effect->SetValue(index, static_cast<BOOL>(v));
                }
                else if constexpr (std::is_same_v<T, std::wstring>)
                {
                    // String properties are rare in D2D effects; skip.
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    D2D1_VECTOR_2F vec{ v.x, v.y };
                    effect->SetValue(index, vec);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    D2D1_VECTOR_3F vec{ v.x, v.y, v.z };
                    effect->SetValue(index, vec);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    D2D1_VECTOR_4F vec{ v.x, v.y, v.z, v.w };
                    effect->SetValue(index, vec);
                }
            }, value);
        }
    }

    // -----------------------------------------------------------------------
    // Input wiring
    // -----------------------------------------------------------------------

    void GraphEvaluator::WireInputs(
        ID2D1Effect* effect,
        const EffectNode& destNode,
        const EffectGraph& graph)
    {
        if (!effect)
            return;

        auto inputEdges = graph.GetInputEdges(destNode.id);
        for (const auto* edge : inputEdges)
        {
            const EffectNode* srcNode = graph.FindNode(edge->sourceNodeId);
            if (srcNode && srcNode->cachedOutput)
            {
                effect->SetInput(edge->destPin, srcNode->cachedOutput);
            }
        }
    }
}
