#pragma once

#include "pch.h"
#include "DisplayInfo.h"

namespace ShaderLab::Rendering
{
    // CIE xy chromaticity coordinate.
    struct ChromaticityXY
    {
        float x{ 0.0f };
        float y{ 0.0f };
    };

    // Identifies a well-known display color gamut.
    enum class GamutId : uint32_t
    {
        sRGB = 0,
        DCI_P3,
        BT2020,
        Custom,
    };

    inline std::wstring GamutIdToString(GamutId id)
    {
        switch (id)
        {
        case GamutId::sRGB:    return L"sRGB";
        case GamutId::DCI_P3:  return L"DCI-P3";
        case GamutId::BT2020:  return L"BT.2020";
        case GamutId::Custom:  return L"Custom";
        default:               return L"Unknown";
        }
    }

    // Extended display profile combining capabilities with colorimetry.
    struct DisplayProfile
    {
        DisplayCapabilities caps;

        // Colorimetry — sRGB defaults
        ChromaticityXY primaryRed{ 0.64f, 0.33f };
        ChromaticityXY primaryGreen{ 0.30f, 0.60f };
        ChromaticityXY primaryBlue{ 0.15f, 0.06f };
        ChromaticityXY whitePoint{ 0.3127f, 0.3290f };  // D65
        GamutId gamut{ GamutId::sRGB };

        // Profile metadata
        std::wstring profileName;
        bool isSimulated{ false };
    };

    // -----------------------------------------------------------------------
    // Preset factory functions
    // -----------------------------------------------------------------------

    inline DisplayProfile PresetSrgbSdr()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = false;
        p.caps.bitsPerColor = 8;
        p.caps.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        p.caps.sdrWhiteLevelNits = 80.0f;
        p.caps.maxLuminanceNits = 80.0f;
        p.caps.minLuminanceNits = 0.5f;
        p.caps.maxFullFrameLuminanceNits = 80.0f;
        p.gamut = GamutId::sRGB;
        p.profileName = L"sRGB SDR (80 nits)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetSrgb270()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = false;
        p.caps.bitsPerColor = 8;
        p.caps.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        p.caps.sdrWhiteLevelNits = 80.0f;
        p.caps.maxLuminanceNits = 270.0f;
        p.caps.minLuminanceNits = 0.5f;
        p.caps.maxFullFrameLuminanceNits = 270.0f;
        p.gamut = GamutId::sRGB;
        p.profileName = L"sRGB SDR (270 nits, typical laptop)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetP3_600()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = true;
        p.caps.bitsPerColor = 10;
        p.caps.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        p.caps.sdrWhiteLevelNits = 80.0f;
        p.caps.maxLuminanceNits = 600.0f;
        p.caps.minLuminanceNits = 0.05f;
        p.caps.maxFullFrameLuminanceNits = 500.0f;
        // DCI-P3 primaries
        p.primaryRed   = { 0.680f, 0.320f };
        p.primaryGreen = { 0.265f, 0.690f };
        p.primaryBlue  = { 0.150f, 0.060f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::DCI_P3;
        p.profileName = L"DCI-P3 HDR (600 nits, MacBook Pro-class)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetP3_1000()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = true;
        p.caps.bitsPerColor = 10;
        p.caps.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        p.caps.sdrWhiteLevelNits = 80.0f;
        p.caps.maxLuminanceNits = 1000.0f;
        p.caps.minLuminanceNits = 0.05f;
        p.caps.maxFullFrameLuminanceNits = 600.0f;
        // DCI-P3 primaries
        p.primaryRed   = { 0.680f, 0.320f };
        p.primaryGreen = { 0.265f, 0.690f };
        p.primaryBlue  = { 0.150f, 0.060f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::DCI_P3;
        p.profileName = L"DCI-P3 HDR (1000 nits, reference monitor)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetBT2020_1000()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = true;
        p.caps.bitsPerColor = 10;
        p.caps.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        p.caps.sdrWhiteLevelNits = 80.0f;
        p.caps.maxLuminanceNits = 1000.0f;
        p.caps.minLuminanceNits = 0.005f;
        p.caps.maxFullFrameLuminanceNits = 600.0f;
        // BT.2020 primaries
        p.primaryRed   = { 0.708f, 0.292f };
        p.primaryGreen = { 0.170f, 0.797f };
        p.primaryBlue  = { 0.131f, 0.046f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::BT2020;
        p.profileName = L"BT.2020 HDR (1000 nits, HDR TV)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetBT2020_4000()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = true;
        p.caps.bitsPerColor = 10;
        p.caps.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        p.caps.sdrWhiteLevelNits = 80.0f;
        p.caps.maxLuminanceNits = 4000.0f;
        p.caps.minLuminanceNits = 0.005f;
        p.caps.maxFullFrameLuminanceNits = 1000.0f;
        // BT.2020 primaries
        p.primaryRed   = { 0.708f, 0.292f };
        p.primaryGreen = { 0.170f, 0.797f };
        p.primaryBlue  = { 0.131f, 0.046f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::BT2020;
        p.profileName = L"BT.2020 HDR (4000 nits, mastering)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetAdobeRGB()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = false;
        p.caps.bitsPerColor = 8;
        p.caps.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        p.caps.sdrWhiteLevelNits = 160.0f;
        p.caps.maxLuminanceNits = 160.0f;
        p.caps.minLuminanceNits = 0.5f;
        p.caps.maxFullFrameLuminanceNits = 160.0f;
        // Adobe RGB (1998) primaries
        p.primaryRed   = { 0.6400f, 0.3300f };
        p.primaryGreen = { 0.2100f, 0.7100f };
        p.primaryBlue  = { 0.1500f, 0.0600f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::Custom;
        p.profileName = L"Adobe RGB (1998)";
        p.isSimulated = true;
        return p;
    }

    inline std::vector<DisplayProfile> AllPresets()
    {
        return {
            PresetSrgbSdr(),
            PresetSrgb270(),
            PresetAdobeRGB(),
            PresetP3_600(),
            PresetP3_1000(),
            PresetBT2020_1000(),
            PresetBT2020_4000(),
        };
    }
}
