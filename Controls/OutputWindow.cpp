#include "pch.h"
#include "OutputWindow.h"

#include <microsoft.ui.xaml.media.dxinterop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Input.h>

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

        // P7: create the cross-thread sink. Render worker pushes-ref via
        // MainWindow::m_outputSinks; UI thread mutates view state under
        // m_sink->viewMutex. The shared_ptr keeps it alive across thread
        // boundaries even if the UI thread closes the window mid-render.
        m_sink = std::make_shared<OutputSinkRenderState>();
        m_sink->nodeId = nodeId;

        try
        {
            namespace MUX = winrt::Microsoft::UI::Xaml;
            namespace MUXC = MUX::Controls;

            m_window = MUX::Window();
            m_window.Title(winrt::hstring(nodeName));

            // Build layout: SwapChainPanel + status bar.
            auto rootGrid = MUXC::Grid();
            auto row0 = MUX::GridLength{ 1.0, MUX::GridUnitType::Star };
            auto row1 = MUX::GridLength{ 1.0, MUX::GridUnitType::Auto };
            auto rowDef0 = MUXC::RowDefinition();
            rowDef0.Height(row0);
            auto rowDef1 = MUXC::RowDefinition();
            rowDef1.Height(row1);
            rootGrid.RowDefinitions().Append(rowDef0);
            rootGrid.RowDefinitions().Append(rowDef1);

            m_panel = MUXC::SwapChainPanel();
            MUXC::Grid::SetRow(m_panel, 0);
            rootGrid.Children().Append(m_panel);

            // Status bar with FPS.
            auto statusBar = MUXC::Grid();
            statusBar.Padding(MUX::ThicknessHelper::FromLengths(8, 2, 8, 2));
            statusBar.Background(MUX::Media::SolidColorBrush(
                winrt::Windows::UI::Color{ 255, 32, 32, 32 }));
            MUXC::Grid::SetRow(statusBar, 1);

            m_fpsText = MUXC::TextBlock();
            m_fpsText.Text(L"-- fps");
            m_fpsText.Foreground(MUX::Media::SolidColorBrush(
                winrt::Windows::UI::Color{ 255, 180, 180, 180 }));
            m_fpsText.FontSize(11);
            m_fpsText.VerticalAlignment(MUX::VerticalAlignment::Center);
            statusBar.Children().Append(m_fpsText);

            // Save button in status bar (right-aligned)
            auto saveBtn = MUXC::Button();
            saveBtn.Content(winrt::box_value(L"Save"));
            saveBtn.FontSize(11);
            saveBtn.Padding(MUX::ThicknessHelper::FromLengths(8, 2, 8, 2));
            saveBtn.HorizontalAlignment(MUX::HorizontalAlignment::Right);
            saveBtn.Click([this](auto&&, auto&&) { SaveImageAsync(); });
            statusBar.Children().Append(saveBtn);

            rootGrid.Children().Append(statusBar);

            m_window.Content(rootGrid);

            // Handle window close.
            m_closedToken = m_window.Closed([this](auto&&, auto&&)
            {
                m_isOpen = false;
            });

            // Pointer events for pan/zoom.
            m_panel.PointerWheelChanged([this](auto&&, MUX::Input::PointerRoutedEventArgs const& args)
            {
                auto point = args.GetCurrentPoint(m_panel);
                int delta = point.Properties().MouseWheelDelta();
                float cursorX = static_cast<float>(point.Position().X);
                float cursorY = static_cast<float>(point.Position().Y);

                float factor = (delta > 0) ? 1.1f : (1.0f / 1.1f);
                float newZoom = std::clamp(m_zoom * factor, 0.01f, 100.0f);

                m_panX = cursorX - (cursorX - m_panX) * (newZoom / m_zoom);
                m_panY = cursorY - (cursorY - m_panY) * (newZoom / m_zoom);
                m_zoom = newZoom;
                m_autoFit = false;
                args.Handled(true);
            });

            m_panel.PointerPressed([this](auto&&, MUX::Input::PointerRoutedEventArgs const& args)
            {
                auto point = args.GetCurrentPoint(m_panel);
                if (point.Properties().IsMiddleButtonPressed() ||
                    point.Properties().IsRightButtonPressed())
                {
                    m_isPanning = true;
                    m_autoFit = false;
                    m_panStartX = static_cast<float>(point.Position().X);
                    m_panStartY = static_cast<float>(point.Position().Y);
                    m_panOriginX = m_panX;
                    m_panOriginY = m_panY;
                    m_panel.as<MUX::UIElement>().CapturePointer(args.Pointer());
                    args.Handled(true);
                }
            });

            m_panel.PointerMoved([this](auto&&, MUX::Input::PointerRoutedEventArgs const& args)
            {
                if (m_isPanning)
                {
                    auto point = args.GetCurrentPoint(m_panel);
                    float dx = static_cast<float>(point.Position().X) - m_panStartX;
                    float dy = static_cast<float>(point.Position().Y) - m_panStartY;
                    m_panX = m_panOriginX + dx;
                    m_panY = m_panOriginY + dy;
                    args.Handled(true);
                }
            });

            m_panel.PointerReleased([this](auto&&, MUX::Input::PointerRoutedEventArgs const& args)
            {
                if (m_isPanning)
                {
                    m_isPanning = false;
                    m_panel.as<MUX::UIElement>().ReleasePointerCapture(args.Pointer());
                    args.Handled(true);
                }
            });

            // Double-click to fit.
            m_panel.DoubleTapped([this](auto&&, auto&&)
            {
                m_needsFit = true;
                m_autoFit = true;
            });

            // Wait for panel Loaded before creating DXGI resources.
            m_panel.Loaded([this](auto&&, auto&&)
            {
                try
                {
                    auto scale = static_cast<double>(m_panel.CompositionScaleX());
                    m_width = static_cast<uint32_t>((std::max)(1.0, m_panel.ActualWidth() * scale));
                    m_height = static_cast<uint32_t>((std::max)(1.0, m_panel.ActualHeight() * scale));

                    if (m_width > 0 && m_height > 0)
                    {
                        CreateSwapChain();
                    }

                    // P7: push initial size into the sink so the worker can
                    // create offscreen buffers immediately. Without this the
                    // worker waits one UI tick for SyncSinkFromUi to push
                    // the size and the first frame would render as black.
                    if (m_sink)
                    {
                        std::scoped_lock lock(m_sink->viewMutex);
                        m_sink->requestedW = m_width;
                        m_sink->requestedH = m_height;
                        m_sink->compositionScale =
                            static_cast<float>(m_panel.CompositionScaleX());
                    }

                    m_sizeChangedToken = m_panel.SizeChanged(
                        { this, &OutputWindow::OnPanelSizeChanged });
                }
                catch (...)
                {
                    OutputDebugStringW(L"[OutputWindow] Failed to create swap chain on Loaded\n");
                }
            });

            m_window.AppWindow().Resize(winrt::Windows::Graphics::SizeInt32{ 800, 600 });
            m_window.Activate();
            m_isOpen = true;
        }
        catch (const winrt::hresult_error& ex)
        {
            OutputDebugStringW(std::format(L"[OutputWindow] Create failed: {}\n",
                std::wstring_view(ex.message())).c_str());
            m_isOpen = false;
        }
    }

    void OutputWindow::Close()
    {
        m_isOpen = false;
        // P7: signal to the render worker that this sink is gone. Worker
        // checks `closed` under viewMutex before rendering and skips. UI
        // side keeps the shared_ptr alive long enough that any in-flight
        // render-thread iteration can finish accessing the buffers without
        // a use-after-free.
        if (m_sink)
        {
            std::scoped_lock lock(m_sink->viewMutex);
            m_sink->closed = true;
        }
        m_swapChain = nullptr;

        if (m_window)
        {
            try
            {
                if (m_panel && m_sizeChangedToken.value != 0)
                    m_panel.SizeChanged(m_sizeChangedToken);
                m_window.Close();
            }
            catch (...)
            {
                // Deliberate swallow: teardown. Revoking a token or closing a
                // window that XAML already tore down throws, and every field
                // this method clears is nulled immediately below either way.
            }
            m_window = nullptr;
        }

        m_panel = nullptr;
        m_fpsText = nullptr;
    }

    void OutputWindow::SetTitle(const std::wstring& title)
    {
        m_nodeName = title;
        if (m_window && title != m_lastTitle)
        {
            m_window.Title(winrt::hstring(title));
            m_lastTitle = title;
        }
    }

    void OutputWindow::SetStatusText(const std::wstring& text)
    {
        if (m_fpsText && text != m_lastStatusText)
        {
            m_fpsText.Text(winrt::hstring(text));
            m_lastStatusText = text;
        }
    }

    void OutputWindow::SetStatusTooltip(const std::wstring& tooltip)
    {
        if (!m_fpsText || tooltip == m_lastTooltip) return;
        m_lastTooltip = tooltip;
        // Wrap the tooltip text in a monospace TextBlock for readability,
        // created once and updated in place.
        namespace MUX = winrt::Microsoft::UI::Xaml;
        namespace MUXC = winrt::Microsoft::UI::Xaml::Controls;
        if (!m_tooltipBlock)
        {
            m_tooltipBlock = MUXC::TextBlock();
            m_tooltipBlock.FontFamily(MUX::Media::FontFamily(L"Cascadia Mono, Consolas, Courier New"));
            m_tooltipBlock.FontSize(11);
            MUXC::ToolTipService::SetToolTip(m_fpsText, m_tooltipBlock);
        }
        m_tooltipBlock.Text(winrt::hstring(tooltip));
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
        m_swapChain->SetColorSpace1(m_format.colorSpace);

        auto panelNative = m_panel.as<ISwapChainPanelNative>();
        panelNative->SetSwapChain(m_swapChain.get());
        ApplyInverseCompositionScale();
    }

    // A SwapChainPanel composes its swap chain in DIPs. The buffer here is
    // sized in PHYSICAL pixels (ActualSize * CompositionScale) so the image is
    // sharp -- which means that without the inverse matrix the panel magnifies
    // it by the display scale, anchored at the top-left. At 150% that showed
    // the top-left two thirds of a correctly fitted image (measured: a 34 px
    // letterbox landing at 50 px, the bottom pushed under the status bar).
    // Invisible at 100% scaling. MainWindow::UpdateGraphPanelScale does the
    // same for the node-graph panel.
    void OutputWindow::ApplyInverseCompositionScale()
    {
        if (!m_swapChain || !m_panel) return;
        winrt::com_ptr<IDXGISwapChain2> swapChain2;
        if (FAILED(m_swapChain->QueryInterface(IID_PPV_ARGS(swapChain2.put())))) return;
        const float sx = (std::max)(1e-3f, static_cast<float>(m_panel.CompositionScaleX()));
        const float sy = (std::max)(1e-3f, static_cast<float>(m_panel.CompositionScaleY()));
        DXGI_MATRIX_3X2_F m{};
        m._11 = 1.0f / sx;
        m._22 = 1.0f / sy;
        swapChain2->SetMatrixTransform(&m);
        m_appliedScaleX = sx;
        m_appliedScaleY = sy;
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
            CreateSwapChain();
        }
        else
        {
            m_needsResize = true;
        }
    }

    winrt::fire_and_forget OutputWindow::SaveImageAsync()
    {
        if (!m_dc || !m_lastImage || !m_window)
            co_return;

        // Get HWND for the file picker.
        HWND hwnd{ nullptr };
        auto windowNative = m_window.try_as<::IWindowNative>();
        if (windowNative)
            windowNative->get_WindowHandle(&hwnd);
        if (!hwnd) co_return;

        winrt::Windows::Storage::Pickers::FileSavePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);

        std::wstring suggestedName = m_nodeName.empty() ? L"output" : m_nodeName;
        for (auto& ch : suggestedName)
            if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|')
                ch = L'_';
        picker.SuggestedFileName(winrt::hstring(suggestedName));
        picker.FileTypeChoices().Insert(L"JPEG XR (HDR)", winrt::single_threaded_vector<winrt::hstring>({ L".jxr" }));
        picker.FileTypeChoices().Insert(L"PNG Image (SDR)", winrt::single_threaded_vector<winrt::hstring>({ L".png" }));

        auto file = co_await picker.PickSaveFileAsync();
        if (!file) co_return;

        auto* dc = m_dc;
        auto* image = m_lastImage;
        if (!dc || !image) co_return;

        try
        {
            float oldDpiX, oldDpiY;
            dc->GetDpi(&oldDpiX, &oldDpiY);
            dc->SetDpi(96.0f, 96.0f);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());

            D2D1_RECT_F bounds{};
            dc->GetImageLocalBounds(image, &bounds);
            uint32_t w = static_cast<uint32_t>(bounds.right - bounds.left);
            uint32_t h = static_cast<uint32_t>(bounds.bottom - bounds.top);
            dc->SetDpi(oldDpiX, oldDpiY);
            if (w == 0 || h == 0) co_return;

            auto fileExt = std::wstring(file.FileType().c_str());
            bool isJxr = (fileExt == L".jxr" || fileExt == L".wdp");

            // PNG uses the _SRGB variant: encode the linear scene on
            // write (plain UNORM wrote linear bytes -> dark in viewers).
            // JXR stays FP16 linear scRGB.
            DXGI_FORMAT renderFormat = isJxr
                ? DXGI_FORMAT_R16G16B16A16_FLOAT
                : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

            winrt::com_ptr<ID2D1Bitmap1> renderBitmap;
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(renderFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bmpProps, renderBitmap.put());

            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(renderBitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 1.0f));
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->DrawImage(image);
            dc->EndDraw();
            dc->SetTarget(oldTarget.get());

            winrt::com_ptr<IWICImagingFactory> wicFactory;
            winrt::check_hresult(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wicFactory.put())));

            auto filePath = std::wstring(file.Path().c_str());
            GUID containerFormat = isJxr ? GUID_ContainerFormatWmp : GUID_ContainerFormatPng;

            winrt::com_ptr<IWICStream> stream;
            winrt::check_hresult(wicFactory->CreateStream(stream.put()));
            winrt::check_hresult(stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE));

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(wicFactory->CreateEncoder(containerFormat, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::com_ptr<IPropertyBag2> encoderOptions;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), encoderOptions.put()));

            if (isJxr && encoderOptions)
            {
                PROPBAG2 option{};
                option.pstrName = const_cast<LPOLESTR>(L"Lossless");
                VARIANT val{};
                val.vt = VT_BOOL;
                val.boolVal = VARIANT_TRUE;
                encoderOptions->Write(1, &option, &val);
            }

            winrt::check_hresult(frame->Initialize(encoderOptions.get()));
            winrt::check_hresult(frame->SetSize(w, h));

            WICPixelFormatGUID pixelFormat = isJxr
                ? GUID_WICPixelFormat64bppRGBAHalf
                : GUID_WICPixelFormat32bppBGRA;
            winrt::check_hresult(frame->SetPixelFormat(&pixelFormat));

            winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(renderFormat, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, cpuProps, cpuBitmap.put());
            D2D1_POINT_2U destPoint = { 0, 0 };
            D2D1_RECT_U srcRect = { 0, 0, w, h };
            cpuBitmap->CopyFromBitmap(&destPoint, renderBitmap.get(), &srcRect);

            D2D1_MAPPED_RECT mapped{};
            winrt::check_hresult(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped));
            winrt::check_hresult(frame->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits));
            cpuBitmap->Unmap();

            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            if (m_fpsText)
                m_fpsText.Text(L"Saved: " + file.Name());
        }
        catch (const winrt::hresult_error& ex)
        {
            // Every step above is check_hresult'd, so a WIC/D2D failure lands
            // here. Report it where the success message goes -- silently doing
            // nothing after the user picked a file reads as a no-op UI bug.
            OutputDebugStringW(std::format(L"[OutputWindow] Save failed: {}\n",
                std::wstring_view(ex.message())).c_str());
            if (m_fpsText)
                m_fpsText.Text(L"Save failed: " + ex.message());
        }
        catch (...)
        {
            OutputDebugStringW(L"[OutputWindow] Save failed (non-hresult exception)\n");
            if (m_fpsText)
                m_fpsText.Text(L"Save failed");
        }
    }

    // -----------------------------------------------------------------------
    // P7: cross-thread output rendering
    // -----------------------------------------------------------------------

    void OutputWindow::SyncSinkFromUi()
    {
        // UI thread. Push the current view state into the sink so the render
        // thread sees a coherent snapshot when it draws into this window's
        // offscreen pair. Called once per UI tick before BlitAndPresent.
        if (!m_sink) return;
        std::scoped_lock lock(m_sink->viewMutex);
        m_sink->requestedW = m_width;
        m_sink->requestedH = m_height;
        m_sink->compositionScale = m_panel
            ? static_cast<float>(m_panel.CompositionScaleX())
            : 1.0f;
        m_sink->panX       = m_panX;
        m_sink->panY       = m_panY;
        m_sink->zoom       = m_zoom;
        m_sink->autoFit    = m_autoFit;
        m_sink->needsFit   = m_needsFit;
        // m_needsFit is one-shot: clear it on UI side after publishing so
        // the render thread only fits once per request.
        m_needsFit         = false;
    }

    void OutputWindow::BlitAndPresent(ID2D1DeviceContext5* uiDc)
    {
        // UI thread. If the worker has published a new offscreen frame,
        // wrap it in a UI-context-bound source bitmap and DrawImage into
        // this window's swap chain back buffer with a fit-to-panel
        // transform, then Present1.
        if (!m_isOpen || !m_swapChain || !uiDc || !m_sink) return;

        // Refresh m_width/m_height from the current panel state. The
        // SizeChanged handler is hooked at the END of the panel's Loaded
        // callback, so layout passes that happen between window activation
        // and handler attachment are missed -- the swap chain stays at the
        // initial Loaded-time size forever, even though the panel actually
        // sits at a different layout size. Rechecking here every tick is
        // cheap and lets us catch any missed resize.
        if (m_panel)
        {
            try
            {
                double scale = static_cast<double>(m_panel.CompositionScaleX());
                uint32_t w = static_cast<uint32_t>(
                    (std::max)(1.0, m_panel.ActualWidth() * scale));
                uint32_t h = static_cast<uint32_t>(
                    (std::max)(1.0, m_panel.ActualHeight() * scale));
                if (w > 0 && h > 0 && (w != m_width || h != m_height))
                {
                    m_width = w;
                    m_height = h;
                    m_needsResize = true;
                    m_needsFit = true;
                }
            }
            catch (...)
            {
                // Deliberate swallow: reading XAML layout properties races
                // window teardown. Leaving m_needsResize alone just means the
                // next tick re-reads the size, which is the desired behavior.
            }
        }

        // Handle pending swap-chain resize before consuming a frame.
        //
        // ResizeBuffers fails while ANY reference to a back buffer is alive
        // (a D2D bitmap wrapping it, a context whose target it is). Until this
        // was fixed a leftover wrapper from the pre-P7 Present path pinned
        // buffer 0, so every resize failed -- and because the flag was
        // cleared BEFORE the call, the failure was consumed: the swap chain
        // stayed at its creation size for the life of the window while the
        // fit math used the new panel size, clipping the image and stretching
        // an undersized buffer. The per-blit wrapper below is released (and
        // the target cleared) before we return, so nothing pins it now; a
        // failure stays pending and is logged instead of being swallowed.
        if (m_needsResize)
        {
            if (m_width > 0 && m_height > 0)
            {
                HRESULT hr = m_swapChain->ResizeBuffers(0, m_width, m_height,
                    DXGI_FORMAT_UNKNOWN, 0);
                if (FAILED(hr))
                {
                    OutputDebugStringW(std::format(
                        L"[OutputWindow] ResizeBuffers({}x{}) failed hr=0x{:08X}; will retry\n",
                        m_width, m_height, static_cast<uint32_t>(hr)).c_str());
                    return;
                }
            }
            m_needsResize = false;
            ApplyInverseCompositionScale();
        }
        else if (m_panel &&
                 (static_cast<float>(m_panel.CompositionScaleX()) != m_appliedScaleX ||
                  static_cast<float>(m_panel.CompositionScaleY()) != m_appliedScaleY))
        {
            ApplyInverseCompositionScale();
        }

        const int32_t idx = m_sink->publishedIdx.load(std::memory_order_acquire);
        if (idx < 0 || idx > 1) return;

        // Lazy rebuild of UI-side source wrappers when bufferGen changes
        // (size change). Wrappers must be created on the UI D2D context.
        const uint64_t bufGen = m_sink->bufferGen.load(std::memory_order_acquire);
        if (bufGen != m_sink->uiObservedGen ||
            !m_sink->uiSources[0] || !m_sink->uiSources[1])
        {
            const auto& fmt = m_format;
            D2D1_BITMAP_PROPERTIES1 sp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(fmt.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            for (uint32_t i = 0; i < 2; ++i)
            {
                m_sink->uiSources[i] = nullptr;
                if (!m_sink->textures[i]) continue;
                winrt::com_ptr<IDXGISurface> surface;
                if (SUCCEEDED(m_sink->textures[i]->QueryInterface(
                        IID_PPV_ARGS(surface.put()))))
                {
                    uiDc->CreateBitmapFromDxgiSurface(
                            surface.get(), sp, m_sink->uiSources[i].put());
                }
            }
            m_sink->uiObservedGen = bufGen;
            // First frame after resize / first paint -- recompute fit so
            // the image is centered in the new panel size.
            m_needsFit = true;
        }

        auto* sourceBitmap = m_sink->uiSources[idx].get();
        if (!sourceBitmap) return;

        // Nothing new to show? A fit still pending counts as new (it changes
        // the view below), as does any resize, DPI or pan/zoom change.
        const uint64_t version = m_sink->publishedVersion.load(std::memory_order_acquire);
        const PresentedView view{ m_width, m_height,
            m_panel ? static_cast<float>(m_panel.CompositionScaleX()) : 1.0f,
            m_zoom, m_panX, m_panY };
        // (Auto-fit needs no exception: its result depends only on the panel
        // size and scale, which the view signature already covers.)
        if (version == m_lastBlittedVersion && view == m_lastPresentedView && !m_needsFit)
            return;

        // Cache for SaveImageAsync. The source bitmap wraps the worker's
        // offscreen texture; the wrapper lifetime is governed by uiSources
        // (rebuilt on bufferGen change). Storing a raw pointer here is safe
        // for the duration of this BlitAndPresent and the subsequent UI
        // tick where Save is most likely to fire.
        m_lastImage = sourceBitmap;

        winrt::com_ptr<IDXGISurface> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
        if (FAILED(hr)) return;

        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(m_format.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1Bitmap1> bbBitmap;
        hr = uiDc->CreateBitmapFromDxgiSurface(backBuffer.get(), bp, bbBitmap.put());
        if (FAILED(hr)) return;

        // Auto-fit: compute zoom + pan in DIP space so the image is
        // centered with full coverage. m_panX/m_panY/m_zoom are also the
        // values used by the pointer-input handlers, so the user can
        // manually pan/zoom from this baseline.
        D2D1_SIZE_F srcSize = sourceBitmap->GetSize();
        if ((m_needsFit || m_autoFit) &&
            srcSize.width > 0 && srcSize.height > 0 && m_panel)
        {
            m_needsFit = false;
            float vpDipW = static_cast<float>(m_panel.ActualWidth());
            float vpDipH = static_cast<float>(m_panel.ActualHeight());
            if (vpDipW > 0 && vpDipH > 0)
            {
                m_zoom = (std::min)(vpDipW / srcSize.width,
                                    vpDipH / srcSize.height);
                m_panX = (vpDipW - srcSize.width  * m_zoom) * 0.5f;
                m_panY = (vpDipH - srcSize.height * m_zoom) * 0.5f;
            }
        }

        // Render in panel-DIP space; SetDpi scales to physical pixels.
        float dpi = 96.0f * (m_panel
            ? static_cast<float>(m_panel.CompositionScaleX())
            : 1.0f);
        uiDc->SetTarget(bbBitmap.get());
        uiDc->SetDpi(dpi, dpi);
        uiDc->SetTransform(D2D1::Matrix3x2F::Identity());
        uiDc->BeginDraw();
        uiDc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        uiDc->SetTransform(
            D2D1::Matrix3x2F::Scale(m_zoom, m_zoom) *
            D2D1::Matrix3x2F::Translation(m_panX, m_panY));
        uiDc->DrawImage(sourceBitmap);
        uiDc->SetTransform(D2D1::Matrix3x2F::Identity());

        hr = uiDc->EndDraw();
        uiDc->SetTarget(nullptr);
        if (FAILED(hr)) return;

        DXGI_PRESENT_PARAMETERS params{};
        m_swapChain->Present1(1, 0, &params);
        m_lastBlittedVersion = version;
        m_lastPresentedView = { m_width, m_height,
            m_panel ? static_cast<float>(m_panel.CompositionScaleX()) : 1.0f,
            m_zoom, m_panX, m_panY };
    }
}
