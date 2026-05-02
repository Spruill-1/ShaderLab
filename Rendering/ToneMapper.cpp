#include "pch_engine.h"
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

        // Create the white level adjustment effect.
        HRESULT hr1 = dc->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, m_whiteLevelEffect.put());

        // Create the HDR tone map effect (D2D built-in operator).
        HRESULT hr2 = dc->CreateEffect(CLSID_D2D1HdrToneMap, m_hdrToneMapEffect.put());

        // Create a color matrix effect for exposure adjustment.
        HRESULT hr3 = dc->CreateEffect(CLSID_D2D1ColorMatrix, m_exposureEffect.put());

        // Create effects for per-channel LUT-based tone mapping (Reinhard/ACES/Hable).
        HRESULT hr4 = dc->CreateEffect(CLSID_D2D1ColorMatrix, m_preScaleEffect.put());
        HRESULT hr5 = dc->CreateEffect(CLSID_D2D1TableTransfer, m_tableTransferEffect.put());

        m_initialized = SUCCEEDED(hr1) && SUCCEEDED(hr2) && SUCCEEDED(hr3)
            && SUCCEEDED(hr4) && SUCCEEDED(hr5);
        if (m_initialized)
            UpdateEffects();
    }

    void ToneMapper::Release()
    {
        m_whiteLevelEffect = nullptr;
        m_hdrToneMapEffect = nullptr;
        m_exposureEffect = nullptr;
        m_preScaleEffect = nullptr;
        m_tableTransferEffect = nullptr;
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

        // Configure white level adjustment (for SDRClamp mode).
        if (m_whiteLevelEffect)
        {
            m_whiteLevelEffect->SetValue(
                D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL, m_sdrWhiteNits);
            m_whiteLevelEffect->SetValue(
                D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL, m_displayMaxNits);
        }

        // Configure HDR tone map (D2D built-in, not used for Reinhard/ACES/Hable).
        if (m_hdrToneMapEffect)
        {
            m_hdrToneMapEffect->SetValue(
                D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, m_displayMaxNits);
            m_hdrToneMapEffect->SetValue(
                D2D1_HDRTONEMAP_PROP_DISPLAY_MODE,
                D2D1_HDRTONEMAP_DISPLAY_MODE_SDR);
        }

        // Build per-channel LUT for Reinhard / ACES / Hable.
        if (m_mode == ToneMapMode::Reinhard
            || m_mode == ToneMapMode::ACESFilmic
            || m_mode == ToneMapMode::Hable)
        {
            BuildToneMappingLUT(m_mode);
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
            // Per-channel tone mapping: pre-scale to [0,1] then apply LUT.
            if (m_preScaleEffect && m_tableTransferEffect)
            {
                m_preScaleEffect->SetInput(0, current);
                winrt::com_ptr<ID2D1Image> preScaled;
                m_preScaleEffect->GetOutput(preScaled.put());

                m_tableTransferEffect->SetInput(0, preScaled.get());
                winrt::com_ptr<ID2D1Image> out;
                m_tableTransferEffect->GetOutput(out.put());
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
    // LUT generation for per-channel tone mapping
    // -----------------------------------------------------------------------

    void ToneMapper::BuildToneMappingLUT(ToneMapMode mode)
    {
        if (!m_preScaleEffect || !m_tableTransferEffect)
            return;

        // Max expected input in scRGB linear (1.0 = 80 nits).
        float maxInput = (std::max)(1.0f, m_displayMaxNits / 80.0f);

        // Pre-scale matrix: normalize RGB from [0, maxInput] to [0, 1] for table lookup.
        float scale = 1.0f / maxInput;
        D2D1_MATRIX_5X4_F preMatrix = D2D1::Matrix5x4F(
            scale, 0, 0, 0,
            0, scale, 0, 0,
            0, 0, scale, 0,
            0, 0, 0, 1,
            0, 0, 0, 0);
        m_preScaleEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, preMatrix);

        // Hable requires white point normalization.
        float hableWhiteScale = 1.0f;
        if (mode == ToneMapMode::Hable)
        {
            constexpr float W = 11.2f;
            float hableW = HableToneMap(W);
            if (hableW > 0.0f)
                hableWhiteScale = 1.0f / hableW;
        }

        // Sample the tone curve into the LUT.
        std::vector<float> lut(kLutSize);
        for (uint32_t i = 0; i < kLutSize; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(kLutSize - 1);
            float x = t * maxInput;

            float y = 0.0f;
            switch (mode)
            {
            case ToneMapMode::Reinhard:   y = ReinhardToneMap(x); break;
            case ToneMapMode::ACESFilmic: y = ACESFilmicToneMap(x); break;
            case ToneMapMode::Hable:      y = HableToneMap(x) * hableWhiteScale; break;
            default:                      y = t; break;
            }
            lut[i] = (std::max)(0.0f, (std::min)(1.0f, y));
        }

        // Apply the same LUT to R, G, B channels; leave alpha untouched.
        auto lutBytes = reinterpret_cast<const BYTE*>(lut.data());
        UINT32 lutByteSize = static_cast<UINT32>(lut.size() * sizeof(float));
        m_tableTransferEffect->SetValue(D2D1_TABLETRANSFER_PROP_RED_TABLE, lutBytes, lutByteSize);
        m_tableTransferEffect->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_TABLE, lutBytes, lutByteSize);
        m_tableTransferEffect->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_TABLE, lutBytes, lutByteSize);
        m_tableTransferEffect->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_DISABLE, TRUE);
        m_tableTransferEffect->SetValue(D2D1_TABLETRANSFER_PROP_CLAMP_OUTPUT, TRUE);
    }

    // -----------------------------------------------------------------------
    // Tone mapping math (for LUT generation and reference)
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
