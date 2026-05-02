#include "pch_engine.h"
#include "FalseColorOverlay.h"

namespace ShaderLab::Rendering
{
    // -----------------------------------------------------------------------
    // Initialization
    // -----------------------------------------------------------------------

    void FalseColorOverlay::Initialize(ID2D1DeviceContext* dc)
    {
        if (!dc) return;
        m_dc.copy_from(dc);

        // LuminanceZones: luminance extraction + zone color LUT.
        HRESULT hr1 = dc->CreateEffect(CLSID_D2D1ColorMatrix, m_luminanceMatrix.put());
        HRESULT hr2 = dc->CreateEffect(CLSID_D2D1TableTransfer, m_zoneTransfer.put());

        // Clipping: pre-scale + clipping indicator LUT.
        HRESULT hr3 = dc->CreateEffect(CLSID_D2D1ColorMatrix, m_clippingPreScale.put());
        HRESULT hr4 = dc->CreateEffect(CLSID_D2D1TableTransfer, m_clippingTransfer.put());

        // OutOfGamut: pre-scale + gamut indicator LUT.
        HRESULT hr5 = dc->CreateEffect(CLSID_D2D1ColorMatrix, m_gamutPreScale.put());
        HRESULT hr6 = dc->CreateEffect(CLSID_D2D1TableTransfer, m_gamutTransfer.put());

        m_initialized = SUCCEEDED(hr1) && SUCCEEDED(hr2)
            && SUCCEEDED(hr3) && SUCCEEDED(hr4)
            && SUCCEEDED(hr5) && SUCCEEDED(hr6);

        if (m_initialized)
        {
            BuildLuminanceZoneLUT();
            BuildClippingLUT();
            BuildOutOfGamutLUT();
        }
    }

    void FalseColorOverlay::Release()
    {
        m_luminanceMatrix = nullptr;
        m_zoneTransfer = nullptr;
        m_clippingPreScale = nullptr;
        m_clippingTransfer = nullptr;
        m_gamutPreScale = nullptr;
        m_gamutTransfer = nullptr;
        m_output = nullptr;
        m_dc = nullptr;
        m_initialized = false;
    }

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------

    void FalseColorOverlay::SetMode(FalseColorMode mode)
    {
        m_mode = mode;
    }

    void FalseColorOverlay::SetDisplayMaxLuminance(float nits)
    {
        float newMax = (std::max)(1.0f, nits);
        if (newMax != m_displayMaxNits)
        {
            m_displayMaxNits = newMax;
            if (m_initialized)
            {
                BuildLuminanceZoneLUT();
                BuildClippingLUT();
                BuildOutOfGamutLUT();
            }
        }
    }

    // -----------------------------------------------------------------------
    // Apply
    // -----------------------------------------------------------------------

