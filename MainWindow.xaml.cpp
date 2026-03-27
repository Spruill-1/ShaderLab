#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "Rendering/PipelineFormat.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ShaderLab::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        Title(L"ShaderLab \u2014 HDR Shader Effect Development");

        m_hwnd = GetWindowHandle();
        if (m_hwnd)
        {
            InitializeRendering();
            RegisterCustomEffects();
            UpdateStatusBar();

            // Wire up event handlers.
            PreviewPanel().SizeChanged({ this, &MainWindow::OnPreviewSizeChanged });
            ShaderEditorBox().KeyDown({ this, &MainWindow::OnShaderEditorKeyDown });

            // Bind the node graph controller to our graph.
            m_nodeGraphController.SetGraph(&m_graph);
            m_nodeGraphController.EnsureOutputNode();

            // Initialize pixel inspector with D3D device.
            if (m_renderEngine.D3DDevice())
            {
                m_pixelInspector.Initialize(m_renderEngine.D3DDevice());
            }

            // Initialize tone mapper.
            if (m_renderEngine.D2DDeviceContext())
            {
                m_toneMapper.Initialize(m_renderEngine.D2DDeviceContext());
                auto caps = m_displayMonitor.CachedCapabilities();
                m_toneMapper.SetDisplayMaxLuminance(caps.maxLuminanceNits);
                m_toneMapper.SetSDRWhiteLevel(caps.sdrWhiteLevelNits);
            }

            // Load default shader template into the editor.
            auto defaultShader = ::ShaderLab::Controls::ShaderEditorController::DefaultPixelShaderTemplate();
            ShaderEditorBox().Text(winrt::to_hstring(defaultShader));

            // Start render loop (Step 17).
            m_fpsTimePoint = std::chrono::steady_clock::now();
            m_renderTimer = DispatcherQueue().CreateTimer();
            m_renderTimer.Interval(std::chrono::milliseconds(16)); // ~60 FPS
            m_renderTimer.Tick({ this, &MainWindow::OnRenderTick });
            m_renderTimer.Start();
        }
    }

    MainWindow::~MainWindow()
    {
        if (m_renderTimer)
            m_renderTimer.Stop();

        m_toneMapper.Release();
        m_graphEvaluator.ReleaseCache();
        m_displayMonitor.Shutdown();
        m_renderEngine.Shutdown();
    }

    HWND MainWindow::GetWindowHandle()
    {
        HWND hwnd{ nullptr };
        auto windowNative = this->try_as<::IWindowNative>();
        if (windowNative)
        {
            windowNative->get_WindowHandle(&hwnd);
        }
        return hwnd;
    }

    void MainWindow::InitializeRendering()
    {
        // Query display capabilities and pick a default pipeline format.
        m_displayMonitor.Initialize(m_hwnd);
        auto caps = m_displayMonitor.CachedCapabilities();
        auto format = ::ShaderLab::Rendering::RecommendedFormat(caps);

        // Create D3D11/D2D1 device stack and swap chain on the PreviewPanel.
        m_renderEngine.Initialize(m_hwnd, PreviewPanel(), format);

        // Now that we have a DXGI factory, register adapter-change monitoring.
        if (m_renderEngine.DXGIFactory())
        {
            m_displayMonitor.Shutdown();
            m_displayMonitor.Initialize(m_hwnd, m_renderEngine.DXGIFactory());
        }

        // Subscribe to display changes so we can update the status bar.
        m_displayMonitor.SetCallback([this](const ::ShaderLab::Rendering::DisplayCapabilities& newCaps)
        {
            this->DispatcherQueue().TryEnqueue([this, newCaps]()
            {
                m_toneMapper.SetDisplayMaxLuminance(newCaps.maxLuminanceNits);
                m_toneMapper.SetSDRWhiteLevel(newCaps.sdrWhiteLevelNits);
                UpdateStatusBar();
            });
        });
    }

    void MainWindow::RegisterCustomEffects()
    {
        if (m_customEffectsRegistered) return;

        auto* factory = m_renderEngine.D2DFactory();
        if (!factory) return;

        winrt::com_ptr<ID2D1Factory1> factory1;
        factory->QueryInterface(IID_PPV_ARGS(factory1.put()));
        if (!factory1) return;

        ::ShaderLab::Effects::CustomPixelShaderEffect::RegisterEffect(factory1.get());
        ::ShaderLab::Effects::CustomComputeShaderEffect::RegisterEffect(factory1.get());

        m_customEffectsRegistered = true;
    }

    void MainWindow::UpdateStatusBar()
    {
        auto caps = m_displayMonitor.CachedCapabilities();

        DisplayModeText().Text(L"Display: " + caps.ModeString());
        DisplayLuminanceText().Text(L"Max Luminance: " + caps.LuminanceString());
        PipelineFormatText().Text(L"Pipeline: " + m_renderEngine.ActiveFormat().name);
    }

    void MainWindow::OnPreviewSizeChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args)
    {
        if (!m_renderEngine.IsInitialized())
            return;

        auto scale = PreviewPanel().CompositionScaleX();
        auto w = static_cast<uint32_t>((std::max)(1.0f, static_cast<float>(args.NewSize().Width) * scale));
        auto h = static_cast<uint32_t>((std::max)(1.0f, static_cast<float>(args.NewSize().Height) * scale));
        m_renderEngine.Resize(w, h);
    }

    void MainWindow::OnShaderEditorKeyDown(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
    {
        // Ctrl+Enter triggers shader compilation.
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter)
        {
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

            if (ctrlDown)
            {
                auto hlsl = winrt::to_string(ShaderEditorBox().Text());
                auto result = m_shaderEditor.Compile(hlsl);

                if (!result.succeeded)
                {
                    // Show error in status bar (simplified).
                    PipelineFormatText().Text(L"Shader Error: " + result.errorText.substr(0, 80));
                }
                else
                {
                    PipelineFormatText().Text(L"Shader compiled OK (" +
                        std::to_wstring(result.autoProperties.size()) + L" properties)");
                    m_graph.MarkAllDirty();
                }

                args.Handled(true);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Render loop (Step 17)
    // -----------------------------------------------------------------------

    void MainWindow::OnRenderTick(
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& /*sender*/,
        winrt::Windows::Foundation::IInspectable const& /*args*/)
    {
        RenderFrame();

        // Update FPS counter every second.
        m_frameCount++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fpsTimePoint).count();
        if (elapsed >= 1000)
        {
            float fps = static_cast<float>(m_frameCount) * 1000.0f / static_cast<float>(elapsed);
            FpsText().Text(std::format(L"FPS: {:.0f}", fps));
            m_frameCount = 0;
            m_fpsTimePoint = now;
        }
    }

    void MainWindow::RenderFrame()
    {
        if (!m_renderEngine.IsInitialized())
            return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        // Evaluate the effect graph.
        auto* graphOutput = m_graphEvaluator.Evaluate(m_graph, dc);

        // Apply tone mapping.
        if (graphOutput && m_toneMapper.IsActive())
        {
            graphOutput = m_toneMapper.Apply(graphOutput);
        }

        // Begin draw to swap chain.
        auto* drawDc = m_renderEngine.BeginDraw();
        if (!drawDc)
            return;

        drawDc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        // Draw the graph output (if any).
        if (graphOutput)
        {
            drawDc->DrawImage(graphOutput);
        }

        // Draw the node graph overlay on the left panel area
        // (this is rendered separately via the controller).
        // In a full implementation, the node graph renders to its own
        // surface/layer. For now, it's available for integration.

        m_renderEngine.EndDraw();
        m_renderEngine.Present();
    }
}
