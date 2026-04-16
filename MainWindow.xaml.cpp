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
        ShaderEditorBox().KeyDown({ this, &MainWindow::OnShaderEditorKeyDown });
        PreviewNodeSelector().SelectionChanged({ this, &MainWindow::OnPreviewNodeSelectionChanged });
        FalseColorSelector().SelectedIndex(0);
        FalseColorSelector().SelectionChanged({ this, &MainWindow::OnFalseColorSelectionChanged });

        PopulateDisplayProfileSelector();
        DisplayProfileSelector().SelectionChanged({ this, &MainWindow::OnDisplayProfileSelectionChanged });

        PreviewPanel().PointerPressed({ this, &MainWindow::OnPreviewPointerPressed });
        PreviewPanel().PointerReleased({ this, &MainWindow::OnPreviewPointerReleased });
        TraceUnitSelector().SelectedIndex(0);
        TraceUnitSelector().SelectionChanged({ this, &MainWindow::OnTraceUnitSelectionChanged });

        SaveGraphButton().Click({ this, &MainWindow::OnSaveGraphClicked });
        LoadGraphButton().Click({ this, &MainWindow::OnLoadGraphClicked });
        AddImageSourceButton().Click({ this, &MainWindow::OnAddImageSourceClicked });
        AddFloodSourceButton().Click({ this, &MainWindow::OnAddFloodSourceClicked });

        // Populate the Add Node flyout with effects from the registry.
        PopulateAddNodeFlyout();

        CompareToggle().Click({ this, &MainWindow::OnCompareToggled });
        CompareNodeSelector().SelectionChanged({ this, &MainWindow::OnCompareNodeSelectionChanged });

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
                args.Handled(true);
            }
        });
        NodeGraphContainer().IsTabStop(true);

        SaveImageButton().Click({ this, &MainWindow::OnSaveImageClicked });
    }

    MainWindow::~MainWindow()
    {
        if (m_renderTimer)
            m_renderTimer.Stop();

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
        m_renderEngine.Initialize(m_hwnd, PreviewPanel(), format);

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

        ::ShaderLab::Effects::CustomPixelShaderEffect::RegisterEffect(factory1.get());
        ::ShaderLab::Effects::CustomComputeShaderEffect::RegisterEffect(factory1.get());

        m_customEffectsRegistered = true;
    }

    void MainWindow::OnPreviewPanelLoaded()
    {
        if (!m_hwnd || m_renderEngine.IsInitialized()) return;

        InitializeRendering();
        RegisterCustomEffects();
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

        auto defaultShader = ::ShaderLab::Controls::ShaderEditorController::DefaultPixelShaderTemplate();
        ShaderEditorBox().Text(winrt::to_hstring(defaultShader));

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
        }
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
        auto scale = PreviewPanel().CompositionScaleX();
        float px = static_cast<float>(point.Position().X) * scale;

        auto bounds = GetPreviewImageBounds();
        float imgW = bounds.right - bounds.left;
        if (imgW > 0.0f)
            m_splitPosition = std::clamp((px - bounds.left) / imgW, 0.0f, 1.0f);

        args.Handled(true);
    }

    void MainWindow::OnPreviewPointerReleased(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        if (m_isDraggingSplit)
        {
            m_isDraggingSplit = false;
            PreviewPanel().ReleasePointerCapture(args.Pointer());
        }
    }

    // -----------------------------------------------------------------------
    // Pixel trace
    // -----------------------------------------------------------------------

    D2D1_RECT_F MainWindow::GetPreviewImageBounds()
    {
        auto* previewImage = GetPreviewImage();
        if (!previewImage) return {};

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return {};

        D2D1_RECT_F bounds{};
        HRESULT hr = dc->GetImageLocalBounds(previewImage, &bounds);
        if (FAILED(hr)) return {};
        return bounds;
    }

    bool MainWindow::PointerToImageCoords(
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args,
        float& outNormX, float& outNormY)
    {
        auto point = args.GetCurrentPoint(PreviewPanel());
        auto scale = PreviewPanel().CompositionScaleX();
        float px = static_cast<float>(point.Position().X) * scale;
        float py = static_cast<float>(point.Position().Y) * scale;

        auto bounds = GetPreviewImageBounds();
        float imgW = bounds.right - bounds.left;
        float imgH = bounds.bottom - bounds.top;
        if (imgW <= 0.0f || imgH <= 0.0f) return false;

        outNormX = std::clamp((px - bounds.left) / imgW, 0.0f, 1.0f);
        outNormY = std::clamp((py - bounds.top) / imgH, 0.0f, 1.0f);
        return true;
    }

    void MainWindow::OnPreviewPointerPressed(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        float normX = 0, normY = 0;
        if (!PointerToImageCoords(args, normX, normY))
            return;

        // Check if clicking near the split line (within 2% of image width).
        if (m_compareActive && std::abs(normX - m_splitPosition) < 0.02f)
        {
            m_isDraggingSplit = true;
            PreviewPanel().CapturePointer(args.Pointer());
            args.Handled(true);
            return;
        }

        m_pixelTrace.SetTrackPosition(normX, normY);
        m_traceActive = true;
        m_lastTraceTopologyHash = 0;  // force full rebuild

        auto bounds = GetPreviewImageBounds();
        uint32_t imgW = static_cast<uint32_t>(bounds.right - bounds.left);
        uint32_t imgH = static_cast<uint32_t>(bounds.bottom - bounds.top);
        uint32_t px = static_cast<uint32_t>(normX * imgW);
        uint32_t py = static_cast<uint32_t>(normY * imgH);

        TracePositionText().Text(std::format(L"Position: ({}, {})", px, py));
        PopulatePixelTraceTree();
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

        auto valuesText = L"\n" + FormatPixelValues(traceNode.pixel, m_traceUnit);
        label.Text(labelText + valuesText);
        label.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Cascadia Mono, Consolas, Courier New"));
        label.FontSize(12);
        label.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::NoWrap);
        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(label, 0);
        row.Children().Append(label);

        // Luminance column (always visible).
        auto lumText = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
        lumText.Text(std::format(L"{:.1f} cd/m\u00B2", traceNode.pixel.luminanceNits));
        lumText.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Cascadia Mono, Consolas, Courier New"));
        lumText.FontSize(12);
        lumText.Margin({ 12, 0, 0, 0 });
        lumText.VerticalAlignment(winrt::Microsoft::UI::Xaml::VerticalAlignment::Top);
        winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(lumText, 1);
        row.Children().Append(lumText);

        return row;
    }

    void MainWindow::PopulatePixelTraceTree()
    {
        if (!m_traceActive) return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        auto bounds = GetPreviewImageBounds();
        uint32_t imgW = static_cast<uint32_t>(bounds.right - bounds.left);
        uint32_t imgH = static_cast<uint32_t>(bounds.bottom - bounds.top);
        if (imgW == 0 || imgH == 0) return;

        if (!m_pixelTrace.ReTrace(dc, m_graph, m_previewNodeId, imgW, imgH))
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
            label.Text(labelText + L"\n" + FormatPixelValues(traceNode.pixel, m_traceUnit));

            // Update luminance (child 1).
            auto lumText = row.Children().GetAt(1).as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>();
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

    void MainWindow::RenderNodeGraph()
    {
        if (!m_graphSwapChain || !m_graphRenderTarget)
            return;

        auto* dc = m_renderEngine.D2DDeviceContext();
        if (!dc) return;

        winrt::com_ptr<ID2D1Image> oldTarget;
        dc->GetTarget(oldTarget.put());

        dc->SetTarget(m_graphRenderTarget.get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0x1e1e1e));

        D2D1_SIZE_F viewSize = { static_cast<float>(m_graphPanelWidth),
                                 static_cast<float>(m_graphPanelHeight) };
        m_nodeGraphController.Render(dc, viewSize);

        dc->EndDraw();
        dc->SetTarget(oldTarget.get());
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
        if (m_nodeGraphController.HitTestPin(canvasPoint, pinNodeId, pinIndex, isOutput))
        {
            m_nodeGraphController.BeginConnection(pinNodeId, pinIndex, isOutput);
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

            // Preview follows selection.
            m_previewNodeId = hitNodeId;
            m_suppressSelectorEvent = true;
            for (uint32_t i = 0; i < m_topoOrder.size(); ++i)
            {
                if (m_topoOrder[i] == hitNodeId)
                {
                    PreviewNodeSelector().SelectedIndex(static_cast<int32_t>(i));
                    break;
                }
            }
            m_suppressSelectorEvent = false;

            const auto* node = m_graph.FindNode(hitNodeId);
            bool isOutputNode = node && node->type == ::ShaderLab::Graph::NodeType::Output;
            if (node && !isOutputNode)
            {
                PreviewOverlayText().Text(L"Previewing: " + winrt::hstring(node->name));
                PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            }
            else
            {
                PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            }
        }
        else
        {
            // Clicked empty space — deselect.
            m_nodeGraphController.DeselectAll();
            m_selectedNodeId = 0;
            UpdatePropertiesPanel();
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
        auto panel = PropertiesPanel();
        panel.Children().Clear();

        if (m_selectedNodeId == 0)
        {
            auto placeholder = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
            placeholder.Text(L"Select a node to view its properties.");
            placeholder.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                winrt::Microsoft::UI::Colors::Gray()));
            panel.Children().Append(placeholder);
            return;
        }

        auto* node = m_graph.FindNode(m_selectedNodeId);
        if (!node) return;

        // Editable node name.
        auto nameLabel = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
        nameLabel.Text(L"Name");
        nameLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        panel.Children().Append(nameLabel);

        auto nameBox = winrt::Microsoft::UI::Xaml::Controls::TextBox();
        nameBox.Text(winrt::hstring(node->name));
        nameBox.Margin({ 0, 2, 0, 8 });
        uint32_t capturedId = m_selectedNodeId;
        nameBox.LostFocus([this, capturedId](auto&&, auto&&)
        {
            // Find the TextBox that lost focus by looking up the node.
            auto* n = m_graph.FindNode(capturedId);
            if (!n) return;
            // The name was already updated in TextChanged, just rebuild selectors.
            PopulatePreviewNodeSelector();
            m_nodeGraphController.RebuildLayout();
        });
        nameBox.TextChanged([this, capturedId](auto&&, auto&&)
        {
            auto* n = m_graph.FindNode(capturedId);
            if (!n) return;
            // Read the current text from the sender — find our TextBox in the panel.
            auto panel2 = PropertiesPanel();
            if (panel2.Children().Size() > 1)
            {
                auto tb = panel2.Children().GetAt(1).try_as<winrt::Microsoft::UI::Xaml::Controls::TextBox>();
                if (tb)
                    n->name = std::wstring(tb.Text().c_str());
            }
        });
        panel.Children().Append(nameBox);

        // Node type (read-only).
        auto typeText = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
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
        typeText.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
            winrt::Microsoft::UI::Colors::Gray()));
        typeText.Margin({ 0, 0, 0, 8 });
        panel.Children().Append(typeText);

        // Editable properties.
        if (!node->properties.empty())
        {
            auto propsHeader = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
            propsHeader.Text(L"Properties");
            propsHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            propsHeader.Margin({ 0, 4, 0, 4 });
            panel.Children().Append(propsHeader);

            for (const auto& [key, value] : node->properties)
            {
                auto propLabel = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
                propLabel.Text(winrt::hstring(key));
                propLabel.FontSize(12);
                propLabel.Margin({ 0, 4, 0, 2 });
                panel.Children().Append(propLabel);

                std::wstring valStr = std::visit([](const auto& v) -> std::wstring
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, float>)
                        return std::format(L"{:.4f}", v);
                    else if constexpr (std::is_same_v<T, int32_t>)
                        return std::to_wstring(v);
                    else if constexpr (std::is_same_v<T, uint32_t>)
                        return std::to_wstring(v);
                    else if constexpr (std::is_same_v<T, bool>)
                        return v ? L"true" : L"false";
                    else if constexpr (std::is_same_v<T, std::wstring>)
                        return v;
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                        return std::format(L"{:.3f}, {:.3f}", v.x, v.y);
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                        return std::format(L"{:.3f}, {:.3f}, {:.3f}", v.x, v.y, v.z);
                    else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                        return std::format(L"{:.3f}, {:.3f}, {:.3f}, {:.3f}", v.x, v.y, v.z, v.w);
                    else
                        return L"<unknown>";
                }, value);

                auto propBox = winrt::Microsoft::UI::Xaml::Controls::TextBox();
                propBox.Text(winrt::hstring(valStr));
                propBox.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(
                    L"Cascadia Mono, Consolas, Courier New"));
                propBox.FontSize(12);
                propBox.Margin({ 0, 0, 0, 4 });

                auto capturedKey = key;
                propBox.LostFocus([this, capturedId, capturedKey](auto&& sender, auto&&)
                {
                    auto* n = m_graph.FindNode(capturedId);
                    if (!n) return;
                    auto it = n->properties.find(capturedKey);
                    if (it == n->properties.end()) return;

                    auto tb = sender.as<winrt::Microsoft::UI::Xaml::Controls::TextBox>();
                    auto text = std::wstring(tb.Text().c_str());

                    try
                    {
                        std::visit([&text](auto& v)
                        {
                            using T = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<T, float>)
                                v = std::stof(text);
                            else if constexpr (std::is_same_v<T, int32_t>)
                                v = std::stoi(text);
                            else if constexpr (std::is_same_v<T, uint32_t>)
                                v = static_cast<uint32_t>(std::stoul(text));
                            else if constexpr (std::is_same_v<T, bool>)
                                v = (text == L"true" || text == L"1");
                            else if constexpr (std::is_same_v<T, std::wstring>)
                                v = text;
                            else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                            {
                                float a, b, c, d;
                                if (swscanf_s(text.c_str(), L"%f, %f, %f, %f", &a, &b, &c, &d) == 4)
                                    v = { a, b, c, d };
                            }
                            else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                            {
                                float a, b, c;
                                if (swscanf_s(text.c_str(), L"%f, %f, %f", &a, &b, &c) == 3)
                                    v = { a, b, c };
                            }
                            else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                            {
                                float a, b;
                                if (swscanf_s(text.c_str(), L"%f, %f", &a, &b) == 2)
                                    v = { a, b };
                            }
                        }, it->second);

                        n->dirty = true;
                        m_graph.MarkAllDirty();
                    }
                    catch (...) { /* invalid input, ignore */ }
                });

                panel.Children().Append(propBox);
            }
        }

        // Input pins.
        if (!node->inputPins.empty())
        {
            auto pinsHeader = winrt::Microsoft::UI::Xaml::Controls::TextBlock();
            pinsHeader.Text(L"Inputs: " + std::to_wstring(node->inputPins.size()));
            pinsHeader.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(
                winrt::Microsoft::UI::Colors::Gray()));
            pinsHeader.Margin({ 0, 8, 0, 0 });
            panel.Children().Append(pinsHeader);
        }
    }

    void MainWindow::OnSaveImageClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        SaveImageAsync();
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
    // Render loop
    // -----------------------------------------------------------------------

    void MainWindow::OnRenderTick(
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& /*sender*/,
        winrt::Windows::Foundation::IInspectable const& /*args*/)
    {
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

        // Evaluate the effect graph.
        m_graphEvaluator.Evaluate(m_graph, dc);

        // Begin draw to swap chain.
        auto* drawDc = m_renderEngine.BeginDraw();
        if (!drawDc)
            return;

        drawDc->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        if (m_compareActive && m_compareNodeId != 0)
        {
            // Split comparison mode: A on left, B on right.
            auto* imageA = ResolveDisplayImage(m_previewNodeId);
            auto* imageB = ResolveDisplayImage(m_compareNodeId);

            auto bounds = GetPreviewImageBounds();
            float imgW = bounds.right - bounds.left;
            float splitX = bounds.left + imgW * m_splitPosition;

            if (imageA)
            {
                D2D1_RECT_F clipA = { bounds.left, bounds.top, splitX, bounds.bottom };
                drawDc->PushAxisAlignedClip(clipA, D2D1_ANTIALIAS_MODE_ALIASED);
                drawDc->DrawImage(imageA);
                drawDc->PopAxisAlignedClip();
            }

            if (imageB)
            {
                D2D1_RECT_F clipB = { splitX, bounds.top, bounds.right, bounds.bottom };
                drawDc->PushAxisAlignedClip(clipB, D2D1_ANTIALIAS_MODE_ALIASED);
                drawDc->DrawImage(imageB);
                drawDc->PopAxisAlignedClip();
            }

            // Draw split line.
            winrt::com_ptr<ID2D1SolidColorBrush> lineBrush;
            drawDc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 0.8f), lineBrush.put());
            if (lineBrush)
            {
                drawDc->DrawLine(
                    D2D1::Point2F(splitX, bounds.top),
                    D2D1::Point2F(splitX, bounds.bottom),
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

        m_renderEngine.EndDraw();
        m_renderEngine.Present();

        // Refresh pixel trace after graph evaluation (before next frame).
        if (m_traceActive)
        {
            PopulatePixelTraceTree();
        }
    }
}