    ID2D1Image* FalseColorOverlay::Apply(ID2D1Image* input)
    {
        if (!input || !m_initialized || m_mode == FalseColorMode::None)
            return input;

        switch (m_mode)
        {
        case FalseColorMode::LuminanceZones:
        {
            if (m_luminanceMatrix && m_zoneTransfer)
            {
                m_luminanceMatrix->SetInput(0, input);
                winrt::com_ptr<ID2D1Image> luminance;
                m_luminanceMatrix->GetOutput(luminance.put());

                m_zoneTransfer->SetInput(0, luminance.get());
                m_output = nullptr;
                m_zoneTransfer->GetOutput(m_output.put());
                return m_output.get();
            }
            break;
        }

        case FalseColorMode::Clipping:
        {
            if (m_clippingPreScale && m_clippingTransfer)
            {
                m_clippingPreScale->SetInput(0, input);
                winrt::com_ptr<ID2D1Image> scaled;
                m_clippingPreScale->GetOutput(scaled.put());

                m_clippingTransfer->SetInput(0, scaled.get());
                m_output = nullptr;
                m_clippingTransfer->GetOutput(m_output.put());
                return m_output.get();
            }
            break;
        }

        case FalseColorMode::OutOfGamut:
        {
            if (m_gamutPreScale && m_gamutTransfer)
            {
                m_gamutPreScale->SetInput(0, input);
                winrt::com_ptr<ID2D1Image> scaled;
                m_gamutPreScale->GetOutput(scaled.put());

                m_gamutTransfer->SetInput(0, scaled.get());
                m_output = nullptr;
                m_gamutTransfer->GetOutput(m_output.put());
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
    // LUT: Luminance Zones
    // -----------------------------------------------------------------------
    //
    // Chain: ColorMatrix (extract BT.709 luminance, replicate to RGB, scale to
    //        [0,1] over [0, maxScRGB]) → TableTransfer (zone colors).
    //
    // Zone thresholds in nits (scRGB 1.0 = 80 nits):
    //   <  1 nit   → deep blue  (0.0, 0.0, 0.3)
    //   1–10       → blue       (0.0, 0.0, 0.8)
    //   10–80      → cyan       (0.0, 0.6, 0.6)
    //   80–200     → green      (0.0, 0.8, 0.0)
    //   200–1000   → yellow     (0.8, 0.8, 0.0)
    //   > 1000     → red        (0.8, 0.0, 0.0)

    void FalseColorOverlay::BuildLuminanceZoneLUT()
    {
        if (!m_luminanceMatrix || !m_zoneTransfer) return;

        float maxScRGB = (std::max)(1.0f, m_displayMaxNits / 80.0f);
        float invMax = 1.0f / maxScRGB;

        // ColorMatrix: extract luminance (BT.709) into all three channels and
        // scale from [0, maxScRGB] to [0, 1] for the table lookup.
        //
        // D2D1_MATRIX_5X4_F layout:
        //   outputColor.r = R*_11 + G*_21 + B*_31 + A*_41 + _51
        //   outputColor.g = R*_12 + G*_22 + B*_32 + A*_42 + _52
        //   ...
        float rw = 0.2126f * invMax;
        float gw = 0.7152f * invMax;
        float bw = 0.0722f * invMax;
        D2D1_MATRIX_5X4_F lumMatrix = D2D1::Matrix5x4F(
            rw, rw, rw, 0,     // R input → all output channels (scaled)
            gw, gw, gw, 0,     // G input
            bw, bw, bw, 0,     // B input
            0,  0,  0,  1,     // A input → A output only
            0,  0,  0,  0      // offset
        );
        m_luminanceMatrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, lumMatrix);

        // Zone color lookup tables.
        struct ZoneColor { float r, g, b; };
        constexpr ZoneColor zones[] = {
            { 0.0f, 0.0f, 0.3f },  // < 1 nit   : deep blue
            { 0.0f, 0.0f, 0.8f },  // 1–10 nits  : blue
            { 0.0f, 0.6f, 0.6f },  // 10–80 nits : cyan
            { 0.0f, 0.8f, 0.0f },  // 80–200 nits: green
            { 0.8f, 0.8f, 0.0f },  // 200–1000   : yellow
            { 0.8f, 0.0f, 0.0f },  // > 1000 nits: red
        };

        // Thresholds in nits.
        constexpr float thresholds[] = { 1.0f, 10.0f, 80.0f, 200.0f, 1000.0f };
        constexpr size_t zoneCount = std::size(zones);

        std::vector<float> lutR(kLutSize);
        std::vector<float> lutG(kLutSize);
        std::vector<float> lutB(kLutSize);

        for (uint32_t i = 0; i < kLutSize; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(kLutSize - 1);
            float nits = t * maxScRGB * 80.0f;

            // Find the zone for this luminance.
            size_t zone = 0;
            for (size_t z = 0; z < std::size(thresholds); ++z)
            {
                if (nits >= thresholds[z])
                    zone = z + 1;
            }
            if (zone >= zoneCount) zone = zoneCount - 1;

            lutR[i] = zones[zone].r;
            lutG[i] = zones[zone].g;
            lutB[i] = zones[zone].b;
        }

        UINT32 lutBytes = static_cast<UINT32>(kLutSize * sizeof(float));
        m_zoneTransfer->SetValue(D2D1_TABLETRANSFER_PROP_RED_TABLE,
            reinterpret_cast<const BYTE*>(lutR.data()), lutBytes);
        m_zoneTransfer->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_TABLE,
            reinterpret_cast<const BYTE*>(lutG.data()), lutBytes);
        m_zoneTransfer->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_TABLE,
            reinterpret_cast<const BYTE*>(lutB.data()), lutBytes);
        m_zoneTransfer->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_DISABLE, TRUE);
        m_zoneTransfer->SetValue(D2D1_TABLETRANSFER_PROP_CLAMP_OUTPUT, TRUE);
    }

    // -----------------------------------------------------------------------
    // LUT: Clipping
    // -----------------------------------------------------------------------
    //
    // Per-channel clipping indicator.  After pre-scaling from [0, maxScRGB]
    // to [0, 1], values near 0 get a blue tint and values near 1 get a red
    // tint.  Mid-range values are rendered as neutral gray so the indicator
    // colors stand out.
    //
    // This is a per-channel approximation — it shows each channel's clip
    // status independently rather than requiring cross-channel OR logic.

    void FalseColorOverlay::BuildClippingLUT()
    {
        if (!m_clippingPreScale || !m_clippingTransfer) return;

        float maxScRGB = (std::max)(1.0f, m_displayMaxNits / 80.0f);
        float invMax = 1.0f / maxScRGB;

        // Pre-scale to [0, 1] (identity color matrix with scale).
        D2D1_MATRIX_5X4_F preMatrix = D2D1::Matrix5x4F(
            invMax, 0, 0, 0,
            0, invMax, 0, 0,
            0, 0, invMax, 0,
            0, 0, 0, 1,
            0, 0, 0, 0
        );
        m_clippingPreScale->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, preMatrix);

