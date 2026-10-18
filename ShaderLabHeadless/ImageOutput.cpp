#include "pch_engine.h"
#include "ImageOutput.h"

// XMConvertFloatToHalf for the JXR (64bpp RGBA half) encode path.
#include <DirectXPackedVector.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace ShaderLab::Headless
{
    namespace
    {
        // Encode an FP32 RGBA buffer as PNG via WIC. PNG is 8-bit per channel;
        // values are gamma-encoded sRGB after a clamp to [0, 1]. This is lossy
        // for HDR scRGB output (anything above 1.0 saturates to 255) -- use a
        // .jxr output path for HDR fidelity (SaveFp32AsJxr below).
        HRESULT SaveFp32AsPng(IWICImagingFactory* wic,
            const float* rgba, uint32_t w, uint32_t h, uint32_t pitchBytes,
            const std::wstring& path)
        {
            // Convert FP32 RGBA to sRGB-gamma 8-bit BGRA.
            std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
            auto sRgbEncode = [](float c) -> uint8_t {
                float clamped = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
                float enc = (clamped <= 0.0031308f)
                    ? clamped * 12.92f
                    : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
                int v = static_cast<int>(enc * 255.0f + 0.5f);
                return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            };
            for (uint32_t y = 0; y < h; ++y)
            {
                const float* srcRow = reinterpret_cast<const float*>(
                    reinterpret_cast<const uint8_t*>(rgba) + y * pitchBytes);
                uint8_t* dstRow = bgra.data() + y * w * 4;
                for (uint32_t x = 0; x < w; ++x)
                {
                    const float* px = srcRow + x * 4;
                    dstRow[x * 4 + 0] = sRgbEncode(px[2]); // B
                    dstRow[x * 4 + 1] = sRgbEncode(px[1]); // G
                    dstRow[x * 4 + 2] = sRgbEncode(px[0]); // R
                    dstRow[x * 4 + 3] = static_cast<uint8_t>(
                        px[3] < 0.0f ? 0 : (px[3] > 1.0f ? 255 : static_cast<int>(px[3] * 255.0f + 0.5f)));
                }
            }

            winrt::com_ptr<IWICStream> stream;
            HRESULT hr = wic->CreateStream(stream.put());
            if (FAILED(hr)) return hr;
            hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
            if (FAILED(hr)) return hr;

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
            if (FAILED(hr)) return hr;
            hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
            if (FAILED(hr)) return hr;

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            hr = encoder->CreateNewFrame(frame.put(), nullptr);
            if (FAILED(hr)) return hr;
            hr = frame->Initialize(nullptr);
            if (FAILED(hr)) return hr;
            hr = frame->SetSize(w, h);
            if (FAILED(hr)) return hr;
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            hr = frame->SetPixelFormat(&fmt);
            if (FAILED(hr)) return hr;
            hr = frame->WritePixels(h, w * 4, w * h * 4, bgra.data());
            if (FAILED(hr)) return hr;
            hr = frame->Commit();
            if (FAILED(hr)) return hr;
            return encoder->Commit();
        }

        // Encode an FP32 RGBA buffer as JPEG XR (.jxr / .wdp) via WIC, preserving
        // HDR. Unlike the PNG path there is NO clamp and NO transfer encoding: the
        // pipeline's scRGB linear values are written as-is into a 64bpp RGBA-half
        // frame, so values above 1.0 (above SDR white) and the negative components
        // that express wide-gamut colour both survive the round trip.
        //
        // FP32 -> FP16 is not a precision loss in practice: the pipeline is
        // R16G16B16A16_FLOAT, so these values originated as halves.
        //
        // Mirrors the GUI's OutputWindow::SaveImageAsync JXR branch
        // (GUID_ContainerFormatWmp + Lossless), so a node saved from an output
        // window and the same node captured headless produce the same file.
        HRESULT SaveFp32AsJxr(IWICImagingFactory* wic,
            const float* rgba, uint32_t w, uint32_t h, uint32_t pitchBytes,
            const std::wstring& path)
        {
            using DirectX::PackedVector::XMConvertFloatToHalf;

            // 64bpp RGBA half, tightly packed.
            std::vector<uint16_t> halfRgba(static_cast<size_t>(w) * h * 4);
            for (uint32_t y = 0; y < h; ++y)
            {
                const float* srcRow = reinterpret_cast<const float*>(
                    reinterpret_cast<const uint8_t*>(rgba) + y * pitchBytes);
                uint16_t* dstRow = halfRgba.data() + static_cast<size_t>(y) * w * 4;
                for (uint32_t x = 0; x < w * 4; ++x)
                    dstRow[x] = XMConvertFloatToHalf(srcRow[x]);
            }

            winrt::com_ptr<IWICStream> stream;
            HRESULT hr = wic->CreateStream(stream.put());
            if (FAILED(hr)) return hr;
            hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
            if (FAILED(hr)) return hr;

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            hr = wic->CreateEncoder(GUID_ContainerFormatWmp, nullptr, encoder.put());
            if (FAILED(hr)) return hr;
            hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
            if (FAILED(hr)) return hr;

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::com_ptr<IPropertyBag2> encoderOptions;
            hr = encoder->CreateNewFrame(frame.put(), encoderOptions.put());
            if (FAILED(hr)) return hr;

            // Lossless: the point of this path is fidelity, not file size.
            if (encoderOptions)
            {
                PROPBAG2 option{};
                option.pstrName = const_cast<LPOLESTR>(L"Lossless");
                VARIANT val{};
                val.vt = VT_BOOL;
                val.boolVal = VARIANT_TRUE;
                encoderOptions->Write(1, &option, &val);
            }

            hr = frame->Initialize(encoderOptions.get());
            if (FAILED(hr)) return hr;
            hr = frame->SetSize(w, h);
            if (FAILED(hr)) return hr;

            WICPixelFormatGUID fmt = GUID_WICPixelFormat64bppRGBAHalf;
            hr = frame->SetPixelFormat(&fmt);
            if (FAILED(hr)) return hr;
            // WIC may negotiate a different format; refuse rather than silently
            // writing something that is not the half-float data we promised.
            if (fmt != GUID_WICPixelFormat64bppRGBAHalf) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;

            const uint32_t stride = w * 4 * sizeof(uint16_t);
            hr = frame->WritePixels(h, stride, stride * h,
                reinterpret_cast<BYTE*>(halfRgba.data()));
            if (FAILED(hr)) return hr;
            hr = frame->Commit();
            if (FAILED(hr)) return hr;
            return encoder->Commit();
        }
    }

    bool IsHdrOutputPath(const std::wstring& path)
    {
        auto dot = path.rfind(L'.');
        if (dot == std::wstring::npos) return false;
        std::wstring ext = path.substr(dot);
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        return ext == L".jxr" || ext == L".wdp";
    }

    HRESULT SaveFp32Image(IWICImagingFactory* wic,
        const float* rgba, uint32_t w, uint32_t h, uint32_t pitchBytes,
        const std::wstring& path)
    {
        if (IsHdrOutputPath(path))
            return SaveFp32AsJxr(wic, rgba, w, h, pitchBytes, path);
        return SaveFp32AsPng(wic, rgba, w, h, pitchBytes, path);
    }

    bool WritePixelRegionFile(const std::wstring& path, const Rendering::ReadPixelRegionResult& region,
                              int32_t x, int32_t y, bool& wroteCsv)
    {
        auto endsWith = [](const std::wstring& s, const wchar_t* suffix) {
            size_t sl = std::wcslen(suffix);
            return s.size() >= sl && std::equal(s.end() - sl, s.end(), suffix);
        };
        wroteCsv = endsWith(path, L".csv") || endsWith(path, L".CSV");

        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        if (wroteCsv)
        {
            f << "x,y,r,g,b,a\n";
            char buf[256];
            for (uint32_t row = 0; row < region.actualHeight; ++row) {
                for (uint32_t col = 0; col < region.actualWidth; ++col) {
                    const float* px = region.pixels.data()
                        + (static_cast<size_t>(row) * region.actualWidth + col) * 4;
                    int n = std::snprintf(buf, sizeof(buf),
                        "%d,%d,%.9g,%.9g,%.9g,%.9g\n",
                        x + static_cast<int32_t>(col),
                        y + static_cast<int32_t>(row),
                        px[0], px[1], px[2], px[3]);
                    f.write(buf, n);
                }
            }
        }
        else
        {
            // The header is the clipped region actually read, which can be
            // smaller than the request at the image edge.
            uint32_t hdr[2] = { region.actualWidth, region.actualHeight };
            f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
            f.write(reinterpret_cast<const char*>(region.pixels.data()),
                static_cast<std::streamsize>(region.pixels.size() * sizeof(float)));
        }
        return true;
    }
}
