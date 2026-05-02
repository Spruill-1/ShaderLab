#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"

namespace ShaderLab::Effects
{
    // Loads image files from disk via WIC and converts them to ID2D1Bitmap1
    // suitable for use as source images in the effect graph.
    //
    // Supports common formats (PNG, JPEG, TIFF, BMP, DDS, HDR, HEIF, etc.)
    // through the Windows Imaging Component codec pipeline.
    //
    // For HDR images, the loader converts to GUID_WICPixelFormat64bppRGBAHalf
    // (FP16) to preserve extended range. For SDR, it uses GUID_WICPixelFormat32bppPBGRA.
    class SHADERLAB_API ImageLoader
    {
    public:
        ImageLoader();

        // Load an image from a file path and return a D2D bitmap.
        // The bitmap pixel format depends on the source:
        //   - SDR images → B8G8R8A8_UNORM (premultiplied alpha)
        //   - HDR images → R16G16B16A16_FLOAT (scRGB)
        // Returns nullptr on failure.
        winrt::com_ptr<ID2D1Bitmap1> LoadFromFile(
            const std::wstring& filePath,
            ID2D1DeviceContext5* dc);

        // Load from an already-opened IStream.
        winrt::com_ptr<ID2D1Bitmap1> LoadFromStream(
            IStream* stream,
            ID2D1DeviceContext5* dc);

    private:
        // Determine if a WIC pixel format is HDR (floating-point / >8bpc).
        static bool IsHdrPixelFormat(const WICPixelFormatGUID& fmt);

        winrt::com_ptr<IWICImagingFactory2> m_wicFactory;
    };
}
