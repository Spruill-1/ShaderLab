#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"

namespace ShaderLab::Rendering
{
    // Analysis overlay mode for false-color visualization.
    enum class FalseColorMode : uint32_t
    {
        None = 0,           // Pass-through (no overlay)
        Clipping,           // Red where channel >= 1.0, blue where channel <= 0.0
        LuminanceZones,     // Color-code by cd/m² luminance bands
        OutOfGamut,         // Magenta where any scRGB channel is negative
    };

    inline std::wstring FalseColorModeToString(FalseColorMode mode)
    {
        switch (mode)
        {
        case FalseColorMode::None:           return L"None";
        case FalseColorMode::Clipping:       return L"Clipping";
        case FalseColorMode::LuminanceZones: return L"Luminance Zones";
        case FalseColorMode::OutOfGamut:     return L"Out of Gamut";
        default:                             return L"Unknown";
        }
    }

    // False-color analysis overlay using built-in D2D effects.
    //
    // Sits between the graph evaluator output (or tone mapper output) and the
    // swap chain render target.  Uses D2D1ColorMatrix and D2D1TableTransfer
    // with crafted LUTs to visualize luminance zones, clipping, or out-of-gamut
    // pixels without a custom pixel shader.
    //
    // Limitations of the per-channel TableTransfer approach:
    //   - Clipping and OutOfGamut highlight per-channel rather than cross-channel
    //     (e.g. "any channel clipped" would require a custom shader).
    //   - TableTransfer clamps inputs to [0,1], so a pre-scale ColorMatrix is
    //     used to map the expected HDR range into [0,1] before the LUT lookup.
    class SHADERLAB_API FalseColorOverlay
    {
    public:
        FalseColorOverlay() = default;

        // Initialize D2D effects for false-color analysis.
        void Initialize(ID2D1DeviceContext* dc);

        // Release all D2D effects.
        void Release();

        // Set the active false-color mode.
        void SetMode(FalseColorMode mode);
        FalseColorMode Mode() const { return m_mode; }

        // Set the display max luminance (needed to scale HDR range into LUT).
        void SetDisplayMaxLuminance(float nits);

        // Apply the false-color overlay to an input image.
        // Returns the overlaid output, or input unchanged if mode is None.
        ID2D1Image* Apply(ID2D1Image* input);

        // Whether the overlay is active (mode != None and initialized).
        bool IsActive() const { return m_mode != FalseColorMode::None && m_initialized; }

    private:
        void BuildLuminanceZoneLUT();
        void BuildClippingLUT();
        void BuildOutOfGamutLUT();

        static constexpr uint32_t kLutSize = 256;

        winrt::com_ptr<ID2D1DeviceContext> m_dc;
        bool m_initialized{ false };
        FalseColorMode m_mode{ FalseColorMode::None };
        float m_displayMaxNits{ 300.0f };

        // LuminanceZones: ColorMatrix (→ luminance) + TableTransfer (→ zone colors)
        winrt::com_ptr<ID2D1Effect> m_luminanceMatrix;
        winrt::com_ptr<ID2D1Effect> m_zoneTransfer;

        // Clipping: pre-scale ColorMatrix + TableTransfer with clipping indicators
        winrt::com_ptr<ID2D1Effect> m_clippingPreScale;
        winrt::com_ptr<ID2D1Effect> m_clippingTransfer;

        // OutOfGamut: pre-scale ColorMatrix + TableTransfer with gamut indicators
        winrt::com_ptr<ID2D1Effect> m_gamutPreScale;
        winrt::com_ptr<ID2D1Effect> m_gamutTransfer;

        // Output image from the last Apply call.
        winrt::com_ptr<ID2D1Image> m_output;
    };
}
