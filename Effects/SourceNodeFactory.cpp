#include "pch.h"
#include "SourceNodeFactory.h"

using namespace ShaderLab::Graph;
namespace Numerics = winrt::Windows::Foundation::Numerics;

namespace ShaderLab::Effects
{
    SourceNodeFactory::SourceNodeFactory() = default;

    // -----------------------------------------------------------------------
    // Node creation (static — no device needed yet)
    // -----------------------------------------------------------------------

    EffectNode SourceNodeFactory::CreateImageSourceNode(
        const std::wstring& filePath,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty()
            ? std::filesystem::path(filePath).filename().wstring()
            : displayName;
        node.shaderPath = filePath;  // reuse shaderPath field for image path

        // Image sources have no inputs and one output.
        node.outputPins.push_back({ L"Image", 0 });

        return node;
    }

    EffectNode SourceNodeFactory::CreateFloodSourceNode(
        const Numerics::float4& color,
        const std::wstring& displayName)
    {
        EffectNode node;
        node.type = NodeType::Source;
        node.name = displayName.empty() ? L"Flood Fill" : displayName;
        node.effectClsid = CLSID_D2D1Flood;

        // Store the color as a property. D2D1Flood property index 0 = Color.
        node.properties[L"Color"] = color;

        // Flood sources have no inputs and one output.
        node.outputPins.push_back({ L"Output", 0 });

        return node;
    }

    // -----------------------------------------------------------------------
    // Source preparation (needs D2D device context)
    // -----------------------------------------------------------------------

    void SourceNodeFactory::PrepareSourceNode(
        EffectNode& node,
        ID2D1DeviceContext5* dc)
    {
        if (!dc || node.type != NodeType::Source)
            return;

        // --- Image source ---
        if (node.shaderPath.has_value() && !node.effectClsid.has_value())
        {
            // Check bitmap cache first.
            auto it = m_bitmapCache.find(node.id);
            if (it != m_bitmapCache.end() && !node.dirty)
            {
                node.cachedOutput = it->second.get();
                return;
            }

            // Load (or reload) the image.
            auto bitmap = m_imageLoader.LoadFromFile(node.shaderPath.value(), dc);
            if (bitmap)
            {
                node.cachedOutput = bitmap.get();
                m_bitmapCache[node.id] = std::move(bitmap);
                node.dirty = false;
            }
            return;
        }

        // --- Flood source ---
        if (node.effectClsid.has_value() && node.effectClsid.value() == CLSID_D2D1Flood)
        {
            // Check flood cache first.
            auto it = m_floodCache.find(node.id);
            winrt::com_ptr<ID2D1Effect> flood;

            if (it != m_floodCache.end())
            {
                flood = it->second;
            }
            else
            {
                HRESULT hr = dc->CreateEffect(CLSID_D2D1Flood, flood.put());
                if (FAILED(hr))
                    return;
                m_floodCache[node.id] = flood;
            }

            // Apply the color property (index 0 = D2D1_FLOOD_PROP_COLOR).
            auto colorIt = node.properties.find(L"Color");
            if (colorIt != node.properties.end())
            {
                auto* colorVal = std::get_if<Numerics::float4>(&colorIt->second);
                if (colorVal)
                {
                    D2D1_VECTOR_4F d2dColor{ colorVal->x, colorVal->y, colorVal->z, colorVal->w };
                    flood->SetValue(D2D1_FLOOD_PROP_COLOR, d2dColor);
                }
            }

            // Cache the flood effect's output as this node's image.
            winrt::com_ptr<ID2D1Image> output;
            flood->GetOutput(output.put());
            node.cachedOutput = output.get();
            node.dirty = false;
            return;
        }
    }

    // -----------------------------------------------------------------------
    // Cache management
    // -----------------------------------------------------------------------

    void SourceNodeFactory::ReleaseCache()
    {
        m_bitmapCache.clear();
        m_floodCache.clear();
    }
}
