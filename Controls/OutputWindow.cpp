#include "pch.h"
#include "OutputWindow.h"

#include <microsoft.ui.xaml.media.dxinterop.h>
#include <winrt/Microsoft.UI.Windowing.h>

namespace ShaderLab::Controls
{
    OutputWindow::~OutputWindow()
    {
        Close();
    }

    void OutputWindow::Create(
        ID3D11Device5* d3dDevice,
        ID2D1DeviceContext5* dc,
        IDXGIFactory7* dxgiFactory,
        uint32_t nodeId,
        const std::wstring& nodeName,
        const Rendering::PipelineFormat& format)
    {
        m_d3dDevice = d3dDevice;
        m_dc = dc;
        m_dxgiFactory = dxgiFactory;
        m_nodeId = nodeId;
        m_format = format;

        // Create WinUI 3 window.
        m_window = winrt::Microsoft::UI::Xaml::Window();
        m_window.Title(winrt::hstring(nodeName));

        // Create SwapChainPanel as the window content.
        m_panel = winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel();
        m_panel.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Windows::UI::Color{ 255, 0, 0, 0 }));

        m_window.Content(m_panel);

        // Defer swap chain creation until the panel has loaded and has a size.
        m_sizeChangedToken = m_panel.SizeChanged({ this, &OutputWindow::OnPanelSizeChanged });

        // Handle window close.
        m_closedToken = m_window.Closed([this](auto&&, auto&&)
        {
            m_isOpen = false;
        });

        // Set a default size and activate.
        m_window.AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{ 800, 600 });
        m_window.Activate();
        m_isOpen = true;
    }

    void OutputWindow::Present(ID2D1DeviceContext5* dc, ID2D1Image* image)
    {
        if (!m_isOpen || !m_swapChain || !m_renderTarget)
            return;

        // Handle pending resize.
        if (m_needsResize)
        {
            m_needsResize = false;
            ReleaseRenderTarget();

            if (m_width > 0 && m_height > 0)
            {
                HRESULT hr = m_swapChain->ResizeBuffers(0, m_width, m_height,
                    DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(hr))
                    CreateRenderTarget();
            }

            if (!m_renderTarget)
                return;
        }

        // Save the main window's render target and state.
        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        auto oldTransform = D2D1::Matrix3x2F::Identity();
        dc->GetTransform(&oldTransform);

        // Set our render target.
        dc->SetTarget(m_renderTarget.get());
        float dpi = 96.0f * m_panel.CompositionScaleX();
        dc->SetDpi(dpi, dpi);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());

        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        if (image)
        {
            // Fit the image to the window, maintaining aspect ratio.
            D2D1_RECT_F bounds{};
            dc->GetImageLocalBounds(image, &bounds);
            float imgW = bounds.right - bounds.left;
            float imgH = bounds.bottom - bounds.top;

            if (imgW > 0 && imgH > 0)
            {
                float vpW = static_cast<float>(m_width) / (dpi / 96.0f);
                float vpH = static_cast<float>(m_height) / (dpi / 96.0f);
                float scale = (std::min)(vpW / imgW, vpH / imgH);
                float offX = (vpW - imgW * scale) * 0.5f;
                float offY = (vpH - imgH * scale) * 0.5f;

                dc->SetTransform(
                    D2D1::Matrix3x2F::Scale(scale, scale) *
                    D2D1::Matrix3x2F::Translation(offX, offY));
            }

            dc->DrawImage(image);
        }

        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        HRESULT hr = dc->EndDraw();
        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[OutputWindow] EndDraw error hr=0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
        }

        // Present this window's swap chain.
        DXGI_PRESENT_PARAMETERS params{};
        m_swapChain->Present1(1, 0, &params);

        // Restore the main window's render target and state.
        dc->SetTarget(oldTarget.get());
        dc->SetDpi(oldDpiX, oldDpiY);
        dc->SetTransform(&oldTransform);
    }

    void OutputWindow::Close()
    {
        if (!m_isOpen && !m_window)
            return;

        ReleaseRenderTarget();
        m_swapChain = nullptr;

        if (m_window)
        {
            try { m_window.Close(); } catch (...) {}
            m_window = nullptr;
        }

        m_panel = nullptr;
        m_isOpen = false;
    }

    void OutputWindow::SetTitle(const std::wstring& title)
    {
        if (m_window)
            m_window.Title(winrt::hstring(title));
    }

    void OutputWindow::CreateSwapChain()
    {
        if (!m_d3dDevice || !m_dxgiFactory || m_width == 0 || m_height == 0)
            return;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.Format = m_format.dxgiFormat;
        desc.Stereo = FALSE;
        desc.SampleDesc = { 1, 0 };
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags = 0;

        winrt::com_ptr<IDXGISwapChain1> swapChain1;
        HRESULT hr = m_dxgiFactory->CreateSwapChainForComposition(
            m_d3dDevice,
            &desc,
            nullptr,
            swapChain1.put());

        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[OutputWindow] CreateSwapChain failed hr=0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
            return;
        }

        m_swapChain = swapChain1.as<IDXGISwapChain3>();

        // Set color space.
        m_swapChain->SetColorSpace1(m_format.colorSpace);

        // Bind to the SwapChainPanel.
        auto panelNative = m_panel.as<ISwapChainPanelNative>();
        panelNative->SetSwapChain(m_swapChain.get());
    }

    void OutputWindow::CreateRenderTarget()
    {
        if (!m_swapChain || !m_dc)
            return;

        winrt::com_ptr<IDXGISurface2> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
        if (FAILED(hr)) return;

        D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(m_format.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));

        hr = m_dc->CreateBitmapFromDxgiSurface(
            backBuffer.get(),
            &bitmapProps,
            m_renderTarget.put());

        if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[OutputWindow] CreateRenderTarget failed hr=0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
        }
    }

    void OutputWindow::ReleaseRenderTarget()
    {
        m_renderTarget = nullptr;
    }

    void OutputWindow::OnPanelSizeChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
    {
        auto scale = static_cast<double>(m_panel.CompositionScaleX());
        uint32_t w = static_cast<uint32_t>((std::max)(1.0, args.NewSize().Width * scale));
        uint32_t h = static_cast<uint32_t>((std::max)(1.0, args.NewSize().Height * scale));

        if (w == m_width && h == m_height)
            return;

        m_width = w;
        m_height = h;

        if (!m_swapChain)
        {
            // First size event — create the swap chain now.
            CreateSwapChain();
            CreateRenderTarget();
        }
        else
        {
            // Defer resize to the next Present call (avoids mid-frame resize).
            m_needsResize = true;
        }
    }
}
