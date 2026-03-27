#pragma once

#include "pch.h"
#include "../Graph/EffectNode.h"
#include "ImageLoader.h"

namespace ShaderLab::Effects
{
    // Factory for creating and preparing Source-type graph nodes.
    //
    // Two source types:
    //   1. Image source — loads a file via WIC, stores ID2D1Bitmap1 as cachedOutput.
    //   2. Flood source — creates a D2D1Flood effect that fills with a solid color.
    //
    // PrepareSourceNode must be called before graph evaluation to ensure
    // the node's cachedOutput is populated for downstream effects.
    class SourceNodeFactory
    {
    public:
        SourceNodeFactory();

        // Create a Source node configured for image file loading.
        // The node's shaderPath holds the file path; cachedOutput is set
        // after calling PrepareSourceNode.
        static Graph::EffectNode CreateImageSourceNode(
            const std::wstring& filePath,
            const std::wstring& displayName = L"");

        // Create a Source node configured as a Flood (solid color) fill.
        // Color is stored as a float4 property named "Color".
        static Graph::EffectNode CreateFloodSourceNode(
            const winrt::Windows::Foundation::Numerics::float4& color,
            const std::wstring& displayName = L"");

        // Prepare a source node for evaluation: loads the image or creates
        // the Flood effect, and sets the node's cachedOutput.
        // Should be called once after the node is added to the graph
        // (or when the source path / color changes).
        void PrepareSourceNode(
            Graph::EffectNode& node,
            ID2D1DeviceContext5* dc);

        // Release all cached bitmaps and flood effects (e.g., on device lost).
        void ReleaseCache();

    private:
        ImageLoader m_imageLoader;

        // Cached loaded bitmaps: nodeId → bitmap. Avoids reloading every frame.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Bitmap1>> m_bitmapCache;

        // Cached flood effects: nodeId → flood effect.
        std::unordered_map<uint32_t, winrt::com_ptr<ID2D1Effect>> m_floodCache;
    };
}
