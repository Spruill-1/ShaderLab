#include "pch_engine.h"
#include "ImageLoader.h"

namespace ShaderLab::Effects
{
    namespace
    {
        // Float/half formats: linear scRGB by convention (JPEG XR HDR,
        // Windows HDR screenshots, float TIFF). No transfer to decode.
        bool IsFloatFormat(const WICPixelFormatGUID& fmt)
        {
            return fmt == GUID_WICPixelFormat64bppRGBAHalf
                || fmt == GUID_WICPixelFormat64bppRGBHalf
                || fmt == GUID_WICPixelFormat128bppRGBAFloat
                || fmt == GUID_WICPixelFormat128bppRGBFloat;
        }

        // 10-bit packed formats: HDR10 stills (PQ transfer, BT.2020
        // primaries) unless an embedded profile says otherwise.
        bool IsPq10Format(const WICPixelFormatGUID& fmt)
        {
            return fmt == GUID_WICPixelFormat32bppRGBA1010102
                || fmt == GUID_WICPixelFormat32bppRGBA1010102XR;
        }

        // 16-bit integer formats: gamma-encoded SDR data at high
        // precision (16-bit PNG/TIFF). Previously misclassified as HDR
        // and fed to the pipeline without any transfer decode.
        bool IsWideIntFormat(const WICPixelFormatGUID& fmt)
        {
            return fmt == GUID_WICPixelFormat48bppRGB
                || fmt == GUID_WICPixelFormat64bppRGBA;
        }

        // First embedded ICC profile as a D2D color context, if any.
        winrt::com_ptr<ID2D1ColorContext> TryGetEmbeddedContext(
            IWICBitmapFrameDecode* frame,
            ID2D1DeviceContext5* dc,
            IWICImagingFactory2* wicFactory)
        {
            winrt::com_ptr<ID2D1ColorContext> ctx;
            UINT count = 0;
            if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0)
                return ctx;

            winrt::com_ptr<IWICColorContext> wicCtx;
            if (FAILED(wicFactory->CreateColorContext(wicCtx.put())))
                return ctx;
            IWICColorContext* slots[] = { wicCtx.get() };
            UINT fetched = 0;
            if (FAILED(frame->GetColorContexts(1, slots, &fetched)) || fetched == 0)
                return ctx;

