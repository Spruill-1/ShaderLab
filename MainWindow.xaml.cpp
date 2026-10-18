#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "Rendering/DeviceStack.h"
#include "Rendering/FramePacer.h"
#include "Rendering/PipelineFormat.h"
#include "Rendering/IccProfileParser.h"
#include "Support/EffectGraphFile.h"
#include "Rendering/PixelReadback.h"
#include "Rendering/CaptureNode.h"
#include "Rendering/ImageCapture.h"
#include "Graph/ClockModel.h"
#include "Graph/SourceKind.h"
#include "Graph/GraphEdits.h"
#include "Graph/ValueFormat.h"
#include "Effects/EffectUpgrade.h"
#include "Engine/Utf8.h"
#include "Effects/ShaderLabEffects.h"
#include "Sources/CaptureSources.h"
#include "Sources/DxgiDuplicationSourceProvider.h"
#include "Effects/Performance.h"
#include "Rendering/HostRenderSettings.h"
#include "Engine/EngineBootstrap.h"
#include "Version.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <shlobj.h>
#include <KnownFolders.h>
#include <DispatcherQueue.h>

#pragma comment(lib, "CoreMessaging.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::ShaderLab::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        Title(std::wstring(L"ShaderLab v") + ::ShaderLab::VersionString + L" \u2014 HDR Shader Effect Development");
        // After Title() we'll refresh from RefreshTitleBar() once a graph
        // is loaded; the initial state is "Untitled".

        m_hwnd = GetWindowHandle();

        // Close all output windows and exit the process when the main window closes.
        this->Closed([this](auto&&, auto&&)
        {
            m_isShuttingDown = true;
            m_outputWindows.clear();
            // Best-effort: remove temp directories we extracted media
            // archives into so the user's %TEMP% doesn't accumulate
            // stale .effectgraph payloads.
            std::error_code ec;
            for (const auto& d : m_extractedMediaDirs)
                std::filesystem::remove_all(d, ec);
            // Join the shader-compile workers now: the window may never be
            // destroyed before the process exits. The render worker may still
            // be running: GetOrCompile keeps compiling inline, but async
            // RequestCompile calls are dropped, so a variant not yet built
            // stays missing until exit.
            ::ShaderLab::Engine::ShutdownEngineServices();
            // WinUI 3 keeps the process alive while any Window exists.
            // Force exit so output windows don't keep us running.
            ::PostQuitMessage(0);
        });

        // Cancellable close: prompt the user when there are unsaved
        // changes. AppWindow.Closing is the WinUI 3 hook that can take
        // a deferral (so we can show an async dialog and decide whether
        // to allow the close after the user picks an option).
        this->AppWindow().Closing(
            [this](auto&&, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
            {
                if (m_isShuttingDown || !m_unsavedChanges) return;
                args.Cancel(true);

                [](MainWindow* self) -> winrt::fire_and_forget
                {
                    auto strong = self->get_strong();
                    int32_t choice = co_await self->PromptUnsavedChangesAsync();
                    if (choice == 2) co_return; // Cancel -> stay open
                    if (choice == 0)            // Save then close
                    {
                        if (!self->m_currentFilePath.empty())
                            co_await self->RunSaveWithProgressAsync();
                        else
                            co_await self->SaveGraphAsAsync();
                        // If the user backed out of the picker, don't close.
                        if (self->m_unsavedChanges) co_return;
                    }
                    self->m_unsavedChanges = false;
                    self->Close();
                }(this);
            });

        // Wire up event handlers (safe before panel is loaded).
        PreviewPanel().SizeChanged({ this, &MainWindow::OnPreviewSizeChanged });
        // Display scale changes (moving to a monitor with different scaling)
        // change the pixel size without changing the DIP size, so SizeChanged
        // does not fire: resize the buffer and refresh the inverse matrix here.
        PreviewPanel().CompositionScaleChanged([this](auto&&, auto&&)
        {
            if (!m_renderEngine.IsInitialized()) return;
            auto panel = PreviewPanel();
            const float sx = panel.CompositionScaleX(), sy = panel.CompositionScaleY();
            m_renderEngine.SetCompositionScale(sx, sy);
            m_renderEngine.Resize(
                static_cast<uint32_t>((std::max)(1.0, panel.ActualWidth() * sx)),
                static_cast<uint32_t>((std::max)(1.0, panel.ActualHeight() * sy)));
            m_forceRender = true;
        });
        PreviewPanel().KeyDown({ this, &MainWindow::OnPreviewKeyDown });
        PreviewPanel().IsTabStop(true);

        PopulateDisplayProfileSelector();
        DisplayProfileSelector().SelectionChanged({ this, &MainWindow::OnDisplayProfileSelectionChanged });

        {
            ::ShaderLab::Controls::PreviewHost host;
            host.imageBounds = [this] { return GetPreviewImageBounds(); };
            host.backBufferSize = [this]
            {
                return D2D1_SIZE_F{ static_cast<float>(m_renderEngine.BackBufferWidth()),
                                    static_cast<float>(m_renderEngine.BackBufferHeight()) };
            };
            host.requestRender = [this] { m_forceRender = true; };
            host.clicked = [this](const ::ShaderLab::Controls::PreviewClick& click) { TracePixelAt(click); };
            m_preview.Attach(PreviewPanel(), std::move(host));
        }
        m_tracePanel.Attach(PixelTracePanel(), TracePositionText(), TraceUnitSelector(), CrosshairCanvas());

        SaveGraphButton().Click({ this, &MainWindow::OnSaveGraphClicked });
        LoadGraphButton().Click({ this, &MainWindow::OnLoadGraphClicked });
        AutoArrangeButton().Click([this](auto&&, auto&&)
        {
            // Reset viewport so the laid-out nodes are visible even if the
            // user had zoomed/panned the canvas off-screen.
            m_nodeGraphController.SetZoom(1.0f);
            m_nodeGraphController.SetPanOffset(0.0f, 0.0f);
            m_nodeGraphController.RequestAutoLayout();
            m_forceRender = true;
        });
        UpdateAllEffectsButton().Click([this](auto&&, auto&&)
        {
            // Graph writes and layout belong to the render thread.
            m_renderDispatcher.DispatchSync([this]
            {
                for (const uint32_t id : ::ShaderLab::Effects::UpgradeOutdatedNodes(Model()))
                    Evaluator().InvalidateNode(id);
                m_nodeGraphController.RebuildLayout();
            });
            m_forceRender = true;
            PopulatePreviewNodeSelector();
            m_propertiesPanel.Rebuild();
            UpdateOutdatedEffectsButton();
        });
        // Register user effects before the flyout is built and before the render worker exists.
        {
            auto& library = ::ShaderLab::Effects::ShaderLabEffects::Instance();
            const auto& report = library.LoadUserEffects(
                ::ShaderLab::Effects::ShaderLabEffects::DefaultUserEffectsDirectory());
            for (const auto& name : report.loaded)
                OutputDebugStringW((L"[UserEffects] loaded " + name + L"\n").c_str());
            for (const auto& err : report.errors)
                OutputDebugStringW((L"[UserEffects] " + err + L"\n").c_str());
        }
        // Populate the Add Node flyout with effects from the registry.
        PopulateAddNodeFlyout();

        BottomTabView().SelectionChanged([this](auto&&, auto&&)
        {
            if (m_isShuttingDown) return;
            m_tracePanel.UpdateCrosshair(Preview().View(), BottomTabView().SelectedIndex() == 1);
        });

        // Defer GPU initialization until the SwapChainPanel is in the visual tree.
        PreviewPanel().Loaded([this](auto&&, auto&&) { OnPreviewPanelLoaded(); });
        NodeGraphPanel().SizeChanged([this](auto&&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& e)
        {
            ResizeGraphPanel(
                static_cast<float>(e.NewSize().Width),
                static_cast<float>(e.NewSize().Height));
        });
        NodeGraphPanel().CompositionScaleChanged([this](auto&&, auto&&)
        {
            UpdateGraphPanelScale();
            ResizeGraphPanel(
                static_cast<float>(NodeGraphPanel().ActualWidth()),
                static_cast<float>(NodeGraphPanel().ActualHeight()));
        });
        AttachCanvasInput();

        // Save flyout: the reference frame of the saved file is an
        // explicit choice, not an inference — a heuristic would darken
        // the plain image→PNG round trip or wash presentation-graded
        // output (see SaveReference in the header).
        {
            winrt::Microsoft::UI::Xaml::Controls::MenuFlyout saveFlyout;
            auto addSaveItem = [this, &saveFlyout](
                winrt::hstring const& text, SaveReference ref)
            {
                winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem item;
                item.Text(text);
                item.Click([this, ref](auto&&, auto&&) { SaveImageAsync(ref); });
                saveFlyout.Items().Append(item);
            };
            addSaveItem(L"PNG (SDR) — from presentation white", SaveReference::PresentationPng);
            addSaveItem(L"PNG (SDR) — file-referenced as-is", SaveReference::FilePng);
            addSaveItem(L"JPEG XR (HDR) — scene-referred", SaveReference::HdrJxr);
            SaveImageButton().Flyout(saveFlyout);
        }
        EffectDesignerButton().Click([this](auto&&, auto&&) { OpenEffectDesigner(); });

        // The Properties panel reads the published snapshot and writes the
        // graph on the render thread, like every other UI edit.
        {
            ::ShaderLab::Controls::PropertiesPanelHost host;
            host.snapshot = [this] { return CurrentGraphSnapshot(); };
            host.selectedNodeId = [this] { return SelectedNodeId(); };
            host.editGraph = [this](const std::function<bool(::ShaderLab::Controls::PanelGraphEdit&)>& edit)
            {
                return m_renderDispatcher.DispatchSync([this, &edit]
                {
                    ::ShaderLab::Controls::PanelGraphEdit context{
                        Model(),
                        [this] { m_nodeGraphController.RebuildLayout(); },
                        [this](uint32_t id) { Evaluator().InvalidateNode(id); },
                        [this](::ShaderLab::Graph::EffectNode& node)
                        {
                            if (auto* dc = m_renderEngine.RenderD2DContext())
                                Sources().PrepareSourceNode(node, dc, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                        } };
                    return edit(context);
                });
            };
            host.requestRender = [this] { m_forceRender = true; };
            host.requestCanvasRedraw = [this] { m_nodeGraphController.SetNeedsRedraw(); };
            host.refreshNodeList = [this] { PopulatePreviewNodeSelector(); };
            host.fitPreview = [this] { Preview().RequestFit(/*keepFitted=*/false); };
            host.openInDesigner = [this](uint32_t nodeId, const ::ShaderLab::Graph::CustomEffectDefinition& definition,
                const std::wstring& name)
            {
                OpenEffectDesigner();
                winrt::get_self<EffectDesignerWindow>(m_designerWindow)->LoadDefinition(nodeId, definition, name);
            };
            host.openLog = [this](uint32_t nodeId) { OpenLogWindow(nodeId); };
            host.logInfo = [this](uint32_t nodeId, const std::wstring& text) { m_nodeLogs[nodeId].Info(text); };
            host.windowHandle = [this] { return m_hwnd; };
            host.isShuttingDown = [this] { return m_isShuttingDown.load(); };
            host.keepAlive = [this]() -> winrt::Windows::Foundation::IInspectable { return *this; };
            m_propertiesPanel.Attach(PropertiesPanel(), std::move(host));
        }

        // MCP server toggle.
        McpServerToggle().Click([this](auto&&, auto&&)
        {
            bool checked = McpServerToggle().IsChecked().GetBoolean();
            // The routes need the context; until the preview panel loads,
            // OnPreviewPanelLoaded starts MCP itself.
            if (checked && !m_context)
            {
                McpServerToggle().IsChecked(false);
                return;
            }
            if (checked)
            {
                if (!m_mcpServer)
                    SetupMcpRoutes();
                // Expose this window to MCP as a hub session; the broker
                // (shim -> hub -> session) is the only transport.
                StartMcpSession();
                UpdateMcpStatusLabel();
                McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                m_mcpIndicator.Reset();
                UpdateMcpActivityIndicator();
            }
            else
            {
                StopMcpSession();
                McpServerLabel().Text(L"MCP: off");
                McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
                m_mcpIndicator.Reset();
                UpdateMcpActivityIndicator();
            }
        });

        McpExportConfigButton().Click([this](auto&&, auto&&)
        {
            namespace DP = winrt::Windows::ApplicationModel::DataTransfer;
            auto jsonEsc = [](const std::wstring& s) {
                std::wstring o;
                for (wchar_t c : s) { if (c == L'\\' || c == L'"') o += L'\\'; o += c; }
                return o;
            };

            std::wstring config, note;
            auto shim = EnsureShimDistributed();
            if (!shim.empty())
            {
                // Preferred: stdio config pointing at the stable unpackaged
                // shim copy. The --hub-aumid lets the shim activate the
                // packaged hub on demand (client-driven bootstrap).
                auto aumid = HubAumid();
                std::wstring argsJson = aumid.empty()
                    ? L"\"--stdio\""
                    : std::format(L"\"--stdio\", \"--hub-aumid\", \"{}\"", jsonEsc(aumid));
                config = std::format(
                    L"{{\n"
                    L"  \"mcpServers\": {{\n"
                    L"    \"shaderlab\": {{\n"
                    L"      \"command\": \"{}\",\n"
                    L"      \"args\": [{}]\n"
                    L"    }}\n"
                    L"  }}\n"
                    L"}}", jsonEsc(shim), argsJson);
                note = L"MCP stdio config copied to clipboard";
            }
            else
            {
                // No broker payload found (unexpected in a real build).
                config.clear();
                note = L"MCP shim not found — reinstall or rebuild ShaderLab.";
            }

            if (!config.empty())
            {
                auto pkg = DP::DataPackage();
                pkg.SetText(config);
                DP::Clipboard::SetContent(pkg);
            }
            PipelineFormatText().Text(note);
        });
    }

    MainWindow::~MainWindow()
    {
        m_isShuttingDown = true;
        m_renderShouldStop.store(true, std::memory_order_release);

        // Stop the MCP session FIRST, while the render worker is still alive,
        // so an in-flight session request drains rather than stranding on a
        // joined worker (the 30 s stall the migration plan warns about).
        // Then (below) the render dispatcher + worker.
        StopMcpSession();

        if (m_renderTimer)
        {
            m_renderTimer.Stop();
            m_renderTimer = nullptr;
        }

        // Two-phase shutdown of the render worker:
        m_renderDispatcher.Shutdown();
        if (m_renderWorker.joinable())
        {
            m_renderWorker.request_stop();
            m_renderWorker.join();
        }

        // Close all output windows.
        m_outputWindows.clear();

        // Release UI-side offscreen wrappers before tearing down render engine.
        for (auto& b : m_offscreenSourceBitmapUi) b = nullptr;

        // The worker is joined, so this thread may release the context.
        m_context = nullptr;
        m_displayMonitor.Shutdown();
        m_renderEngine.Shutdown();
        ::ShaderLab::Engine::ShutdownEngineServices();
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
        // DisplayInformation::GetForWindow requires a running
        // Windows.System.DispatcherQueue on this thread. WinUI 3 threads
        // run Microsoft.UI.Dispatching.DispatcherQueue — a distinct type —
        // so create the system one if absent (same pattern system-backdrop
        // controllers use). It pumps via this thread's existing message
        // loop; the controller must outlive the queue's consumers.
        if (!m_systemDqController &&
            !winrt::Windows::System::DispatcherQueue::GetForCurrentThread())
        {
            DispatcherQueueOptions options{
                sizeof(DispatcherQueueOptions),
                DQTYPE_THREAD_CURRENT,
                DQTAT_COM_NONE };
            ABI::Windows::System::IDispatcherQueueController* controller{ nullptr };
            if (SUCCEEDED(::CreateDispatcherQueueController(options, &controller)))
                m_systemDqController.attach(
                    reinterpret_cast<::IUnknown*>(controller));
        }

        m_displayMonitor.Initialize(m_hwnd);

        // The device, then the context on it (created once, moved on an
        // adapter switch), then the swap chain on the context's D2D device.
        m_renderEngine.CreateDevice(m_devicePref);
        if (!m_context)
        {
            EFFECTGRAPH_CONTEXT_DESC desc{};
            // Compile on the cache's worker threads, not the render thread,
            // so a slow compile cannot stall UI actions that dispatch to it.
            desc.flags = EFFECTGRAPH_CONTEXT_FLAGS_ASYNC_COMPILE;
            const std::wstring cacheRoot = BytecodeCacheRoot();
            desc.shaderCacheDirectory = cacheRoot.c_str();
            winrt::check_hresult(EffectGraphCreateContext(m_renderEngine.D3DDevice(), &desc,
                __uuidof(IEffectGraphContextInternal), m_context.put_void()));
        }
        else
        {
            winrt::check_hresult(m_context->MoveToDevice(m_renderEngine.D3DDevice()));
        }
        m_renderEngine.Initialize(m_hwnd, PreviewPanel(), ::ShaderLab::Rendering::FormatScRgbFP16,
            m_context->Devices().d2dDevice.get(), m_context->DeviceContext());

        // The GUI's evaluator follows the process defaults, which the
        // performance flyout and the /perf MCP routes edit. Skip unneeded
        // readbacks: every frame the RenderTick installs the selected node
        // (and its binding sources) as the CPU-analysis interest set, so its
        // Properties panel stays live while other compute nodes skip the Map
        // when their consumers are served on the GPU. The live loop never
        // needs values the frame they are produced, so it collects them
        // asynchronously instead of draining the GPU pipeline per compute
        // node. MCP /analysis/{id} reads ask for a fresh blocking readback
        // per request (GraphEvaluator::RequestFreshCpuReadback).
        ::ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
        ::ShaderLab::Performance::SetAsyncAnalysisReadbackEnabled(true);
        // Per-node GPU attribution. The evaluator brackets each compute
        // dispatch when this is set AND timing is enabled; no-op otherwise.
        Evaluator().SetGpuTimer(&m_renderEngine.Timer());
        // Option variants report on a status line rather than the compile dialog.
        m_compileProgress.SetVariantStatusText(VariantCompileText());
        // Reflect that default in the flyout, so the checkbox is not
        // showing the opposite of what the engine is doing.
        if (AnalysisRefreshCombo()) AnalysisRefreshCombo().SelectedIndex(0);
        if (GpuTimingCheck())    GpuTimingCheck().IsChecked(false);
        if (ForceRedrawCheck())  ForceRedrawCheck().IsChecked(false);
        if (GpuBindingsCheck())  GpuBindingsCheck().IsChecked(::ShaderLab::Performance::IsGpuBindingsEnabled());
        if (NodeGpuStatsCheck()) NodeGpuStatsCheck().IsChecked(false);
        if (UnthrottledCheck())  UnthrottledCheck().IsChecked(::ShaderLab::Rendering::HostRenderSettings::IsUnthrottledRenderEnabled());

        // Subscribe to display changes (AdvancedColorInfoChanged: HDR
        // toggle, SDR-brightness slider, monitor move, profile sim).
        // NO graph work here: the render worker's per-tick
        // UpdateWorkingSpaceNodes reads ActiveProfile() and dirties the
        // Working Space node when a field really moved — that dirty is
        // the designed propagation to binding consumers, and nothing
        // else in the graph depends on display state ("bind, don't
        // hide"). Displays with adaptive color fire this event at
        // sensor rate (several Hz, sub-nit deltas), so the UI refresh
        // is coalesced behind a pending flag — an event storm results
        // in at most one queued refresh at a time. Dirtying the graph here
        // would turn that storm into continuous full re-evaluation.
        m_displayMonitor.SetCallback([this](const ::ShaderLab::Rendering::DisplayCapabilities& /*newCaps*/)
        {
            if (m_displayUiRefreshPending.exchange(true))
                return;
            this->DispatcherQueue().TryEnqueue([this]()
            {
                m_displayUiRefreshPending = false;
                // Pick up new refresh rate (e.g. user changed displays
                // or switched modes from 60 Hz to 144 Hz).
                UpdateRenderTimerInterval();
                UpdateStatusBar();
            });
        });
    }

    // The on-disk bytecode cache, under %LOCALAPPDATA%; empty when that
    // folder cannot be found, which leaves the cache in memory only.
    std::wstring MainWindow::BytecodeCacheRoot()
    {
        wchar_t* localAppData = nullptr;
        std::wstring root;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData)
            root = std::wstring(localAppData) + L"\\ShaderLab\\bytecode";
        ::CoTaskMemFree(localAppData);
        return root;
    }

    void MainWindow::OnPreviewPanelLoaded()
    {
        if (!m_hwnd || m_renderEngine.IsInitialized()) return;

        InitializeRendering();

        // Register the MCP routes; the session starts now unless auto-start is off.
        SetupMcpRoutes();
        if (m_autoStartMcp && m_mcpServer)
        {
            StartMcpSession();
            McpServerToggle().IsChecked(true);
            DispatcherQueue().TryEnqueue([this]()
            {
                UpdateMcpStatusLabel();
                McpExportConfigButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
                m_mcpIndicator.Reset();
                UpdateMcpActivityIndicator();
            });
        }

        UpdateStatusBar();

        // Refresh title bar now that the effect library is loaded so the
        // title shows both the app and effect-library version.
        RefreshTitleBar();

        // UI-thread reads in the controller go through the per-frame snapshot
        // the render worker publishes, never the live graph. See the THREADING
        // RULE block in NodeGraphController.h. The worker is not running yet,
        // so this thread lays out.
        m_nodeGraphController.Attach(Model(), m_renderDispatcher, [this] { return CurrentGraphSnapshot(); });
        m_nodeGraphController.RebuildLayout();
        m_nodeGraphController.SetConnectionCallback(
            [this](uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin, bool isData) {
                const auto snapshot = CurrentGraphSnapshot();
                auto* srcNode = snapshot ? snapshot->FindNode(srcId) : nullptr;
                auto* dstNode = snapshot ? snapshot->FindNode(dstId) : nullptr;
                std::wstring srcName = srcNode ? srcNode->name : std::format(L"Node {}", srcId);
                std::wstring dstName = dstNode ? dstNode->name : std::format(L"Node {}", dstId);
                if (isData)
                {
                    m_nodeLogs[dstId].Info(std::format(L"Property bound from {} (data pin)", srcName));
                    m_nodeLogs[srcId].Info(std::format(L"Data output bound to {}", dstName));
                }
                else
                {
                    m_nodeLogs[dstId].Info(std::format(L"Input {} connected from {}", dstPin, srcName));
                    m_nodeLogs[srcId].Info(std::format(L"Output {} connected to {}", srcPin, dstName));
                }
                MarkUnsaved();
            });

        // Start render loop. The interval tracks the monitor's refresh
        // rate (clamped to 60..240 Hz), so 120 Hz / 144 Hz / 240 Hz panels
        // and high-FPS video sources actually run at their native cadence.
        m_fpsTimePoint = std::chrono::steady_clock::now();
        m_lastRenderTick = m_fpsTimePoint;
        m_renderTimer = DispatcherQueue().CreateTimer();
        m_renderTimer.Tick({ this, &MainWindow::OnRenderTick });
        UpdateRenderTimerInterval();
        m_renderTimer.Start();

        // Spawn the render-worker thread. Owns
        // the engine D2D context and runs the graph + GPU loop independently
        // of the UI thread, drawing into a double-buffered offscreen target.
        // UI thread blits the latest published buffer into the SwapChainPanel-
        // bound swap chain in OnRenderTick.
        //
        // Publish a snapshot before the worker exists so UI reads never see
        // nullptr once it runs, and republish after every dispatched closure
        // so a UI read that follows a dispatched write sees it.
        PublishGraphSnapshot();
        m_renderDispatcher.SetAfterSyncClosure([this] { PublishGraphSnapshot(); });
        StartRenderWorker();

        // Initialize the node graph editor panel.
        InitializeGraphPanel();

        // Sweep %TEMP% for ShaderLab-* media dirs left behind by a
        // crashed prior instance and offer to delete them. Runs after
        // panel init so the dialog has a XamlRoot to attach to.
        ReapStaleMediaDirsAsync();

        // If the app was launched via file double-click, load that
        // graph now that rendering is wired up.
        if (!m_pendingOpenPath.empty())
        {
            auto path = std::move(m_pendingOpenPath);
            m_pendingOpenPath.clear();
            LoadGraphFromPathAsync(winrt::hstring(path));
        }
        else
        {
            RefreshTitleBar();
        }
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

        // GPU info.
        if (m_renderEngine.IsInitialized())
        {
            if (m_renderEngine.IsWarp())
                GpuInfoText().Text(L"GPU: Software (WARP)");
            else
                GpuInfoText().Text(L"GPU: " + m_renderEngine.AdapterName());
        }
    }

    void MainWindow::UpdateMcpActivityIndicator()
    {
        UpdateMcpStatusLabel();
        m_mcpIndicator.Update(m_sessionClient != nullptr, McpActivityDot(), McpActivityDotBrush(), McpServerToggle());
    }

    uint32_t MainWindow::QueryDisplayRefreshHz() const noexcept
    {
        // Use EnumDisplaySettings on the monitor that contains the app HWND.
        // dmDisplayFrequency is reported in whole Hz; modern panels round to
        // 60/120/144/165/240 etc. Returning 0 here is the "unknown" signal.
        if (!m_hwnd)
            return 0;

        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
        if (!mon)
            return 0;

        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(mon, &mi))
            return 0;

        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!::EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            return 0;

        return dm.dmDisplayFrequency;
    }

    void MainWindow::UpdateRenderTimerInterval()
    {
        // Pick a target Hz from the active monitor, clamped to [60, 240].
        // - 60 Hz floor: avoid laggy interactions on 30 Hz TV-out modes.
        // - 240 Hz ceiling: dispatcher tick scheduling becomes noisy below
        //   ~4 ms, and rendering faster than panel refresh is wasted work.
        // The render worker reads the same value for its FramePacer.
        const uint32_t hz = ::ShaderLab::Rendering::FramePacer::ClampRate(QueryDisplayRefreshHz());

        if (hz == m_targetRefreshHz.load(std::memory_order_relaxed) && m_renderTimer)
            return;

        m_targetRefreshHz.store(hz, std::memory_order_relaxed);

        // Use microseconds so non-integer-ms periods (e.g. 144 Hz ~6.944 ms)
        // don't get rounded to a slower or faster cadence than the panel.
        const auto interval = std::chrono::microseconds(1'000'000 / hz);
        if (m_renderTimer)
            m_renderTimer.Interval(interval);
    }

    void MainWindow::OnGpuInfoTapped(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& /*args*/)
    {
        namespace Controls = winrt::Microsoft::UI::Xaml::Controls;
        using namespace ::ShaderLab::Rendering;

        auto adapters = RenderEngine::EnumerateAdapters();
        if (adapters.empty()) return;

        auto flyout = Controls::MenuFlyout();
        for (size_t i = 0; i < adapters.size(); ++i)
        {
            auto& a = adapters[i];
            auto item = Controls::MenuFlyoutItem();
            std::wstring label = a.name;
            if (a.dedicatedVideoMemoryMB > 0)
                label += std::format(L" ({} MB)", a.dedicatedVideoMemoryMB);
            if (a.isWarp)
                label += L" [Software]";

            // Mark current adapter.
            if (a.name == m_renderEngine.AdapterName() ||
                (a.isWarp && m_renderEngine.IsWarp()))
                label = L"\u2713 " + label;

            item.Text(winrt::hstring(label));
            auto luid = a.luid;
            bool isWarp = a.isWarp;
            item.Click([this, luid, isWarp](auto&&, auto&&) {
                SwitchAdapter(
                    isWarp ? DevicePreference::Warp : DevicePreference::Adapter,
                    luid);
            });
            flyout.Items().Append(item);
        }

        flyout.ShowAt(sender.as<winrt::Microsoft::UI::Xaml::FrameworkElement>());
    }

    void MainWindow::StartRenderWorker()
    {
        m_renderShouldStop.store(false, std::memory_order_release);
        m_renderWorker = std::jthread([this](std::stop_token stop) { RenderWorkerLoop(stop); });
    }

    void MainWindow::StopRenderWorker()
    {
        m_renderShouldStop.store(true, std::memory_order_release);
        m_renderDispatcher.Wake();
        if (m_renderWorker.joinable())
        {
            m_renderWorker.request_stop();
            m_renderWorker.join();
        }
        // No consumer now: this thread owns the graph until StartRenderWorker.
        m_renderDispatcher.ResetConsumer();
    }

    // Drops the window's resources bound to the current device; the context
    // drops its own when it moves. The worker must be stopped.
    void MainWindow::TearDownDeviceResources()
    {
        m_nodeGraphController.ReleaseDeviceResources();
        if (auto panelNative = NodeGraphPanel().try_as<ISwapChainPanelNative>())
            panelNative->SetSwapChain(nullptr);
        m_graphRenderTarget = nullptr;
        m_graphSwapChain = nullptr;
        m_graphGridBrush = nullptr;
        for (auto& b : m_offscreenSourceBitmapUi) b = nullptr;
        m_offscreenWrapperWidth = 0;
        m_offscreenWrapperHeight = 0;
        m_offscreenPublishedIdx.store(-1, std::memory_order_release);
        ReleaseUiD2dContext();
        for (auto& w : m_outputWindows) w->Close();
        m_outputWindows.clear();
        {
            std::scoped_lock lock(m_outputSinksMutex);
            m_outputSinks.clear();
        }
        for (auto& w : m_logWindows) w->Close();
        m_logWindows.clear();
        m_displayMonitor.SetCallback(nullptr);
        m_displayMonitor.Shutdown();
        m_renderEngine.Shutdown();
    }

    // Brings the device stack up on the preferred adapter, or on the default
    // one when that fails. Returns the error when neither comes up.
    std::optional<std::wstring> MainWindow::RebuildDeviceResources(
        ::ShaderLab::Rendering::DevicePreference pref, LUID adapterLuid)
    {
        m_devicePref = pref;
        if (pref == ::ShaderLab::Rendering::DevicePreference::Adapter)
            m_renderEngine.SetPreferredAdapterLuid(adapterLuid);

        auto tryInitialize = [this]() -> std::optional<std::wstring>
        {
            try
            {
                InitializeRendering();
                return std::nullopt;
            }
            catch (const winrt::hresult_error& error)
            {
                return std::wstring(error.message());
            }
            catch (const std::exception& error)
            {
                return ::ShaderLab::Utf8ToWide(error.what());
            }
        };
        if (auto error = tryInitialize())
        {
            OutputDebugStringW((L"[GPU Switch] " + *error + L"; falling back to the default adapter\n").c_str());
            // Whatever the failed attempt created goes before the retry.
            m_displayMonitor.Shutdown();
            m_renderEngine.Shutdown();
            m_devicePref = ::ShaderLab::Rendering::DevicePreference::Default;
            if (auto fallbackError = tryInitialize())
            {
                m_displayMonitor.Shutdown();
                m_renderEngine.Shutdown();
                return fallbackError;
            }
        }
        InitializeGraphPanel();
        return std::nullopt;
    }

    // Re-prepares the sources on the new device, on the render thread with its
    // own context. Videos are reopened afterwards, once the device has settled.
    void MainWindow::ReloadSourcesAfterSwitch()
    {
        const auto videoNodeIds = m_renderDispatcher.DispatchSync([this]
        {
            std::vector<uint32_t> videoIds;
            auto* dc = m_renderEngine.RenderD2DContext();
            if (!dc) return videoIds;
            for (auto& node : Model().MutableNodes())
            {
                if (node.type != ::ShaderLab::Graph::NodeType::Source) continue;
                ::ShaderLab::Graph::MarkNodeChanged(node);
                if (::ShaderLab::Graph::ClassifySource(node) == ::ShaderLab::Graph::SourceKind::Video)
                {
                    videoIds.push_back(node.id);
                    continue;
                }
                Sources().PrepareSourceNode(node, dc, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
            }
            return videoIds;
        });
        for (uint32_t videoNodeId : videoNodeIds)
        {
            DispatcherQueue().TryEnqueue([this, videoNodeId]
            {
                m_renderDispatcher.DispatchSync([this, videoNodeId]
                {
                    auto* videoNode = Model().FindNode(videoNodeId);
                    auto* dc = m_renderEngine.RenderD2DContext();
                    if (!videoNode || !dc) return;
                    Sources().PrepareSourceNode(*videoNode, dc, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                });
            });
        }

        // After the first frames: sources that failed on the new device retry.
        DispatcherQueue().TryEnqueue(winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [this]
        {
            m_renderDispatcher.DispatchSync([this]
            {
                for (auto& node : Model().MutableNodes())
                {
                    if (node.type == ::ShaderLab::Graph::NodeType::Source && !node.runtimeError.empty())
                        ::ShaderLab::Graph::RetryNode(Model(), node.id);
                }
            });
            m_forceRender = true;
        });
    }

    void MainWindow::ReopenOutputWindows()
    {
        const auto snapshot = CurrentGraphSnapshot();
        if (!snapshot) return;
        for (const auto& node : snapshot->nodes)
        {
            if (node.type != ::ShaderLab::Graph::NodeType::Output) continue;
            try
            {
                OpenOutputWindow(node.id);
            }
            catch (const winrt::hresult_error& error)
            {
                // The node stays without a window, as after the user closes one.
                GetNodeLog(node.id).Error(L"Its output window could not be opened: " + std::wstring(error.message()));
            }
            catch (const std::exception& error)
            {
                GetNodeLog(node.id).Error(L"Its output window could not be opened: " + ::ShaderLab::Utf8ToWide(error.what()));
            }
        }
    }

    void MainWindow::SwitchAdapter(::ShaderLab::Rendering::DevicePreference pref, LUID adapterLuid)
    {
        // MCP requests answer 503 until the switch is over
        // (GuiEngineCommandSink::Dispatch).
        m_adapterSwitchInProgress.store(true, std::memory_order_release);
        if (m_renderTimer) m_renderTimer.Stop();
        StopRenderWorker();

        TearDownDeviceResources();
        if (const auto error = RebuildDeviceResources(pref, adapterLuid))
        {
            // No device: the graph is kept in the context, nothing renders,
            // and the UI tick tries again in a few seconds.
            PipelineFormatText().Text(L"Error: no GPU device (" + *error + L"); retrying");
            m_deviceUnavailable.store(true, std::memory_order_release);
            m_nextDeviceRetry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (m_renderTimer) m_renderTimer.Start();
            m_adapterSwitchInProgress.store(false, std::memory_order_release);
            return;
        }
        m_deviceUnavailable.store(false, std::memory_order_release);
        m_deviceLost.store(S_OK, std::memory_order_release);

        // The graph, view and selection are as they were; the context left
        // every node dirty.
        PublishGraphSnapshot();
        m_forceRender = true;
        UpdateStatusBar();

        StartRenderWorker();
        ReloadSourcesAfterSwitch();
        ReopenOutputWindows();
        if (m_renderTimer) m_renderTimer.Start();
        m_adapterSwitchInProgress.store(false, std::memory_order_release);
    }

    void MainWindow::RecoverDevice()
    {
        const HRESULT reason = m_deviceLost.exchange(S_OK, std::memory_order_acq_rel);
        if (FAILED(reason))
            OutputDebugStringW(std::format(L"[GPU] Device removed (0x{:08X}); recovering\n",
                static_cast<uint32_t>(reason)).c_str());
        SwitchAdapter(m_devicePref, m_renderEngine.PreferredAdapterLuid());
        if (!m_deviceUnavailable.load(std::memory_order_acquire))
        {
            PipelineFormatText().Text(FAILED(reason)
                ? winrt::hstring(std::format(L"Recovered from a lost GPU device (0x{:08X})", static_cast<uint32_t>(reason)))
                : winrt::hstring(L"GPU device available again"));
        }
    }

    // -----------------------------------------------------------------------
    // Drag and drop — create source nodes from dropped files
    // -----------------------------------------------------------------------

    void MainWindow::OnNodeGraphDragOver(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::DragEventArgs const& args)
    {
        if (args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
            args.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
    }

    winrt::fire_and_forget MainWindow::OnNodeGraphDrop(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::DragEventArgs const& args)
    {
        auto strong = get_strong();
        if (!args.DataView().Contains(winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
            co_return;

        auto items = co_await args.DataView().GetStorageItemsAsync();

        // The nodes are added and prepared on the render thread: the graph
        // is the worker's, and the bitmaps belong to its D2D context.
        std::vector<std::wstring> paths;
        for (const auto& item : items)
        {
            if (auto file = item.try_as<winrt::Windows::Storage::StorageFile>())
                paths.emplace_back(std::wstring(file.Path()));
        }
        if (paths.empty()) co_return;

        try {
            m_renderDispatcher.DispatchSync([this, &paths] {
                auto* dc = m_renderEngine.RenderD2DContext();
                for (const auto& path : paths)
                {
                    bool isVideo = ::ShaderLab::Effects::SourceNodeFactory::IsVideoFile(path);
                    ::ShaderLab::Graph::EffectNode node = isVideo
                        ? ::ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(path)
                        : ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(path);

                    auto id = Model().AddNode(std::move(node));
                    m_nodeLogs[id].Info(std::format(L"Dropped: {}",
                        std::filesystem::path(path).filename().wstring()));

                    auto* addedNode = Model().FindNode(id);
                    if (addedNode && dc)
                        Sources().PrepareSourceNode(*addedNode, dc, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
                }
                m_nodeGraphController.AutoLayout();
                return 0;
            });
        }
        catch (const winrt::hresult_error& error)
        {
            PipelineFormatText().Text(L"Error: Failed to add the dropped files: " + error.message());
        }
        catch (const std::exception& error)   // DispatchSync timed out or is shutting down
        {
            PipelineFormatText().Text(L"Error: Failed to add the dropped files: " + ::ShaderLab::Utf8ToWide(error.what()));
        }

        PopulatePreviewNodeSelector();
        m_forceRender = true;
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
        if (Preview().AutoFit())
            Preview().RequestFit();
    }

    // -----------------------------------------------------------------------
    // Per-node preview
    // -----------------------------------------------------------------------

    void MainWindow::PopulatePreviewNodeSelector()
    {
        // Cache the topological order (used by graph evaluation and navigation).
        const auto snapshot = CurrentGraphSnapshot();
        if (snapshot)
            m_topoOrder = snapshot->topologicalOrder;
        else
            m_topoOrder.clear();

        UpdateOutdatedEffectsButton();
    }

    void MainWindow::UpdateOutdatedEffectsButton()
    {
        uint32_t outdatedCount = 0;
        if (const auto snapshot = CurrentGraphSnapshot())
            for (const auto& node : snapshot->nodes)
                if (::ShaderLab::Effects::NewerDescriptorFor(node))
                    outdatedCount++;
        if (outdatedCount > 0)
        {
            UpdateAllEffectsText().Text(std::format(L"Update Effects ({})", outdatedCount));
            UpdateAllEffectsButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            UpdateAllEffectsButton().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
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
            // Find current index in topo order.
            int32_t curIdx = -1;
            for (int32_t i = 0; i < static_cast<int32_t>(m_topoOrder.size()); ++i)
            {
                if (m_topoOrder[i] == Preview().NodeId())
                { curIdx = i; break; }
            }

            int32_t count = static_cast<int32_t>(m_topoOrder.size());
            if (key == vkOpenBracket && curIdx > 0)
                SelectPreviewNode(m_topoOrder[curIdx - 1]);
            else if (key == vkCloseBracket && curIdx < count - 1)
                SelectPreviewNode(m_topoOrder[curIdx + 1]);

            UpdatePreviewOverlay();
            m_forceRender = true;
            args.Handled(true);
        }
    }

    void MainWindow::UpdatePreviewOverlay()
    {
        const auto snapshot = CurrentGraphSnapshot();
        const auto* node = snapshot ? snapshot->FindNode(Preview().NodeId()) : nullptr;
        if (node)
        {
            PreviewOverlayText().Text(winrt::hstring(node->name));
            PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
        else
        {
            PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }

    // -----------------------------------------------------------------------
    // Add node
    // -----------------------------------------------------------------------

    void MainWindow::PopulateAddNodeFlyout()
    {
        auto flyout = AddNodeFlyout();
        flyout.Items().Clear();

        namespace MUX = winrt::Microsoft::UI::Xaml::Controls;

        // ---- Built-in D2D effects ----
        auto builtInGroup = MUX::MenuFlyoutSubItem();
        builtInGroup.Text(L"Built-in D2D");

        auto& registry = ::ShaderLab::Effects::EffectRegistry::Instance();
        auto categories = registry.Categories();

        MUX::MenuFlyoutSubItem sourceSubItem{ nullptr };

        for (const auto& cat : categories)
        {
            // The D2D Analysis category is covered by the ShaderLab analysis effects.
            if (cat == L"Analysis") continue;

            auto subItem = MUX::MenuFlyoutSubItem();
            subItem.Text(winrt::hstring(cat));

            auto effects = registry.ByCategory(cat);
            for (const auto* desc : effects)
            {
                auto menuItem = MUX::MenuFlyoutItem();
                menuItem.Text(winrt::hstring(desc->name));

                auto capturedDesc = *desc;
                menuItem.Click([this, capturedDesc](auto&&, auto&&)
                {
                    OnAddEffectNode(capturedDesc);
                });

                subItem.Items().Append(menuItem);
            }

            builtInGroup.Items().Append(subItem);

            if (cat == L"Source")
                sourceSubItem = subItem;
        }

        // Append Image and Flood sources to the Source category.
        if (!sourceSubItem)
        {
            sourceSubItem = MUX::MenuFlyoutSubItem();
            sourceSubItem.Text(L"Source");
            builtInGroup.Items().Append(sourceSubItem);
        }

        auto imageSourceItem = MUX::MenuFlyoutItem();
        imageSourceItem.Text(L"Image Source");
        imageSourceItem.Click([this](auto&&, auto&&)
        {
            auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Image Source");
            auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
            OnNodeAdded(nodeId);
        });
        sourceSubItem.Items().Append(imageSourceItem);

        auto floodSourceItem = MUX::MenuFlyoutItem();
        floodSourceItem.Text(L"Flood Fill (Solid Color)");
        floodSourceItem.Click([this](auto&&, auto&&)
        {
            OnAddFloodSourceClicked(nullptr, nullptr);
        });
        sourceSubItem.Items().Append(floodSourceItem);

        flyout.Items().Append(builtInGroup);

        // ---- ShaderLab effects ----
        auto& slRegistry = ::ShaderLab::Effects::ShaderLabEffects::Instance();
        auto slCategories = slRegistry.Categories();

        if (!slCategories.empty())
        {
            auto slGroup = MUX::MenuFlyoutSubItem();
            slGroup.Text(L"ShaderLab");

            for (const auto& cat : slCategories)
            {
                auto subItem = MUX::MenuFlyoutSubItem();
                subItem.Text(winrt::hstring(cat));

                auto effects = slRegistry.ByCategory(cat);

                // Group effects by their optional `subcategory` field. The
                // analysis category in particular has 16+ entries; folding
                // them into Highlights / Scopes / Statistics / Gamut Mapping
                // / Comparison sub-trees makes the flyout navigable. Effects
                // without a subcategory remain at the top level.
                std::map<std::wstring, std::vector<const ::ShaderLab::Effects::ShaderLabEffectDescriptor*>> grouped;
                std::vector<const ::ShaderLab::Effects::ShaderLabEffectDescriptor*> ungrouped;
                for (const auto* desc : effects)
                {
                    if (desc->subcategory.empty()) ungrouped.push_back(desc);
                    else grouped[desc->subcategory].push_back(desc);
                }

                auto addEffectItem = [this](MUX::MenuFlyoutSubItem& parent,
                                            const ::ShaderLab::Effects::ShaderLabEffectDescriptor* desc)
                {
                    auto menuItem = MUX::MenuFlyoutItem();
                    menuItem.Text(winrt::hstring(desc->name));

                    auto capturedName = desc->name;
                    menuItem.Click([this, capturedName](auto&&, auto&&)
                    {
                        auto* slDesc = ::ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(capturedName);
                        if (!slDesc) return;
                        auto node = ::ShaderLab::Effects::ShaderLabEffects::CreateNode(*slDesc);
                        m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
                        m_nodeGraphController.RequestRelayout();
                        PopulatePreviewNodeSelector();
                        // Force a render tick that triggers the post-eval
                        // RebuildLayout, which is what reveals on-node UI
                        // (Clock play/pause + slider) for newly-added
                        // parameter nodes. Without this the user has to
                        // click the node before the controls appear.
                        m_forceRender = true;
                    });
                    parent.Items().Append(menuItem);
                };

                // Sub-trees first (sorted by name for stable ordering).
                for (auto& [subName, subList] : grouped)
                {
                    auto subSub = MUX::MenuFlyoutSubItem();
                    subSub.Text(winrt::hstring(subName));
                    for (const auto* desc : subList)
                        addEffectItem(subSub, desc);
                    subItem.Items().Append(subSub);
                }
                // Then ungrouped effects directly under the category.
                for (const auto* desc : ungrouped)
                    addEffectItem(subItem, desc);

                slGroup.Items().Append(subItem);

                // Append Video Source to the Source subcategory.
                if (cat == L"Source")
                {
                    auto videoSourceItem = MUX::MenuFlyoutItem();
                    videoSourceItem.Text(L"Video Source");
                    videoSourceItem.Click([this](auto&&, auto&&)
                    {
                        BrowseVideoForSourceNode();
                    });
                    subItem.Items().Append(videoSourceItem);

                    auto dxgiCaptureGroup = MUX::MenuFlyoutSubItem();
                    dxgiCaptureGroup.Text(L"DXGI DuplicateOutput");
                    auto outputs = ::ShaderLab::Sources::DxgiDuplicationSourceProvider::EnumerateOutputs();
                    for (const auto& output : outputs)
                    {
                        auto item = MUX::MenuFlyoutItem();
                        std::wstring text = output.deviceName.empty()
                            ? std::format(L"Adapter {} Output {}", output.adapterIndex, output.outputIndex)
                            : std::format(L"{} ({},{})", output.deviceName, output.adapterIndex, output.outputIndex);
                        item.Text(winrt::hstring(text));
                        item.Click([this, output, text](auto&&, auto&&)
                        {
                            auto node = ::ShaderLab::Sources::CreateDesktopDuplicationNode(
                                output.adapterIndex,
                                output.outputIndex,
                                L"DXGI " + text);
                            auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
                            OnNodeAdded(nodeId);
                        });
                        dxgiCaptureGroup.Items().Append(item);
                    }
                    if (dxgiCaptureGroup.Items().Size() == 0)
                    {
                        auto unavailable = MUX::MenuFlyoutItem();
                        unavailable.Text(L"No outputs available");
                        unavailable.IsEnabled(false);
                        dxgiCaptureGroup.Items().Append(unavailable);
                    }
                    subItem.Items().Append(dxgiCaptureGroup);

                    auto wgcSourceItem = MUX::MenuFlyoutItem();
                    wgcSourceItem.Text(L"Windows Graphics Capture...");
                    wgcSourceItem.Click([this](auto&&, auto&&)
                    {
                        AddWindowsGraphicsCaptureSourceAsync();
                    });
                    subItem.Items().Append(wgcSourceItem);
                }
            }

            // List user effect files that failed to load where the effect would be looked for.
            const auto& userReport = slRegistry.UserEffectReport();
            if (!userReport.errors.empty())
            {
                std::wstring details;
                for (const auto& err : userReport.errors)
                    details += (details.empty() ? L"" : L"\n") + err;
                auto warnItem = MUX::MenuFlyoutItem();
                warnItem.Text(std::to_wstring(userReport.errors.size()) +
                    (userReport.errors.size() == 1 ? L" user effect failed to load"
                                                   : L" user effects failed to load"));
                warnItem.IsEnabled(false);
                MUX::ToolTipService::SetToolTip(warnItem, winrt::box_value(winrt::hstring(details)));
                slGroup.Items().Append(MUX::MenuFlyoutSeparator());
                slGroup.Items().Append(warnItem);
            }

            flyout.Items().Append(slGroup);
        }

        // ---- Add Output Node ----
        {
            auto sep = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator();
            flyout.Items().Append(sep);

            auto outputItem = MUX::MenuFlyoutItem();
            outputItem.Text(L"Output Window");
            outputItem.Click([this](auto&&, auto&&)
            {
                // Create a new Output node with auto-incrementing name.
                size_t outputCount = 0;
                if (const auto snapshot = CurrentGraphSnapshot())
                {
                    for (const auto& node : snapshot->nodes)
                        if (node.type == ::ShaderLab::Graph::NodeType::Output) ++outputCount;
                }
                std::wstring name = L"Output";
                if (outputCount > 0)
                    name = L"Output " + std::to_wstring(outputCount + 1);

                auto outputNode = ::ShaderLab::Effects::EffectRegistry::CreateOutputNode();
                ::ShaderLab::Graph::RenameNode(outputNode, name);
                auto nodeId = m_nodeGraphController.AddNode(std::move(outputNode), { 0.0f, 0.0f });

                m_nodeGraphController.RequestRelayout();
                PopulatePreviewNodeSelector();

                // Auto-open an output window for the new node.
                OpenOutputWindow(nodeId);
            });
            flyout.Items().Append(outputItem);
        }

        // ---- Custom Effects (duplicate existing) ----
        // Collect unique custom effect names from graph nodes.
        struct CustomEffectTemplate
        {
            std::wstring name;
            uint32_t sourceNodeId;
        };
        std::vector<CustomEffectTemplate> customTemplates;
        const auto graphSnapshot = CurrentGraphSnapshot();
        const std::vector<::ShaderLab::Graph::EffectNode> noNodes;
        for (const auto& node : graphSnapshot ? graphSnapshot->nodes : noNodes)
        {
            if ((node.type == ::ShaderLab::Graph::NodeType::PixelShader ||
                 node.type == ::ShaderLab::Graph::NodeType::ComputeShader) &&
                node.customEffect.has_value() &&
                !node.customEffect->hlslSource.empty())
            {
                // Only add if this name isn't already in the list.
                bool found = false;
                for (const auto& t : customTemplates)
                    if (t.name == node.name) { found = true; break; }
                if (!found)
                    customTemplates.push_back({ node.name, node.id });
            }
        }

        if (!customTemplates.empty())
        {
            auto customGroup = MUX::MenuFlyoutSubItem();
            customGroup.Text(L"Custom Effects");

            for (const auto& tmpl : customTemplates)
            {
                auto menuItem = MUX::MenuFlyoutItem();
                menuItem.Text(winrt::hstring(tmpl.name));

                auto capturedNodeId = tmpl.sourceNodeId;
                menuItem.Click([this, capturedNodeId](auto&&, auto&&)
                {
                    // Deep-copy the custom effect from the source node (UI thread: read the snapshot).
                    const auto snapshot = CurrentGraphSnapshot();
                    const auto* srcNode = snapshot ? snapshot->FindNode(capturedNodeId) : nullptr;
                    if (!srcNode || !srcNode->customEffect.has_value()) return;

                    m_nodeGraphController.AddNode(::ShaderLab::Graph::NewInstanceOf(*srcNode), { 0.0f, 0.0f });
                    m_nodeGraphController.RequestRelayout();
                    PopulatePreviewNodeSelector();
                    PopulateAddNodeFlyout(); // refresh custom effects list
                });

                customGroup.Items().Append(menuItem);
            }

            flyout.Items().Append(customGroup);
        }
    }

    void MainWindow::OnAddEffectNode(const ::ShaderLab::Effects::EffectDescriptor& desc)
    {
        auto node = ::ShaderLab::Effects::EffectRegistry::CreateNode(desc);
        // Pass {0,0} to use the controller's auto-layout which avoids overlaps.
        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
        OnNodeAdded(nodeId);
    }

    void MainWindow::OnAddFloodSourceClicked(
        winrt::Windows::Foundation::IInspectable const& /*sender*/,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateFloodSourceNode(
            { 1.0f, 1.0f, 1.0f, 1.0f }, L"White");

        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });

        PrepareSourceOnRenderThread(nodeId);

        OnNodeAdded(nodeId);
    }

    winrt::fire_and_forget MainWindow::AddWindowsGraphicsCaptureSourceAsync()
    {
        auto strong = get_strong();

        winrt::Windows::Graphics::Capture::GraphicsCapturePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);

        auto item = co_await picker.PickSingleItemAsync();
        if (!item) co_return;

        std::wstring name = item.DisplayName().empty()
            ? L"Windows Graphics Capture"
            : std::wstring(item.DisplayName().c_str());
        auto node = ::ShaderLab::Sources::CreateGraphicsCaptureNode(name);
        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
        // The factory belongs to the render thread; a new target replaces a
        // source the worker may be ticking.
        m_renderDispatcher.DispatchSync([this, nodeId, item]
        {
            Sources().SetLiveSourceTarget(nodeId, item);
            ::ShaderLab::Graph::ResetNodeOutput(Model(), nodeId);
        });

        PrepareSourceOnRenderThread(nodeId);

        OnNodeAdded(nodeId);
    }

    void MainWindow::PrepareSourceOnRenderThread(uint32_t nodeId)
    {
        m_renderDispatcher.DispatchSync([this, nodeId]
        {
            auto* node = Model().FindNode(nodeId);
            auto* dc = m_renderEngine.RenderD2DContext();
            if (node && dc)
                Sources().PrepareSourceNode(*node, dc, m_renderEngine.D3DDevice(), m_renderEngine.D3DContext());
        });
    }

    void MainWindow::OnNodeAdded(uint32_t /*nodeId*/)
    {
        // A new node starts dirty and nothing consumes it yet, so there is
        // nothing else to invalidate.
        m_nodeGraphController.RequestRelayout();
        PopulatePreviewNodeSelector();
        MarkUnsaved();
    }

    // -----------------------------------------------------------------------
    // Split comparison
    // -----------------------------------------------------------------------

    ID2D1Image* MainWindow::ResolveDisplayImage(uint32_t nodeId)
    {
        return ::ShaderLab::Rendering::ResolveNodeImage(Model(), nodeId);
    }

    // -----------------------------------------------------------------------
    // Pixel trace
    // -----------------------------------------------------------------------

    // Safe on any thread: returns what the render worker measured on the
    // render context while drawing the preview (at most one frame stale). The
    // image itself is never touched here -- it belongs to the render context,
    // and the UI thread must not walk Model() to find it anyway.
    D2D1_RECT_F MainWindow::GetPreviewImageBounds()
    {
        std::scoped_lock lock(m_previewBoundsMutex);
        return m_previewBounds;
    }

    void MainWindow::TracePixelAt(const ::ShaderLab::Controls::PreviewClick& click)
    {
        m_tracePanel.SetMarker(click.dipX, click.dipY, click.view);
        const auto& rect = click.imageRect;
        const auto traceW = static_cast<uint32_t>(rect.right - rect.left);
        const auto traceH = static_cast<uint32_t>(rect.bottom - rect.top);
        if (traceW == 0 || traceH == 0) return;

        // The label counts physical pixels; the trace reads the image's own
        // 96-DPI coordinates, offset by its origin.
        const float dpiScale = (std::max)(1.0f, PreviewPanel().CompositionScaleX());
        const auto label = std::format(L"Position: ({}, {})",
            static_cast<uint32_t>(click.normX * (rect.right - rect.left) * dpiScale),
            static_cast<uint32_t>(click.normY * (rect.bottom - rect.top) * dpiScale));
        const int32_t x = static_cast<int32_t>(rect.left) +
            static_cast<int32_t>((std::min)(click.normX * traceW, static_cast<float>(traceW - 1)));
        const int32_t y = static_cast<int32_t>(rect.top) +
            static_cast<int32_t>((std::min)(click.normY * traceH, static_cast<float>(traceH - 1)));

        // The trace reads node outputs, which belong to the render thread.
        const uint32_t nodeId = Preview().NodeId();
        std::optional<::ShaderLab::Rendering::PixelTraceNode> trace;
        try
        {
            trace = m_renderDispatcher.DispatchSync([this, nodeId, x, y]
            {
                return ::ShaderLab::Rendering::TracePixel(Model(), nodeId, x, y,
                                                          m_renderEngine.RenderD2DContext()).root;
            });
        }
        catch (const std::exception& error)   // timed out or shutting down: no trace to show
        {
            OutputDebugStringW((L"[PixelTrace] " + ::ShaderLab::Utf8ToWide(error.what()) + L"\n").c_str());
        }
        m_tracePanel.Show(trace, label, click.outOfBounds);
        m_tracePanel.UpdateCrosshair(Preview().View(), BottomTabView().SelectedIndex() == 1);
    }

    // -----------------------------------------------------------------------
    // Node graph editor rendering
    // -----------------------------------------------------------------------

    void MainWindow::EnsureUiD2dContext()
    {
        if (m_uiD2dContext) return;
        if (!m_renderEngine.D2DDevice()) return;

        // The UI context comes from the render engine's multithreaded D2D
        // device: contexts on one device share resources without explicit
        // sync, so the UI's DrawImage sees what the render context wrote. A
        // separate device would need keyed-mutex sync or read stale pixels.
        m_uiD2dDevice.copy_from(m_renderEngine.D2DDevice());
        if (FAILED(m_uiD2dDevice->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                m_uiD2dContext.put())))
        {
            m_uiD2dContext = nullptr;
            m_uiD2dDevice = nullptr;
            return;
        }
    }

    void MainWindow::ReleaseUiD2dContext()
    {
        // Brushes and bitmap targets are device-context-bound. When the UI
        // context goes away (e.g. adapter switch) they have to be released
        // before recreating the context against the new device.
        m_graphGridBrush = nullptr;
        m_graphRenderTarget = nullptr;
        m_uiD2dContext = nullptr;
        m_uiD2dDevice = nullptr;
    }

    void MainWindow::InitializeGraphPanel()
    {
        if (!m_renderEngine.DXGIFactory() || !m_renderEngine.D3DDevice())
            return;

        EnsureUiD2dContext();
        if (!m_uiD2dContext) return;

        auto panel = NodeGraphPanel();
        m_graphPanelDipsWidth = (std::max)(1.0f, static_cast<float>(panel.ActualWidth()));
        m_graphPanelDipsHeight = (std::max)(1.0f, static_cast<float>(panel.ActualHeight()));
        if (m_graphPanelDipsWidth <= 1.0f) m_graphPanelDipsWidth = 400.0f;
        if (m_graphPanelDipsHeight <= 1.0f) m_graphPanelDipsHeight = 300.0f;

        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));
        m_graphPanelWidth = static_cast<uint32_t>((std::max)(1.0f, std::ceil(m_graphPanelDipsWidth * scaleX)));
        m_graphPanelHeight = static_cast<uint32_t>((std::max)(1.0f, std::ceil(m_graphPanelDipsHeight * scaleY)));

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
        UpdateGraphPanelScale();

        // The target belongs to the UI context: the render context is the
        // worker's.
        auto* dc = m_uiD2dContext.get();
        winrt::com_ptr<IDXGISurface> surface;
        m_graphSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_graphRenderTarget.put());

        m_nodeGraphController.SetViewportSize(
            m_graphPanelDipsWidth,
            m_graphPanelDipsHeight);
    }

    void MainWindow::ResizeGraphPanel(float widthDips, float heightDips)
    {
        auto panel = NodeGraphPanel();
        m_graphPanelDipsWidth = (std::max)(1.0f, widthDips);
        m_graphPanelDipsHeight = (std::max)(1.0f, heightDips);

        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));
        auto w = static_cast<uint32_t>((std::max)(1.0f, std::ceil(m_graphPanelDipsWidth * scaleX)));
        auto h = static_cast<uint32_t>((std::max)(1.0f, std::ceil(m_graphPanelDipsHeight * scaleY)));

        m_nodeGraphController.SetViewportSize(m_graphPanelDipsWidth, m_graphPanelDipsHeight);

        if (!m_graphSwapChain || (w == m_graphPanelWidth && h == m_graphPanelHeight))
        {
            m_nodeGraphController.SetNeedsRedraw();
            return;
        }

        m_graphPanelWidth = w;
        m_graphPanelHeight = h;

        m_graphRenderTarget = nullptr;
        m_graphSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);

        auto* dc = m_uiD2dContext.get();
        if (!dc) return;

        winrt::com_ptr<IDXGISurface> surface;
        m_graphSwapChain->GetBuffer(0, IID_PPV_ARGS(surface.put()));

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmapFromDxgiSurface(surface.get(), bmpProps, m_graphRenderTarget.put());

        m_nodeGraphController.SetNeedsRedraw();
    }

    void MainWindow::UpdateGraphPanelScale()
    {
        if (!m_graphSwapChain)
            return;

        winrt::com_ptr<IDXGISwapChain2> swapChain2;
        if (FAILED(m_graphSwapChain->QueryInterface(IID_PPV_ARGS(swapChain2.put()))))
            return;

        auto panel = NodeGraphPanel();
        float scaleX = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleX()));
        float scaleY = (std::max)(1.0f, static_cast<float>(panel.CompositionScaleY()));

        DXGI_MATRIX_3X2_F matrix{};
        matrix._11 = 1.0f / scaleX;
        matrix._22 = 1.0f / scaleY;
        swapChain2->SetMatrixTransform(&matrix);
    }

    void MainWindow::RenderGraphScene(ID2D1DeviceContext5* dc, D2D1_SIZE_F viewSize)
    {
        if (!dc) return;

        dc->Clear(D2D1::ColorF(0x1A1A1E));

        // Draw subtle dot grid for spatial reference.
        {
            auto pan = m_nodeGraphController.PanOffset();
            float zoom = m_nodeGraphController.Zoom();
            float gridSpacing = 24.0f * zoom;
            if (gridSpacing > 8.0f)
            {
                if (!m_graphGridBrush)
                    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.06f), m_graphGridBrush.put());
                if (m_graphGridBrush)
                {
                    float startX = fmodf(pan.x, gridSpacing);
                    float startY = fmodf(pan.y, gridSpacing);
                    if (startX < 0) startX += gridSpacing;
                    if (startY < 0) startY += gridSpacing;
                    for (float y = startY; y < viewSize.height; y += gridSpacing)
                        for (float x = startX; x < viewSize.width; x += gridSpacing)
                            dc->FillEllipse({ { x, y }, 1.0f, 1.0f }, m_graphGridBrush.get());
                }
            }
        }

        m_nodeGraphController.Render(dc);
    }

    void MainWindow::RenderNodeGraph()
    {
        if (!m_graphSwapChain || !m_graphRenderTarget)
            return;

        // Skip redraw if nothing changed in the graph canvas.
        if (!m_nodeGraphController.NeedsRedraw())
            return;

        // UI-side D2D context: editor canvas is rendered entirely on the UI
        // thread, separately from the render thread's evaluator context.
        auto* dc = m_uiD2dContext.get();
        if (!dc) return;

        // NodeGraphController paints from the per-frame GraphUiSnapshot, so
        // this thread touches no mutable graph state and never waits on the
        // render worker's tick.

        float graphDpiX = 96.0f * (std::max)(1.0f, static_cast<float>(NodeGraphPanel().CompositionScaleX()));
        float graphDpiY = 96.0f * (std::max)(1.0f, static_cast<float>(NodeGraphPanel().CompositionScaleY()));
        dc->SetDpi(graphDpiX, graphDpiY);

        dc->SetTarget(m_graphRenderTarget.get());
        dc->BeginDraw();

        D2D1_SIZE_F viewSize = { m_graphPanelDipsWidth, m_graphPanelDipsHeight };
        RenderGraphScene(dc, viewSize);

        dc->EndDraw();
        dc->SetTarget(nullptr);

        m_graphSwapChain->Present(0, 0);
    }

    void MainWindow::AttachCanvasInput()
    {
        ::ShaderLab::Controls::GraphCanvasHost host;
        host.snapshot = [this] { return CurrentGraphSnapshot(); };
        host.selectedNodeId = [this] { return SelectedNodeId(); };
        host.selectNode = [this](uint32_t nodeId)
        {
            Preview().SetSelectedNode(nodeId);
            m_propertiesPanel.Rebuild();
        };
        host.nodeClicked = [this](uint32_t nodeId, bool previewable)
        {
            Preview().SetSelectedNode(nodeId);
            m_propertiesPanel.Rebuild();
            BottomTabView().SelectedIndex(0);
            if (previewable)
                SelectPreviewNode(nodeId);   // fit on first view, else restore this node's pan/zoom
            m_forceRender = true;
            UpdatePreviewOverlay();
        };
        host.requestRender = [this] { m_forceRender = true; };
        host.refreshProperties = [this] { m_propertiesPanel.Rebuild(); };
        host.nodeListChanged = [this] { PopulatePreviewNodeSelector(); };
        host.graphEdited = [this] { MarkUnsaved(); };
        host.logInfo = [this](uint32_t nodeId, const std::wstring& text) { m_nodeLogs[nodeId].Info(text); };
        host.nodesDeleting = [this](const std::unordered_set<uint32_t>& nodeIds)
        {
            for (uint32_t nodeId : nodeIds)
                CloseOutputWindow(nodeId);
        };
        host.nodesDeleted = [this]
        {
            Preview().SetSelectedNode(0);
            PopulatePreviewNodeSelector();
            m_propertiesPanel.Rebuild();
            MarkUnsaved();
            // The preview falls back to the Output node.
            if (const auto snapshot = CurrentGraphSnapshot())
            {
                for (const auto& n : snapshot->nodes)
                {
                    if (n.type == ::ShaderLab::Graph::NodeType::Output)
                    {
                        SelectPreviewNode(n.id);
                        break;
                    }
                }
            }
            PreviewOverlayBorder().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        };
        m_canvasInput.Attach(NodeGraphContainer(), m_nodeGraphController, std::move(host));
    }

    // -----------------------------------------------------------------------
    // Video source file picker
    // -----------------------------------------------------------------------

    winrt::fire_and_forget MainWindow::BrowseVideoForSourceNode()
    {
        auto strong = get_strong();

        winrt::Windows::Storage::Pickers::FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::VideosLibrary);
        for (const auto ext : ::ShaderLab::Effects::SourceNodeFactory::VideoExtensions())
            picker.FileTypeFilter().Append(winrt::hstring(ext));

        auto file = co_await picker.PickSingleFileAsync();
        if (!file) co_return;

        auto filePath = std::wstring(file.Path().c_str());
        auto fileName = std::wstring(file.Name().c_str());

        auto node = ::ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(filePath, fileName);
        auto nodeId = m_nodeGraphController.AddNode(std::move(node), { 0.0f, 0.0f });
        OnNodeAdded(nodeId);

        // Prepare immediately so first frame is decoded.
        PrepareSourceOnRenderThread(nodeId);

        m_nodeGraphController.RequestRelayout();
        PopulatePreviewNodeSelector();
        Preview().RequestFit(/*keepFitted=*/false);
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
                m_nodeGraphController.RequestRelayout();
                PopulatePreviewNodeSelector();
                PopulateAddNodeFlyout(); // refresh custom effects list
                return nodeId;
            });
            designerImpl->SetUpdateInGraphCallback([this](uint32_t nodeId, ::ShaderLab::Graph::CustomEffectDefinition def)
            {
                // Graph writes belong to the render thread.
                const bool hlslChanged = m_renderDispatcher.DispatchSync([this, nodeId, &def]
                {
                    auto* node = Model().FindNode(nodeId);
                    if (!node) return false;

                    // Name enforcement: if HLSL changed and other nodes share
                    // this name with the OLD HLSL, this node must be renamed.
                    const bool changed = !node->customEffect.has_value() ||
                        node->customEffect->hlslSource != def.hlslSource;

                    // The designer's definition has no contracts. Derive them
                    // from the node's properties: host-injected values such as
                    // InputMask live there, not among the parameters.
                    ::ShaderLab::Graph::DeriveContracts(def, node->properties);
                    ::ShaderLab::Graph::ReplaceCustomEffect(Model(), nodeId, std::move(def));
                    Evaluator().UpdateNodeShader(nodeId, *node);

                    if (changed && Model().EnsureUniqueCustomEffectName(nodeId))
                        m_nodeGraphController.RebuildLayout();
                    return changed;
                });
                m_forceRender = true;
                if (hlslChanged)
                    PopulateAddNodeFlyout();
            });

            m_designerWindow.Closed([this](auto&&, auto&&) { m_designerWindow = nullptr; });
        }
        m_designerWindow.Activate();
    }

    std::vector<uint8_t> MainWindow::CapturePreviewAsPng()
    {
        // The render-thread context: this runs inside a render dispatch.
        auto result = ::ShaderLab::Rendering::CaptureNodeAsPng(
            Model(), Preview().NodeId(), m_renderEngine.RenderD2DContext());
        return std::move(result.png);
    }

    std::vector<uint8_t> MainWindow::CaptureGraphAsPng()
    {
        // Use the UI-side D2D context -- this draws the editor canvas and
        // produces a CPU-readable bitmap purely on the UI thread, with no
        // dependency on the render-engine D2D context.
        auto* dc = m_uiD2dContext.get();
        if (!dc) return {};

        // Layout may be stale (e.g., after MCP set-property) — rebuild before
        // measuring or drawing.
        m_nodeGraphController.RequestRelayout();

        const uint32_t w = (std::max)(uint32_t{ 1 }, m_graphPanelWidth);
        const uint32_t h = (std::max)(uint32_t{ 1 }, m_graphPanelHeight);

        // A capture is a side query: RenderGraphScene clears the controller's
        // redraw flag, which would otherwise skip the next live render.
        const bool wasDirty = m_nodeGraphController.NeedsRedraw();
        ::ShaderLab::Rendering::PixelBuffer pixels;
        const HRESULT hr = ::ShaderLab::Rendering::RenderToPixels(dc, w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1::ColorF(0, 0, 0, 0),
            [&](ID2D1DeviceContext*)
            {
                RenderGraphScene(dc, { static_cast<float>(w), static_cast<float>(h) });
            },
            pixels);
        if (wasDirty) m_nodeGraphController.SetNeedsRedraw();

        std::vector<uint8_t> png;
        if (FAILED(hr) || FAILED(::ShaderLab::Rendering::EncodePng(pixels, png)))
            return {};
        return png;
    }

    void MainWindow::FitGraphView(float padding)
    {
        // Layout may be stale (e.g., after MCP set-property) — rebuild before
        // measuring bounds.
        m_nodeGraphController.RequestRelayout();

        D2D1_RECT_F b = m_nodeGraphController.ContentBounds();
        float contentW = b.right - b.left;
        float contentH = b.bottom - b.top;
        if (contentW <= 0.0f || contentH <= 0.0f) return; // Empty graph.

        float vpW = static_cast<float>(m_graphPanelWidth);
        float vpH = static_cast<float>(m_graphPanelHeight);
        if (vpW <= 1.0f || vpH <= 1.0f) return;

        // Padding is in viewport (screen) space.  Guard against viewport too
        // small for the requested padding.
        padding = (std::max)(0.0f, padding);
        if (padding * 2.0f > vpW * 0.5f) padding = vpW * 0.25f;
        if (padding * 2.0f > vpH * 0.5f) padding = (std::min)(padding, vpH * 0.25f);

        float availW = (std::max)(1.0f, vpW - 2.0f * padding);
        float availH = (std::max)(1.0f, vpH - 2.0f * padding);

        float zoom = (std::min)(availW / contentW, availH / contentH);
        // Controller will clamp to [0.1, 5.0] internally; mirror so pan is
        // computed against the actual zoom that will take effect.
        zoom = (std::max)(0.1f, (std::min)(5.0f, zoom));

        float contentCx = (b.left + b.right) * 0.5f;
        float contentCy = (b.top + b.bottom) * 0.5f;

        // screen = zoom * canvas + pan, so pan = screenCenter - zoom * canvasCenter.
        float panX = vpW * 0.5f - zoom * contentCx;
        float panY = vpH * 0.5f - zoom * contentCy;

        m_nodeGraphController.SetZoom(zoom);
        m_nodeGraphController.SetPanOffset(panX, panY);
    }

    winrt::fire_and_forget MainWindow::SaveImageAsync(SaveReference ref)
    {
        auto strong = get_strong();
        const bool isJxr = (ref == SaveReference::HdrJxr);

        winrt::Windows::Storage::Pickers::FileSavePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(m_hwnd);
        picker.SuggestedStartLocation(winrt::Windows::Storage::Pickers::PickerLocationId::PicturesLibrary);

        // Use the previewed node's name as suggested filename.
        std::wstring suggestedName = L"output";
        const uint32_t previewNodeId = Preview().NodeId();
        if (const auto snapshot = CurrentGraphSnapshot())
            if (const auto* previewNode = snapshot->FindNode(previewNodeId))
                suggestedName = previewNode->name;
        // Sanitize for filename
        for (auto& ch : suggestedName)
            if (ch == L'/' || ch == L'\\' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|')
                ch = L'_';
        picker.SuggestedFileName(winrt::hstring(suggestedName));
        if (isJxr)
            picker.FileTypeChoices().Insert(L"JPEG XR (HDR)", winrt::single_threaded_vector<winrt::hstring>({ L".jxr" }));
        else
            picker.FileTypeChoices().Insert(L"PNG Image (SDR)", winrt::single_threaded_vector<winrt::hstring>({ L".png" }));

        auto file = co_await picker.PickSaveFileAsync();
        if (!file) co_return;

        // Presentation→file re-referencing: the scene is linear scRGB
        // where the OS presents SDR reference white at SdrWhiteNits.
        // An SDR file's 1.0 must mean "SDR reference white", so scale by
        // 80/SdrWhiteNits in linear space before the sRGB encode; DWM
        // multiplies it back on display. Respects a simulated profile
        // (CachedCapabilities prefers it).
        float presentationScale = 1.0f;
        if (ref == SaveReference::PresentationPng)
        {
            const float sdrWhite =
                m_displayMonitor.CachedCapabilities().sdrWhiteLevelNits;
            if (sdrWhite > 80.0f)
                presentationScale = 80.0f / sdrWhite;
        }

        // JXR: render in FP16 scRGB for full HDR fidelity (linear,
        // scene-referred — no transfer encode wanted).
        // PNG: render in 8-bit BGRA with the _SRGB variant so the
        // scene's linear values are gamma-ENCODED on write; plain
        // UNORM wrote linear bytes that viewers then sRGB-decoded,
        // producing a crushed, far-too-dark image.
        const DXGI_FORMAT renderFormat = isJxr
            ? DXGI_FORMAT_R16G16B16A16_FLOAT
            : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

        // The preview image belongs to the render thread, so it is drawn and
        // read back there with the render context; only the encode runs here.
        struct SavedPixels
        {
            bool hasImage{ false };
            HRESULT hr{ S_OK };
            ::ShaderLab::Rendering::PixelBuffer pixels;
        };
        SavedPixels saved;
        try
        {
            saved = m_renderDispatcher.DispatchSync([&]() -> SavedPixels
            {
                SavedPixels result;
                auto* previewImage = ResolveDisplayImage(previewNodeId);
                auto* dc = m_renderEngine.RenderD2DContext();
                if (!previewImage || !dc) return result;
                result.hasImage = true;

                winrt::com_ptr<ID2D1Effect> scaleFx;
                winrt::com_ptr<ID2D1Image> scaledImage;
                ID2D1Image* imageToSave = previewImage;
                if (presentationScale != 1.0f &&
                    SUCCEEDED(dc->CreateEffect(CLSID_D2D1ColorMatrix, scaleFx.put())))
                {
                    scaleFx->SetInput(0, previewImage);
                    const float scale = presentationScale;
                    D2D1_MATRIX_5X4_F m = D2D1::Matrix5x4F(
                        scale, 0, 0, 0,
                        0, scale, 0, 0,
                        0, 0, scale, 0,
                        0, 0, 0, 1,
                        0, 0, 0, 0);
                    scaleFx->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
                    scaleFx->GetOutput(scaledImage.put());
                    if (scaledImage)
                        imageToSave = scaledImage.get();
                }

                D2D1_RECT_F bounds{};
                result.hr = ::ShaderLab::Rendering::GetImageBounds96Dpi(dc, previewImage, bounds);
                const uint32_t w = static_cast<uint32_t>((std::max)(0.0f, bounds.right - bounds.left));
                const uint32_t h = static_cast<uint32_t>((std::max)(0.0f, bounds.bottom - bounds.top));
                if (FAILED(result.hr) || w == 0 || h == 0) return result;
                result.hr = ::ShaderLab::Rendering::RenderToPixels(dc, w, h, renderFormat, D2D1::ColorF(0, 0, 0, 1.0f),
                    [&](ID2D1DeviceContext* target)
                    {
                        target->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top));
                        target->DrawImage(imageToSave);
                    },
                    result.pixels);
                return result;
            });
        }
        catch (const winrt::hresult_error& error)
        {
            PipelineFormatText().Text(L"Error: Failed to save image: " + error.message());
            co_return;
        }
        catch (const std::exception& error)   // DispatchSync timed out or is shutting down
        {
            PipelineFormatText().Text(L"Error: Failed to save image: " + ::ShaderLab::Utf8ToWide(error.what()));
            co_return;
        }

        if (!saved.hasImage)
        {
            PipelineFormatText().Text(L"Error: No image to save");
            co_return;
        }
        if (FAILED(saved.hr))
        {
            PipelineFormatText().Text(std::format(L"Error: Failed to render the image (0x{:08X})",
                static_cast<uint32_t>(saved.hr)));
            co_return;
        }
        if (saved.pixels.width == 0 || saved.pixels.height == 0) co_return;
        const uint32_t w = saved.pixels.width;
        const uint32_t h = saved.pixels.height;

        const std::wstring filePath(file.Path());
        const HRESULT encodeHr = ::ShaderLab::Rendering::EncodeToFile(saved.pixels, filePath);
        if (FAILED(encodeHr))
        {
            PipelineFormatText().Text(std::format(L"Error: Failed to save image (0x{:08X})",
                static_cast<uint32_t>(encodeHr)));
            co_return;
        }
        std::error_code sizeError;
        const auto sizeKB = std::filesystem::file_size(filePath, sizeError) / 1024;
        auto msg = isJxr
            ? L"Saved scRGB FP16 JXR (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L", " + std::to_wstring(sizeKB) + L" KB): " + file.Name()
            : L"Saved PNG (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L"): " + file.Name();
        PipelineFormatText().Text(msg);
    }

    // -----------------------------------------------------------------------
    // Preview pan/zoom
    // -----------------------------------------------------------------------

    void MainWindow::SelectPreviewNode(uint32_t nodeId)
    {
        Preview().SelectNode(nodeId);
    }

    // -----------------------------------------------------------------------
    // Output windows
    // -----------------------------------------------------------------------

    void MainWindow::OpenOutputWindow(uint32_t nodeId)
    {
        // Don't open duplicates.
        for (const auto& w : m_outputWindows)
        {
            if (w->NodeId() == nodeId && w->IsOpen())
                return;
        }

        const auto snapshot = CurrentGraphSnapshot();
        const auto* node = snapshot ? snapshot->FindNode(nodeId) : nullptr;
        if (!node) return;

        auto window = std::make_unique<::ShaderLab::Controls::OutputWindow>();
        window->Create(
            m_renderEngine.D3DDevice(),
            m_renderEngine.D2DDeviceContext(),
            m_renderEngine.DXGIFactory(),
            nodeId,
            node->name,
            m_renderEngine.ActiveFormat());

        // The worker draws into the window's sink; its own reference keeps
        // the sink alive through a close.
        if (auto sink = window->Sink())
        {
            std::scoped_lock lock(m_outputSinksMutex);
            m_outputSinks.push_back(sink);
        }

        m_outputWindows.push_back(std::move(window));
    }

    void MainWindow::CloseOutputWindow(uint32_t nodeId)
    {
        // Drop the sink first, so the worker stops drawing for it before the
        // window tears down its swap chain.
        {
            std::scoped_lock lock(m_outputSinksMutex);
            std::erase_if(m_outputSinks,
                [nodeId](const auto& s){ return s && s->nodeId == nodeId; });
        }
        std::erase_if(m_outputWindows, [nodeId](const auto& w)
        {
            return w->NodeId() == nodeId;
        });
    }

    void MainWindow::PresentOutputWindows()
    {
        // UI thread: each window blits the frame the worker last drew for it
        // (RenderOutputSinks).
        if (m_outputWindows.empty())
            return;

        // Remove closed windows and their corresponding graph nodes (and
        // their sinks).
        std::vector<uint32_t> closedNodeIds;
        std::erase_if(m_outputWindows, [&closedNodeIds](const auto& w) {
            if (!w->IsOpen()) { closedNodeIds.push_back(w->NodeId()); return true; }
            return false;
        });
        if (!closedNodeIds.empty())
        {
            std::scoped_lock lock(m_outputSinksMutex);
            std::erase_if(m_outputSinks, [&closedNodeIds](const auto& s){
                return !s || std::find(closedNodeIds.begin(), closedNodeIds.end(),
                    s->nodeId) != closedNodeIds.end();
            });
        }
        if (!closedNodeIds.empty())
        {
            // Graph writes and layout belong to the render thread (the
            // graph-access rule). RemoveNode dirties the removed node's edge
            // and binding consumers, which is all that can change: it used
            // to null EVERY node's cachedOutput and dirty the whole graph, so
            // closing one window re-ran every compute node. Consumers' D2D
            // effects hold their own references to the old input image, so
            // nothing dangles meanwhile.
            m_renderDispatcher.DispatchSync([this, &closedNodeIds] {
                for (uint32_t nodeId : closedNodeIds)
                {
                    Model().RemoveNode(nodeId);
                    Evaluator().InvalidateNode(nodeId);
                }
                m_nodeGraphController.RebuildLayout();
            });
            PopulatePreviewNodeSelector();
        }
        if (!closedNodeIds.empty())
        {
            m_forceRender = true;
            m_nodeGraphController.SetNeedsRedraw();
            MarkUnsaved();
        }

        if (m_outputWindows.empty()) return;
        const auto tStart = std::chrono::high_resolution_clock::now();

        // Canonical status string + tooltip pushed to every output window.
        std::wstring statusText  = BuildFpsStatusText();
        std::wstring tooltipText = BuildFpsTooltipText();
        // UI thread: names come from the per-frame snapshot, never the live
        // graph the render worker is writing (the graph-access rule).
        const auto snapshot = CurrentGraphSnapshot();

        for (auto& window : m_outputWindows)
        {
            if (!window->IsReady())
                continue;

            if (const auto* node = snapshot ? snapshot->FindNode(window->NodeId()) : nullptr)
                window->SetTitle(node->name);
            window->SetStatusText(statusText);
            window->SetStatusTooltip(tooltipText);

            window->BlitAndPresent(m_uiD2dContext.get());
        }

        // Its own line in the perf readout (it was declared but never written,
        // so the cost hid inside uiTickUs at a permanent 0.00 ms).
        const double us = std::chrono::duration<double, std::micro>(
            std::chrono::high_resolution_clock::now() - tStart).count();
        m_uiTiming.outputWindowsUs = m_uiTiming.outputWindowsUs * 0.9 + us * 0.1;
        m_frameTimings.Publish(m_uiTiming);
    }

    void MainWindow::OpenLogWindow(uint32_t nodeId)
    {
        // Check if already open.
        for (auto& w : m_logWindows)
        {
            if (w->NodeId() == nodeId && w->IsOpen())
                return;
        }

        const auto snapshot = CurrentGraphSnapshot();
        const auto* node = snapshot ? snapshot->FindNode(nodeId) : nullptr;
        if (!node) return;

        auto window = std::make_unique<::ShaderLab::Controls::LogWindow>();
        window->Create(nodeId, node->name);

        // Populate with existing log entries.
        auto it = m_nodeLogs.find(nodeId);
        if (it != m_nodeLogs.end())
            window->Update(it->second);

        m_logWindows.push_back(std::move(window));
    }

    void MainWindow::UpdateLogWindows()
    {
        // Remove closed windows.
        m_logWindows.erase(
            std::remove_if(m_logWindows.begin(), m_logWindows.end(),
                [](const auto& w) { return !w->IsOpen(); }),
            m_logWindows.end());

        // Update open windows with new log entries.
        for (auto& w : m_logWindows)
        {
            auto it = m_nodeLogs.find(w->NodeId());
            if (it != m_nodeLogs.end())
                w->Update(it->second);
        }
    }

    std::wstring MainWindow::BuildFpsTooltipText() const
    {
        // Builds the multi-line per-phase breakdown shown in the FPS tooltip.
        // Split into "Graph eval" (the actual cost of one render-thread frame
        // -- the meaningful number for HDR shader / tonemap perf evaluation,
        // which is the primary purpose of this app) and "UI overhead" (the
        // SwapChain blit + Present1 vsync wait + canvas redraw on the UI
        // tick). FPS and total-ms reflect graph-eval throughput only.
        const auto ft = m_frameTimings.Render();
        const auto& ui = m_uiTiming;
        double fps = m_lastFps;
        double total = ft.totalUs / 1000.0;
        double sumGraph =
            ft.frameStartUs / 1000.0 +
            ft.sourcesPrepUs / 1000.0 + ft.evaluateUs / 1000.0 +
            ft.deferredComputeUs / 1000.0 +
            ft.drawUs / 1000.0 + ft.endDrawFlushUs / 1000.0;
        double idle = (std::max)(0.0, total - sumGraph);

        std::wstring text = L"Previewed node: " + BuildFpsStatusText() +
            (m_previewRenderStats.GpuTimed()
                ? L"\n  renders per second; ms from frame start to GPU done\n"
                : L"\n  renders per second; ms of CPU time (no GPU timestamps)\n");
        const double previewCpuMs = m_previewRenderStats.CpuMs();
        const double previewGpuMs = m_previewRenderStats.GpuMs();
        if (previewCpuMs >= 0.0 && previewGpuMs >= 0.0)
            text += std::format(L"  CPU to submit {:.2f} ms, GPU timestamp span {:.2f} ms\n", previewCpuMs, previewGpuMs);
        text += L"\n";
        text += std::format(
            L"Render loop: {:.0f} evaluations/s  ({:.1f} ms CPU / graph eval)\n"
            L"\n"
            L"  Graph eval (render thread):\n"
            L"    frame start     {:>6.2f} ms  (waiting on the GPU)\n"
            L"    sources prep    {:>6.2f} ms\n"
            L"    eval            {:>6.2f} ms\n"
            L"    compute         {:>6.2f} ms  ({} dispatch{})\n"
            L"    draw            {:>6.2f} ms\n"
            L"    end-draw flush  {:>6.2f} ms\n"
            L"    -----------------------------\n"
            L"    sum             {:>6.2f} ms\n"
            L"    idle / sched    {:>6.2f} ms\n"
            L"\n"
            L"  UI overhead (UI thread, not in totals):\n"
            L"    ui tick         {:>6.2f} ms  (drain + blit + Present1 + canvas)\n"
            L"    output windows  {:>6.2f} ms",
            fps, total,
            ft.frameStartUs / 1000.0,
            ft.sourcesPrepUs / 1000.0,
            ft.evaluateUs / 1000.0,
            ft.deferredComputeUs / 1000.0, ft.computeDispatches,
                (ft.computeDispatches == 1 ? L"" : L"es"),
            ft.drawUs / 1000.0,
            ft.endDrawFlushUs / 1000.0,
            sumGraph,
            idle,
            ui.uiTickUs / 1000.0,
            ui.outputWindowsUs / 1000.0);

        if (m_lastVideoFps > 0.1f)
            text += std::format(L"\n\n  video decode    {:>6.0f} fps", m_lastVideoFps);

        // GPU block. Everything above is CPU wall-clock around an ASYNCHRONOUS
        // API -- it measures how long RECORDING the commands took, not how long
        // the GPU spent running them. For shader work the two are unrelated, so
        // the numbers above cannot answer "is my tone mapper slow". These can.
        if (!ft.gpuAvailable)
        {
            text += L"\n\n  GPU time: unavailable (no timestamp queries)";
        }
        else if (!ft.gpuEnabled)
        {
            text += L"\n\n  GPU time: OFF -- the phase times above are CPU-side"
                    L"\n            command recording, not GPU execution."
                    L"\n            Enable with MCP perf_gpu_timing.";
        }
        else if (ft.gpuFramesResolved == 0)
        {
            text += L"\n\n  GPU time: sampling (waiting for first frame)";
        }
        else
        {
            text += std::format(
                L"\n\n  GPU time (D3D11 timestamps, real execution):\n"
                L"    sources prep    {:>6.2f} ms\n"
                L"    eval            {:>6.2f} ms\n"
                L"    compute         {:>6.2f} ms\n"
                L"    draw            {:>6.2f} ms   <- effect chain runs here\n"
                L"    -----------------------------\n"
                L"    frame           {:>6.2f} ms",
                ft.gpuSourcesPrepMs, ft.gpuEvaluateMs,
                ft.gpuDeferredComputeMs, ft.gpuDrawMs, ft.gpuFrameMs);
            if (ft.gpuDisjointDrops > 0)
                text += std::format(
                    L"\n    {} sample(s) voided by a GPU clock change",
                    ft.gpuDisjointDrops);
        }
        return text;
    }

    // ---- Performance flyout toggles ------------------------------------
    //
    // All three change how the app is MEASURED rather than what it renders,
    // and each exists because its absence produced a misleading reading that
    // cost real debugging time.

    void MainWindow::OnGpuTimingToggled(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto box = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::CheckBox>();
        if (!box) return;
        const bool on = box.IsChecked() && box.IsChecked().Value();
        m_renderEngine.Timer().SetEnabled(on);
        UpdateFpsTooltip();
    }

    void MainWindow::OnForceRedrawToggled(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto box = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::CheckBox>();
        if (!box) return;
        const bool on = box.IsChecked() && box.IsChecked().Value();
        // Atomic: the render worker reads this every tick. As a plain bool the
        // worker never observed the write on ARM64 -- the same weak-memory
        // failure the GPU timer's enable flag had.
        m_forceContinuousRedraw.store(on, std::memory_order_release);
        UpdateFpsTooltip();
    }

    void MainWindow::OnUnthrottledToggled(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto box = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::CheckBox>();
        if (!box) return;
        const bool on = box.IsChecked() && box.IsChecked().Value();
        ::ShaderLab::Rendering::HostRenderSettings::SetUnthrottledRenderEnabled(on);
        // Wake the worker so the change takes effect on this tick rather than
        // after its current frame wait.
        m_renderDispatcher.Wake();
        UpdateFpsTooltip();
    }

    void MainWindow::OnNodeGpuStatsToggled(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto box = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::CheckBox>();
        if (!box) return;
        const bool on = box.IsChecked() && box.IsChecked().Value();
        m_nodeGraphController.SetShowNodeGpuStats(on);
        // The annotations are only meaningful while the timer is running, and
        // asking for them is a clear enough statement of intent to turn it on
        // -- silently drawing "not measured" on every node would look like the
        // feature was broken. Turning them off does NOT turn the timer back
        // off: the user may have enabled it deliberately.
        if (on && GpuTimingCheck() && !(GpuTimingCheck().IsChecked() &&
                                        GpuTimingCheck().IsChecked().Value()))
        {
            GpuTimingCheck().IsChecked(true);   // fires OnGpuTimingToggled
        }
        UpdateFpsTooltip();
    }

    void MainWindow::OnAnalysisRefreshChanged(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        auto combo = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::ComboBox>();
        if (!combo) return;
        // Display refresh only. Skip-unneeded-readback stays ON in every mode,
        // so a GPU-bound parameter never forces a readback; this only sets
        // which nodes count as "displayed" and how often they are re-read.
        // (Its predecessor, "Keep analysis values live", turned the skip OFF,
        // which also re-imposed the ~10 ms/frame Map() stall that GPU bindings
        // exist to remove.)
        // Item order matches the presets in HostRenderSettings.h.
        if (::ShaderLab::Rendering::HostRenderSettings::SetAnalysisReadoutPreset(combo.SelectedIndex()))
            UpdateFpsTooltip();
    }

    void MainWindow::OnGpuBindingsToggled(
        winrt::Windows::Foundation::IInspectable const& sender,
        winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto box = sender.try_as<winrt::Microsoft::UI::Xaml::Controls::CheckBox>();
        if (!box) return;
        const bool on = box.IsChecked() && box.IsChecked().Value();
        // Engine default is OFF, and until now nothing in the GUI ever turned
        // it on -- SetGpuBindingsEnabled was reachable only from headless's
        // --enable-gpu-bindings. So CanServeBindingViaGpu returned false on its
        // first line for every binding in the app, and every analysis consumer
        // forced a readback no matter how it was wired.
        // Already in that mode: nothing to invalidate. This is the common case
        // for the construction-time IsChecked() sync and for the MCP route,
        // which applies the mode itself before mirroring the box.
        if (::ShaderLab::Performance::IsGpuBindingsEnabled() == on) return;
        ApplyGpuBindingsMode(on);
        UpdateFpsTooltip();
    }

    void MainWindow::ApplyGpuBindingsMode(bool on)
    {
        ::ShaderLab::Performance::SetGpuBindingsEnabled(on);
        // Binding mode is baked into each effect's compiled shader via the
        // _SLPARAM_<name>_GPU defines, so flipping this has to invalidate the
        // effect cache or the graph keeps running the old variants.
        //
        // On the render thread: ReleaseCache frees effects and nulls every
        // node's cachedOutput, and the worker is the graph's single writer.
        m_renderDispatcher.DispatchSync([this] {
            Evaluator().ReleaseCache(Model());
            Model().MarkAllDirty();
            m_forceRender = true;
        });
    }

    std::wstring MainWindow::BuildFpsStatusText() const
    {
        // "24 fps | 3.1 ms": how often the previewed node re-rendered, and its
        // last render's time through the GPU. N/A when it has not rendered
        // recently. Shared by the main status bar and every output window.
        const double fps = m_previewRenderStats.Fps();
        if (fps < 0.0)
            return L"N/A fps | N/A ms";
        const double ms = m_previewRenderStats.Ms();
        return ms < 0.0 ? std::format(L"{:.0f} fps | N/A ms", fps)
                        : std::format(L"{:.0f} fps | {:.1f} ms", fps, ms);
    }

    void MainWindow::UpdateFpsTooltip()
    {
        // Refresh the TextBlock inside the FPS counter's tooltip with a
        // fresh per-phase breakdown. The TextBlock's Text property is
        // observable, so updating it while the tooltip is open re-renders
        // in place -- giving the user a real-time view of where each
        // millisecond is going. Sub-phases sum to <= totalUs (= 1000/fps);
        // the remainder is dispatcher idle / OS overhead between ticks.
        if (!FpsTooltipText()) return;
        FpsTooltipText().Text(winrt::hstring(BuildFpsTooltipText()));
    }
}
