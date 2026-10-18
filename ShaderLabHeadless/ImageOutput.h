#pragma once

// Writing a rendered FP32 RGBA image or pixel region to disk: PNG (8-bit
// sRGB, clamped), JPEG XR (64bpp half, HDR preserved), and the raw / CSV
// formats of --pixels.

#include "Rendering/PixelReadback.h"

#include <string>
#include <wincodec.h>

namespace ShaderLab::Headless
{
    // True when the path's extension asks for the HDR-preserving encoder
    // (.jxr / .wdp).
    bool IsHdrOutputPath(const std::wstring& path);

    // Encodes `rgba` (FP32, `pitchBytes` per row) by the path's extension:
    // JPEG XR for .jxr / .wdp, PNG for anything else.
    HRESULT SaveFp32Image(IWICImagingFactory* wic,
        const float* rgba, uint32_t width, uint32_t height, uint32_t pitchBytes,
        const std::wstring& path);

    // Writes a --pixels region: .csv as 'x,y,r,g,b,a' rows, anything else
    // as uint32 width, uint32 height, then float RGBA row-major. `x`, `y`
    // label the CSV rows. False when the file cannot be opened.
    bool WritePixelRegionFile(const std::wstring& path, const Rendering::ReadPixelRegionResult& region,
                              int32_t x, int32_t y, bool& wroteCsv);
}
