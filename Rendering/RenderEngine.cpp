#include "pch.h"
#include "RenderEngine.h"
#include "DeviceStack.h"

#include <microsoft.ui.xaml.media.dxinterop.h>

namespace ShaderLab::Rendering
{
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void RenderEngine::Initialize(
        HWND hwnd,
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel,
        const PipelineFormat& format,
        DevicePreference devicePref)
    {
        m_hwnd = hwnd;
        m_panel = panel;
        m_format = format;

        CreateDeviceResources(devicePref);
        CreateSwapChain(panel);
        ConfigureSwapChainColorSpace();
        CreateRenderTarget();
    }

    void RenderEngine::Shutdown()
    {
        ReleaseRenderTarget();
        // The offscreen pair belongs to this device. Kept across an adapter
        // switch, EnsureOffscreenTargets would see the right size, skip the
        // recreate, and every later EndDraw would target the old device.
        ReleaseOffscreenTargets();

        // Clear the swap chain reference from the XAML panel BEFORE
        // releasing it, otherwise the compositor crashes accessing
        // a dangling swap chain.
        if (m_panel)
        {
            try
            {
                auto panelNative = m_panel.as<ISwapChainPanelNative>();
                if (panelNative) panelNative->SetSwapChain(nullptr);
            }
            catch (...)
            {
                // Deliberate swallow: teardown. The panel may already be torn
                // down by XAML, and there is nothing useful to do about it --
                // we are releasing the swap chain on the next line regardless.
            }
        }

        m_swapChain = nullptr;
        m_d2dDeviceContext = nullptr;
        m_renderD2dContext = nullptr;
        m_d2dDevice = nullptr;
        m_d2dFactory = nullptr;
        m_gpuTimer.Shutdown();
        m_d3dContext = nullptr;
        m_d3dDevice = nullptr;
        m_dxgiFactory = nullptr;
        m_panel = nullptr;
    }

    // -----------------------------------------------------------------------
    // Device creation
    // -----------------------------------------------------------------------

    void RenderEngine::CreateDeviceResources(DevicePreference devicePref)
    {
        DeviceStackOptions options;
        options.adapterLuid = m_preferredAdapterLuid;
        switch (devicePref)
        {
        case DevicePreference::Default:  options.choice = AdapterChoice::Default;  break;
        case DevicePreference::Hardware: options.choice = AdapterChoice::Hardware; break;
        case DevicePreference::Warp:     options.choice = AdapterChoice::Warp;     break;
        case DevicePreference::Adapter:  options.choice = AdapterChoice::Adapter;  break;
        }
        DeviceStack stack;
        std::wstring failedStep;
        if (const HRESULT hr = CreateDeviceStack(options, stack, &failedStep); FAILED(hr))
            throw winrt::hresult_error(hr, winrt::hstring(L"Creating the GPU device failed at " + failedStep));

        m_dxgiFactory = stack.dxgiFactory;
        m_d3dDevice   = stack.d3dDevice;
        m_d3dContext  = stack.d3dContext;
        m_d2dFactory  = stack.d2dFactory;
        m_d2dDevice   = stack.d2dDevice;
        m_adapterName = stack.adapterName;
        m_isWarp      = stack.isWarp;
        m_gpuTimer.Initialize(m_d3dDevice.get());

        // Two contexts on the one device: the UI thread's, and the render
        // worker's. Target, transform and DPI are per context, so the worker's
        // BeginDraw/EndDraw cannot be disturbed by UI-thread draws (capture,
        // pixel inspector).
        winrt::check_hresult(stack.CreateDeviceContext(m_d2dDeviceContext));
        winrt::check_hresult(stack.CreateDeviceContext(m_renderD2dContext));
    }

    // -----------------------------------------------------------------------
    // Swap chain
    // -----------------------------------------------------------------------

    void RenderEngine::CreateSwapChain(
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel)
    {
        // Query the panel's current pixel size.
        auto scale = static_cast<double>(panel.CompositionScaleX());
        m_width = static_cast<uint32_t>((std::max)(1.0, panel.ActualWidth() * scale));
        m_height = static_cast<uint32_t>((std::max)(1.0, panel.ActualHeight() * scale));

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
        winrt::check_hresult(
            m_dxgiFactory->CreateSwapChainForComposition(
                m_d3dDevice.as<IDXGIDevice>().get(),
                &desc,
                nullptr,
                swapChain1.put()));

        m_swapChain = swapChain1.as<IDXGISwapChain3>();

        // Bind to SwapChainPanel via ISwapChainPanelNative.
        auto panelNative = panel.as<ISwapChainPanelNative>();
        winrt::check_hresult(panelNative->SetSwapChain(m_swapChain.get()));

        m_compositionScaleX = static_cast<float>(panel.CompositionScaleX());
        m_compositionScaleY = static_cast<float>(panel.CompositionScaleY());
        ApplyCompositionScaleMatrix();
    }

