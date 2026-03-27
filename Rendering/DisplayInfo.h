#pragma once

#include "pch.h"

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

        // Human-readable summary for the status bar.
        std::wstring ModeString() const
        {
            return hdrEnabled ? L"HDR" : L"SDR";
        }

        std::wstring LuminanceString() const
        {
            return std::format(L"{:.0f} nits", maxLuminanceNits);
        }
    };

    // Callback signature fired when display capabilities change.
    using DisplayChangeCallback = std::function<void(const DisplayCapabilities&)>;
}