        // Thresholds in normalized [0,1] space.
        // "Black clip" = original value <= 0.0 → first LUT entry.
        // "White clip" = original value >= maxScRGB → last LUT entry.
        // Small region near each end is flagged.
        float blackThreshold = 1.0f / 80.0f * invMax;  // ~0 nits boundary
        float whiteThreshold = 1.0f - (1.0f / static_cast<float>(kLutSize - 1));

        std::vector<float> lutR(kLutSize);
        std::vector<float> lutG(kLutSize);
        std::vector<float> lutB(kLutSize);

        for (uint32_t i = 0; i < kLutSize; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(kLutSize - 1);

            if (t <= blackThreshold)
            {
                // Clipped to black → blue indicator.
                lutR[i] = 0.0f;
                lutG[i] = 0.0f;
                lutB[i] = 0.9f;
            }
            else if (t >= whiteThreshold)
            {
                // Clipped to white → red indicator.
                lutR[i] = 0.9f;
                lutG[i] = 0.0f;
                lutB[i] = 0.0f;
            }
            else
            {
                // Mid-range: desaturated pass-through (neutral gray from the channel value).
                lutR[i] = t;
                lutG[i] = t;
                lutB[i] = t;
            }
        }

        UINT32 lutBytes = static_cast<UINT32>(kLutSize * sizeof(float));
        m_clippingTransfer->SetValue(D2D1_TABLETRANSFER_PROP_RED_TABLE,
            reinterpret_cast<const BYTE*>(lutR.data()), lutBytes);
        m_clippingTransfer->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_TABLE,
            reinterpret_cast<const BYTE*>(lutG.data()), lutBytes);
        m_clippingTransfer->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_TABLE,
            reinterpret_cast<const BYTE*>(lutB.data()), lutBytes);
        m_clippingTransfer->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_DISABLE, TRUE);
        m_clippingTransfer->SetValue(D2D1_TABLETRANSFER_PROP_CLAMP_OUTPUT, TRUE);
    }

    // -----------------------------------------------------------------------
    // LUT: Out of Gamut
    // -----------------------------------------------------------------------
    //
    // Per-channel gamut indicator.  scRGB values can be negative (out of sRGB
    // gamut).  TableTransfer clamps its input to [0,1] before lookup, so any
    // negative input always maps to table[0].  We exploit this:
    //
    //   table[0] = magenta indicator value (per-channel R/B high, G low)
    //   table[1..N] = neutral gray pass-through
    //
    // Pre-scale shifts values so that scRGB 0.0 maps to a small positive LUT
    // index, making the table[0] entry reachable only for originally-negative
    // values.

    void FalseColorOverlay::BuildOutOfGamutLUT()
    {
        if (!m_gamutPreScale || !m_gamutTransfer) return;

        float maxScRGB = (std::max)(1.0f, m_displayMaxNits / 80.0f);

        // Shift so that scRGB 0.0 lands at LUT index ~2 (out of 255).
        // scRGB range [-margin, maxScRGB] → [0, 1].
        float margin = maxScRGB * 0.02f;
        float range = maxScRGB + margin;
        float scale = 1.0f / range;
        float offset = margin / range;

        D2D1_MATRIX_5X4_F preMatrix = D2D1::Matrix5x4F(
            scale, 0, 0, 0,
            0, scale, 0, 0,
            0, 0, scale, 0,
            0, 0, 0, 1,
            offset, offset, offset, 0
        );
        m_gamutPreScale->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, preMatrix);

        // Index where scRGB 0.0 falls after the shift.
        uint32_t zeroIdx = static_cast<uint32_t>(offset * static_cast<float>(kLutSize - 1) + 0.5f);
        if (zeroIdx < 1) zeroIdx = 1;

        std::vector<float> lutR(kLutSize);
        std::vector<float> lutG(kLutSize);
        std::vector<float> lutB(kLutSize);

        for (uint32_t i = 0; i < kLutSize; ++i)
        {
            if (i < zeroIdx)
            {
                // Negative scRGB → magenta indicator.
                lutR[i] = 0.8f;
                lutG[i] = 0.0f;
                lutB[i] = 0.8f;
            }
            else
            {
                // In-gamut: neutral gray pass-through.
                float t = static_cast<float>(i - zeroIdx)
                    / static_cast<float>(kLutSize - 1 - zeroIdx);
                lutR[i] = t;
                lutG[i] = t;
                lutB[i] = t;
            }
        }

        UINT32 lutBytes = static_cast<UINT32>(kLutSize * sizeof(float));
        m_gamutTransfer->SetValue(D2D1_TABLETRANSFER_PROP_RED_TABLE,
            reinterpret_cast<const BYTE*>(lutR.data()), lutBytes);
        m_gamutTransfer->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_TABLE,
            reinterpret_cast<const BYTE*>(lutG.data()), lutBytes);
        m_gamutTransfer->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_TABLE,
            reinterpret_cast<const BYTE*>(lutB.data()), lutBytes);
        m_gamutTransfer->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_DISABLE, TRUE);
        m_gamutTransfer->SetValue(D2D1_TABLETRANSFER_PROP_CLAMP_OUTPUT, TRUE);
    }
}
