#pragma once

#include "pch.h"
#include "PipelineFormat.h"

namespace ShaderLab::Rendering
{
    // Tone mapping mode.
    enum class ToneMapMode : uint32_t
    {
        None = 0,           // Pass-through (no tone mapping)
        Reinhard,           // Reinhard global operator
        ACESFilmic,         // ACES filmic approximation
        Hable,              // Uncharted 2 (John Hable's filmic curve)
        SDRClamp,           // Clamp to [0,1] for SDR display
    };

    inline std::wstring ToneMapModeToString(ToneMapMode mode)
    {
        switch (mode)
        {
        case ToneMapMode::None:       return L"None";
        case ToneMapMode::Reinhard:   return L"Reinhard";
        case ToneMapMode::ACESFilmic: return L"ACES Filmic";
        case ToneMapMode::Hable:      return L"Hable";
        case ToneMapMode::SDRClamp:   return L"SDR Clamp";
        default:                      return L"Unknown";
        }
    }

    // Tone mapper — converts HDR content for display on SDR or HDR monitors.
    //
    // Uses D2D built-in effects where possible:
    //   - CLSID_D2D1WhiteLevelAdjustment for SDR→HDR white level scaling
    //   - CLSID_D2D1ColorMatrix for per-channel Reinhard, ACES, Hable via LUT
    //   - Custom pixel shader effects for advanced operators
    //
    // The tone mapper sits between the graph evaluator output and the
    // swap chain render target. It can be bypassed (None mode) or
    // switched at runtime.
    class ToneMapper
    {
    public:
        ToneMapper() = default;

        // Initialize D2D effects for tone mapping.
        void Initialize(ID2D1DeviceContext* dc);

        // Release all D2D effects.
        void Release();

        // Set the tone mapping mode.
        void SetMode(ToneMapMode mode);
        ToneMapMode Mode() const { return m_mode; }

        // Set the SDR reference white level (in nits, default 80).
        void SetSDRWhiteLevel(float nits);
        float SDRWhiteLevel() const { return m_sdrWhiteNits; }

        // Set the display max luminance (from DisplayCapabilities).
        void SetDisplayMaxLuminance(float nits);

        // Set exposure adjustment (in stops, default 0).
        void SetExposure(float stops);
        float Exposure() const { return m_exposureStops; }

        // Apply tone mapping to an input image.
        // Returns the tone-mapped output, or the input unchanged if mode is None.
        ID2D1Image* Apply(ID2D1Image* input);

        // Whether the tone mapper is active (mode != None and initialized).
        bool IsActive() const { return m_mode != ToneMapMode::None && m_initialized; }

    private:
        // Create or update D2D effects for the current mode.
        void UpdateEffects();

        // Build a LUT from the tone curve and apply it to the table transfer effect.
        void BuildToneMappingLUT(ToneMapMode mode);

        // Tone mapping math (used to build LUTs).
        static float ReinhardToneMap(float x);
        static float ACESFilmicToneMap(float x);
        static float HableToneMap(float x);

        static constexpr uint32_t kLutSize = 256;

        winrt::com_ptr<ID2D1DeviceContext> m_dc;
        bool m_initialized{ false };

        ToneMapMode m_mode{ ToneMapMode::None };
        float m_sdrWhiteNits{ 80.0f };
        float m_displayMaxNits{ 300.0f };
        float m_exposureStops{ 0.0f };

        // D2D effects.
        winrt::com_ptr<ID2D1Effect> m_whiteLevelEffect;
        winrt::com_ptr<ID2D1Effect> m_hdrToneMapEffect;
        winrt::com_ptr<ID2D1Effect> m_exposureEffect;
        winrt::com_ptr<ID2D1Effect> m_preScaleEffect;         // ColorMatrix to normalize to [0,1]
        winrt::com_ptr<ID2D1Effect> m_tableTransferEffect;    // Per-channel tone curve LUT

        // Output image (from the last Apply call).
        winrt::com_ptr<ID2D1Image> m_output;
    };
}
