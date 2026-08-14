#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"

namespace ShaderLab::Effects
{
    // Loads image files from disk via WIC and converts them to ID2D1Bitmap1
    // suitable for use as source images in the effect graph.
    //
    // Contract: EVERY image the loader returns is a linear scRGB FP16
    // bitmap (R16G16B16A16_FLOAT, 1.0 = 80 nits). WIC hands us encoded
    // pixels; the D2D ColorManagement effect performs the transfer decode
    // and primaries conversion at float precision, honoring an embedded
    // ICC profile when present and otherwise assuming per-format:
    //   - 8-bit and 16-bit integer formats → sRGB
    //   - 10-bit 1010102 formats           → HDR10 (PQ / BT.2020)
    //   - float / half formats             → already linear scRGB (no-op)
    // The result is flattened once at load, so downstream consumers see a
    // single canonical format regardless of source.
    class SHADERLAB_API ImageLoader
    {
    public:
        ImageLoader();

        // Load an image from a file path. Returns a linear scRGB FP16
        // bitmap, or nullptr on failure.
        winrt::com_ptr<ID2D1Bitmap1> LoadFromFile(
            const std::wstring& filePath,
            ID2D1DeviceContext5* dc);

        // Load from an already-opened IStream. Same contract.
        winrt::com_ptr<ID2D1Bitmap1> LoadFromStream(
            IStream* stream,
            ID2D1DeviceContext5* dc);

    private:
        // Decode one WIC frame into the canonical linear scRGB FP16
        // bitmap (see class comment). Must be called OUTSIDE an active
        // BeginDraw on `dc` — it flattens through a temporary target.
        winrt::com_ptr<ID2D1Bitmap1> CreateScRgbBitmap(
            IWICBitmapFrameDecode* frame,
            ID2D1DeviceContext5* dc);

        winrt::com_ptr<IWICImagingFactory2> m_wicFactory;
    };
}