            // Fails for uncalibrated/Exif-only contexts — caller falls
            // back to the per-format assumption.
            dc->CreateColorContextFromWicColorContext(wicCtx.get(), ctx.put());
            return ctx;
        }
    }

    std::vector<std::wstring> ImageLoader::SupportedExtensions()
    {
        // WIC reports each decoder's extensions as one comma-separated string,
        // e.g. ".jpeg,.jpg,.jpe,.jfif,.exif". Enumerating the registered
        // decoders is the only way to see codecs that arrived as OS extensions
        // (HEIF, AVIF) or third-party RAW packages.
        std::set<std::wstring> unique;

        winrt::com_ptr<IWICImagingFactory2> factory;
        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.put()));

        winrt::com_ptr<IEnumUnknown> components;
        if (SUCCEEDED(hr) && factory)
        {
            hr = factory->CreateComponentEnumerator(
                WICDecoder, WICComponentEnumerateDefault, components.put());
        }

        if (SUCCEEDED(hr) && components)
        {
            // Raw IUnknown*, not com_ptr: com_ptr::put() asserts the pointer is
            // already null, which it is not on the second loop iteration.
            IUnknown* raw = nullptr;
            ULONG fetched = 0;
            while (components->Next(1, &raw, &fetched) == S_OK && fetched == 1)
            {
                winrt::com_ptr<IUnknown> unk;
                unk.attach(raw);
                raw = nullptr;

                auto info = unk.try_as<IWICBitmapDecoderInfo>();
                if (!info) continue;

                UINT needed = 0;
                if (FAILED(info->GetFileExtensions(0, nullptr, &needed)) || needed == 0)
                    continue;
                std::wstring buffer(needed, L'\0');
                if (FAILED(info->GetFileExtensions(needed, buffer.data(), &needed)))
                    continue;
                // `needed` counts the terminating NUL on the second call.
                buffer.resize(needed > 0 ? needed - 1 : 0);

                size_t start = 0;
                while (start <= buffer.size())
                {
                    const size_t comma = buffer.find(L',', start);
                    std::wstring token = buffer.substr(
                        start, comma == std::wstring::npos ? std::wstring::npos : comma - start);

                    // Trim, lowercase, and require a well-formed ".ext":
                    // FileOpenPicker throws E_INVALIDARG on a malformed filter
                    // entry, which would take out the whole dialog over one
                    // badly-registered third-party codec.
                    while (!token.empty() && iswspace(token.front())) token.erase(token.begin());
                    while (!token.empty() && iswspace(token.back()))  token.pop_back();
                    std::transform(token.begin(), token.end(), token.begin(),
                        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

                    const bool wellFormed =
                        token.size() > 1 && token.front() == L'.' &&
                        token.find_first_of(L" \t\\/:*?\"<>|") == std::wstring::npos;
                    if (wellFormed) unique.insert(token);

                    if (comma == std::wstring::npos) break;
                    start = comma + 1;
                }
            }
            if (raw) raw->Release();
        }

        if (unique.empty())
        {
            // Enumeration failed (no WIC, locked-down session). Fall back to the
            // codecs Windows has always shipped in-box.
            //
            // NOT the list that was hardcoded here before: that one offered
            // ".exr" and ".hdr", which no in-box WIC decoder handles and this
            // loader has no special case for -- so picking one could only fail.
            // It is the same defect as the missing .heic, pointing the other
            // way: a hand-maintained list drifts from what WIC can do in both
            // directions at once.
            return { L".bmp", L".dib", L".gif", L".ico", L".jpeg", L".jpg",
                     L".jxr", L".png", L".tif", L".tiff", L".wdp" };
        }
        return { unique.begin(), unique.end() };
    }

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
    // Canonical decode: any WIC frame -> linear scRGB FP16 bitmap
    // -----------------------------------------------------------------------

    winrt::com_ptr<ID2D1Bitmap1> ImageLoader::CreateScRgbBitmap(
        IWICBitmapFrameDecode* frame,
        ID2D1DeviceContext5* dc)
    {
        WICPixelFormatGUID srcFormat{};
        frame->GetPixelFormat(&srcFormat);
        const bool isFloat   = IsFloatFormat(srcFormat);
        const bool isPq10    = IsPq10Format(srcFormat);
        const bool isWideInt = IsWideIntFormat(srcFormat);

        // Intermediate conversion preserves precision but must NOT touch
        // the transfer function — WIC's int->half conversion is a plain
        // normalization of the ENCODED values, and the ColorManagement
        // effect below is what decodes them.
        WICPixelFormatGUID targetFormat = (isFloat || isPq10 || isWideInt)
            ? GUID_WICPixelFormat64bppRGBAHalf
            : GUID_WICPixelFormat32bppPBGRA;

        winrt::com_ptr<IWICFormatConverter> converter;
        if (FAILED(m_wicFactory->CreateFormatConverter(converter.put())))
            return nullptr;
        if (FAILED(converter->Initialize(
                frame, targetFormat,
                WICBitmapDitherTypeNone, nullptr, 0.0f,
                WICBitmapPaletteTypeCustom)))
            return nullptr;

        // Wrap in a PLAIN-format D2D bitmap (no _SRGB variant): decoding
        // is the ColorManagement effect's job, and a hardware
        // decode-on-sample here would double-decode.
        // Note on alpha: PBGRA is premultiplied in the ENCODED space;
        // color-managing premultiplied values is slightly wrong for
        // translucent pixels and exact for opaque ones — acceptable for
        // photographic sources.
        D2D1_BITMAP_PROPERTIES1 rawProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(
                (targetFormat == GUID_WICPixelFormat64bppRGBAHalf)
                    ? DXGI_FORMAT_R16G16B16A16_FLOAT
                    : DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));

        winrt::com_ptr<ID2D1Bitmap1> raw;
        if (FAILED(dc->CreateBitmapFromWicBitmap(converter.get(), &rawProps, raw.put())))
            return nullptr;

        // Source color space: embedded ICC profile wins; otherwise assume
        // by format. Float formats without a profile are already linear
        // scRGB — return them untouched.
        winrt::com_ptr<ID2D1ColorContext> srcCtx =
            TryGetEmbeddedContext(frame, dc, m_wicFactory.get());
        if (!srcCtx)
        {
            if (isFloat)
                return raw;
            if (isPq10)
            {
                winrt::com_ptr<ID2D1ColorContext1> hdr10Ctx;
                dc->CreateColorContextFromDxgiColorSpace(
                    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, hdr10Ctx.put());
                srcCtx = hdr10Ctx;
            }
            else
                dc->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, srcCtx.put());
        }

        winrt::com_ptr<ID2D1ColorContext> dstCtx;
        dc->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, dstCtx.put());
        if (!srcCtx || !dstCtx)
            return raw;   // color management unavailable — best effort

        winrt::com_ptr<ID2D1Effect> cm;
        if (FAILED(dc->CreateEffect(CLSID_D2D1ColorManagement, cm.put())))
            return raw;
        cm->SetInput(0, raw.get());
        cm->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, srcCtx.get());
        cm->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, dstCtx.get());
        // BEST = float-precision path; required for HDR color spaces.
        cm->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);

        // Flatten once at load so every downstream consumer sees one
        // canonical linear-scRGB FP16 bitmap. Assumes no BeginDraw is
        // active on `dc` (loads happen in the source-prep phase).
        const D2D1_SIZE_U px = raw->GetPixelSize();
        D2D1_BITMAP_PROPERTIES1 flatProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1Bitmap1> flat;
        if (FAILED(dc->CreateBitmap(px, nullptr, 0, flatProps, flat.put())))
            return raw;

        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());
        dc->SetTarget(flat.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->DrawImage(cm.get());
        const HRESULT hrEnd = dc->EndDraw();
        dc->SetTarget(oldTarget.get());
        if (FAILED(hrEnd))
            return raw;

        return flat;
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

        winrt::com_ptr<IWICBitmapDecoder> decoder;
        HRESULT hr = m_wicFactory->CreateDecoderFromFilename(
            filePath.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.put());
        if (FAILED(hr))
            return nullptr;

        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.put());
        if (FAILED(hr))
            return nullptr;

        return CreateScRgbBitmap(frame.get(), dc);
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

        return CreateScRgbBitmap(frame.get(), dc);
    }
}
