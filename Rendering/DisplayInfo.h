#pragma once

#include "pch_engine.h"

namespace ShaderLab::Rendering
{
    // Snapshot of the display's HDR / color capabilities,
    // queried from DXGI_OUTPUT_DESC1 via IDXGIOutput6::GetDesc1.
    struct DisplayCapabilities
    {
        // True when the OS reports Advanced Color (HDR) is active on this output.
        bool hdrEnabled{ false };

        // Bits per color channel reported by the output.
        uint32_t bitsPerColor{ 8 };

        // Color space of the output (e.g. DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 for SDR).
        DXGI_COLOR_SPACE_TYPE colorSpace{ DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 };

        // SDR white level in nits (typically 80 for SDR, higher when HDR is on).
        float sdrWhiteLevelNits{ 80.0f };

        // Peak luminance the display can produce (in nits).
        float maxLuminanceNits{ 270.0f };

        // Minimum luminance the display can produce (in nits).
        float minLuminanceNits{ 0.5f };

        // Maximum full-frame luminance (in nits).
        float maxFullFrameLuminanceNits{ 270.0f };

        // Monitor color primaries from DXGI (CIE xy chromaticity).
        // Default to sRGB/Rec.709 if not available.
        float redPrimaryX{ 0.64f };
        float redPrimaryY{ 0.33f };
        float greenPrimaryX{ 0.30f };
        float greenPrimaryY{ 0.60f };
        float bluePrimaryX{ 0.15f };
        float bluePrimaryY{ 0.06f };
        float whitePointX{ 0.3127f };
        float whitePointY{ 0.3290f };

        // Human-readable summary for the status bar.
        std::wstring ModeString() const
        {
            return hdrEnabled ? L"HDR" : L"SDR";
        }

        std::wstring LuminanceString() const
        {
            // Show both peak and SDR-reference white. SDR white drives any
            // pipeline that needs to know "what nit level does scRGB 1.0
            // resolve to" (everything in the new ICtCp suite, plus the
            // built-in tone mapper). Reading the live OS value is what
            // makes the suite track the user's "SDR content brightness"
            // slider in Windows Settings without a manual override.
            return std::format(L"{:.0f} nits (SDR white {:.0f})",
                maxLuminanceNits, sdrWhiteLevelNits);
        }
    };

    // Callback signature fired when display capabilities change.
    using DisplayChangeCallback = std::function<void(const DisplayCapabilities&)>;
}
