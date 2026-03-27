#pragma once

#include "pch.h"
#include "../Graph/EffectGraph.h"
#include "../Rendering/PipelineFormat.h"

namespace ShaderLab::Controls
{
    // Color value in multiple representations for the pixel inspector.
    struct InspectedPixel
    {
        // Linear scRGB (native pipeline values).
        float scR{ 0 }, scG{ 0 }, scB{ 0 }, scA{ 0 };

        // sRGB (gamma-corrected, 0–255 range).
        uint8_t sR{ 0 }, sG{ 0 }, sB{ 0 }, sA{ 0 };

        // HDR10 PQ values (0–1 range, Perceptual Quantizer).
        float pqR{ 0 }, pqG{ 0 }, pqB{ 0 };

        // Luminance (cd/m²).
        float luminanceNits{ 0 };

        // Pixel position.
        uint32_t x{ 0 }, y{ 0 };

        // Which node was sampled.
        uint32_t nodeId{ 0 };

        // Format strings for UI display.
        std::wstring ScRGBString() const;
        std::wstring SRGBString() const;
        std::wstring HDR10String() const;
        std::wstring LuminanceString() const;
        std::wstring PositionString() const;
    };

    // Pixel inspector controller — reads back pixel values from any node's
    // cached output and converts to multiple color space representations.
    //
    // Responsibilities:
    //   - GPU read-back of ID2D1Image to CPU-accessible staging texture
    //   - Sample a specific pixel coordinate
    //   - Convert scRGB linear → sRGB gamma, HDR10 PQ, luminance
    //   - Track inspection point across graph re-evaluations
    //
    // The controller does NOT own the D2D device. The host provides
    // the device context and calls InspectPixel() on demand.
    class PixelInspectorController
    {
    public:
        PixelInspectorController() = default;

        // Initialize with D3D device for staging texture creation.
        void Initialize(ID3D11Device* device);

        // Read back a pixel from a node's cached output.
        // Returns true if the pixel was successfully sampled.
        bool InspectPixel(
            ID2D1DeviceContext* dc,
            const Graph::EffectGraph& graph,
            uint32_t nodeId,
            uint32_t pixelX,
            uint32_t pixelY);

        // Get the last inspected pixel.
        const InspectedPixel& LastPixel() const { return m_lastPixel; }

        // Whether a valid pixel has been inspected.
        bool HasPixel() const { return m_hasPixel; }

        // Set/get the tracking position (persists across re-evaluations).
        void SetTrackPosition(uint32_t x, uint32_t y, uint32_t nodeId);
        uint32_t TrackX() const { return m_trackX; }
        uint32_t TrackY() const { return m_trackY; }
        uint32_t TrackNodeId() const { return m_trackNodeId; }

        // Re-inspect at the tracked position (call after graph re-evaluation).
        bool ReInspect(ID2D1DeviceContext* dc, const Graph::EffectGraph& graph);

    private:
        // Create or resize the staging texture for GPU → CPU readback.
        bool EnsureStagingTexture(uint32_t width, uint32_t height);

        // Convert scRGB linear to sRGB gamma (per-component).
        static uint8_t LinearToSRGB(float linear);

        // Convert scRGB to PQ (Perceptual Quantizer / ST.2084).
        static float LinearToPQ(float linear);

        // Compute luminance from scRGB linear values (BT.709 primaries).
        static float ComputeLuminance(float r, float g, float b);

        winrt::com_ptr<ID3D11Device>       m_device;
        winrt::com_ptr<ID3D11DeviceContext> m_deviceContext;
        winrt::com_ptr<ID3D11Texture2D>    m_stagingTexture;
        uint32_t m_stagingWidth{ 0 };
        uint32_t m_stagingHeight{ 0 };

        InspectedPixel m_lastPixel;
        bool m_hasPixel{ false };

        // Tracked position for persistent inspection.
        uint32_t m_trackX{ 0 };
        uint32_t m_trackY{ 0 };
        uint32_t m_trackNodeId{ 0 };
    };
}
