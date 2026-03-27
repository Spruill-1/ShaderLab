#pragma once

#include "pch.h"
#include "DisplayInfo.h"

namespace ShaderLab::Rendering
{
    // Identifies a pipeline-wide pixel format + color space combination.
    // Everything that touches pixels — swap chain, render targets, tone mapper,
    // pixel inspector — reads from the active PipelineFormat.
    struct PipelineFormat
    {
        DXGI_FORMAT             dxgiFormat{ DXGI_FORMAT_R16G16B16A16_FLOAT };
        DXGI_COLOR_SPACE_TYPE   colorSpace{ DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 };
        std::wstring            name{ L"scRGB FP16" };

        // Bits per channel (for UI / inspector display).
        uint32_t bitsPerChannel{ 16 };

        // True if the format uses a linear (gamma 1.0) transfer function.
        bool isLinear{ true };

        // True if the format can represent values outside [0,1] (HDR headroom).
        bool isFloatingPoint{ true };

        // Bytes per pixel (useful for buffer allocation).
        uint32_t BytesPerPixel() const
        {
            switch (dxgiFormat)
            {
            case DXGI_FORMAT_B8G8R8A8_UNORM:      return 4;
            case DXGI_FORMAT_R10G10B10A2_UNORM:    return 4;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:   return 8;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:   return 16;
            default:                               return 0;
            }
        }

        bool operator==(const PipelineFormat& other) const
        {
            return dxgiFormat == other.dxgiFormat && colorSpace == other.colorSpace;
        }
    };

    // ----- Predefined pipeline formats -----

    // scRGB FP16 — default. Linear, full HDR range, negative values allowed.
    // Ideal for HDR compositing and effect authoring.
    inline const PipelineFormat FormatScRgbFP16{
        .dxgiFormat    = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .colorSpace    = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
        .name          = L"scRGB FP16",
        .bitsPerChannel = 16,
        .isLinear       = true,
        .isFloatingPoint = true,
    };

    // sRGB 8-bit — standard SDR. Gamma 2.2, [0–255] per channel.
    inline const PipelineFormat FormatSrgb8{
        .dxgiFormat    = DXGI_FORMAT_B8G8R8A8_UNORM,
        .colorSpace    = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
        .name          = L"sRGB 8-bit",
        .bitsPerChannel = 8,
        .isLinear       = false,
        .isFloatingPoint = false,
    };

    // HDR10 — PQ transfer, BT.2020 primaries, 10-bit.
    // Used when targeting HDR10 displays or PQ-encoded content.
    inline const PipelineFormat FormatHdr10{
        .dxgiFormat    = DXGI_FORMAT_R10G10B10A2_UNORM,
        .colorSpace    = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
        .name          = L"HDR10",
        .bitsPerChannel = 10,
        .isLinear       = false,
        .isFloatingPoint = false,
    };

    // Linear FP32 — maximum precision. 32-bit float per channel, linear.
    // For precision-critical work (deep compositing, scientific visualization).
    inline const PipelineFormat FormatLinearFP32{
        .dxgiFormat    = DXGI_FORMAT_R32G32B32A32_FLOAT,
        .colorSpace    = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
        .name          = L"Linear FP32",
        .bitsPerChannel = 32,
        .isLinear       = true,
        .isFloatingPoint = true,
    };

    // All available formats in display order (for UI combo boxes, etc.).
    inline const PipelineFormat AllFormats[] = {
        FormatScRgbFP16,
        FormatSrgb8,
        FormatHdr10,
        FormatLinearFP32,
    };

    // Returns a sensible default format based on the current display capabilities.
    // - HDR display active → scRGB FP16 (full HDR compositing)
    // - SDR display        → sRGB 8-bit (avoids unnecessary bandwidth)
    inline PipelineFormat RecommendedFormat(const DisplayCapabilities& caps)
    {
        return caps.hdrEnabled ? FormatScRgbFP16 : FormatSrgb8;
    }
}
