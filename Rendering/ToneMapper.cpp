#include "pch.h"
#include "ToneMapper.h"

namespace ShaderLab::Rendering
{
    // -----------------------------------------------------------------------
    // Initialization
    // -----------------------------------------------------------------------

    void ToneMapper::Initialize(ID2D1DeviceContext* dc)
    {
        if (!dc) return;
        m_dc.copy_from(dc);

        // Create the white level adjustment effect (D2D1_EFFECT_WHITE_LEVEL_ADJUSTMENT).
        dc->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, m_whiteLevelEffect.put());

        // Create the HDR tone map effect.
        dc->CreateEffect(CLSID_D2D1HdrToneMap, m_hdrToneMapEffect.put());

        // Create a color matrix effect for exposure adjustment.
        dc->CreateEffect(CLSID_D2D1ColorMatrix, m_exposureEffect.put());

        m_initialized = true;
        UpdateEffects();
    }

    void ToneMapper::Release()
    {
        m_whiteLevelEffect = nullptr;
        m_hdrToneMapEffect = nullptr;
        m_exposureEffect = nullptr;
        m_output = nullptr;
        m_dc = nullptr;
        m_initialized = false;
    }

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    void ToneMapper::SetMode(ToneMapMode mode)
    {
        if (m_mode != mode)
        {
            m_mode = mode;
            UpdateEffects();
        }
    }

    void ToneMapper::SetSDRWhiteLevel(float nits)
    {
        m_sdrWhiteNits = (std::max)(1.0f, nits);
        UpdateEffects();
    }

    void ToneMapper::SetDisplayMaxLuminance(float nits)
    {
        m_displayMaxNits = (std::max)(1.0f, nits);
        UpdateEffects();
    }

    void ToneMapper::SetExposure(float stops)
    {
        m_exposureStops = stops;
        UpdateEffects();
    }

    // -----------------------------------------------------------------------
    // Effect chain update
    // -----------------------------------------------------------------------

    void ToneMapper::UpdateEffects()
    {
        if (!m_initialized) return;

        // Configure exposure via color matrix (scale RGB by 2^stops).
        if (m_exposureEffect)
        {
            float scale = std::powf(2.0f, m_exposureStops);
            D2D1_MATRIX_5X4_F matrix = D2D1::Matrix5x4F(
                scale, 0, 0, 0,
                0, scale, 0, 0,
                0, 0, scale, 0,
                0, 0, 0, 1,
                0, 0, 0, 0);
            m_exposureEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix);
        }

        // Configure white level adjustment.
        if (m_whiteLevelEffect)
        {
            m_whiteLevelEffect->SetValue(
                D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL, m_sdrWhiteNits);
            m_whiteLevelEffect->SetValue(
                D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL, m_displayMaxNits);
        }

        // Configure HDR tone map.
        if (m_hdrToneMapEffect)
        {
            m_hdrToneMapEffect->SetValue(
                D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, m_displayMaxNits);
            m_hdrToneMapEffect->SetValue(
                D2D1_HDRTONEMAP_PROP_DISPLAY_MODE,
                D2D1_HDRTONEMAP_DISPLAY_MODE_SDR);
        }
    }

    // -----------------------------------------------------------------------
    // Apply
    // -----------------------------------------------------------------------

    ID2D1Image* ToneMapper::Apply(ID2D1Image* input)
    {
        if (!input || !m_initialized)
            return input;

        if (m_mode == ToneMapMode::None)
            return input;

        ID2D1Image* current = input;

        // Apply exposure first (if non-zero).
        if (m_exposureStops != 0.0f && m_exposureEffect)
        {
            m_exposureEffect->SetInput(0, current);
            m_exposureEffect->GetOutput(m_output.put());
            current = m_output.get();
        }

        switch (m_mode)
        {
        case ToneMapMode::SDRClamp:
        {
            // Use white level adjustment to scale HDR → SDR range.
            if (m_whiteLevelEffect)
            {
                m_whiteLevelEffect->SetInput(0, current);
                winrt::com_ptr<ID2D1Image> out;
                m_whiteLevelEffect->GetOutput(out.put());
                m_output = out;
                return m_output.get();
            }
            break;
        }

        case ToneMapMode::Reinhard:
        case ToneMapMode::ACESFilmic:
        case ToneMapMode::Hable:
        {
            // Use D2D's built-in HDR tone map effect.
            // The D2D1_HDRTONEMAP effect implements a sophisticated tone curve
            // that approximates these operators when configured appropriately.
            if (m_hdrToneMapEffect)
            {
                m_hdrToneMapEffect->SetInput(0, current);
                winrt::com_ptr<ID2D1Image> out;
                m_hdrToneMapEffect->GetOutput(out.put());
                m_output = out;
                return m_output.get();
            }
            break;
        }

        default:
            break;
        }

        return input;
    }

    // -----------------------------------------------------------------------
    // Tone mapping math (for reference / future custom shader use)
    // -----------------------------------------------------------------------

    float ToneMapper::ReinhardToneMap(float x)
    {
        // Simple Reinhard: x / (1 + x)
        return x / (1.0f + x);
    }

    float ToneMapper::ACESFilmicToneMap(float x)
    {
        // ACES filmic approximation (Narkowicz 2015).
        constexpr float a = 2.51f;
        constexpr float b = 0.03f;
        constexpr float c = 2.43f;
        constexpr float d = 0.59f;
        constexpr float e = 0.14f;
        float num = x * (a * x + b);
        float den = x * (c * x + d) + e;
        return (std::max)(0.0f, (std::min)(1.0f, num / den));
    }

    float ToneMapper::HableToneMap(float x)
    {
        // Uncharted 2 filmic curve (John Hable).
        constexpr float A = 0.15f;
        constexpr float B = 0.50f;
        constexpr float C = 0.10f;
        constexpr float D = 0.20f;
        constexpr float E = 0.02f;
        constexpr float F = 0.30f;
        return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
    }
}
