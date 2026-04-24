#pragma once

#include "MainWindow.g.h"
#include "Rendering/RenderEngine.h"
#include "Rendering/DisplayMonitor.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/ToneMapper.h"
#include "Rendering/FalseColorOverlay.h"
#include "Graph/EffectGraph.h"
#include "Effects/EffectRegistry.h"
#include "Effects/SourceNodeFactory.h"
#include "Effects/CustomPixelShaderEffect.h"
#include "Effects/CustomComputeShaderEffect.h"
#include "Controls/ShaderEditorController.h"
#include "Controls/NodeGraphController.h"
#include "Controls/PixelInspectorController.h"
#include "Controls/PixelTraceController.h"
#include "EffectDesignerWindow.xaml.h"
#include "ShaderLab/McpHttpServer.h"

namespace winrt::ShaderLab::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        // Auto-start MCP server (set from --mcp command-line flag).
        void SetAutoStartMcp(bool autoStart) { m_autoStartMcp = autoStart; }

        // Device preference (set from --gpu / --warp command-line flags).
        void SetDevicePreference(::ShaderLab::Rendering::DevicePreference pref) { m_devicePref = pref; }

        // XAML-bound event handlers (must be public for generated code).
        void OnColumnSplitterPointerPressed(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnColumnSplitterPointerMoved(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnColumnSplitterPointerReleased(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

    private:
        HWND GetWindowHandle();
        void InitializeRendering();
        void OnPreviewPanelLoaded();
        void RegisterCustomEffects();
        void UpdateStatusBar();
        void PopulatePreviewNodeSelector();
        void PopulateDisplayProfileSelector();
        void PopulateCompareNodeSelector();
        ID2D1Image* GetPreviewImage();
        ID2D1Image* ResolveDisplayImage(uint32_t nodeId);

        // Returns the preview viewport size in DIPs (not physical pixels).
        D2D1_SIZE_F PreviewViewportDips() const;
        void ApplyDisplayProfile(const ::ShaderLab::Rendering::DisplayProfile& profile);
        void RevertToLiveDisplay();
        void ResetAfterGraphLoad();

        // Pixel trace helpers.
        D2D1_RECT_F GetPreviewImageBounds();
        bool PointerToImageCoords(
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args,
            float& outNormX, float& outNormY);
        void PopulatePixelTraceTree();
        void UpdatePixelTraceValues();
        winrt::Microsoft::UI::Xaml::Controls::Grid CreateTraceRow(
            const ::ShaderLab::Controls::PixelTraceNode& traceNode);

        // Event handlers.
        void OnPreviewSizeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
        void OnPreviewNodeSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnPreviewPointerMoved(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPreviewKeyDown(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
        void OnFalseColorSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnDisplayProfileSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        winrt::fire_and_forget LoadIccProfileAsync();
        void OnPreviewPointerPressed(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnTraceUnitSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnSaveGraphClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnLoadGraphClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget SaveGraphAsync();
        winrt::fire_and_forget LoadGraphAsync();
        void PopulateAddNodeFlyout();
        void OnAddEffectNode(const ::ShaderLab::Effects::EffectDescriptor& desc);
        void OnAddImageSourceClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget AddImageSourceAsync();
        void OnAddFloodSourceClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNodeAdded(uint32_t nodeId);
        void OnCompareToggled(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnCompareNodeSelectionChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnPreviewPointerDragged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnPreviewPointerReleased(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

        // Render loop.
        void OnRenderTick(
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& sender,
            winrt::Windows::Foundation::IInspectable const& args);
        void RenderFrame(double deltaSeconds = 0.0);

        // Device stack.
        ::ShaderLab::Rendering::RenderEngine       m_renderEngine;
        ::ShaderLab::Rendering::DisplayMonitor     m_displayMonitor;
        ::ShaderLab::Rendering::GraphEvaluator     m_graphEvaluator;
        ::ShaderLab::Rendering::ToneMapper         m_toneMapper;
        ::ShaderLab::Rendering::FalseColorOverlay  m_falseColor;

        // Effect graph.
        ::ShaderLab::Graph::EffectGraph         m_graph;
        ::ShaderLab::Effects::SourceNodeFactory m_sourceFactory;

        // Controllers.
        ::ShaderLab::Controls::ShaderEditorController    m_shaderEditor;
        ::ShaderLab::Controls::NodeGraphController       m_nodeGraphController;
        ::ShaderLab::Controls::PixelInspectorController  m_pixelInspector;
        ::ShaderLab::Controls::PixelTraceController      m_pixelTrace;

        // Render loop timer.
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_renderTimer{ nullptr };
        uint32_t m_frameCount{ 0 };
        std::chrono::steady_clock::time_point m_fpsTimePoint;
        std::chrono::steady_clock::time_point m_lastRenderTick;

        HWND m_hwnd{ nullptr };
        bool m_customEffectsRegistered{ false };
        bool m_isShuttingDown{ false };

        // Video seek slider / position label (updated per-tick while playing).
        winrt::Microsoft::UI::Xaml::Controls::Slider m_videoSeekSlider{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_videoPositionLabel{ nullptr };
        uint32_t m_videoSeekNodeId{ 0 };
        bool m_videoSeekSuppressEvents{ false };

        // Per-node preview.
        uint32_t m_previewNodeId{ 0 };       // 0 = Output node (default)
        std::vector<uint32_t> m_topoOrder;   // cached for [ ] navigation
        bool m_suppressSelectorEvent{ false };

        // Display profile selection.
        std::vector<::ShaderLab::Rendering::DisplayProfile> m_displayPresets;
        std::optional<::ShaderLab::Rendering::DisplayProfile> m_loadedIccProfile;
        int32_t m_committedProfileIndex{ 0 };
        bool m_suppressProfileEvent{ false };

        // Pixel trace.
        bool m_traceActive{ false };
        uint32_t m_traceUnit{ 0 };          // 0=scRGB, 1=sRGB, 2=Nits, 3=PQ
        uint32_t m_lastTraceTopologyHash{ 0 };
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Grid> m_traceRowCache;

        // Split comparison.
        bool m_compareActive{ false };
        uint32_t m_compareNodeId{ 0 };
        float m_splitPosition{ 0.5f };  // 0.0–1.0 within image bounds
        bool m_isDraggingSplit{ false };
        bool m_suppressCompareEvent{ false };

        // Node graph editor rendering.
        winrt::com_ptr<IDXGISwapChain1>     m_graphSwapChain;
        winrt::com_ptr<ID2D1Bitmap1>        m_graphRenderTarget;
        winrt::com_ptr<ID2D1SolidColorBrush> m_graphGridBrush;
        uint32_t m_graphPanelWidth{ 0 };
        uint32_t m_graphPanelHeight{ 0 };

        void InitializeGraphPanel();
        void ResizeGraphPanel(uint32_t w, uint32_t h);
        void RenderNodeGraph();
        D2D1_POINT_2F GraphPanelPointerToCanvas(
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGraphPanelPointerPressed(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGraphPanelPointerMoved(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGraphPanelPointerReleased(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnGraphPanelPointerWheel(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);

        bool m_isGraphPanning{ false };
        D2D1_POINT_2F m_graphPanStart{};
        D2D1_POINT_2F m_graphPanOrigin{};
        void UpdatePropertiesPanel();
        void ShowCurveEditorDialog(uint32_t nodeId, const std::wstring& propertyKey, std::function<void()> markDirty);
        winrt::fire_and_forget BrowseImageForSourceNode(uint32_t nodeId);
        winrt::fire_and_forget BrowseVideoForSourceNode();
        winrt::fire_and_forget BrowseVideoForExistingNode(uint32_t nodeId);
        void OnSaveImageClicked(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget SaveImageAsync();

        // Capture the current preview as a PNG byte buffer.
        // Returns empty vector on failure.
        std::vector<uint8_t> CapturePreviewAsPng();
        void OpenEffectDesigner();
        void EnforceCustomEffectNameUniqueness(uint32_t modifiedNodeId);

        uint32_t m_selectedNodeId{ 0 };
        bool m_isDraggingNode{ false };
        bool m_isDraggingConnection{ false };

        // Effect Designer window.
        winrt::ShaderLab::EffectDesignerWindow m_designerWindow{ nullptr };

        // MCP HTTP server for AI agent integration.
        std::unique_ptr<::ShaderLab::McpHttpServer> m_mcpServer;
        bool m_autoStartMcp{ false };
        ::ShaderLab::Rendering::DevicePreference m_devicePref{ ::ShaderLab::Rendering::DevicePreference::Default };
        void SetupMcpRoutes();
        template<typename F> auto DispatchSync(F&& fn) -> decltype(fn());

        // Column splitter drag state.
        bool m_isDraggingSplitter{ false };
        double m_splitterDragStartX{ 0 };
        double m_splitterStartCol0Width{ 0 };
        double m_splitterStartCol2Width{ 0 };

        // Preview pan/zoom.
        float m_previewPanX{ 0.0f };
        float m_previewPanY{ 0.0f };
        float m_previewZoom{ 1.0f };
        bool  m_needsFitPreview{ false };
        bool  m_forceRender{ true }; // Force first render + after pan/zoom changes
        bool m_isPreviewPanning{ false };
        float m_previewPanStartX{ 0.0f };
        float m_previewPanStartY{ 0.0f };
        float m_previewPanOriginX{ 0.0f };
        float m_previewPanOriginY{ 0.0f };
        bool m_previewDragMoved{ false };
        float m_traceClickDipX{ 0.0f };
        float m_traceClickDipY{ 0.0f };
        float m_traceClickPanX{ 0.0f };
        float m_traceClickPanY{ 0.0f };
        float m_traceClickZoom{ 1.0f };
        bool m_traceOutOfBounds{ false };
        void UpdateCrosshairOverlay();
        void FitPreviewToView();

        // Trace swatch HDR swap chain.
        winrt::com_ptr<IDXGISwapChain1>   m_traceSwapChain;
        winrt::com_ptr<ID2D1Bitmap1>      m_traceSwatchTarget;
        uint32_t m_traceSwatchHeight{ 0 };
        void InitializeTraceSwatchPanel();
        void RenderTraceSwatches();
    };
}

namespace winrt::ShaderLab::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
