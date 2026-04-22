#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "Rendering/PipelineFormat.h"
#include "Rendering/IccProfileParser.h"
#include <microsoft.ui.xaml.media.dxinterop.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ShaderLab::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        Title(L"ShaderLab \u2014 HDR Shader Effect Development");

        m_hwnd = GetWindowHandle();

        // Wire up event handlers (safe before panel is loaded).
        PreviewPanel().SizeChanged({ this, &MainWindow::OnPreviewSizeChanged });
        PreviewPanel().PointerMoved({ this, &MainWindow::OnPreviewPointerMoved });
        PreviewPanel().KeyDown({ this, &MainWindow::OnPreviewKeyDown });
        PreviewPanel().IsTabStop(true);
        PreviewNodeSelector().SelectionChanged({ this, &MainWindow::OnPreviewNodeSelectionChanged });
        FalseColorSelector().SelectedIndex(0);
        FalseColorSelector().SelectionChanged({ this, &MainWindow::OnFalseColorSelectionChanged });

        PopulateDisplayProfileSelector();
        DisplayProfileSelector().SelectionChanged({ this, &MainWindow::OnDisplayProfileSelectionChanged });

        PreviewPanel().PointerPressed({ this, &MainWindow::OnPreviewPointerPressed });
        PreviewPanel().PointerReleased({ this, &MainWindow::OnPreviewPointerReleased });
        PreviewPanel().PointerWheelChanged([this](auto&&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            auto point = args.GetCurrentPoint(PreviewPanel());
            int delta = point.Properties().MouseWheelDelta();
            float cursorX = static_cast<float>(point.Position().X);
            float cursorY = static_cast<float>(point.Position().Y);

            float zoomFactor = (delta > 0) ? 1.1f : (1.0f / 1.1f);
            float newZoom = std::clamp(m_previewZoom * zoomFactor, 0.01f, 100.0f);

            // Zoom centered on cursor position (DIP coordinates).
            m_previewPanX = cursorX - (cursorX - m_previewPanX) * (newZoom / m_previewZoom);
            m_previewPanY = cursorY - (cursorY - m_previewPanY) * (newZoom / m_previewZoom);
            m_previewZoom = newZoom;
            args.Handled(true);
        });
        TraceUnitSelector().SelectedIndex(0);
        TraceUnitSelector().SelectionChanged({ this, &MainWindow::OnTraceUnitSelectionChanged });

        SaveGraphButton().Click({ this, &MainWindow::OnSaveGraphClicked });
        LoadGraphButton().Click({ this, &MainWindow::OnLoadGraphClicked });
        // Populate the Add Node flyout with effects from the registry.
        PopulateAddNodeFlyout();

        CompareToggle().Click({ this, &MainWindow::OnCompareToggled });
        CompareNodeSelector().SelectionChanged({ this, &MainWindow::OnCompareNodeSelectionChanged });

        // Compare and Pixel Trace are mutually exclusive.
        BottomTabView().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isShuttingDown) return;
            if (BottomTabView().SelectedIndex() == 1 && m_compareActive)
            {
                // Switching to Pixel Trace → disable compare.
                m_compareActive = false;
                CompareToggle().IsChecked(false);
                CompareToolbar().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            }
            UpdateCrosshairOverlay();
        });

        // Defer GPU initialization until the SwapChainPanel is in the visual tree.
        PreviewPanel().Loaded([this](auto&&, auto&&) { OnPreviewPanelLoaded(); });
        NodeGraphPanel().SizeChanged([this](auto&&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& e)
        {
            // Use DIP dimensions directly — DXGI stretches to physical pixels.
            auto w = static_cast<uint32_t>((std::max)(1.0f, static_cast<float>(e.NewSize().Width)));
            auto h = static_cast<uint32_t>((std::max)(1.0f, static_cast<float>(e.NewSize().Height)));
            ResizeGraphPanel(w, h);
        });
        NodeGraphContainer().PointerPressed({ this, &MainWindow::OnGraphPanelPointerPressed });
        NodeGraphContainer().PointerMoved({ this, &MainWindow::OnGraphPanelPointerMoved });
        NodeGraphContainer().PointerReleased({ this, &MainWindow::OnGraphPanelPointerReleased });
        NodeGraphContainer().KeyDown([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
        {
            if (args.Key() == winrt::Windows::System::VirtualKey::Delete && m_selectedNodeId != 0)
            {
                // Protect the Output node from deletion.
                const auto* node = m_graph.FindNode(m_selectedNodeId);
                if (node && node->type == ::ShaderLab::Graph::NodeType::Output)
                    return;

                m_nodeGraphController.DeleteSelected();
                m_selectedNodeId = 0;
                m_graph.MarkAllDirty();
                m_nodeGraphController.RebuildLayout();
                PopulatePreviewNodeSelector();
                UpdatePropertiesPanel();

                // Reset preview to Output node.
                for (const auto& n : m_graph.Nodes())
                {
                    if (n.type == ::ShaderLab::Graph::NodeType::Output)
                    {
                        m_previewNodeId = n.id;
                        break;
                    }
                }
                PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                args.Handled(true);
            }
        });
        NodeGraphContainer().IsTabStop(true);

        SaveImageButton().Click({ this, &MainWindow::OnSaveImageClicked });
        EffectDesignerButton().Click([this](auto&&, auto&&) { OpenEffectDesigner(); });

        // MCP server toggle.
        McpServerToggle().Click([this](auto&&, auto&&)
        {
            bool checked = McpServerToggle().IsChecked().GetBoolean();
            if (checked)
            {
                if (!m_mcpServer)
                    SetupMcpRoutes();
                if (m_mcpServer && !m_mcpServer->IsRunning())
                    m_mcpServer->Start(47808);
                McpServerToggle().Content(winrt::box_value(L"MCP Server :47808"));
                McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            }
            else
            {
                if (m_mcpServer)
                    m_mcpServer->Stop();
                McpServerToggle().Content(winrt::box_value(L"MCP Server"));
                McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            }
        });

        McpExportConfigButton().Click([this](auto&&, auto&&)
        {
            // Copy MCP server URL to clipboard.
            namespace DP = winrt::Windows::ApplicationModel::DataTransfer;
            auto pkg = DP::DataPackage();
            std::wstring config = std::format(
                L"{{\n"
                L"  \"mcpServers\": {{\n"
                L"    \"shaderlab\": {{\n"
                L"      \"url\": \"http://localhost:47808/\"\n"
                L"    }}\n"
                L"  }}\n"
                L"}}");
            pkg.SetText(config);
            DP::Clipboard::SetContent(pkg);
            PipelineFormatText().Text(L"MCP config copied to clipboard (http://localhost:47808)");
        });
    }

    MainWindow::~MainWindow()
    {
        m_isShuttingDown = true;

        // Stop MCP server before tearing down resources.
        if (m_mcpServer)
            m_mcpServer->Stop();

        if (m_renderTimer)
        {
            m_renderTimer.Stop();
            m_renderTimer = nullptr;
        }

        m_traceSwapChain = nullptr;
        m_traceSwatchTarget = nullptr;
        m_falseColor.Release();
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
        m_renderEngine.Initialize(m_hwnd, PreviewPanel(), format, m_devicePref);

        // Now that we have a DXGI factory, register adapter-change monitoring.
        if (m_renderEngine.DXGIFactory())
        {
            m_displayMonitor.Shutdown();
            m_displayMonitor.Initialize(m_hwnd, m_renderEngine.DXGIFactory());
        }

        // Subscribe to display changes so we can update the status bar.
        m_displayMonitor.SetCallback([this](const ::ShaderLab::Rendering::DisplayCapabilities& /*newCaps*/)
        {
            this->DispatcherQueue().TryEnqueue([this]()
            {
                // Use ActiveProfile() so simulated profiles aren't overwritten by live changes.
                auto caps = m_displayMonitor.CachedCapabilities();
                m_toneMapper.SetDisplayMaxLuminance(caps.maxLuminanceNits);
                m_toneMapper.SetSDRWhiteLevel(caps.sdrWhiteLevelNits);
                m_falseColor.SetDisplayMaxLuminance(caps.maxLuminanceNits);
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

        HRESULT hr1 = ::ShaderLab::Effects::CustomPixelShaderEffect::RegisterEffect(factory1.get());
        HRESULT hr2 = ::ShaderLab::Effects::CustomComputeShaderEffect::RegisterEffect(factory1.get());
        OutputDebugStringW(std::format(L"[CustomFX] RegisterEffect pixel=0x{:08X} compute=0x{:08X}\n",
            static_cast<uint32_t>(hr1), static_cast<uint32_t>(hr2)).c_str());

        m_customEffectsRegistered = true;
    }

    void MainWindow::OnPreviewPanelLoaded()
    {
        if (!m_hwnd || m_renderEngine.IsInitialized()) return;

        InitializeRendering();
        RegisterCustomEffects();

        // Pre-setup MCP routes (server starts when user toggles the button or --mcp flag).
        SetupMcpRoutes();
        if (m_autoStartMcp && m_mcpServer)
        {
            m_mcpServer->Start(47808);
            McpServerToggle().IsChecked(true);
            McpServerToggle().Content(winrt::box_value(L"MCP Server :47808"));
            McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }

        UpdateStatusBar();

        m_nodeGraphController.SetGraph(&m_graph);
        m_nodeGraphController.EnsureOutputNode();
        PopulatePreviewNodeSelector();

        if (m_renderEngine.D3DDevice())
        {
            m_pixelInspector.Initialize(m_renderEngine.D3DDevice());
            m_pixelTrace.Initialize(m_renderEngine.D3DDevice());
        }

        if (m_renderEngine.D2DDeviceContext())
        {
            m_toneMapper.Initialize(m_renderEngine.D2DDeviceContext());
            m_falseColor.Initialize(m_renderEngine.D2DDeviceContext());
            auto caps = m_displayMonitor.CachedCapabilities();
            m_toneMapper.SetDisplayMaxLuminance(caps.maxLuminanceNits);
            m_toneMapper.SetSDRWhiteLevel(caps.sdrWhiteLevelNits);
            m_falseColor.SetDisplayMaxLuminance(caps.maxLuminanceNits);
        }

        // Start render loop.
        m_fpsTimePoint = std::chrono::steady_clock::now();
        m_renderTimer = DispatcherQueue().CreateTimer();
        m_renderTimer.Interval(std::chrono::milliseconds(16));
        m_renderTimer.Tick({ this, &MainWindow::OnRenderTick });
        m_renderTimer.Start();

        // Initialize the node graph editor panel.
        InitializeGraphPanel();
    }

    void MainWindow::UpdateStatusBar()
    {
        auto caps = m_displayMonitor.CachedCapabilities();

        if (m_displayMonitor.IsSimulated())
        {
            auto profile = m_displayMonitor.ActiveProfile();
            DisplayModeText().Text(L"Display: Simulated \u2014 " + winrt::hstring(profile.profileName));
        }
        else
        {
            DisplayModeText().Text(L"Display: " + caps.ModeString());
        }

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

    // -----------------------------------------------------------------------
    // Per-node preview
    // -----------------------------------------------------------------------

    void MainWindow::PopulatePreviewNodeSelector()
    {
        m_suppressSelectorEvent = true;
        auto selector = PreviewNodeSelector();
        selector.Items().Clear();

        // Cache the topological order for navigation.
        try { m_topoOrder = m_graph.TopologicalSort(); }
        catch (...) { m_topoOrder.clear(); }

        uint32_t selectedIndex = 0;
        uint32_t idx = 0;

        for (uint32_t nodeId : m_topoOrder)
        {
            const auto* node = m_graph.FindNode(nodeId);
            if (!node) continue;

            auto label = node->name.empty()
                ? std::format(L"Node {}", nodeId)
                : node->name;
            selector.Items().Append(winrt::box_value(winrt::hstring(label)));

            if (nodeId == m_previewNodeId)
                selectedIndex = idx;
            idx++;
        }

        // If nothing selected or m_previewNodeId is 0, pick the Output node.
        if (m_previewNodeId == 0 && !m_topoOrder.empty())
        {
            // Output node is typically the last in topo order.
            m_previewNodeId = m_topoOrder.back();
            selectedIndex = idx > 0 ? idx - 1 : 0;
        }

        if (selector.Items().Size() > 0)
            selector.SelectedIndex(static_cast<int32_t>(selectedIndex));

        m_suppressSelectorEvent = false;
    }

    void MainWindow::OnPreviewNodeSelectionChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /*args*/)
    {
        if (m_suppressSelectorEvent) return;

        auto idx = PreviewNodeSelector().SelectedIndex();
        if (idx >= 0 && static_cast<uint32_t>(idx) < m_topoOrder.size())
        {
            m_previewNodeId = m_topoOrder[static_cast<uint32_t>(idx)];
            FitPreviewToView();
        }

        // Update overlay text.
        const auto* node = m_graph.FindNode(m_previewNodeId);
        bool isOutput = node && node->type == ::ShaderLab::Graph::NodeType::Output;
        if (node && !isOutput)
        {
            PreviewOverlayText().Text(L"Previewing: " + winrt::hstring(node->name));
            PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }

    void MainWindow::OnPreviewKeyDown(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
    {
        if (m_topoOrder.empty()) return;

        auto key = args.Key();
        constexpr auto vkOpenBracket = static_cast<winrt::Windows::System::VirtualKey>(219);   // [
        constexpr auto vkCloseBracket = static_cast<winrt::Windows::System::VirtualKey>(221);  // ]
        if (key == vkOpenBracket || key == vkCloseBracket)
        {
            auto idx = PreviewNodeSelector().SelectedIndex();
            int32_t count = static_cast<int32_t>(m_topoOrder.size());

            if (key == vkOpenBracket && idx > 0)
                PreviewNodeSelector().SelectedIndex(idx - 1);
            else if (key == vkCloseBracket && idx < count - 1)
                PreviewNodeSelector().SelectedIndex(idx + 1);

            args.Handled(true);
        }
    }

    ID2D1Image* MainWindow::GetPreviewImage()
    {
        const auto* node = m_graph.FindNode(m_previewNodeId);
        if (node && node->cachedOutput)
            return node->cachedOutput;
        return nullptr;
    }

    void MainWindow::OnFalseColorSelectionChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /*args*/)
    {
        auto idx = FalseColorSelector().SelectedIndex();
        switch (idx)
        {
        case 0:  m_falseColor.SetMode(::ShaderLab::Rendering::FalseColorMode::None); break;
        case 1:  m_falseColor.SetMode(::ShaderLab::Rendering::FalseColorMode::Clipping); break;
        case 2:  m_falseColor.SetMode(::ShaderLab::Rendering::FalseColorMode::LuminanceZones); break;
        case 3:  m_falseColor.SetMode(::ShaderLab::Rendering::FalseColorMode::OutOfGamut); break;
        default: m_falseColor.SetMode(::ShaderLab::Rendering::FalseColorMode::None); break;
        }
    }

    // -----------------------------------------------------------------------
    // Display profile selection
    // -----------------------------------------------------------------------

    void MainWindow::PopulateDisplayProfileSelector()
    {
        m_suppressProfileEvent = true;
        auto selector = DisplayProfileSelector();
        selector.Items().Clear();

        // "Current Monitor" item.
        auto currentItem = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
        currentItem.Content(winrt::box_value(L"Current Monitor"));
        currentItem.Tag(winrt::box_value(L"current"));
        selector.Items().Append(currentItem);

        // Preset items.
        m_displayPresets = ::ShaderLab::Rendering::AllPresets();
        for (size_t i = 0; i < m_displayPresets.size(); ++i)
        {
            auto item = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
            item.Content(winrt::box_value(winrt::hstring(m_displayPresets[i].profileName)));
            item.Tag(winrt::box_value(L"preset:" + std::to_wstring(i)));
            selector.Items().Append(item);
        }

        // "Load ICC Profile..." action item.
        auto loadItem = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
        loadItem.Content(winrt::box_value(L"Load ICC Profile\u2026"));
        loadItem.Tag(winrt::box_value(L"load"));
        selector.Items().Append(loadItem);

        selector.SelectedIndex(0);
        m_committedProfileIndex = 0;
        m_suppressProfileEvent = false;
    }

    void MainWindow::ApplyDisplayProfile(const ::ShaderLab::Rendering::DisplayProfile& profile)
    {
        m_displayMonitor.SetSimulatedProfile(profile);
        auto caps = m_displayMonitor.CachedCapabilities();
        m_toneMapper.SetDisplayMaxLuminance(caps.maxLuminanceNits);
        m_toneMapper.SetSDRWhiteLevel(caps.sdrWhiteLevelNits);
        m_falseColor.SetDisplayMaxLuminance(caps.maxLuminanceNits);
        UpdateStatusBar();
    }

    void MainWindow::RevertToLiveDisplay()
    {
        m_displayMonitor.ClearSimulatedProfile();
        auto caps = m_displayMonitor.CachedCapabilities();
        m_toneMapper.SetDisplayMaxLuminance(caps.maxLuminanceNits);
        m_toneMapper.SetSDRWhiteLevel(caps.sdrWhiteLevelNits);
        m_falseColor.SetDisplayMaxLuminance(caps.maxLuminanceNits);
        UpdateStatusBar();
    }

    void MainWindow::OnDisplayProfileSelectionChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /*args*/)
    {
        if (m_suppressProfileEvent) return;

        auto selector = DisplayProfileSelector();
        auto selected = selector.SelectedItem();
        if (!selected) return;

        auto item = selected.as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>();
        auto tag = winrt::unbox_value<winrt::hstring>(item.Tag());

        if (tag == L"current")
        {
            RevertToLiveDisplay();
            m_committedProfileIndex = selector.SelectedIndex();
        }
        else if (tag.size() > 7 && tag.c_str()[0] == L'p') // "preset:N"
        {
            auto indexStr = std::wstring(tag.c_str() + 7);
            size_t presetIdx = static_cast<size_t>(std::stoul(indexStr));
            if (presetIdx < m_displayPresets.size())
            {
                ApplyDisplayProfile(m_displayPresets[presetIdx]);
                m_committedProfileIndex = selector.SelectedIndex();
            }
        }
        else if (tag == L"icc")
        {
            if (m_loadedIccProfile.has_value())
            {
                ApplyDisplayProfile(m_loadedIccProfile.value());
                m_committedProfileIndex = selector.SelectedIndex();
            }
        }
        else if (tag == L"load")
        {
            LoadIccProfileAsync();
        }
    }

    winrt::fire_and_forget MainWindow::LoadIccProfileAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L".icc");
        picker.FileTypeFilter().Append(L".icm");

        auto file = co_await picker.PickSingleFileAsync();

        if (!file)
        {
            // User cancelled — revert to the previously committed selection.
            m_suppressProfileEvent = true;
            DisplayProfileSelector().SelectedIndex(m_committedProfileIndex);
            m_suppressProfileEvent = false;
            co_return;
        }

        auto path = std::wstring(file.Path().c_str());
        auto parsed = ::ShaderLab::Rendering::IccProfileParser::LoadFromFile(path);

        if (!parsed.has_value() || !parsed->valid)
        {
            // Parse failed — revert selection and show error in status bar.
            m_suppressProfileEvent = true;
            DisplayProfileSelector().SelectedIndex(m_committedProfileIndex);
            m_suppressProfileEvent = false;
            PipelineFormatText().Text(L"ICC Error: Failed to parse profile");
            co_return;
        }

        auto profile = ::ShaderLab::Rendering::DisplayProfileFromIcc(parsed.value());
        m_loadedIccProfile = profile;

        // Insert or update the ICC item (just before the "Load ICC..." sentinel).
        m_suppressProfileEvent = true;
        auto selector = DisplayProfileSelector();
        uint32_t loadIdx = selector.Items().Size() - 1;

        // Remove any previous ICC item.
        if (loadIdx > 0)
        {
            auto prevItem = selector.Items().GetAt(loadIdx - 1).as<winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem>();
            auto prevTag = winrt::unbox_value<winrt::hstring>(prevItem.Tag());
            if (prevTag == L"icc")
            {
                selector.Items().RemoveAt(loadIdx - 1);
                loadIdx--;
            }
        }

        // Insert the new ICC item before the "Load..." sentinel.
        auto iccItem = winrt::Microsoft::UI::Xaml::Controls::ComboBoxItem();
        iccItem.Content(winrt::box_value(winrt::hstring(profile.profileName)));
        iccItem.Tag(winrt::box_value(L"icc"));
        selector.Items().InsertAt(loadIdx, iccItem);

        // Select the newly inserted item.
        selector.SelectedIndex(static_cast<int32_t>(loadIdx));
        m_committedProfileIndex = static_cast<int32_t>(loadIdx);
        m_suppressProfileEvent = false;

        ApplyDisplayProfile(profile);
    }

    // -----------------------------------------------------------------------
    // Graph save/load
    // -----------------------------------------------------------------------

    void MainWindow::OnSaveGraphClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        SaveGraphAsync();
    }

    void MainWindow::OnLoadGraphClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        LoadGraphAsync();
    }

    winrt::fire_and_forget MainWindow::SaveGraphAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileSavePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
        picker.SuggestedFileName(L"graph");
        picker.FileTypeChoices().Insert(L"JSON Graph", winrt::single_threaded_vector<winrt::hstring>({ L".json" }));

        auto file = co_await picker.PickSaveFileAsync();
        if (!file) co_return;

        try
        {
            auto json = m_graph.ToJson();
            co_await winrt::Windows::Storage::FileIO::WriteTextAsync(file, json);
            PipelineFormatText().Text(L"Graph saved: " + file.Name());
        }
        catch (...)
        {
            PipelineFormatText().Text(L"Error: Failed to save graph");
        }
    }

    winrt::fire_and_forget MainWindow::LoadGraphAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::DocumentsLibrary);
        picker.FileTypeFilter().Append(L".json");

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        try
        {
            auto text = co_await winrt::Windows::Storage::FileIO::ReadTextAsync(file);
            auto loaded = ::ShaderLab::Graph::EffectGraph::FromJson(text);

            // Release stale evaluator caches before swapping the graph.
            m_graphEvaluator.ReleaseCache();
            m_graph = std::move(loaded);

            ResetAfterGraphLoad();
            PipelineFormatText().Text(L"Graph loaded: " + file.Name());
        }
        catch (const std::exception& ex)
        {
            PipelineFormatText().Text(L"Load error: " + winrt::to_hstring(ex.what()));
        }
        catch (...)
        {
            PipelineFormatText().Text(L"Error: Failed to load graph");
        }
    }

    void MainWindow::ResetAfterGraphLoad()
    {
        m_previewNodeId = 0;
        m_traceActive = false;
        m_lastTraceTopologyHash = 0;
        m_traceRowCache.clear();
        m_compareActive = false;
        m_compareNodeId = 0;

        m_nodeGraphController.SetGraph(&m_graph);
        m_graph.MarkAllDirty();
        PopulatePreviewNodeSelector();
        PopulateCompareNodeSelector();

        // Reset compare UI.
        CompareToggle().IsChecked(false);
        CompareToolbar().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);

        // Reset trace UI.
        PixelTracePanel().Children().Clear();
        TracePositionText().Text(L"Click preview to trace a pixel");

        // Defer FitPreviewToView until after the first evaluation
        // produces valid cachedOutput (image bounds aren't available yet).
        m_needsFitPreview = true;
        UpdateStatusBar();
    }

    // -----------------------------------------------------------------------
    // Add node
    // -----------------------------------------------------------------------

    void MainWindow::PopulateAddNodeFlyout()
    {
        auto flyout = AddNodeFlyout();
        flyout.Items().Clear();

        auto& registry = ::ShaderLab::Effects::EffectRegistry::Instance();
        auto categories = registry.Categories();

        winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem sourceSubItem{ nullptr };

        for (const auto& cat : categories)
        {
            auto subItem = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem();
            subItem.Text(winrt::hstring(cat));

            auto effects = registry.ByCategory(cat);
            for (const auto* desc : effects)
            {
                auto menuItem = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
                menuItem.Text(winrt::hstring(desc->name));

                // Capture descriptor by value for the click handler.
                auto capturedDesc = *desc;
                menuItem.Click([this, capturedDesc](auto&&, auto&&)
                {
                    OnAddEffectNode(capturedDesc);
                });

                subItem.Items().Append(menuItem);
            }

            flyout.Items().Append(subItem);

            if (cat == L"Source")
                sourceSubItem = subItem;
        }

        // Append Image and Flood sources to the Source category.
        if (!sourceSubItem)
        {
            sourceSubItem = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem();
            sourceSubItem.Text(L"Source");
            flyout.Items().Append(sourceSubItem);
        }

        auto imageSourceItem = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
        imageSourceItem.Text(L"Image Source");
        imageSourceItem.Click([this](auto&&, auto&&)
        {
            auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Image Source");
            auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
            OnNodeAdded(nodeId);
        });
        sourceSubItem.Items().Append(imageSourceItem);

        auto floodSourceItem = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
        floodSourceItem.Text(L"Flood Fill (Solid Color)");
        floodSourceItem.Click([this](auto&&, auto&&)
        {
            OnAddFloodSourceClicked(nullptr, nullptr);
        });
        sourceSubItem.Items().Append(floodSourceItem);
    }

    void MainWindow::OnAddEffectNode(const ::ShaderLab::Effects::EffectDescriptor& desc)
    {
        auto node = ::ShaderLab::Effects::EffectRegistry::CreateNode(desc);
        // Pass {0,0} to use the controller's auto-layout which avoids overlaps.
        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
        OnNodeAdded(nodeId);
    }

    void MainWindow::OnAddImageSourceClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        AddImageSourceAsync();
    }

    winrt::fire_and_forget MainWindow::AddImageSourceAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
        picker.FileTypeFilter().Append(L".png");
        picker.FileTypeFilter().Append(L".jpg");
        picker.FileTypeFilter().Append(L".jpeg");
        picker.FileTypeFilter().Append(L".bmp");
        picker.FileTypeFilter().Append(L".tif");
        picker.FileTypeFilter().Append(L".tiff");
        picker.FileTypeFilter().Append(L".hdr");
        picker.FileTypeFilter().Append(L".exr");
        picker.FileTypeFilter().Append(L".jxr");

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        auto filePath = std::wstring(file.Path().c_str());
        auto fileName = std::wstring(file.Name().c_str());

        auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(filePath, fileName);

        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });

        // Prepare the source (load the image bitmap).
        auto* graphNode = m_graph.FindNode(nodeId);
        if (graphNode && m_renderEngine.D2DDeviceContext())
        {
            m_sourceFactory.PrepareSourceNode(*graphNode, m_renderEngine.D2DDeviceContext());
        }

        OnNodeAdded(nodeId);
    }

    void MainWindow::OnAddFloodSourceClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateFloodSourceNode(
            { 1.0f, 1.0f, 1.0f, 1.0f }, L"White");

        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });

        auto* graphNode = m_graph.FindNode(nodeId);
        if (graphNode && m_renderEngine.D2DDeviceContext())
        {
            m_sourceFactory.PrepareSourceNode(*graphNode, m_renderEngine.D2DDeviceContext());
        }

        OnNodeAdded(nodeId);
    }

    void MainWindow::OnNodeAdded(uint32_t /*nodeId*/)
    {
        m_graph.MarkAllDirty();
        m_nodeGraphController.RebuildLayout();
        PopulatePreviewNodeSelector();
        if (m_compareActive)
            PopulateCompareNodeSelector();
    }

    // -----------------------------------------------------------------------
    // Split comparison
    // -----------------------------------------------------------------------

    void MainWindow::PopulateCompareNodeSelector()
    {
        m_suppressCompareEvent = true;
        auto selector = CompareNodeSelector();
        selector.Items().Clear();

        for (uint32_t nodeId : m_topoOrder)
        {
            const auto* node = m_graph.FindNode(nodeId);
            if (!node) continue;

            auto label = node->name.empty()
                ? std::format(L"Node {}", nodeId)
                : node->name;
            selector.Items().Append(winrt::box_value(winrt::hstring(label)));
        }

        if (selector.Items().Size() > 0)
            selector.SelectedIndex(0);

        m_suppressCompareEvent = false;
    }

    ID2D1Image* MainWindow::ResolveDisplayImage(uint32_t nodeId)
    {
        const auto* node = m_graph.FindNode(nodeId);
        if (!node || !node->cachedOutput) return nullptr;

        // Don't render effects that have required inputs but none connected.
        if (!node->inputPins.empty())
        {
            auto inputs = m_graph.GetInputEdges(nodeId);
            if (inputs.empty())
                return nullptr;
        }

        ID2D1Image* image = node->cachedOutput;

        // Apply tone mapping only for the Output node.
        bool isOutput = node->type == ::ShaderLab::Graph::NodeType::Output;
        if (isOutput && m_toneMapper.IsActive())
            image = m_toneMapper.Apply(image);

        // Apply false-color overlay if active.
        if (m_falseColor.IsActive())
            image = m_falseColor.Apply(image);

        return image;
    }

    void MainWindow::OnCompareToggled(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        m_compareActive = CompareToggle().IsChecked().GetBoolean();

        if (m_compareActive)
        {
            PopulateCompareNodeSelector();
            CompareToolbar().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            m_splitPosition = 0.5f;

            // Set compare node from the selector (event was suppressed during populate).
            auto idx = CompareNodeSelector().SelectedIndex();
            if (idx >= 0 && static_cast<uint32_t>(idx) < m_topoOrder.size())
                m_compareNodeId = m_topoOrder[static_cast<uint32_t>(idx)];

            // Switch away from Pixel Trace tab (mutually exclusive).
            if (BottomTabView().SelectedIndex() == 1)
                BottomTabView().SelectedIndex(0);

            m_traceActive = false;
            UpdateCrosshairOverlay();
        }
        else
        {
            CompareToolbar().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }

    void MainWindow::OnCompareNodeSelectionChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /*args*/)
    {
        if (m_suppressCompareEvent) return;

        auto idx = CompareNodeSelector().SelectedIndex();
        if (idx >= 0 && static_cast<uint32_t>(idx) < m_topoOrder.size())
            m_compareNodeId = m_topoOrder[static_cast<uint32_t>(idx)];
    }

    void MainWindow::OnPreviewPointerDragged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!m_isDraggingSplit || !m_compareActive) return;

        auto point = args.GetCurrentPoint(PreviewPanel());
        float px = static_cast<float>(point.Position().X);

        float vpW = PreviewViewportDips().width;
        if (vpW > 0.0f)
            m_splitPosition = std::clamp(px / vpW, 0.0f, 1.0f);

        args.Handled(true);
    }

    void MainWindow::OnPreviewPointerReleased(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isPreviewPanning)
        {
            bool wasClick = !m_previewDragMoved;
            m_isPreviewPanning = false;
            PreviewPanel().ReleasePointerCapture(args.Pointer());

            // If it was a click (no drag), trigger pixel trace.
            if (wasClick)
            {
                auto point = args.GetCurrentPoint(PreviewPanel());
                m_traceClickDipX = static_cast<float>(point.Position().X);
                m_traceClickDipY = static_cast<float>(point.Position().Y);
                m_traceClickPanX = m_previewPanX;
                m_traceClickPanY = m_previewPanY;
                m_traceClickZoom = m_previewZoom;

                float normX = 0, normY = 0;
                if (PointerToImageCoords(args, normX, normY))
                {
                    m_pixelTrace.SetTrackPosition(normX, normY);
                    m_traceActive = true;
                    m_lastTraceTopologyHash = 0;

                    auto imgBounds = GetPreviewImageBounds();
                    float dpiScale = (std::max)(1.0f, PreviewPanel().CompositionScaleX());
                    float imgDipX = (m_traceClickDipX - m_previewPanX) / m_previewZoom;
                    float imgDipY = (m_traceClickDipY - m_previewPanY) / m_previewZoom;
                    float iW = imgBounds.right - imgBounds.left;
                    float iH = imgBounds.bottom - imgBounds.top;
                    if (iW <= 0 || iW > 100000.0f) { auto vp = PreviewViewportDips(); iW = vp.width; }
                    if (iH <= 0 || iH > 100000.0f) { auto vp = PreviewViewportDips(); iH = vp.height; }
                    // Display position in actual image pixels (context DIPs * DPI scale).
                    uint32_t px = static_cast<uint32_t>(normX * iW * dpiScale);
                    uint32_t py = static_cast<uint32_t>(normY * iH * dpiScale);
                    TracePositionText().Text(std::format(L"Position: ({}, {})", px, py));

                    PopulatePixelTraceTree();
                    UpdateCrosshairOverlay();
                }
            }
        }
        if (m_isDraggingSplit)
        {
            m_isDraggingSplit = false;
            PreviewPanel().ReleasePointerCapture(args.Pointer());
        }
    }

    // -----------------------------------------------------------------------
    // Pixel trace
    // -----------------------------------------------------------------------

    D2D1_SIZE_F MainWindow::PreviewViewportDips() const
    {
        // The preview renders at 96 DPI, so the D2D viewport matches WinUI DIPs.
        // Use the panel's actual size (which is in WinUI DIPs).
        auto panel = const_cast<MainWindow*>(this)->PreviewPanel();
        float w = static_cast<float>(panel.ActualWidth());
        float h = static_cast<float>(panel.ActualHeight());
        if (w <= 0) w = static_cast<float>(m_renderEngine.BackBufferWidth());
        if (h <= 0) h = static_cast<float>(m_renderEngine.BackBufferHeight());
        return { w, h };
    }

    D2D1_RECT_F MainWindow::GetPreviewImageBounds()
    {
        auto* previewImage = GetPreviewImage();
        if (!previewImage) return {};

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return {};

        // Get bounds at 96 DPI to match the preview rendering DPI.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        D2D1_RECT_F bounds{};
        HRESULT hr = dc->GetImageLocalBounds(previewImage, &bounds);

        dc->SetDpi(oldDpiX, oldDpiY);
        if (FAILED(hr)) return {};
        return bounds;
    }

    bool MainWindow::PointerToImageCoords(
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args,
        float& outNormX, float& outNormY)
    {
        auto point = args.GetCurrentPoint(PreviewPanel());
        float px = static_cast<float>(point.Position().X);
        float py = static_cast<float>(point.Position().Y);

        // Invert the preview pan/zoom to get D2D image-space coordinates (context DIPs).
        float imgDipX = (px - m_previewPanX) / m_previewZoom;
        float imgDipY = (py - m_previewPanY) / m_previewZoom;

        // The image is drawn at identity in D2D's DIP space via the preview transform.
        // GetImageLocalBounds returns bounds in context DIPs (accounting for image DPI
        // vs context DPI). The source rect in ReadPixel also operates in context DIPs.
        // So we normalize by context DIP bounds.
        auto bounds = GetPreviewImageBounds();
        float imgW = bounds.right - bounds.left;
        float imgH = bounds.bottom - bounds.top;

        // For infinite/huge images, fall back to viewport dimensions.
        if (imgW <= 0 || imgH <= 0 || imgW > 100000.0f || imgH > 100000.0f)
        {
            auto vp = PreviewViewportDips();
            imgW = vp.width;
            imgH = vp.height;
            bounds = { 0, 0, imgW, imgH };
        }

        if (imgW <= 0 || imgH <= 0) return false;

        // Check if the click is outside the image bounds before clamping.
        float rawNormX = (imgDipX - bounds.left) / imgW;
        float rawNormY = (imgDipY - bounds.top) / imgH;
        m_traceOutOfBounds = (rawNormX < 0.0f || rawNormX > 1.0f ||
                              rawNormY < 0.0f || rawNormY > 1.0f);

        outNormX = std::clamp(rawNormX, 0.0f, 1.0f);
        outNormY = std::clamp(rawNormY, 0.0f, 1.0f);
        return true;
    }

    void MainWindow::OnPreviewPointerPressed(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        auto point = args.GetCurrentPoint(PreviewPanel());
        bool isPixelTraceTab = (BottomTabView().SelectedIndex() == 1);

        // Check for compare split line drag (only when compare is active and NOT on Pixel Trace tab).
        if (m_compareActive && !isPixelTraceTab && point.Properties().IsLeftButtonPressed())
        {
            float px = static_cast<float>(point.Position().X);
            auto vp = PreviewViewportDips();
            float splitScreenX = vp.width * m_splitPosition;
            if (std::abs(px - splitScreenX) < 10.0f)
            {
                m_isDraggingSplit = true;
                PreviewPanel().CapturePointer(args.Pointer());
                args.Handled(true);
                return;
            }
        }

        // Left or middle button → start pan (click without drag triggers pixel trace on release).
        if (point.Properties().IsMiddleButtonPressed() || point.Properties().IsLeftButtonPressed())
        {
            m_isPreviewPanning = true;
            m_previewPanStartX = static_cast<float>(point.Position().X);
            m_previewPanStartY = static_cast<float>(point.Position().Y);
            m_previewPanOriginX = m_previewPanX;
            m_previewPanOriginY = m_previewPanY;
            m_previewDragMoved = false;
            PreviewPanel().CapturePointer(args.Pointer());
            args.Handled(true);
        }
    }

    void MainWindow::OnTraceUnitSelectionChanged(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& /*args*/)
    {
        m_traceUnit = static_cast<uint32_t>(TraceUnitSelector().SelectedIndex());
        if (m_traceActive && m_pixelTrace.HasTrace())
            UpdatePixelTraceValues();
    }

    namespace
    {
        // Format pixel values based on the selected unit mode.
        std::wstring FormatPixelValues(
            const ::ShaderLab::Controls::InspectedPixel& p,
            uint32_t unit)
        {
            switch (unit)
            {
            case 0: // scRGB
                return std::format(L"R:{:.4f}  G:{:.4f}  B:{:.4f}  A:{:.4f}",
                    p.scR, p.scG, p.scB, p.scA);
            case 1: // sRGB
                return std::format(L"R:{}  G:{}  B:{}  A:{}",
                    p.sR, p.sG, p.sB, p.sA);
            case 2: // Nits (per-channel × 80)
                return std::format(L"R:{:.1f}  G:{:.1f}  B:{:.1f}",
                    p.scR * 80.0f, p.scG * 80.0f, p.scB * 80.0f);
            case 3: // PQ
                return std::format(L"R:{:.4f}  G:{:.4f}  B:{:.4f}",
                    p.pqR, p.pqG, p.pqB);
            default:
                return p.ScRGBString();
            }
        }

        // Simple topology hash from trace tree structure.
        uint32_t HashTraceTopology(const ::ShaderLab::Controls::PixelTraceNode& node)
        {
            uint32_t h = node.nodeId * 31 + node.inputPin;
            for (const auto& child : node.inputs)
                h = h * 37 + HashTraceTopology(child);
            return h;
        }
    }

    winrt::Microsoft::UI::Xaml::Controls::Grid MainWindow::CreateTraceRow(
        const ::ShaderLab::Controls::PixelTraceNode& traceNode)
    {
        auto row = winrt::Microsoft::UI::Xaml::Controls::Grid();
        row.ColumnDefinitions().Append(winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition());
        row.ColumnDefinitions().GetAt(0).Width({ 1, winrt::Microsoft::UI::Xaml::GridUnitType::Star });
        auto lumCol = winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition();
        lumCol.Width({ 0, winrt::Microsoft::UI::Xaml::GridUnitType::Auto });
        row.ColumnDefinitions().Append(lumCol);

        // Node label: "[pin] Name"
        auto label = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
        std::wstring labelText = traceNode.nodeName;
        if (!traceNode.pinName.empty())
            labelText = L"[" + traceNode.pinName + L"] " + labelText;

        // For analysis nodes: show field values instead of pixel values.
        std::wstring valuesText;
        if (traceNode.hasAnalysisOutput && !traceNode.analysisFields.empty())
        {
            valuesText = L"\n";
            for (const auto& fv : traceNode.analysisFields)
            {
                valuesText += fv.name + L": ";
                if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                {
                    uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    for (uint32_t c = 0; c < cc; ++c)
                    {
                        if (c > 0) valuesText += L"  ";
                        valuesText += std::format(L"{:.4f}", fv.components[c]);
                    }
                }
                else
                {
                    uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    uint32_t count = stride > 0 ? static_cast<uint32_t>(fv.arrayData.size()) / stride : 0;
                    valuesText += std::format(L"[{} elements]", count);
                }
                valuesText += L"\n";
            }
        }
        else
        {
            valuesText = L"\n" + FormatPixelValues(traceNode.pixel, m_traceUnit);
        }

        label.Text(labelText + valuesText);
        label.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Cascadia Mono, Consolas, Courier New"));
        label.FontSize(12);
        label.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap);
        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(label, 0);
        row.Children().Append(label);

        // Luminance column (skip for analysis nodes).
        auto lumText = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
        if (traceNode.hasAnalysisOutput)
            lumText.Text(L"");
        else
            lumText.Text(std::format(L"{:.1f} cd/m\u00B2", traceNode.pixel.luminanceNits));
        lumText.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Cascadia Mono, Consolas, Courier New"));
        lumText.FontSize(12);
        lumText.Margin({ 8, 0, 0, 0 });
        lumText.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(lumText, 1);
        row.Children().Append(lumText);

        return row;
    }

    void MainWindow::UpdateCrosshairOverlay()
    {
        auto canvas = CrosshairCanvas();
        canvas.Children().Clear();

        // Clip the canvas to the preview panel bounds.
        auto clipGeom = winrt::Microsoft::UI::Xaml::Media::RectangleGeometry();
        clipGeom.Rect({ 0, 0,
            static_cast<float>(canvas.ActualWidth()),
            static_cast<float>(canvas.ActualHeight()) });
        canvas.Clip(clipGeom);

        // Only show crosshair when Pixel Trace tab is active.
        if (!m_traceActive || BottomTabView().SelectedIndex() != 1)
            return;

        // Compute the image-space point from the click position and the transform at click time,
        // then project it back to screen space using the CURRENT transform.
        // This makes the crosshair track 1:1 with the image during pan/zoom.
        float imgSpaceX = (m_traceClickDipX - m_traceClickPanX) / m_traceClickZoom;
        float imgSpaceY = (m_traceClickDipY - m_traceClickPanY) / m_traceClickZoom;
        float cx = imgSpaceX * m_previewZoom + m_previewPanX;
        float cy = imgSpaceY * m_previewZoom + m_previewPanY;
        float arm = 16.0;
        float gap = 4.0;

        auto makeLine = [&](float x1, float y1, float x2, float y2,
            winrt::Windows::UI::Color color, double thickness)
        {
            auto line = winrt::Microsoft::UI::Xaml::Shapes::Line();
            line.X1(x1); line.Y1(y1); line.X2(x2); line.Y2(y2);
            line.Stroke(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(color));
            line.StrokeThickness(thickness);
            canvas.Children().Append(line);
        };

        winrt::Windows::UI::Color black{ 230, 0, 0, 0 };
        winrt::Windows::UI::Color white{ 230, 255, 255, 255 };

        // Black outline.
        makeLine(cx - arm, cy, cx - gap, cy, black, 3.0);
        makeLine(cx + gap, cy, cx + arm, cy, black, 3.0);
        makeLine(cx, cy - arm, cx, cy - gap, black, 3.0);
        makeLine(cx, cy + gap, cx, cy + arm, black, 3.0);
        // White fill.
        makeLine(cx - arm, cy, cx - gap, cy, white, 1.5);
        makeLine(cx + gap, cy, cx + arm, cy, white, 1.5);
        makeLine(cx, cy - arm, cx, cy - gap, white, 1.5);
        makeLine(cx, cy + gap, cx, cy + arm, white, 1.5);
    }

    void MainWindow::PopulatePixelTraceTree()
    {
        if (!m_traceActive) return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        // Use actual image dimensions in context DIPs for pixel coordinate mapping.
        // DrawImage's sourceRectangle is in the same coordinate space as the D2D context.
        auto bounds = GetPreviewImageBounds();
        float imgW = bounds.right - bounds.left;
        float imgH = bounds.bottom - bounds.top;
        if (imgW <= 0 || imgH <= 0 || imgW > 100000.0f || imgH > 100000.0f)
        {
            auto vp = PreviewViewportDips();
            imgW = vp.width;
            imgH = vp.height;
        }
        uint32_t traceW = static_cast<uint32_t>(imgW);
        uint32_t traceH = static_cast<uint32_t>(imgH);
        if (traceW == 0 || traceH == 0) return;

        if (!m_pixelTrace.ReTrace(dc, m_graph, m_previewNodeId, traceW, traceH))
            return;

        auto topoHash = HashTraceTopology(m_pixelTrace.Root());
        if (topoHash == m_lastTraceTopologyHash)
        {
            // Topology unchanged — just update text values in place.
            UpdatePixelTraceValues();
            return;
        }

        m_lastTraceTopologyHash = topoHash;
        m_traceRowCache.clear();

        auto panel = PixelTracePanel();
        panel.Children().Clear();

        // Recursive lambda to build indented rows in a flat StackPanel.
        std::function<void(const ::ShaderLab::Controls::PixelTraceNode&, uint32_t)> buildRows;

        buildRows = [&](const ::ShaderLab::Controls::PixelTraceNode& traceNode, uint32_t depth)
        {
            auto rowGrid = CreateTraceRow(traceNode);
            rowGrid.Margin({ static_cast<double>(depth) * 20.0, 0, 0, 0 });
            m_traceRowCache.push_back(rowGrid);
            panel.Children().Append(rowGrid);

            for (const auto& child : traceNode.inputs)
                buildRows(child, depth + 1);
        };

        buildRows(m_pixelTrace.Root(), 0);

        // Show out-of-bounds warning if the selected pixel is outside the image.
        if (m_traceOutOfBounds)
        {
            auto warning = winrt::Microsoft::UI::Xaml::Controls::InfoBar();
            warning.IsOpen(true);
            warning.IsClosable(false);
            warning.Severity(winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
            warning.Title(L"Outside image bounds");
            warning.Message(L"The selected pixel is outside the image area. Values shown are from the nearest edge pixel.");
            warning.Margin({ 0, 8, 0, 0 });
            panel.Children().Append(warning);
        }
    }

    void MainWindow::UpdatePixelTraceValues()
    {
        if (!m_pixelTrace.HasTrace()) return;

        // Walk the trace tree in the same order as the cache was built (pre-order).
        uint32_t idx = 0;
        std::function<void(const ::ShaderLab::Controls::PixelTraceNode&)> update;

        update = [&](const ::ShaderLab::Controls::PixelTraceNode& traceNode)
        {
            if (idx >= m_traceRowCache.size()) return;
            auto row = m_traceRowCache[idx++];

            // Update label text (child 0).
            auto label = row.Children().GetAt(0).as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>();
            std::wstring labelText = traceNode.nodeName;
            if (!traceNode.pinName.empty())
                labelText = L"[" + traceNode.pinName + L"] " + labelText;

            if (traceNode.hasAnalysisOutput && !traceNode.analysisFields.empty())
            {
                std::wstring valText = L"\n";
                for (const auto& fv : traceNode.analysisFields)
                {
                    valText += fv.name + L": ";
                    if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                    {
                        uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                        for (uint32_t c = 0; c < cc; ++c)
                        {
                            if (c > 0) valText += L"  ";
                            valText += std::format(L"{:.4f}", fv.components[c]);
                        }
                    }
                    else
                    {
                        uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                        uint32_t count = stride > 0 ? static_cast<uint32_t>(fv.arrayData.size()) / stride : 0;
                        valText += std::format(L"[{} elements]", count);
                    }
                    valText += L"\n";
                }
                label.Text(labelText + valText);
            }
            else
            {
                label.Text(labelText + L"\n" + FormatPixelValues(traceNode.pixel, m_traceUnit));
            }

            // Update luminance (child 1).
            auto lumText = row.Children().GetAt(1).as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>();
            if (traceNode.hasAnalysisOutput)
                lumText.Text(L"");
            else
                lumText.Text(std::format(L"{:.1f} cd/m\u00B2", traceNode.pixel.luminanceNits));

            for (const auto& child : traceNode.inputs)
                update(child);
        };

        update(m_pixelTrace.Root());
    }

    // -----------------------------------------------------------------------
    // Cursor readout
    // -----------------------------------------------------------------------

    void MainWindow::OnPreviewPointerMoved(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!m_renderEngine.IsInitialized()) return;

        // Handle compare split-line dragging (takes priority over pan).
        if (m_isDraggingSplit && m_compareActive)
        {
            OnPreviewPointerDragged(sender, args);
            return;
        }

        // Handle preview pan.
        if (m_isPreviewPanning)
        {
            auto point = args.GetCurrentPoint(PreviewPanel());
            float px = static_cast<float>(point.Position().X);
            float py = static_cast<float>(point.Position().Y);
            float dx = px - m_previewPanStartX;
            float dy = py - m_previewPanStartY;
            if (std::abs(dx) > 5.0f || std::abs(dy) > 5.0f)
                m_previewDragMoved = true;
            m_previewPanX = m_previewPanOriginX + dx;
            m_previewPanY = m_previewPanOriginY + dy;
            args.Handled(true);
            return;
        }

        // Handle split-line dragging.
        if (m_isDraggingSplit && m_compareActive)
        {
            OnPreviewPointerDragged(sender, args);
            return;
        }

        auto point = args.GetCurrentPoint(PreviewPanel());
        auto scale = PreviewPanel().CompositionScaleX();
        uint32_t px = static_cast<uint32_t>(point.Position().X * scale);
        uint32_t py = static_cast<uint32_t>(point.Position().Y * scale);

        // Single-pixel readback at cursor position from the previewed node.
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        auto* previewImage = GetPreviewImage();
        if (!previewImage)
        {
            CursorReadoutText().Text(L"");
            return;
        }

        // Lightweight readback: inspect the previewed node at cursor position.
        if (m_pixelInspector.InspectPixel(dc, m_graph, m_previewNodeId, px, py))
        {
            const auto& p = m_pixelInspector.LastPixel();
            CursorReadoutText().Text(std::format(
                L"({},{}) R:{:.3f} G:{:.3f} B:{:.3f} \u00B7 {:.0f} nits",
                px, py, p.scR, p.scG, p.scB, p.luminanceNits));
        }
    }

    // -----------------------------------------------------------------------
    // Node graph editor rendering
    // -----------------------------------------------------------------------

    void MainWindow::InitializeGraphPanel()
    {
        if (!m_renderEngine.DXGIFactory() || !m_renderEngine.D3DDevice())
            return;

        auto panel = NodeGraphPanel();
        m_graphPanelWidth = static_cast<uint32_t>((std::max)(1.0f, static_cast<float>(panel.ActualWidth())));
        m_graphPanelHeight = static_cast<uint32_t>((std::max)(1.0f, static_cast<float>(panel.ActualHeight())));
        if (m_graphPanelWidth == 0) m_graphPanelWidth = 400;
        if (m_graphPanelHeight == 0) m_graphPanelHeight = 300;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = m_graphPanelWidth;
        desc.Height = m_graphPanelHeight;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        auto factory = m_renderEngine.DXGIFactory();
        auto* d3dDevice = m_renderEngine.D3DDevice();

        winrt::com_ptr<IDXGIDevice1> dxgiDevice;
        d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));

        HRESULT hr = factory->CreateSwapChainForComposition(
            dxgiDevice.get(), &desc, nullptr, m_graphSwapChain.put());

        if (FAILED(hr)) return;

        // Bind swap chain to the panel.
        auto panelNative = panel.as<ISwapChainPanelNative>();
        panelNative->SetSwapChain(m_graphSwapChain.get());

        // Create render target.
        auto* dc = m_renderEngine.D2DDeviceContext();
        winrt::com_ptr<IDXGISurface> surface;
        m_graphSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_graphRenderTarget.put());
    }

    void MainWindow::ResizeGraphPanel(uint32_t w, uint32_t h)
    {
        if (!m_graphSwapChain || (w == m_graphPanelWidth && h == m_graphPanelHeight))
            return;

        m_graphPanelWidth = w;
        m_graphPanelHeight = h;

        m_graphRenderTarget = nullptr;
        m_graphSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        winrt::com_ptr<IDXGISurface> surface;
        m_graphSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_graphRenderTarget.put());
    }

    void MainWindow::InitializeTraceSwatchPanel()
    {
        if (m_traceSwapChain || !m_renderEngine.DXGIFactory() || !m_renderEngine.D3DDevice())
            return;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 28;
        desc.Height = 600;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        winrt::com_ptr<IDXGIDevice1> dxgiDevice;
        m_renderEngine.D3DDevice()->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));

        HRESULT hr = m_renderEngine.DXGIFactory()->CreateSwapChainForComposition(
            dxgiDevice.get(), &desc, nullptr, m_traceSwapChain.put());
        if (FAILED(hr)) return;

        auto panelNative = TraceSwatchPanel().as<ISwapChainPanelNative>();
        panelNative->SetSwapChain(m_traceSwapChain.get());

        // Set scRGB color space for HDR rendering.
        winrt::com_ptr<IDXGISwapChain3> sc3;
        m_traceSwapChain.as(sc3);
        if (sc3)
            sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);

        m_traceSwatchHeight = 600;

        auto* dc = m_renderEngine.D2DDeviceContext();
        winrt::com_ptr<IDXGISurface> surface;
        m_traceSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_traceSwatchTarget.put());
    }

    void MainWindow::RenderTraceSwatches()
    {
        if (!m_traceActive || !m_pixelTrace.HasTrace())
            return;

        if (!m_traceSwapChain)
            InitializeTraceSwatchPanel();
        if (!m_traceSwapChain || !m_traceSwatchTarget)
            return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());

        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        dc->SetTarget(m_traceSwatchTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        // Walk the trace tree and render swatches at approximate row positions.
        float y = 4.0f;
        constexpr float swatchSize = 20.0f;
        constexpr float rowHeight = 40.0f;

        std::function<void(const ::ShaderLab::Controls::PixelTraceNode&, int)> drawSwatches;
        drawSwatches = [&](const ::ShaderLab::Controls::PixelTraceNode& node, int depth)
        {
            // scRGB color: 1.0 = 80 nits SDR white. Values > 1.0 = HDR.
            D2D1_COLOR_F color = {
                node.pixel.scR, node.pixel.scG, node.pixel.scB, 1.0f
            };

            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(color, brush.put());
            if (brush)
            {
                D2D1_ROUNDED_RECT rr = {
                    { 4.0f, y, 4.0f + swatchSize, y + swatchSize },
                    3.0f, 3.0f
                };
                dc->FillRoundedRectangle(rr, brush.get());
            }
            y += rowHeight;

            for (const auto& child : node.inputs)
                drawSwatches(child, depth + 1);
        };

        drawSwatches(m_pixelTrace.Root(), 0);

        dc->EndDraw();
        dc->SetTarget(oldTarget.get());
        dc->SetDpi(oldDpiX, oldDpiY);

        m_traceSwapChain->Present(0, 0);
    }

    void MainWindow::RenderNodeGraph()
    {
        if (!m_graphSwapChain || !m_graphRenderTarget)
            return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());

        // The graph swap chain is sized in DIPs (not physical pixels), so
        // render at 96 DPI so D2D coordinates match the DIP-based pointer
        // input.  The XAML compositor handles scaling to physical pixels.
        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        dc->SetTarget(m_graphRenderTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0x1e1e1e));

        D2D1_SIZE_F viewSize = { static_cast<float>(m_graphPanelWidth),
                                 static_cast<float>(m_graphPanelHeight) };
        m_nodeGraphController.Render(dc, viewSize);

        dc->EndDraw();
        dc->SetTarget(oldTarget.get());

        dc->SetDpi(oldDpiX, oldDpiY);

        m_graphSwapChain->Present(0, 0);
    }

    D2D1_POINT_2F MainWindow::GraphPanelPointerToCanvas(
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        // Pointer position is in DIPs. Node positions are also in DIPs.
        // Return DIP coordinates directly — the render function handles the
        // physical pixel scaling separately.
        auto point = args.GetCurrentPoint(NodeGraphContainer());
        return { static_cast<float>(point.Position().X),
                 static_cast<float>(point.Position().Y) };
    }

    void MainWindow::OnGraphPanelPointerPressed(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        auto canvasPoint = GraphPanelPointerToCanvas(args);

        OutputDebugStringW(std::format(L"[GraphClick] canvas=({:.1f},{:.1f}) visuals={}\n",
            canvasPoint.x, canvasPoint.y, m_nodeGraphController.SelectedNodes().size()).c_str());

        // Debug: dump all node positions
        for (const auto& node : m_graph.Nodes())
        {
            OutputDebugStringW(std::format(L"  node {} '{}' pos=({:.0f},{:.0f})\n",
                node.id, node.name, node.position.x, node.position.y).c_str());
        }

        // Check for pin hit first (start connection drag).
        uint32_t pinNodeId = 0;
        uint32_t pinIndex = 0;
        bool isOutput = false;
        bool isDataPin = false;
        if (m_nodeGraphController.HitTestPin(canvasPoint, pinNodeId, pinIndex, isOutput, isDataPin))
        {
            m_nodeGraphController.BeginConnection(pinNodeId, pinIndex, isOutput, isDataPin);
            m_isDraggingConnection = true;
            NodeGraphContainer().CapturePointer(args.Pointer());
            args.Handled(true);
            return;
        }

        // Check for node hit (select + start drag).
        uint32_t hitNodeId = m_nodeGraphController.HitTestNode(canvasPoint);
        if (hitNodeId != 0)
        {
            m_nodeGraphController.SelectNode(hitNodeId);
            m_selectedNodeId = hitNodeId;
            m_nodeGraphController.BeginDragNodes(canvasPoint);
            m_isDraggingNode = true;
            NodeGraphContainer().CapturePointer(args.Pointer());
            UpdatePropertiesPanel();
            BottomTabView().SelectedIndex(0);
            NodeGraphContainer().Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);

            // For analysis effects (e.g., Histogram), keep the preview on the
            // Output node since analysis effects don't produce viewable images.
            const auto* clickedNode = m_graph.FindNode(hitNodeId);
            bool isAnalysisEffect = clickedNode &&
                clickedNode->effectClsid.has_value() &&
                IsEqualGUID(clickedNode->effectClsid.value(), CLSID_D2D1Histogram);

            if (isAnalysisEffect)
            {
                // Find the Output node for preview.
                for (const auto& n : m_graph.Nodes())
                {
                    if (n.type == ::ShaderLab::Graph::NodeType::Output)
                    {
                        m_previewNodeId = n.id;
                        break;
                    }
                }
            }
            else
            {
                m_previewNodeId = hitNodeId;
            }
            FitPreviewToView();
            m_suppressSelectorEvent = true;
            for (uint32_t i = 0; i < m_topoOrder.size(); ++i)
            {
                if (m_topoOrder[i] == m_previewNodeId)
                {
                    PreviewNodeSelector().SelectedIndex(static_cast<int32_t>(i));
                    break;
                }
            }
            m_suppressSelectorEvent = false;

            const auto* previewNode = m_graph.FindNode(m_previewNodeId);
            bool isOutputNode = previewNode && previewNode->type == ::ShaderLab::Graph::NodeType::Output;
            if (previewNode && !isOutputNode)
            {
                PreviewOverlayText().Text(L"Previewing: " + winrt::hstring(previewNode->name));
                PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            }
            else
            {
                PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            }
        }
        else
        {
            // Clicked empty space — deselect and reset preview to Output node.
            m_nodeGraphController.DeselectAll();
            m_selectedNodeId = 0;
            UpdatePropertiesPanel();

            // Find the Output node and preview it.
            uint32_t outputId = 0;
            for (const auto& n : m_graph.Nodes())
            {
                if (n.type == ::ShaderLab::Graph::NodeType::Output)
                {
                    outputId = n.id;
                    break;
                }
            }
            if (outputId != 0)
            {
                m_previewNodeId = outputId;
                m_suppressSelectorEvent = true;
                for (uint32_t i = 0; i < m_topoOrder.size(); ++i)
                {
                    if (m_topoOrder[i] == outputId)
                    {
                        PreviewNodeSelector().SelectedIndex(static_cast<int32_t>(i));
                        break;
                    }
                }
                m_suppressSelectorEvent = false;
            }
            PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
        args.Handled(true);
    }

    void MainWindow::OnGraphPanelPointerMoved(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        auto canvasPoint = GraphPanelPointerToCanvas(args);

        if (m_isDraggingNode)
        {
            m_nodeGraphController.UpdateDragNodes(canvasPoint);
            args.Handled(true);
        }
        else if (m_isDraggingConnection)
        {
            m_nodeGraphController.UpdateConnection(canvasPoint);
            args.Handled(true);
        }
    }

    void MainWindow::OnGraphPanelPointerReleased(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isDraggingNode)
        {
            m_nodeGraphController.EndDragNodes();
            m_isDraggingNode = false;
            NodeGraphContainer().ReleasePointerCapture(args.Pointer());
        }
        else if (m_isDraggingConnection)
        {
            auto canvasPoint = GraphPanelPointerToCanvas(args);
            bool connected = m_nodeGraphController.EndConnection(canvasPoint);
            m_isDraggingConnection = false;
            NodeGraphContainer().ReleasePointerCapture(args.Pointer());
            if (connected)
            {
                m_graph.MarkAllDirty();
                PopulatePreviewNodeSelector();
            }
        }
        args.Handled(true);
    }

    void MainWindow::UpdatePropertiesPanel()
    {
        namespace Controls = winrt::Microsoft::UI::Xaml::Controls;
        namespace Media = winrt::Microsoft::UI::Xaml::Media;
        using ::ShaderLab::Effects::PropertyUIHint;
        using ::ShaderLab::Effects::PropertyMetadata;

        auto panel = PropertiesPanel();
        panel.Children().Clear();

        if (m_selectedNodeId == 0)
        {
            auto placeholder = Controls::TextBlock();
            placeholder.Text(L"Select a node to view its properties.");
            placeholder.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
            panel.Children().Append(placeholder);
            return;
        }

        auto* node = m_graph.FindNode(m_selectedNodeId);
        if (!node) return;

        uint32_t capturedId = m_selectedNodeId;

        // ---- Node name (editable) ----
        auto nameLabel = Controls::TextBlock();
        nameLabel.Text(L"Name");
        nameLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        panel.Children().Append(nameLabel);

        auto nameBox = Controls::TextBox();
        nameBox.Text(winrt::hstring(node->name));
        nameBox.Margin({ 0, 2, 0, 8 });
        nameBox.LostFocus([this, capturedId](auto&&, auto&&)
        {
            auto* n = m_graph.FindNode(capturedId);
            if (!n) return;
            PopulatePreviewNodeSelector();
            m_nodeGraphController.RebuildLayout();
        });
        nameBox.TextChanged([this, capturedId](auto&&, auto&&)
        {
            auto* n = m_graph.FindNode(capturedId);
            if (!n) return;
            auto p = PropertiesPanel();
            if (p.Children().Size() > 1)
            {
                auto tb = p.Children().GetAt(1).try_as<Controls::TextBox>();
                if (tb) n->name = std::wstring(tb.Text().c_str());
            }
        });
        panel.Children().Append(nameBox);

        // ---- Node type (read-only) ----
        auto typeText = Controls::TextBlock();
        std::wstring typeStr;
        switch (node->type)
        {
        case ::ShaderLab::Graph::NodeType::Source:         typeStr = L"Source"; break;
        case ::ShaderLab::Graph::NodeType::BuiltInEffect:  typeStr = L"Built-in Effect"; break;
        case ::ShaderLab::Graph::NodeType::PixelShader:    typeStr = L"Custom Pixel Shader"; break;
        case ::ShaderLab::Graph::NodeType::ComputeShader:  typeStr = L"Custom Compute Shader"; break;
        case ::ShaderLab::Graph::NodeType::Output:         typeStr = L"Output"; break;
        }
        typeText.Text(L"Type: " + typeStr);
        typeText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
        typeText.Margin({ 0, 0, 0, 8 });
        panel.Children().Append(typeText);

        // ---- Custom effect: "Edit in Designer" button ----
        if ((node->type == ::ShaderLab::Graph::NodeType::PixelShader ||
             node->type == ::ShaderLab::Graph::NodeType::ComputeShader) &&
            node->customEffect.has_value())
        {
            auto editBtn = Controls::Button();
            editBtn.Content(winrt::box_value(L"\xE70F  Edit in Effect Designer"));
            editBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            editBtn.Margin({ 0, 0, 0, 8 });
            editBtn.Click([this, capturedId](auto&&, auto&&)
            {
                auto* n = m_graph.FindNode(capturedId);
                if (!n || !n->customEffect.has_value()) return;

                OpenEffectDesigner();

                auto designerImpl = winrt::get_self<
                    winrt::ShaderLab::implementation::EffectDesignerWindow>(m_designerWindow);
                designerImpl->LoadDefinition(capturedId, n->customEffect.value(), n->name);
            });
            panel.Children().Append(editBtn);
        }

        // ---- Runtime error display ----
        if (!node->runtimeError.empty())
        {
            auto errorBorder = Controls::Border();
            errorBorder.Background(Media::SolidColorBrush(winrt::Windows::UI::Color{ 255, 180, 30, 30 }));
            errorBorder.CornerRadius({ 4, 4, 4, 4 });
            errorBorder.Padding({ 8, 6, 8, 6 });
            errorBorder.Margin({ 0, 0, 0, 8 });

            auto errorText = Controls::TextBlock();
            errorText.Text(winrt::hstring(node->runtimeError));
            errorText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::White()));
            errorText.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
            errorText.FontSize(12);
            errorBorder.Child(errorText);
            panel.Children().Append(errorBorder);
        }

        // ---- Image source: file path + Browse button ----
        if (node->type == ::ShaderLab::Graph::NodeType::Source &&
            !(node->effectClsid.has_value()))
        {
            auto pathLabel = Controls::TextBlock();
            pathLabel.Text(L"Image Path");
            pathLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            pathLabel.Margin({ 0, 4, 0, 2 });
            panel.Children().Append(pathLabel);

            auto pathText = Controls::TextBlock();
            pathText.Text(node->shaderPath.has_value() && !node->shaderPath.value().empty()
                ? winrt::hstring(node->shaderPath.value())
                : L"(no file selected)");
            pathText.FontSize(12);
            pathText.IsTextSelectionEnabled(true);
            pathText.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::WrapWholeWords);
            pathText.Foreground(Media::SolidColorBrush(
                node->shaderPath.has_value() && !node->shaderPath.value().empty()
                    ? winrt::Microsoft::UI::Colors::White()
                    : winrt::Microsoft::UI::Colors::Gray()));
            pathText.Margin({ 0, 0, 0, 4 });
            panel.Children().Append(pathText);

            auto browseBtn = Controls::Button();
            browseBtn.Content(winrt::box_value(L"Browse..."));
            browseBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
            browseBtn.Margin({ 0, 0, 0, 8 });
            browseBtn.Click([this, capturedId](auto&&, auto&&)
            {
                BrowseImageForSourceNode(capturedId);
            });
            panel.Children().Append(browseBtn);
        }

        // ---- Look up metadata from registry ----
        const ::ShaderLab::Effects::EffectDescriptor* desc = nullptr;
        if (node->type == ::ShaderLab::Graph::NodeType::BuiltInEffect && node->effectClsid.has_value())
            desc = ::ShaderLab::Effects::EffectRegistry::Instance().FindByClsid(node->effectClsid.value());

        // Build metadata map for custom effect parameters (min/max/step from ParameterDefinition).
        std::map<std::wstring, PropertyMetadata> customMeta;
        if (node->customEffect.has_value())
        {
            for (const auto& p : node->customEffect->parameters)
            {
                PropertyMetadata pm;
                pm.uiHint = PropertyUIHint::Slider;
                pm.minValue = p.minValue;
                pm.maxValue = p.maxValue;
                pm.step = p.step;
                customMeta[p.name] = pm;
            }
        }

        // ---- Editable properties ----
        if (node->properties.empty())
        {
            auto noProps = Controls::TextBlock();
            noProps.Text(L"No editable properties.");
            noProps.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
            noProps.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
            noProps.Margin({ 0, 4, 0, 0 });
            panel.Children().Append(noProps);
        }
        else
        {
            auto propsHeader = Controls::TextBlock();
            propsHeader.Text(L"Properties");
            propsHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            propsHeader.Margin({ 0, 4, 0, 4 });
            panel.Children().Append(propsHeader);

            for (const auto& [key, value] : node->properties)
            {
                // Resolve metadata for this property.
                const PropertyMetadata* meta = nullptr;
                if (desc)
                {
                    auto it = desc->propertyMetadata.find(key);
                    if (it != desc->propertyMetadata.end())
                        meta = &it->second;
                }
                if (!meta)
                {
                    auto cmIt = customMeta.find(key);
                    if (cmIt != customMeta.end())
                        meta = &cmIt->second;
                }

                auto propLabel = Controls::TextBlock();
                propLabel.Text(winrt::hstring(key));
                propLabel.FontSize(12);
                propLabel.Margin({ 0, 6, 0, 2 });

                auto capturedKey = key;

                // Check if this property has an active binding.
                auto bindIt = node->propertyBindings.find(key);
                bool isBound = (bindIt != node->propertyBindings.end());

                // Build a row: [label] [bind/unbind button]
                auto labelRow = Controls::StackPanel();
                labelRow.Orientation(Controls::Orientation::Horizontal);
                labelRow.Spacing(6);
                labelRow.Children().Append(propLabel);

                if (isBound)
                {
                    // Show binding info: "← NodeName.FieldName"
                    auto& binding = bindIt->second;
                    auto* srcNode = m_graph.FindNode(binding.sourceNodeId);
                    std::wstring bindLabel = L"\u2190 ";  // ← arrow
                    if (srcNode) bindLabel += srcNode->name + L".";
                    bindLabel += binding.sourceFieldName;
                    if (!::ShaderLab::Graph::AnalysisFieldIsArray(::ShaderLab::Graph::AnalysisFieldType::Float))
                    {
                        // Show component for scalar bindings.
                        const wchar_t* comp[] = { L".x", L".y", L".z", L".w" };
                        if (binding.sourceComponent < 4)
                            bindLabel += comp[binding.sourceComponent];
                    }

                    auto bindInfo = Controls::TextBlock();
                    bindInfo.Text(winrt::hstring(bindLabel));
                    bindInfo.FontSize(11);
                    bindInfo.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
                    labelRow.Children().Append(bindInfo);

                    // Unbind button.
                    auto unbindBtn = Controls::HyperlinkButton();
                    unbindBtn.Content(winrt::box_value(L"Unbind"));
                    unbindBtn.FontSize(11);
                    unbindBtn.Padding({ 2, 0, 2, 0 });
                    unbindBtn.Click([this, capturedId, capturedKey](auto&&, auto&&)
                    {
                        auto* n = m_graph.FindNode(capturedId);
                        if (n) {
                            n->propertyBindings.erase(capturedKey);
                            n->dirty = true;
                            m_graph.MarkAllDirty();
                            UpdatePropertiesPanel();
                        }
                    });
                    labelRow.Children().Append(unbindBtn);
                }
                else
                {
                    // Bind button — only show if there are analysis sources available.
                    bool hasAnalysisSources = false;
                    for (const auto& n : m_graph.Nodes())
                    {
                        if (n.id != capturedId &&
                            n.analysisOutput.type == ::ShaderLab::Graph::AnalysisOutputType::Typed &&
                            !n.analysisOutput.fields.empty())
                        {
                            hasAnalysisSources = true;
                            break;
                        }
                    }

                    if (hasAnalysisSources)
                    {
                        // Build a flyout with available analysis fields.
                        auto bindBtn = Controls::DropDownButton();
                        bindBtn.Content(winrt::box_value(L"\U0001F517"));  // 🔗
                        bindBtn.FontSize(10);
                        bindBtn.Padding({ 4, 0, 4, 0 });
                        bindBtn.MinWidth(0);
                        bindBtn.MinHeight(0);

                        auto flyout = Controls::MenuFlyout();
                        for (const auto& srcNode : m_graph.Nodes())
                        {
                            if (srcNode.id == capturedId) continue;
                            if (srcNode.analysisOutput.type != ::ShaderLab::Graph::AnalysisOutputType::Typed) continue;
                            if (srcNode.analysisOutput.fields.empty()) continue;

                            auto subItem = Controls::MenuFlyoutSubItem();
                            subItem.Text(winrt::hstring(srcNode.name));

                            for (const auto& fv : srcNode.analysisOutput.fields)
                            {
                                uint32_t srcNodeId = srcNode.id;
                                auto fieldName = fv.name;
                                bool isArray = ::ShaderLab::Graph::AnalysisFieldIsArray(fv.type);
                                uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);

                                if (isArray || cc == 1)
                                {
                                    // Single item: bind directly.
                                    auto item = Controls::MenuFlyoutItem();
                                    item.Text(winrt::hstring(fieldName));
                                    item.Click([this, capturedId, capturedKey, srcNodeId, fieldName](auto&&, auto&&)
                                    {
                                        auto* n = m_graph.FindNode(capturedId);
                                        if (!n) return;
                                        if (m_graph.WouldCreateCycle(srcNodeId, capturedId)) return;
                                        ::ShaderLab::Graph::PropertyBinding b;
                                        b.sourceNodeId = srcNodeId;
                                        b.sourceFieldName = fieldName;
                                        b.sourceComponent = 0;
                                        n->propertyBindings[capturedKey] = std::move(b);
                                        n->dirty = true;
                                        m_graph.MarkAllDirty();
                                        UpdatePropertiesPanel();
                                    });
                                    subItem.Items().Append(item);
                                }
                                else
                                {
                                    // Multi-component: show sub-items for .x/.y/.z/.w + whole.
                                    auto fieldSub = Controls::MenuFlyoutSubItem();
                                    fieldSub.Text(winrt::hstring(fieldName));

                                    // Whole vector binding.
                                    auto wholeItem = Controls::MenuFlyoutItem();
                                    wholeItem.Text(L"(all)");
                                    wholeItem.Click([this, capturedId, capturedKey, srcNodeId, fieldName](auto&&, auto&&)
                                    {
                                        auto* n = m_graph.FindNode(capturedId);
                                        if (!n) return;
                                        if (m_graph.WouldCreateCycle(srcNodeId, capturedId)) return;
                                        ::ShaderLab::Graph::PropertyBinding b;
                                        b.sourceNodeId = srcNodeId;
                                        b.sourceFieldName = fieldName;
                                        b.sourceComponent = 0;
                                        n->propertyBindings[capturedKey] = std::move(b);
                                        n->dirty = true;
                                        m_graph.MarkAllDirty();
                                        UpdatePropertiesPanel();
                                    });
                                    fieldSub.Items().Append(wholeItem);

                                    const wchar_t* compNames[] = { L".x", L".y", L".z", L".w" };
                                    for (uint32_t c = 0; c < cc; ++c)
                                    {
                                        auto compItem = Controls::MenuFlyoutItem();
                                        compItem.Text(winrt::hstring(fieldName + compNames[c]));
                                        auto capturedComp = c;
                                        compItem.Click([this, capturedId, capturedKey, srcNodeId, fieldName, capturedComp](auto&&, auto&&)
                                        {
                                            auto* n = m_graph.FindNode(capturedId);
                                            if (!n) return;
                                            if (m_graph.WouldCreateCycle(srcNodeId, capturedId)) return;
                                            ::ShaderLab::Graph::PropertyBinding b;
                                            b.sourceNodeId = srcNodeId;
                                            b.sourceFieldName = fieldName;
                                            b.sourceComponent = capturedComp;
                                            n->propertyBindings[capturedKey] = std::move(b);
                                            n->dirty = true;
                                            m_graph.MarkAllDirty();
                                            UpdatePropertiesPanel();
                                        });
                                        fieldSub.Items().Append(compItem);
                                    }
                                    subItem.Items().Append(fieldSub);
                                }
                            }
                            flyout.Items().Append(subItem);
                        }
                        bindBtn.Flyout(flyout);
                        labelRow.Children().Append(bindBtn);
                    }
                }

                panel.Children().Append(labelRow);

                // Lambda to mark the node dirty after a property change.
                auto markDirty = [this, capturedId]()
                {
                    auto* n = m_graph.FindNode(capturedId);
                    if (n) { n->dirty = true; m_graph.MarkAllDirty(); }

                    // For analysis effects, schedule a properties panel refresh
                    // after the next evaluation computes new output data.
                    if (n && n->effectClsid.has_value() &&
                        IsEqualGUID(n->effectClsid.value(), CLSID_D2D1Histogram))
                    {
                        this->DispatcherQueue().TryEnqueue(
                            winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
                            [this]() { if (!m_isShuttingDown) UpdatePropertiesPanel(); });
                    }
                };

                // If bound, skip creating the editable control — show the live value instead.
                if (isBound)
                {
                    auto boundVal = Controls::TextBlock();
                    // Show the current effective value.
                    std::wstring valStr;
                    std::visit([&](const auto& v)
                    {
                        using VT = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<VT, float>)
                            valStr = std::format(L"{:.4f}", v);
                        else if constexpr (std::is_same_v<VT, int32_t> || std::is_same_v<VT, uint32_t>)
                            valStr = std::to_wstring(v);
                        else if constexpr (std::is_same_v<VT, bool>)
                            valStr = v ? L"true" : L"false";
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float2>)
                            valStr = std::format(L"{:.4f}, {:.4f}", v.x, v.y);
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float3>)
                            valStr = std::format(L"{:.4f}, {:.4f}, {:.4f}", v.x, v.y, v.z);
                        else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float4>)
                            valStr = std::format(L"{:.4f}, {:.4f}, {:.4f}, {:.4f}", v.x, v.y, v.z, v.w);
                        else if constexpr (std::is_same_v<VT, std::vector<float>>)
                            valStr = std::format(L"[{} floats]", v.size());
                        else
                            valStr = L"(bound)";
                    }, value);
                    boundVal.Text(winrt::hstring(valStr));
                    boundVal.FontSize(12);
                    boundVal.FontFamily(Media::FontFamily(L"Consolas"));
                    boundVal.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
                    boundVal.Margin({ 0, 0, 0, 4 });
                    panel.Children().Append(boundVal);
                    continue;  // Skip the normal control creation.
                }

                // ---- Dispatch by variant type + metadata hint ----
                std::visit([&](const auto& v)
                {
                    using T = std::decay_t<decltype(v)>;

                    if constexpr (std::is_same_v<T, bool>)
                    {
                        // Checkbox / ToggleSwitch
                        auto toggle = Controls::ToggleSwitch();
                        toggle.IsOn(v);
                        toggle.OnContent(winrt::box_value(L"True"));
                        toggle.OffContent(winrt::box_value(L"False"));
                        toggle.Margin({ 0, 0, 0, 4 });
                        toggle.Toggled([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                        {
                            auto* n = m_graph.FindNode(capturedId);
                            if (!n) return;
                            auto it = n->properties.find(capturedKey);
                            if (it == n->properties.end()) return;
                            auto ts = sender.template as<Controls::ToggleSwitch>();
                            it->second = ts.IsOn();
                            markDirty();
                        });
                        panel.Children().Append(toggle);
                    }
                    else if constexpr (std::is_same_v<T, uint32_t>)
                    {
                        if (meta && meta->uiHint == PropertyUIHint::ComboBox && !meta->enumLabels.empty())
                        {
                            // ComboBox for enum values.
                            auto combo = Controls::ComboBox();
                            for (const auto& label : meta->enumLabels)
                                combo.Items().Append(winrt::box_value(winrt::hstring(label)));
                            if (v < meta->enumLabels.size())
                                combo.SelectedIndex(static_cast<int32_t>(v));
                            combo.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                            combo.Margin({ 0, 0, 0, 4 });
                            combo.SelectionChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto cb = sender.template as<Controls::ComboBox>();
                                int32_t idx = cb.SelectedIndex();
                                if (idx >= 0)
                                {
                                    it->second = static_cast<uint32_t>(idx);
                                    markDirty();
                                }
                            });
                            panel.Children().Append(combo);
                        }
                        else
                        {
                            // Slider for uint32 with range, or NumberBox fallback.
                            float minV = meta ? meta->minValue : 0.0f;
                            float maxV = meta ? meta->maxValue : 100.0f;
                            float stepV = meta ? meta->step : 1.0f;
                            auto nb = Controls::NumberBox();
                            nb.Value(static_cast<double>(v));
                            nb.Minimum(minV);
                            nb.Maximum(maxV);
                            nb.SmallChange(stepV);
                            nb.LargeChange(stepV * 10.0);
                            nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
                            nb.Margin({ 0, 0, 0, 4 });
                            nb.ValueChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto box = sender.template as<Controls::NumberBox>();
                                double val = box.Value();
                                if (!std::isnan(val))
                                {
                                    it->second = static_cast<uint32_t>(val);
                                    markDirty();
                                }
                            });
                            panel.Children().Append(nb);
                        }
                    }
                    else if constexpr (std::is_same_v<T, float>)
                    {
                        bool useSlider = meta && meta->uiHint == PropertyUIHint::Slider;
                        float minV = meta ? meta->minValue : -FLT_MAX;
                        float maxV = meta ? meta->maxValue : FLT_MAX;
                        float stepV = meta ? meta->step : 0.01f;

                        if (useSlider)
                        {
                            // Slider + NumberBox pair for ranged floats.
                            // Shared flag prevents re-entrant sync loops.
                            auto syncing = std::make_shared<bool>(false);

                            auto slider = Controls::Slider();
                            slider.Minimum(minV);
                            slider.Maximum(maxV);
                            slider.StepFrequency(stepV);
                            slider.Value(static_cast<double>(v));
                            slider.Margin({ 0, 0, 0, 0 });

                            auto nb = Controls::NumberBox();
                            nb.Value(static_cast<double>(v));
                            nb.Minimum(minV);
                            nb.Maximum(maxV);
                            nb.SmallChange(stepV);
                            nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
                            nb.Margin({ 0, 0, 0, 4 });

                            // Slider → NumberBox sync.
                            slider.ValueChanged([nb, syncing, this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                if (*syncing) return;
                                *syncing = true;
                                auto sl = sender.template as<Controls::Slider>();
                                nb.Value(sl.Value());
                                auto* n = m_graph.FindNode(capturedId);
                                if (n)
                                {
                                    auto it = n->properties.find(capturedKey);
                                    if (it != n->properties.end())
                                    {
                                        it->second = static_cast<float>(sl.Value());
                                        markDirty();
                                    }
                                }
                                *syncing = false;
                            });

                            // NumberBox → Slider sync.
                            nb.ValueChanged([slider, syncing, this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                if (*syncing) return;
                                *syncing = true;
                                auto box = sender.template as<Controls::NumberBox>();
                                double val = box.Value();
                                if (!std::isnan(val))
                                {
                                    slider.Value(val);
                                    auto* n = m_graph.FindNode(capturedId);
                                    if (n)
                                    {
                                        auto it = n->properties.find(capturedKey);
                                        if (it != n->properties.end())
                                        {
                                            it->second = static_cast<float>(val);
                                            markDirty();
                                        }
                                    }
                                }
                                *syncing = false;
                            });

                            panel.Children().Append(slider);
                            panel.Children().Append(nb);
                        }
                        else
                        {
                            // NumberBox only.
                            auto nb = Controls::NumberBox();
                            nb.Value(static_cast<double>(v));
                            nb.SmallChange(stepV);
                            nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
                            nb.Margin({ 0, 0, 0, 4 });
                            nb.ValueChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto box = sender.template as<Controls::NumberBox>();
                                double val = box.Value();
                                if (!std::isnan(val))
                                {
                                    it->second = static_cast<float>(val);
                                    markDirty();
                                }
                            });
                            panel.Children().Append(nb);
                        }
                    }
                    else if constexpr (std::is_same_v<T, int32_t>)
                    {
                        float minV = meta ? meta->minValue : static_cast<float>(INT_MIN);
                        float maxV = meta ? meta->maxValue : static_cast<float>(INT_MAX);
                        float stepV = meta ? meta->step : 1.0f;
                        auto nb = Controls::NumberBox();
                        nb.Value(static_cast<double>(v));
                        nb.Minimum(minV);
                        nb.Maximum(maxV);
                        nb.SmallChange(stepV);
                        nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Inline);
                        nb.Margin({ 0, 0, 0, 4 });
                        nb.ValueChanged([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                        {
                            auto* n = m_graph.FindNode(capturedId);
                            if (!n) return;
                            auto it = n->properties.find(capturedKey);
                            if (it == n->properties.end()) return;
                            auto box = sender.template as<Controls::NumberBox>();
                            double val = box.Value();
                            if (!std::isnan(val))
                            {
                                it->second = static_cast<int32_t>(val);
                                markDirty();
                            }
                        });
                        panel.Children().Append(nb);
                    }
                    else if constexpr (std::is_same_v<T, std::wstring>)
                    {
                        auto tb = Controls::TextBox();
                        tb.Text(winrt::hstring(v));
                        tb.Margin({ 0, 0, 0, 4 });
                        tb.LostFocus([this, capturedId, capturedKey, markDirty](auto&& sender, auto&&)
                        {
                            auto* n = m_graph.FindNode(capturedId);
                            if (!n) return;
                            auto it = n->properties.find(capturedKey);
                            if (it == n->properties.end()) return;
                            auto box = sender.template as<Controls::TextBox>();
                            it->second = std::wstring(box.Text().c_str());
                            markDirty();
                        });
                        panel.Children().Append(tb);
                    }
                    else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                    {
                        // 5×4 grid of NumberBoxes for the color matrix.
                        static const std::wstring rowLabels[] = { L"R", L"G", L"B", L"A", L"Ofs" };
                        static const std::wstring colLabels[] = { L"R", L"G", L"B", L"A" };

                        auto grid = Controls::StackPanel();
                        grid.Orientation(Controls::Orientation::Vertical);
                        grid.Margin({ 0, 0, 0, 4 });

                        // Column header row.
                        auto headerRow = Controls::StackPanel();
                        headerRow.Orientation(Controls::Orientation::Horizontal);
                        auto spacer = Controls::TextBlock();
                        spacer.Width(32);
                        headerRow.Children().Append(spacer);
                        for (int c = 0; c < 4; ++c)
                        {
                            auto colHdr = Controls::TextBlock();
                            colHdr.Text(winrt::hstring(colLabels[c]));
                            colHdr.Width(70);
                            colHdr.FontSize(11);
                            colHdr.HorizontalTextAlignment(winrt::Microsoft::UI::Xaml::TextAlignment::Center);
                            headerRow.Children().Append(colHdr);
                        }
                        grid.Children().Append(headerRow);

                        for (int r = 0; r < 5; ++r)
                        {
                            auto row = Controls::StackPanel();
                            row.Orientation(Controls::Orientation::Horizontal);
                            row.Margin({ 0, 1, 0, 1 });

                            auto rowLbl = Controls::TextBlock();
                            rowLbl.Text(winrt::hstring(rowLabels[r]));
                            rowLbl.Width(32);
                            rowLbl.FontSize(11);
                            rowLbl.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
                            row.Children().Append(rowLbl);

                            for (int c = 0; c < 4; ++c)
                            {
                                int cellIdx = r * 4 + c;
                                const float* p = &v._11;
                                auto nb = Controls::NumberBox();
                                nb.Value(static_cast<double>(p[cellIdx]));
                                nb.SmallChange(meta ? meta->step : 0.01);
                                nb.Width(70);
                                nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);

                                nb.ValueChanged([this, capturedId, capturedKey, cellIdx, markDirty](auto&& sender, auto&&)
                                {
                                    auto* n = m_graph.FindNode(capturedId);
                                    if (!n) return;
                                    auto it = n->properties.find(capturedKey);
                                    if (it == n->properties.end()) return;
                                    auto box = sender.template as<Controls::NumberBox>();
                                    double val = box.Value();
                                    if (std::isnan(val)) return;
                                    auto* mat = std::get_if<D2D1_MATRIX_5X4_F>(&it->second);
                                    if (mat)
                                    {
                                        float* mp = &mat->_11;
                                        mp[cellIdx] = static_cast<float>(val);
                                        markDirty();
                                    }
                                });
                                row.Children().Append(nb);
                            }
                            grid.Children().Append(row);
                        }
                        panel.Children().Append(grid);
                    }
                    else if constexpr (std::is_same_v<T, std::vector<float>>)
                    {
                        // Curve editor: show a button to open a flyout, plus a small preview.
                        auto curveContainer = Controls::StackPanel();
                        curveContainer.Orientation(Controls::Orientation::Vertical);
                        curveContainer.Margin({ 0, 0, 0, 4 });

                        // Miniature curve preview using a Polyline.
                        auto previewCanvas = winrt::Microsoft::UI::Xaml::Controls::Canvas();
                        previewCanvas.Width(200);
                        previewCanvas.Height(60);
                        previewCanvas.Background(Media::SolidColorBrush(
                            winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

                        // Draw curve preview as a Polyline.
                        auto polyline = winrt::Microsoft::UI::Xaml::Shapes::Polyline();
                        polyline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
                        polyline.StrokeThickness(1.5);
                        auto points = winrt::Microsoft::UI::Xaml::Media::PointCollection();
                        if (!v.empty())
                        {
                            uint32_t step = (std::max)(1u, static_cast<uint32_t>(v.size()) / 50u);
                            for (uint32_t i = 0; i < v.size(); i += step)
                            {
                                float x = static_cast<float>(i) / static_cast<float>(v.size() - 1) * 200.0f;
                                float y = 60.0f - std::clamp(v[i], 0.0f, 1.0f) * 60.0f;
                                points.Append(winrt::Windows::Foundation::Point(x, y));
                            }
                        }
                        polyline.Points(points);
                        previewCanvas.Children().Append(polyline);
                        curveContainer.Children().Append(previewCanvas);

                        // "Edit Curve" button opens a ContentDialog with full curve editor.
                        auto editBtn = Controls::Button();
                        editBtn.Content(winrt::box_value(L"Edit Curve..."));
                        editBtn.Margin({ 0, 4, 0, 0 });
                        editBtn.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
                        editBtn.Click([this, capturedId, capturedKey, markDirty](auto&&, auto&&)
                        {
                            ShowCurveEditorDialog(capturedId, capturedKey, markDirty);
                        });
                        curveContainer.Children().Append(editBtn);
                        panel.Children().Append(curveContainer);
                    }
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>
                                    || std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>
                                    || std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                    {
                        // Vector editor: horizontal row of labeled NumberBoxes.
                        constexpr int N = std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2> ? 2
                                        : std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3> ? 3 : 4;

                        std::vector<std::wstring> labels;
                        if (meta && meta->componentLabels.size() >= static_cast<size_t>(N))
                            labels = { meta->componentLabels.begin(), meta->componentLabels.begin() + N };
                        else
                        {
                            // Heuristic: if the property name contains "Color", use R/G/B/A labels.
                            bool isColor = (capturedKey.find(L"Color") != std::wstring::npos)
                                        || (capturedKey.find(L"color") != std::wstring::npos);
                            if (isColor)
                            {
                                if constexpr (N == 2) labels = { L"R", L"G" };
                                else if constexpr (N == 3) labels = { L"R", L"G", L"B" };
                                else                       labels = { L"R", L"G", L"B", L"A" };
                            }
                            else
                            {
                                if constexpr (N == 2) labels = { L"X", L"Y" };
                                else if constexpr (N == 3) labels = { L"X", L"Y", L"Z" };
                                else                       labels = { L"X", L"Y", L"Z", L"W" };
                            }
                        }

                        float components[4] = {};
                        if constexpr (N >= 2) { components[0] = v.x; components[1] = v.y; }
                        if constexpr (N >= 3) { components[2] = v.z; }
                        if constexpr (N >= 4) { components[3] = v.w; }

                        float minV = meta ? meta->minValue : -FLT_MAX;
                        float maxV = meta ? meta->maxValue : FLT_MAX;
                        float stepV = meta ? meta->step : 0.01f;

                        auto row = Controls::StackPanel();
                        row.Orientation(Controls::Orientation::Vertical);
                        row.Margin({ 0, 0, 0, 4 });

                        for (int i = 0; i < N; ++i)
                        {
                            auto compRow = Controls::StackPanel();
                            compRow.Orientation(Controls::Orientation::Horizontal);
                            compRow.Margin({ 0, 1, 0, 1 });

                            auto lbl = Controls::TextBlock();
                            lbl.Text(winrt::hstring(labels[i]));
                            lbl.FontSize(11);
                            lbl.Width(20);
                            lbl.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
                            compRow.Children().Append(lbl);

                            float compMin = (meta && static_cast<int>(meta->componentMin.size()) > i)
                                ? meta->componentMin[i] : minV;
                            float compMax = (meta && static_cast<int>(meta->componentMax.size()) > i)
                                ? meta->componentMax[i] : maxV;

                            auto nb = Controls::NumberBox();
                            nb.Value(static_cast<double>(components[i]));
                            nb.Minimum(compMin);
                            nb.Maximum(compMax);
                            nb.SmallChange(stepV);
                            nb.SpinButtonPlacementMode(Controls::NumberBoxSpinButtonPlacementMode::Compact);
                            nb.HorizontalAlignment(winrt::Microsoft::UI::Xaml::HorizontalAlignment::Stretch);

                            int compIdx = i;
                            nb.ValueChanged([this, capturedId, capturedKey, compIdx, markDirty](auto&& sender, auto&&)
                            {
                                auto* n = m_graph.FindNode(capturedId);
                                if (!n) return;
                                auto it = n->properties.find(capturedKey);
                                if (it == n->properties.end()) return;
                                auto box = sender.template as<Controls::NumberBox>();
                                double val = box.Value();
                                if (std::isnan(val)) return;
                                float fval = static_cast<float>(val);

                                std::visit([compIdx, fval](auto& vec)
                                {
                                    using VT = std::decay_t<decltype(vec)>;
                                    if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float2>)
                                    {
                                        if (compIdx == 0) vec.x = fval;
                                        else if (compIdx == 1) vec.y = fval;
                                    }
                                    else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float3>)
                                    {
                                        if (compIdx == 0) vec.x = fval;
                                        else if (compIdx == 1) vec.y = fval;
                                        else if (compIdx == 2) vec.z = fval;
                                    }
                                    else if constexpr (std::is_same_v<VT, winrt::Windows::Foundation::Numerics::float4>)
                                    {
                                        if (compIdx == 0) vec.x = fval;
                                        else if (compIdx == 1) vec.y = fval;
                                        else if (compIdx == 2) vec.z = fval;
                                        else if (compIdx == 3) vec.w = fval;
                                    }
                                }, it->second);
                                markDirty();
                            });
                            compRow.Children().Append(nb);
                            row.Children().Append(compRow);
                        }
                        panel.Children().Append(row);
                    }
                }, value);
            }
        }

        // ---- Gamma Transfer curve preview ----
        if (node->type == ::ShaderLab::Graph::NodeType::BuiltInEffect &&
            node->effectClsid.has_value() && IsEqualGUID(node->effectClsid.value(), CLSID_D2D1GammaTransfer))
        {
            auto previewLabel = Controls::TextBlock();
            previewLabel.Text(L"Curve Preview");
            previewLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            previewLabel.Margin({ 0, 8, 0, 4 });
            panel.Children().Append(previewLabel);

            auto canvas = Controls::Canvas();
            canvas.Width(200);
            canvas.Height(80);
            canvas.Background(Media::SolidColorBrush(
                winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

            // Draw per-channel gamma curves: output = amplitude * input^exponent + offset
            struct ChannelInfo { std::wstring prefix; winrt::Windows::UI::Color lineColor; };
            ChannelInfo channels[] = {
                { L"Red",   { 200, 255, 80, 80 } },
                { L"Green", { 200, 80, 255, 80 } },
                { L"Blue",  { 200, 80, 80, 255 } },
            };

            for (const auto& ch : channels)
            {
                float amp = 1.0f, exp = 1.0f, ofs = 0.0f;
                bool disabled = false;
                auto ampIt = node->properties.find(ch.prefix + L"Amplitude");
                auto expIt = node->properties.find(ch.prefix + L"Exponent");
                auto ofsIt = node->properties.find(ch.prefix + L"Offset");
                auto disIt = node->properties.find(ch.prefix + L"Disable");
                if (ampIt != node->properties.end()) if (auto* pf = std::get_if<float>(&ampIt->second)) amp = *pf;
                if (expIt != node->properties.end()) if (auto* pf = std::get_if<float>(&expIt->second)) exp = *pf;
                if (ofsIt != node->properties.end()) if (auto* pf = std::get_if<float>(&ofsIt->second)) ofs = *pf;
                if (disIt != node->properties.end()) if (auto* pb = std::get_if<bool>(&disIt->second)) disabled = *pb;

                if (disabled) continue;

                auto poly = winrt::Microsoft::UI::Xaml::Shapes::Polyline();
                poly.Stroke(Media::SolidColorBrush(ch.lineColor));
                poly.StrokeThickness(1.5);
                auto pts = winrt::Microsoft::UI::Xaml::Media::PointCollection();
                for (int i = 0; i <= 50; ++i)
                {
                    float input = static_cast<float>(i) / 50.0f;
                    float output = amp * std::pow(input, exp) + ofs;
                    output = std::clamp(output, 0.0f, 1.0f);
                    pts.Append({ input * 200.0f, 80.0f - output * 80.0f });
                }
                poly.Points(pts);
                canvas.Children().Append(poly);
            }
            panel.Children().Append(canvas);
        }

        // ---- Analysis output visualization ----
        if (node->analysisOutput.type == ::ShaderLab::Graph::AnalysisOutputType::Histogram &&
            !node->analysisOutput.data.empty())
        {
            auto histHeader = Controls::TextBlock();
            histHeader.Text(winrt::hstring(node->analysisOutput.label));
            histHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            histHeader.Margin({ 0, 8, 0, 4 });
            panel.Children().Append(histHeader);

            // Draw histogram as a bar chart using XAML Canvas.
            constexpr float chartW = 300.0f;
            constexpr float chartH = 120.0f;
            auto canvas = Controls::Canvas();
            canvas.Width(chartW);
            canvas.Height(chartH);
            canvas.Background(Media::SolidColorBrush(
                winrt::Windows::UI::Color{ 255, 30, 30, 30 }));

            const auto& bins = node->analysisOutput.data;
            uint32_t numBins = static_cast<uint32_t>(bins.size());
            if (numBins > 0)
            {
                // Find max bin value for normalization.
                float maxVal = *std::max_element(bins.begin(), bins.end());
                if (maxVal <= 0.0f) maxVal = 1.0f;

                // Choose bar color based on channel.
                winrt::Windows::UI::Color barColor;
                switch (node->analysisOutput.channelIndex)
                {
                case 0: barColor = { 200, 255, 60, 60 }; break;   // Red
                case 1: barColor = { 200, 60, 255, 60 }; break;   // Green
                case 2: barColor = { 200, 60, 100, 255 }; break;  // Blue
                default: barColor = { 200, 200, 200, 200 }; break; // Alpha / other
                }
                auto barBrush = Media::SolidColorBrush(barColor);

                float barWidth = chartW / static_cast<float>(numBins);
                for (uint32_t i = 0; i < numBins; ++i)
                {
                    float normalized = bins[i] / maxVal;
                    float barH = normalized * chartH;
                    if (barH < 0.5f) continue; // skip empty bins

                    auto rect = winrt::Microsoft::UI::Xaml::Shapes::Rectangle();
                    rect.Width((std::max)(1.0, static_cast<double>(barWidth)));
                    rect.Height(static_cast<double>(barH));
                    rect.Fill(barBrush);
                    Controls::Canvas::SetLeft(rect, static_cast<double>(i * barWidth));
                    Controls::Canvas::SetTop(rect, static_cast<double>(chartH - barH));
                    canvas.Children().Append(rect);
                }
            }
            panel.Children().Append(canvas);
        }

        // ---- Typed analysis output ----
        if (node->analysisOutput.type == ::ShaderLab::Graph::AnalysisOutputType::Typed &&
            !node->analysisOutput.fields.empty())
        {
            auto analysisHeader = Controls::TextBlock();
            analysisHeader.Text(L"Analysis Results");
            analysisHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            analysisHeader.Margin({ 0, 8, 0, 4 });
            panel.Children().Append(analysisHeader);

            for (const auto& fv : node->analysisOutput.fields)
            {
                auto row = Controls::StackPanel();
                row.Orientation(Controls::Orientation::Horizontal);
                row.Spacing(8);
                row.Margin({ 0, 2, 0, 2 });

                auto labelText = Controls::TextBlock();
                labelText.Text(winrt::hstring(fv.name));
                labelText.FontSize(12);
                labelText.Width(140);
                labelText.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
                row.Children().Append(labelText);

                auto valueText = Controls::TextBlock();
                std::wstring valStr;
                if (!::ShaderLab::Graph::AnalysisFieldIsArray(fv.type))
                {
                    uint32_t cc = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    for (uint32_t c = 0; c < cc; ++c)
                    {
                        if (c > 0) valStr += L"  ";
                        valStr += std::format(L"{:.4f}", fv.components[c]);
                    }
                }
                else
                {
                    uint32_t stride = ::ShaderLab::Graph::AnalysisFieldComponentCount(fv.type);
                    uint32_t count = static_cast<uint32_t>(fv.arrayData.size()) / (std::max)(stride, 1u);
                    valStr = std::format(L"[{} elements]", count);
                }
                valueText.Text(winrt::hstring(valStr));
                valueText.FontSize(12);
                valueText.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
                valueText.IsTextSelectionEnabled(true);
                row.Children().Append(valueText);

                panel.Children().Append(row);
            }
        }

        // ---- Input pins info ----
        if (!node->inputPins.empty())
        {
            auto pinsHeader = Controls::TextBlock();
            pinsHeader.Text(L"Inputs: " + std::to_wstring(node->inputPins.size()));
            pinsHeader.Foreground(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::Gray()));
            pinsHeader.Margin({ 0, 8, 0, 0 });
            panel.Children().Append(pinsHeader);
        }
    }

    void MainWindow::ShowCurveEditorDialog(
        uint32_t nodeId, const std::wstring& propertyKey, std::function<void()> markDirty)
    {
        namespace Controls = winrt::Microsoft::UI::Xaml::Controls;
        namespace Media = winrt::Microsoft::UI::Xaml::Media;

        auto* node = m_graph.FindNode(nodeId);
        if (!node) return;
        auto propIt = node->properties.find(propertyKey);
        if (propIt == node->properties.end()) return;
        auto* lutPtr = std::get_if<std::vector<float>>(&propIt->second);
        if (!lutPtr) return;

        constexpr float canvasW = 400.0f;
        constexpr float canvasH = 300.0f;
        constexpr float pointRadius = 6.0f;

        // Build a set of editable control points from the LUT.
        // Sample ~16 evenly-spaced points for dragging; the full LUT is interpolated from them.
        auto controlPoints = std::make_shared<std::vector<winrt::Windows::Foundation::Point>>();
        uint32_t lutSize = static_cast<uint32_t>(lutPtr->size());
        uint32_t numCtrlPts = (std::min)(16u, lutSize);
        if (numCtrlPts < 2) numCtrlPts = 2;
        for (uint32_t i = 0; i < numCtrlPts; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(numCtrlPts - 1);
            uint32_t idx = (std::min)(static_cast<uint32_t>(t * (lutSize - 1)), lutSize - 1);
            float x = t * canvasW;
            float y = canvasH - std::clamp((*lutPtr)[idx], 0.0f, 1.0f) * canvasH;
            controlPoints->push_back({ x, y });
        }

        // Canvas for the curve.
        auto canvas = Controls::Canvas();
        canvas.Width(canvasW);
        canvas.Height(canvasH);
        canvas.Background(Media::SolidColorBrush(
            winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

        // Draw grid lines.
        for (int i = 1; i < 4; ++i)
        {
            float pos = canvasH * i / 4.0f;
            auto hline = winrt::Microsoft::UI::Xaml::Shapes::Line();
            hline.X1(0); hline.X2(canvasW); hline.Y1(pos); hline.Y2(pos);
            hline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 60, 60, 60)));
            hline.StrokeThickness(0.5);
            canvas.Children().Append(hline);

            float posX = canvasW * i / 4.0f;
            auto vline = winrt::Microsoft::UI::Xaml::Shapes::Line();
            vline.X1(posX); vline.X2(posX); vline.Y1(0); vline.Y2(canvasH);
            vline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 60, 60, 60)));
            vline.StrokeThickness(0.5);
            canvas.Children().Append(vline);
        }

        // Curve polyline.
        auto polyline = winrt::Microsoft::UI::Xaml::Shapes::Polyline();
        polyline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
        polyline.StrokeThickness(2.0);
        canvas.Children().Append(polyline);

        // Helper to rebuild the polyline from control points.
        auto rebuildCurve = [polyline, controlPoints, canvasW, canvasH]()
        {
            auto pts = winrt::Microsoft::UI::Xaml::Media::PointCollection();
            for (const auto& cp : *controlPoints)
                pts.Append(cp);
            polyline.Points(pts);
        };
        rebuildCurve();

        // Add draggable point ellipses.
        auto draggingIdx = std::make_shared<int>(-1);
        std::vector<winrt::Microsoft::UI::Xaml::Shapes::Ellipse> pointEllipses;

        for (uint32_t i = 0; i < controlPoints->size(); ++i)
        {
            auto ellipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse();
            ellipse.Width(pointRadius * 2);
            ellipse.Height(pointRadius * 2);
            ellipse.Fill(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::White()));
            Controls::Canvas::SetLeft(ellipse, (*controlPoints)[i].X - pointRadius);
            Controls::Canvas::SetTop(ellipse, (*controlPoints)[i].Y - pointRadius);
            canvas.Children().Append(ellipse);
            pointEllipses.push_back(ellipse);
        }

        // Pointer handlers for dragging control points.
        canvas.PointerPressed([draggingIdx, controlPoints, pointRadius](auto&&,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            auto pos = args.GetCurrentPoint(nullptr).Position();
            for (int i = static_cast<int>(controlPoints->size()) - 1; i >= 0; --i)
            {
                float dx = pos.X - (*controlPoints)[i].X;
                float dy = pos.Y - (*controlPoints)[i].Y;
                if (dx * dx + dy * dy < (pointRadius * 3) * (pointRadius * 3))
                {
                    *draggingIdx = i;
                    break;
                }
            }
        });

        canvas.PointerMoved([draggingIdx, controlPoints, &pointEllipses, rebuildCurve,
            canvasW, canvasH, pointRadius](auto&&,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            if (*draggingIdx < 0) return;
            auto pos = args.GetCurrentPoint(nullptr).Position();
            int idx = *draggingIdx;

            // Clamp X to maintain ordering (first/last points locked to edges).
            float x = pos.X;
            float y = std::clamp(pos.Y, 0.0f, canvasH);
            if (idx == 0)
                x = 0.0f;
            else if (idx == static_cast<int>(controlPoints->size()) - 1)
                x = canvasW;
            else
            {
                float minX = (*controlPoints)[idx - 1].X + 1.0f;
                float maxX = (*controlPoints)[idx + 1].X - 1.0f;
                x = std::clamp(x, minX, maxX);
            }

            (*controlPoints)[idx] = { x, y };
            Controls::Canvas::SetLeft(pointEllipses[idx], x - pointRadius);
            Controls::Canvas::SetTop(pointEllipses[idx], y - pointRadius);
            rebuildCurve();
        });

        canvas.PointerReleased([draggingIdx](auto&&, auto&&) { *draggingIdx = -1; });

        // Build dialog.
        auto dialog = Controls::ContentDialog();
        dialog.Title(winrt::box_value(L"Curve Editor — " + propertyKey));
        dialog.PrimaryButtonText(L"Apply");
        dialog.CloseButtonText(L"Cancel");
        dialog.XamlRoot(this->Content().XamlRoot());

        auto dialogContent = Controls::StackPanel();
        dialogContent.Children().Append(canvas);

        // "Reset to Identity" button.
        auto resetBtn = Controls::Button();
        resetBtn.Content(winrt::box_value(L"Reset to Identity"));
        resetBtn.Margin({ 0, 8, 0, 0 });
        resetBtn.Click([controlPoints, &pointEllipses, rebuildCurve, canvasW, canvasH, pointRadius](auto&&, auto&&)
        {
            for (uint32_t i = 0; i < controlPoints->size(); ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(controlPoints->size() - 1);
                (*controlPoints)[i] = { t * canvasW, canvasH - t * canvasH };
                Controls::Canvas::SetLeft(pointEllipses[i], t * canvasW - pointRadius);
                Controls::Canvas::SetTop(pointEllipses[i], (canvasH - t * canvasH) - pointRadius);
            }
            rebuildCurve();
        });
        dialogContent.Children().Append(resetBtn);
        dialog.Content(dialogContent);

        // Show dialog and apply result.
        auto capturedNodeId = nodeId;
        auto capturedKey = propertyKey;
        auto capturedMarkDirty = markDirty;
        auto capturedCtrlPts = controlPoints;

        dialog.PrimaryButtonClick([this, capturedNodeId, capturedKey, capturedMarkDirty,
            capturedCtrlPts, canvasW, canvasH](auto&&, auto&&)
        {
            auto* n = m_graph.FindNode(capturedNodeId);
            if (!n) return;
            auto it = n->properties.find(capturedKey);
            if (it == n->properties.end()) return;
            auto* lut = std::get_if<std::vector<float>>(&it->second);
            if (!lut) return;

            // Interpolate control points into the full LUT array.
            uint32_t lutSize = static_cast<uint32_t>(lut->size());
            for (uint32_t i = 0; i < lutSize; ++i)
            {
                float x = static_cast<float>(i) / static_cast<float>(lutSize - 1) * canvasW;

                // Find the two surrounding control points.
                int cpIdx = 0;
                for (int j = 1; j < static_cast<int>(capturedCtrlPts->size()); ++j)
                {
                    if ((*capturedCtrlPts)[j].X >= x)
                    {
                        cpIdx = j - 1;
                        break;
                    }
                    cpIdx = j;
                }

                float val;
                if (cpIdx >= static_cast<int>(capturedCtrlPts->size()) - 1)
                {
                    val = 1.0f - (*capturedCtrlPts).back().Y / canvasH;
                }
                else
                {
                    float x0 = (*capturedCtrlPts)[cpIdx].X;
                    float x1 = (*capturedCtrlPts)[cpIdx + 1].X;
                    float y0 = (*capturedCtrlPts)[cpIdx].Y;
                    float y1 = (*capturedCtrlPts)[cpIdx + 1].Y;
                    float t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0f;
                    float yInterp = y0 + t * (y1 - y0);
                    val = 1.0f - yInterp / canvasH;
                }
                (*lut)[i] = std::clamp(val, 0.0f, 1.0f);
            }
            capturedMarkDirty();
            UpdatePropertiesPanel();
        });

        dialog.ShowAsync();
    }

    winrt::fire_and_forget MainWindow::BrowseImageForSourceNode(uint32_t nodeId)
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
        picker.FileTypeFilter().Append(L".png");
        picker.FileTypeFilter().Append(L".jpg");
        picker.FileTypeFilter().Append(L".jpeg");
        picker.FileTypeFilter().Append(L".bmp");
        picker.FileTypeFilter().Append(L".tif");
        picker.FileTypeFilter().Append(L".tiff");
        picker.FileTypeFilter().Append(L".hdr");
        picker.FileTypeFilter().Append(L".exr");
        picker.FileTypeFilter().Append(L".jxr");

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        auto* node = m_graph.FindNode(nodeId);
        if (!node) co_return;

        node->shaderPath = std::wstring(file.Path().c_str());
        node->name = std::wstring(file.Name().c_str());
        node->dirty = true;

        // Prepare the source (load the image bitmap).
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (dc)
            m_sourceFactory.PrepareSourceNode(*node, dc);

        m_graph.MarkAllDirty();
        m_nodeGraphController.RebuildLayout();
        PopulatePreviewNodeSelector();
        FitPreviewToView();
        UpdatePropertiesPanel();
    }

    // -----------------------------------------------------------------------
    // Column splitter
    // -----------------------------------------------------------------------

    void MainWindow::OnColumnSplitterPointerPressed(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        m_isDraggingSplitter = true;
        auto point = args.GetCurrentPoint(this->Content());
        m_splitterDragStartX = point.Position().X;

        auto col0 = Content().as<winrt::Microsoft::UI::Xaml::Controls::Grid>().ColumnDefinitions().GetAt(0);
        auto col2 = Content().as<winrt::Microsoft::UI::Xaml::Controls::Grid>().ColumnDefinitions().GetAt(2);
        m_splitterStartCol0Width = col0.ActualWidth();
        m_splitterStartCol2Width = col2.ActualWidth();

        sender.as<winrt::Microsoft::UI::Xaml::UIElement>().CapturePointer(args.Pointer());
        args.Handled(true);
    }

    void MainWindow::OnColumnSplitterPointerMoved(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (!m_isDraggingSplitter) return;

        auto point = args.GetCurrentPoint(this->Content());
        double delta = point.Position().X - m_splitterDragStartX;

        double newCol0 = (std::max)(100.0, m_splitterStartCol0Width + delta);
        double newCol2 = (std::max)(100.0, m_splitterStartCol2Width - delta);

        auto grid = Content().as<winrt::Microsoft::UI::Xaml::Controls::Grid>();
        grid.ColumnDefinitions().GetAt(0).Width(
            winrt::Microsoft::UI::Xaml::GridLengthHelper::FromPixels(newCol0));
        grid.ColumnDefinitions().GetAt(2).Width(
            winrt::Microsoft::UI::Xaml::GridLengthHelper::FromPixels(newCol2));

        args.Handled(true);
    }

    void MainWindow::OnColumnSplitterPointerReleased(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isDraggingSplitter)
        {
            m_isDraggingSplitter = false;
            sender.as<winrt::Microsoft::UI::Xaml::UIElement>().ReleasePointerCapture(args.Pointer());
        }
    }

    void MainWindow::OpenEffectDesigner()
    {
        if (!m_designerWindow)
        {
            m_designerWindow = winrt::make<winrt::ShaderLab::implementation::EffectDesignerWindow>();

            // Set up callbacks from the designer to the main window.
            auto designerImpl = winrt::get_self<winrt::ShaderLab::implementation::EffectDesignerWindow>(m_designerWindow);
            designerImpl->SetAddToGraphCallback([this](::ShaderLab::Graph::EffectNode node) -> uint32_t
            {
                auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
                m_graph.MarkAllDirty();
                m_nodeGraphController.RebuildLayout();
                PopulatePreviewNodeSelector();
                return nodeId;
            });
            designerImpl->SetUpdateInGraphCallback([this](uint32_t nodeId, ::ShaderLab::Graph::CustomEffectDefinition def)
            {
                auto* node = m_graph.FindNode(nodeId);
                if (node)
                {
                    node->customEffect = std::move(def);
                    node->dirty = true;
                    m_graph.MarkAllDirty();
                    m_graphEvaluator.InvalidateNode(nodeId);
                }
            });

            m_designerWindow.Closed([this](auto&&, auto&&) { m_designerWindow = nullptr; });
        }
        m_designerWindow.Activate();
    }

    void MainWindow::OnSaveImageClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        SaveImageAsync();
    }

    std::vector<uint8_t> MainWindow::CapturePreviewAsPng()
    {
        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return {};

        auto* image = ResolveDisplayImage(m_previewNodeId);
        if (!image) return {};

        float oldDpiX, oldDpiY;
        dc->GetDpi(&oldDpiX, &oldDpiY);
        dc->SetDpi(96.0f, 96.0f);

        // Get the swap chain dimensions as the capture size.
        // This matches what the user sees in the preview panel.
        uint32_t w = m_renderEngine.BackBufferWidth();
        uint32_t h = m_renderEngine.BackBufferHeight();

        // If no swap chain, fall back to image bounds.
        if (w == 0 || h == 0)
        {
            D2D1_RECT_F bounds{};
            dc->GetImageLocalBounds(image, &bounds);
            w = static_cast<uint32_t>(bounds.right - bounds.left);
            h = static_cast<uint32_t>(bounds.bottom - bounds.top);
        }

        dc->SetDpi(oldDpiX, oldDpiY);

        if (w == 0 || h == 0) return {};
        w = (std::min)(w, 2048u);
        h = (std::min)(h, 2048u);

        try
        {
            winrt::com_ptr<ID2D1Bitmap1> renderBitmap;
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bmpProps, renderBitmap.put()));

            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(renderBitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
            // Apply same pan/zoom as preview so capture matches what user sees.
            D2D1_MATRIX_3X2_F transform =
                D2D1::Matrix3x2F::Scale(m_previewZoom, m_previewZoom) *
                D2D1::Matrix3x2F::Translation(m_previewPanX, m_previewPanY);
            dc->SetTransform(transform);
            dc->DrawImage(image);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->EndDraw();
            dc->SetTarget(oldTarget.get());

            winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, cpuProps, cpuBitmap.put()));
            D2D1_POINT_2U destPt = { 0, 0 };
            D2D1_RECT_U srcRc = { 0, 0, w, h };
            winrt::check_hresult(cpuBitmap->CopyFromBitmap(&destPt, renderBitmap.get(), &srcRc));

            D2D1_MAPPED_RECT mapped{};
            winrt::check_hresult(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped));

            winrt::com_ptr<IWICImagingFactory> wicFactory;
            winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wicFactory.put())));

            winrt::com_ptr<IStream> memStream;
            CreateStreamOnHGlobal(nullptr, TRUE, memStream.put());

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(memStream.get(), WICBitmapEncoderNoCache));

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
            winrt::check_hresult(frame->Initialize(nullptr));
            winrt::check_hresult(frame->SetSize(w, h));
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            winrt::check_hresult(frame->SetPixelFormat(&fmt));
            winrt::check_hresult(frame->WritePixels(h, mapped.pitch, mapped.pitch * h, mapped.bits));
            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            cpuBitmap->Unmap();

            STATSTG stat{};
            memStream->Stat(&stat, STATFLAG_NONAME);
            ULONG pngSize = static_cast<ULONG>(stat.cbSize.QuadPart);
            std::vector<uint8_t> result(pngSize);
            LARGE_INTEGER zero{};
            memStream->Seek(zero, STREAM_SEEK_SET, nullptr);
            memStream->Read(result.data(), pngSize, nullptr);
            return result;
        }
        catch (...) { return {}; }
    }

    winrt::fire_and_forget MainWindow::SaveImageAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileSavePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);
        picker.SuggestedFileName(L"output");
        picker.FileTypeChoices().Insert(L"PNG Image", winrt::single_threaded_vector<winrt::hstring>({ L".png" }));
        picker.FileTypeChoices().Insert(L"JPEG Image", winrt::single_threaded_vector<winrt::hstring>({ L".jpg" }));

        auto file = co_await picker.PickSaveFileAsync();
        if (!file) co_return;

        auto* previewImage = ResolveDisplayImage(m_previewNodeId);
        if (!previewImage)
        {
            PipelineFormatText().Text(L"Error: No image to save");
            co_return;
        }

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) co_return;

        try
        {
            // Get image bounds to determine size.
            D2D1_RECT_F bounds{};
            dc->GetImageLocalBounds(previewImage, &bounds);
            uint32_t w = static_cast<uint32_t>(bounds.right - bounds.left);
            uint32_t h = static_cast<uint32_t>(bounds.bottom - bounds.top);
            if (w == 0 || h == 0) co_return;

            // Render image to a bitmap.
            winrt::com_ptr<ID2D1Bitmap1> renderBitmap;
            D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, bmpProps, renderBitmap.put());

            winrt::com_ptr<ID2D1Image> oldTarget;
            dc->GetTarget(oldTarget.put());
            dc->SetTarget(renderBitmap.get());
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(D2D1::ColorF::Black));
            dc->DrawImage(previewImage);
            dc->EndDraw();
            dc->SetTarget(oldTarget.get());

            // Use WIC to encode and save.
            winrt::com_ptr<IWICImagingFactory> wicFactory;
            winrt::check_hresult(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wicFactory.put())));

            auto filePath = std::wstring(file.Path().c_str());
            auto fileExt = std::wstring(file.FileType().c_str());

            GUID containerFormat = GUID_ContainerFormatPng;
            if (fileExt == L".jpg" || fileExt == L".jpeg")
                containerFormat = GUID_ContainerFormatJpeg;

            winrt::com_ptr<IWICStream> stream;
            winrt::check_hresult(wicFactory->CreateStream(stream.put()));
            winrt::check_hresult(stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE));

            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(wicFactory->CreateEncoder(containerFormat, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));

            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
            winrt::check_hresult(frame->Initialize(nullptr));
            winrt::check_hresult(frame->SetSize(w, h));

            WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
            winrt::check_hresult(frame->SetPixelFormat(&pixelFormat));

            // Map the D2D bitmap and write pixels.
            winrt::com_ptr<ID2D1Bitmap1> cpuBitmap;
            D2D1_BITMAP_PROPERTIES1 cpuProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
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

            PipelineFormatText().Text(L"Image saved: " + file.Name());
        }
        catch (...)
        {
            PipelineFormatText().Text(L"Error: Failed to save image");
        }
    }

    // -----------------------------------------------------------------------
    // Preview pan/zoom
    // -----------------------------------------------------------------------

    void MainWindow::FitPreviewToView()
    {
        auto vp = PreviewViewportDips();
        float vpW = vp.width;
        float vpH = vp.height;
        if (vpW <= 0 || vpH <= 0)
        {
            m_previewZoom = 1.0f;
            m_previewPanX = 0.0f;
            m_previewPanY = 0.0f;
            return;
        }

        auto bounds = GetPreviewImageBounds();
        float imgW = bounds.right - bounds.left;
        float imgH = bounds.bottom - bounds.top;

        // For infinite or very large images (e.g., Flood), use a default view.
        if (imgW <= 0 || imgH <= 0 || imgW > 100000.0f || imgH > 100000.0f)
        {
            m_previewZoom = 1.0f;
            m_previewPanX = 0.0f;
            m_previewPanY = 0.0f;
            return;
        }

        // Scale to fit with some padding.
        float scaleX = vpW / imgW;
        float scaleY = vpH / imgH;
        m_previewZoom = (std::min)(scaleX, scaleY) * 0.95f;

        // Center the image.
        m_previewPanX = (vpW - imgW * m_previewZoom) * 0.5f - bounds.left * m_previewZoom;
        m_previewPanY = (vpH - imgH * m_previewZoom) * 0.5f - bounds.top * m_previewZoom;
    }

    // -----------------------------------------------------------------------
    // Render loop
    // -----------------------------------------------------------------------

    void MainWindow::OnRenderTick(
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& /*sender*/,
        winrt::Windows::Foundation::IInspectable const& /*args*/)
    {
        if (m_isShuttingDown) return;
        RenderFrame();
        RenderNodeGraph();

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

        // Re-prepare dirty source nodes (e.g., Flood color changed in properties panel).
        for (auto& node : const_cast<std::vector<::ShaderLab::Graph::EffectNode>&>(m_graph.Nodes()))
        {
            if (node.type == ::ShaderLab::Graph::NodeType::Source && node.dirty)
                m_sourceFactory.PrepareSourceNode(node, dc);
        }

        // Evaluate the effect graph.
        m_graphEvaluator.Evaluate(m_graph, dc);

        // Deferred fit: after first evaluation with valid output, fit the preview.
        if (m_needsFitPreview && GetPreviewImage())
        {
            m_needsFitPreview = false;
            FitPreviewToView();
        }

        // Begin draw to swap chain.
        auto* drawDc = m_renderEngine.BeginDraw();
        if (!drawDc)
            return;

        // Set DPI to 96 so D2D coordinates match WinUI DIPs exactly.
        // The XAML compositor handles physical pixel scaling.
        // This ensures the preview transform, crosshair overlay, and pixel
        // trace coordinates all use the same coordinate space.
        float oldDpiX, oldDpiY;
        drawDc->GetDpi(&oldDpiX, &oldDpiY);
        drawDc->SetDpi(96.0f, 96.0f);

        drawDc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        // Apply preview pan/zoom transform.
        D2D1_MATRIX_3X2_F previewTransform =
            D2D1::Matrix3x2F::Scale(m_previewZoom, m_previewZoom) *
            D2D1::Matrix3x2F::Translation(m_previewPanX, m_previewPanY);
        drawDc->SetTransform(previewTransform);

        if (m_compareActive && m_compareNodeId != 0)
        {
            // Split comparison mode: A on left, B on right.
            auto* imageA = ResolveDisplayImage(m_previewNodeId);
            auto* imageB = ResolveDisplayImage(m_compareNodeId);

            // Split line in viewport coordinates (pre-transform).
            drawDc->SetTransform(D2D1::Matrix3x2F::Identity());
            auto vp = PreviewViewportDips();
            float vpW = vp.width;
            float vpH = vp.height;
            float splitX = vpW * m_splitPosition;

            if (imageA)
            {
                D2D1_RECT_F clipA = { 0, 0, splitX, vpH };
                drawDc->PushAxisAlignedClip(clipA, D2D1_ANTIALIAS_MODE_ALIASED);
                drawDc->SetTransform(previewTransform);
                drawDc->DrawImage(imageA);
                drawDc->SetTransform(D2D1::Matrix3x2F::Identity());
                drawDc->PopAxisAlignedClip();
            }

            if (imageB)
            {
                D2D1_RECT_F clipB = { splitX, 0, vpW, vpH };
                drawDc->PushAxisAlignedClip(clipB, D2D1_ANTIALIAS_MODE_ALIASED);
                drawDc->SetTransform(previewTransform);
                drawDc->DrawImage(imageB);
                drawDc->SetTransform(D2D1::Matrix3x2F::Identity());
                drawDc->PopAxisAlignedClip();
            }

            // Draw split line (no transform — viewport coords).
            winrt::com_ptr<ID2D1SolidColorBrush> lineBrush;
            drawDc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 0.8f), lineBrush.put());
            if (lineBrush)
            {
                drawDc->DrawLine(
                    D2D1::Point2F(splitX, 0),
                    D2D1::Point2F(splitX, vpH),
                    lineBrush.get(), 2.0f);
            }
        }
        else
        {
            // Normal single-image mode.
            auto* previewImage = ResolveDisplayImage(m_previewNodeId);
            if (previewImage)
                drawDc->DrawImage(previewImage);
        }

        drawDc->SetTransform(D2D1::Matrix3x2F::Identity());
        drawDc->SetDpi(oldDpiX, oldDpiY);

        m_renderEngine.EndDraw();
        m_renderEngine.Present();

        // Refresh pixel trace after graph evaluation (before next frame).
        if (m_traceActive)
        {
            PopulatePixelTraceTree();
            RenderTraceSwatches();
        }
        // Update crosshair position each frame (tracks with pan/zoom).
        UpdateCrosshairOverlay();
    }
}
