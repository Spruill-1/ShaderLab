#include "pch.h"
#include "OutputSinkRenderer.h"
#include "Rendering/ImageCapture.h"

namespace ShaderLab::Controls
{
    namespace
    {
        // A runaway-bounds source must not allocate without limit.
        constexpr uint32_t cMaxSinkSide = 8192;

        HRESULT CreateBuffers(uint32_t width, uint32_t height, ID2D1DeviceContext* dc, ID3D11Device* device,
                              const Rendering::PipelineFormat& format, std::shared_ptr<const SinkBuffers>& out)
        {
            auto buffers = std::make_shared<SinkBuffers>();
            buffers->width = width;
            buffers->height = height;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format.dxgiFormat;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            const auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(format.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            for (uint32_t i = 0; i < 2; ++i)
            {
                HRESULT hr = device->CreateTexture2D(&desc, nullptr, buffers->textures[i].put());
                if (FAILED(hr)) return hr;
                winrt::com_ptr<IDXGISurface> surface;
                hr = buffers->textures[i]->QueryInterface(IID_PPV_ARGS(surface.put()));
                if (FAILED(hr)) return hr;
                hr = dc->CreateBitmapFromDxgiSurface(surface.get(), props, buffers->renderTargets[i].put());
                if (FAILED(hr)) return hr;
            }
            out = std::move(buffers);
            return S_OK;
        }
    }

    HRESULT RenderOutputSink(OutputSinkRenderState& sink, ID2D1Image* image, ID2D1DeviceContext* dc,
                             ID3D11Device* device, const Rendering::PipelineFormat& format)
    {
        if (!image || !dc || !device) return S_FALSE;

        D2D1_RECT_F bounds{};
        HRESULT hr = Rendering::GetImageBounds96Dpi(dc, image, bounds);
        if (FAILED(hr)) return hr;
        const uint32_t width = (std::min)(static_cast<uint32_t>(bounds.right - bounds.left), cMaxSinkSide);
        const uint32_t height = (std::min)(static_cast<uint32_t>(bounds.bottom - bounds.top), cMaxSinkSide);
        if (width == 0 || height == 0) return S_FALSE;

        // The window fits the image when it blits, so the buffer is the
        // image's size and knows nothing of the panel's.
        if (!sink.renderBuffers || sink.renderBuffers->width != width || sink.renderBuffers->height != height)
        {
            sink.renderBuffers.reset();
            hr = CreateBuffers(width, height, dc, device, format, sink.renderBuffers);
            if (FAILED(hr)) return hr;
        }

        const auto last = sink.published.load(std::memory_order_acquire);
        const int32_t writeIndex = (last && last->buffers == sink.renderBuffers) ? 1 - last->index : 0;
        auto* target = sink.renderBuffers->renderTargets[writeIndex].get();

        // The render context is shared with the frame: put its target, DPI and
        // transform back.
        winrt::com_ptr<ID2D1Image> previousTarget;
        dc->GetTarget(previousTarget.put());
        float dpiX = 96.0f, dpiY = 96.0f;
        dc->GetDpi(&dpiX, &dpiY);
        D2D1_MATRIX_3X2_F previousTransform{};
        dc->GetTransform(&previousTransform);
        dc->SetTarget(target);
        dc->SetDpi(96.0f, 96.0f);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top));
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        dc->DrawImage(image);
        hr = dc->EndDraw();
        dc->SetTransform(previousTransform);
        dc->SetDpi(dpiX, dpiY);
        dc->SetTarget(previousTarget.get());
        if (FAILED(hr)) return hr;

        auto frame = std::make_shared<SinkFrame>();
        frame->buffers = sink.renderBuffers;
        frame->index = writeIndex;
        frame->version = last ? last->version + 1 : 1;
        sink.published.store(std::move(frame), std::memory_order_release);
        return S_OK;
    }
}
