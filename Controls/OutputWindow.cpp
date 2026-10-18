#include "pch.h"
#include "OutputWindow.h"
#include "Rendering/ImageCapture.h"

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
        m_saveDc.copy_from(dc);
        m_dxgiFactory = dxgiFactory;
        m_nodeId = nodeId;
        m_format = format;

        // Shared with the render worker through MainWindow::m_outputSinks.
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

            // Double-click turns auto-fit back on.
            m_panel.DoubleTapped([this](auto&&, auto&&) { m_autoFit = true; });

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
        // The worker skips a closed sink; one it is drawing stays alive
        // through its own reference.
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
    // it by the display scale, anchored at the top-left (invisible at 100%).
    // MainWindow::UpdateGraphPanelScale does the same for the node-graph panel.
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
        if (!m_saveDc || !m_lastImage || !m_window)
            co_return;

        // The picker suspends this coroutine: hold the image and context, and
        // check the window still exists before touching members again.
        const auto dcHold = m_saveDc;
        winrt::com_ptr<ID2D1Image> imageHold = m_lastImage;
        std::weak_ptr<bool> alive = m_alive;

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
        if (!file || alive.expired()) co_return;

        auto* dc = dcHold.get();
        auto* image = imageHold.get();

        try
        {
            const std::wstring fileExt(file.FileType());
            const bool isJxr = (fileExt == L".jxr" || fileExt == L".wdp");
            // PNG uses the _SRGB variant: encode the linear scene on
            // write (plain UNORM wrote linear bytes -> dark in viewers).
            // JXR stays FP16 linear scRGB.
            const DXGI_FORMAT renderFormat = isJxr
                ? DXGI_FORMAT_R16G16B16A16_FLOAT
                : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

            D2D1_RECT_F bounds{};
            winrt::check_hresult(Rendering::GetImageBounds96Dpi(dc, image, bounds));
            const uint32_t w = static_cast<uint32_t>((std::max)(0.0f, bounds.right - bounds.left));
            const uint32_t h = static_cast<uint32_t>((std::max)(0.0f, bounds.bottom - bounds.top));
            if (w == 0 || h == 0) co_return;

            Rendering::PixelBuffer pixels;
            winrt::check_hresult(Rendering::RenderToPixels(dc, w, h, renderFormat, D2D1::ColorF(0, 0, 0, 1.0f),
                [&](ID2D1DeviceContext* target)
                {
                    target->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top));
                    target->DrawImage(image);
                },
                pixels));
            winrt::check_hresult(Rendering::EncodeToFile(pixels, std::wstring(file.Path())));

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
    // Presenting the worker's frames
    // -----------------------------------------------------------------------

    void OutputWindow::BlitAndPresent(ID2D1DeviceContext5* uiDc)
    {
        // UI thread. If the worker has published a new offscreen frame,
        // wrap it in a UI-context-bound source bitmap and DrawImage into
        // this window's swap chain back buffer with a fit-to-panel
        // transform, then Present1.
        if (!m_isOpen || !m_swapChain || !uiDc || !m_sink) return;

        // Re-read the panel size: SizeChanged is hooked only at the end of
        // the panel's Loaded handler, so layout passes before that are missed.
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
                }
            }
            catch (...)
            {
                // Deliberate swallow: reading XAML layout properties races
                // window teardown. Leaving m_needsResize alone just means the
                // next tick re-reads the size, which is the desired behavior.
            }
        }

        // Resize before consuming a frame. ResizeBuffers fails while anything
        // references a back buffer, so the per-blit wrapper below is released
        // (and the target cleared) before returning; a failed resize stays
        // pending and is retried.
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

        // One load: the buffer set, index and version belong together.
        const auto frame = m_sink->published.load(std::memory_order_acquire);
        if (!frame || !frame->buffers) return;

        // Source bitmaps on the UI context, rebuilt when the worker
        // reallocates (a resize); the frame's shared_ptr keeps its buffer set
        // alive however far the worker has moved on.
        if (frame->buffers != m_sink->uiBuffers)
        {
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(m_format.dxgiFormat, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            for (uint32_t i = 0; i < 2; ++i)
            {
                m_sink->uiSources[i] = nullptr;
                winrt::com_ptr<IDXGISurface> surface;
                if (frame->buffers->textures[i] &&
                    SUCCEEDED(frame->buffers->textures[i]->QueryInterface(IID_PPV_ARGS(surface.put()))))
                    uiDc->CreateBitmapFromDxgiSurface(surface.get(), props, m_sink->uiSources[i].put());
            }
            m_sink->uiBuffers = frame->buffers;
            // A new size: fit it to the panel.
            m_needsFit = true;
        }

        auto* sourceBitmap = m_sink->uiSources[frame->index].get();
        if (!sourceBitmap) return;

        // Nothing new to show? A fit still pending counts as new (it changes
        // the view below), as does any resize, DPI or pan/zoom change.
        const uint64_t version = frame->version;
        const PresentedView view{ m_width, m_height,
            m_panel ? static_cast<float>(m_panel.CompositionScaleX()) : 1.0f,
            m_zoom, m_panX, m_panY };
        // (Auto-fit needs no exception: its result depends only on the panel
        // size and scale, which the view signature already covers.)
        if (version == m_lastBlittedVersion && view == m_lastPresentedView && !m_needsFit)
            return;

        // Saved by SaveImageAsync, which holds its own reference.
        m_lastImage.copy_from(sourceBitmap);

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