    void RenderEngine::SetCompositionScale(float scaleX, float scaleY)
    {
        m_compositionScaleX = scaleX;
        m_compositionScaleY = scaleY;
        ApplyCompositionScaleMatrix();
    }

    void RenderEngine::ApplyCompositionScaleMatrix()
    {
        if (!m_swapChain) return;
        winrt::com_ptr<IDXGISwapChain2> swapChain2;
        if (FAILED(m_swapChain->QueryInterface(IID_PPV_ARGS(swapChain2.put())))) return;
        DXGI_MATRIX_3X2_F m{};
        m._11 = 1.0f / (std::max)(1e-3f, m_compositionScaleX);
        m._22 = 1.0f / (std::max)(1e-3f, m_compositionScaleY);
        swapChain2->SetMatrixTransform(&m);
    }

    void RenderEngine::ConfigureSwapChainColorSpace()
    {
        if (!m_swapChain)
            return;

        // SetColorSpace1 tells DXGI how to interpret the pixel data.
        winrt::check_hresult(
            m_swapChain->SetColorSpace1(m_format.colorSpace));
    }

    // -----------------------------------------------------------------------
    // Render target (D2D bitmap backed by swap chain back buffer)
    // -----------------------------------------------------------------------

    void RenderEngine::CreateRenderTarget()
    {
        if (!m_swapChain || !m_d2dDeviceContext)
            return;

        winrt::com_ptr<IDXGISurface2> backBuffer;
        winrt::check_hresult(
            m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())));

        D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(m_format.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));

        winrt::com_ptr<ID2D1Bitmap1> targetBitmap;
        winrt::check_hresult(
            m_d2dDeviceContext->CreateBitmapFromDxgiSurface(
                backBuffer.get(),
                &bitmapProps,
                targetBitmap.put()));

        m_renderTarget = std::move(targetBitmap);
        m_d2dDeviceContext->SetTarget(m_renderTarget.get());

        // Fixed 96 DPI: a per-frame DPI change invalidates every
        // D2D1_PROPERTY_CACHED intermediate (caches are DPI-referenced).
        // Pixel-exact sizing uses explicit transforms, not context DPI.
        m_d2dDeviceContext->SetDpi(96.0f, 96.0f);
    }

    void RenderEngine::ReleaseRenderTarget()
    {
        if (m_d2dDeviceContext)
            m_d2dDeviceContext->SetTarget(nullptr);
        m_renderTarget = nullptr;
    }

    // -----------------------------------------------------------------------
    // Resize
    // -----------------------------------------------------------------------

    void RenderEngine::Resize(uint32_t widthPixels, uint32_t heightPixels)
    {
        if (!m_swapChain)
            return;

        if (widthPixels == 0 || heightPixels == 0)
            return;

        if (widthPixels == m_width && heightPixels == m_height)
            return;

        m_width = widthPixels;
        m_height = heightPixels;

        // Must release the render target before resizing buffers.
        ReleaseRenderTarget();

        // Flush any pending D3D work.
        m_d3dContext->Flush();

        winrt::check_hresult(
            m_swapChain->ResizeBuffers(
                0,          // keep current count
                m_width,
                m_height,
                DXGI_FORMAT_UNKNOWN,  // keep current format
                0));

        ApplyCompositionScaleMatrix();
        CreateRenderTarget();
    }

    // -----------------------------------------------------------------------
    // Per-frame rendering
    // -----------------------------------------------------------------------

    ID2D1DeviceContext5* RenderEngine::BeginDraw()
    {
        m_d2dDeviceContext->BeginDraw();
        return m_d2dDeviceContext.get();
    }

    void RenderEngine::EndDraw()
    {
        HRESULT hr = m_d2dDeviceContext->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET)
        {
            // Device lost — caller should recreate resources.
            ReleaseRenderTarget();
        }
        else if (FAILED(hr))
        {
            OutputDebugStringW(std::format(L"[Render] EndDraw error hr=0x{:08X}\n",
                static_cast<uint32_t>(hr)).c_str());
        }
    }

    void RenderEngine::Present(bool vsync)
    {
        DXGI_PRESENT_PARAMETERS params{};
        winrt::check_hresult(
            m_swapChain->Present1(vsync ? 1 : 0, 0, &params));
    }

    // -----------------------------------------------------------------------
    // Offscreen target pair
    // -----------------------------------------------------------------------

    bool RenderEngine::EnsureOffscreenTargets(uint32_t width, uint32_t height)
    {
        if (!m_d3dDevice || !m_renderD2dContext)
            return false;

        if (width == 0 || height == 0)
            return false;

        // Already at the right size?
        if (m_offscreenWidth == width && m_offscreenHeight == height &&
            m_offscreenTexture[0] && m_offscreenTexture[1] &&
            m_offscreenRenderBitmap[0] && m_offscreenRenderBitmap[1])
        {
            return true;
        }

        ReleaseOffscreenTargets();

        // Pipeline format matches the swap chain so there's no color-space
        // conversion at the blit. scRGB FP16 by default.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = m_format.dxgiFormat; // matches swap chain
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        for (uint32_t i = 0; i < 2; ++i)
        {
            HRESULT hr = m_d3dDevice->CreateTexture2D(&td, nullptr,
                m_offscreenTexture[i].put());
            if (FAILED(hr))
            {
                ReleaseOffscreenTargets();
                return false;
            }

            // Wrap as D2D bitmap on the render-thread D2D context. This is
            // the bitmap the render thread sets as its draw target.
            winrt::com_ptr<IDXGISurface> surface;
            hr = m_offscreenTexture[i]->QueryInterface(IID_PPV_ARGS(surface.put()));
            if (FAILED(hr))
            {
                ReleaseOffscreenTargets();
                return false;
            }

            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(m_format.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            hr = m_renderD2dContext->CreateBitmapFromDxgiSurface(
                surface.get(), bp, m_offscreenRenderBitmap[i].put());
            if (FAILED(hr))
            {
                ReleaseOffscreenTargets();
                return false;
            }
        }

        m_offscreenWidth = width;
        m_offscreenHeight = height;
        return true;
    }

    void RenderEngine::ReleaseOffscreenTargets()
    {
        for (uint32_t i = 0; i < 2; ++i)
        {
            m_offscreenRenderBitmap[i] = nullptr;
            m_offscreenTexture[i] = nullptr;
        }
        m_offscreenWidth = 0;
        m_offscreenHeight = 0;
    }

    // -----------------------------------------------------------------------
    // Adapter enumeration
    // -----------------------------------------------------------------------

    std::vector<AdapterInfo> RenderEngine::EnumerateAdapters()
    {
        std::vector<AdapterInfo> result;

        winrt::com_ptr<IDXGIFactory6> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put()))))
            return result;

        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            // Skip software adapters other than WARP.
            bool isWarp = (desc.VendorId == 0x1414 && desc.DeviceId == 0x008C);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !isWarp)
            {
                adapter = nullptr;
                continue;
            }

            AdapterInfo info;
            info.name = desc.Description;
            info.vendorId = desc.VendorId;
            info.deviceId = desc.DeviceId;
            info.dedicatedVideoMemoryMB = static_cast<size_t>(desc.DedicatedVideoMemory / (1024 * 1024));
            info.luid = desc.AdapterLuid;
            info.isWarp = isWarp;
            result.push_back(std::move(info));

            adapter = nullptr;
        }

        // Always add WARP as an option if not already present.
        bool hasWarp = false;
        for (const auto& a : result)
            if (a.isWarp) { hasWarp = true; break; }
        if (!hasWarp)
        {
            AdapterInfo warp;
            warp.name = L"Microsoft Basic Render Driver (WARP)";
            warp.isWarp = true;
            result.push_back(std::move(warp));
        }

        return result;
    }

    // -----------------------------------------------------------------------
    // Reinitialize on a different adapter
    // -----------------------------------------------------------------------

    void RenderEngine::Reinitialize(DevicePreference devicePref, LUID adapterLuid)
    {
        m_preferredAdapterLuid = adapterLuid;

        // Save state that Shutdown clears.
        auto savedPanel = m_panel;
        auto savedHwnd = m_hwnd;
        auto savedFormat = m_format;
        auto savedWidth = m_width;
        auto savedHeight = m_height;

        Shutdown();

        m_panel = savedPanel;
        m_hwnd = savedHwnd;
        m_format = savedFormat;

        // Recreate on the new adapter.
        CreateDeviceResources(devicePref);
        CreateSwapChain(m_panel);
        ConfigureSwapChainColorSpace();
        CreateRenderTarget();

        if (savedWidth > 0 && savedHeight > 0)
            Resize(savedWidth, savedHeight);
    }
}
