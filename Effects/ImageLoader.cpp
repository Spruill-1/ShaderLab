#include "pch_engine.h"
#include "ImageLoader.h"

namespace ShaderLab::Effects
{
    ImageLoader::ImageLoader()
    {
        winrt::check_hresult(
            CoCreateInstance(
                CLSID_WICImagingFactory2,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(m_wicFactory.put())));
    }

    // -----------------------------------------------------------------------
    // HDR pixel format detection
    // -----------------------------------------------------------------------

    bool ImageLoader::IsHdrPixelFormat(const WICPixelFormatGUID& fmt)
    {
        // Floating-point and >8bpc formats that indicate HDR content.
        return fmt == GUID_WICPixelFormat64bppRGBAHalf
            || fmt == GUID_WICPixelFormat64bppRGBHalf
            || fmt == GUID_WICPixelFormat128bppRGBAFloat
            || fmt == GUID_WICPixelFormat128bppRGBFloat
            || fmt == GUID_WICPixelFormat48bppRGB
            || fmt == GUID_WICPixelFormat64bppRGBA
            || fmt == GUID_WICPixelFormat32bppRGBA1010102
            || fmt == GUID_WICPixelFormat32bppRGBA1010102XR;
    }

    // -----------------------------------------------------------------------
    // Load from file
    // -----------------------------------------------------------------------

    winrt::com_ptr<ID2D1Bitmap1> ImageLoader::LoadFromFile(
        const std::wstring& filePath,
        ID2D1DeviceContext5* dc)
    {
        if (!dc || !m_wicFactory || filePath.empty())
            return nullptr;

        // Decode the image file.
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        HRESULT hr = m_wicFactory->CreateDecoderFromFilename(
            filePath.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.put());
        if (FAILED(hr))
            return nullptr;

        // Get the first frame.
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.put());
        if (FAILED(hr))
            return nullptr;

        // Check the source pixel format to decide SDR vs HDR path.
        WICPixelFormatGUID srcFormat{};
        frame->GetPixelFormat(&srcFormat);
        bool isHdr = IsHdrPixelFormat(srcFormat);

        // Convert to a D2D-compatible pixel format.
        // HDR → 64bpp RGBA Half (FP16, scRGB-compatible)
        // SDR → 32bpp PBGRA (premultiplied, B8G8R8A8_UNORM)
        WICPixelFormatGUID targetFormat = isHdr
            ? GUID_WICPixelFormat64bppRGBAHalf
            : GUID_WICPixelFormat32bppPBGRA;

        winrt::com_ptr<IWICFormatConverter> converter;
        hr = m_wicFactory->CreateFormatConverter(converter.put());
        if (FAILED(hr))
            return nullptr;

        hr = converter->Initialize(
            frame.get(),
            targetFormat,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
            return nullptr;

        // Create the D2D bitmap from the WIC source.
        D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(
                isHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));

        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        hr = dc->CreateBitmapFromWicBitmap(converter.get(), &bitmapProps, bitmap.put());
        if (FAILED(hr))
            return nullptr;

        return bitmap;
    }

    // -----------------------------------------------------------------------
    // Load from stream
    // -----------------------------------------------------------------------

    winrt::com_ptr<ID2D1Bitmap1> ImageLoader::LoadFromStream(
        IStream* stream,
        ID2D1DeviceContext5* dc)
    {
        if (!dc || !m_wicFactory || !stream)
            return nullptr;

        winrt::com_ptr<IWICBitmapDecoder> decoder;
        HRESULT hr = m_wicFactory->CreateDecoderFromStream(
            stream,
            nullptr,
            WICDecodeMetadataCacheOnDemand,
            decoder.put());
        if (FAILED(hr))
            return nullptr;

        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.put());
        if (FAILED(hr))
            return nullptr;

        WICPixelFormatGUID srcFormat{};
        frame->GetPixelFormat(&srcFormat);
        bool isHdr = IsHdrPixelFormat(srcFormat);

        WICPixelFormatGUID targetFormat = isHdr
            ? GUID_WICPixelFormat64bppRGBAHalf
            : GUID_WICPixelFormat32bppPBGRA;

        winrt::com_ptr<IWICFormatConverter> converter;
        hr = m_wicFactory->CreateFormatConverter(converter.put());
        if (FAILED(hr))
            return nullptr;

        hr = converter->Initialize(
            frame.get(),
            targetFormat,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
            return nullptr;

        D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(
                isHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));

        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        hr = dc->CreateBitmapFromWicBitmap(converter.get(), &bitmapProps, bitmap.put());
        if (FAILED(hr))
            return nullptr;

        return bitmap;
    }
}
