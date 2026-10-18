#pragma once

#include "pch.h"
#include "Rendering/PipelineFormat.h"
#include "OutputSinkRenderState.h"

namespace ShaderLab::Controls
{
    // A lightweight output window that presents a single graph node's output
    // in its own OS window with pan/zoom controls and FPS ticker.
    class OutputWindow
    {
    public:
        OutputWindow() = default;
        ~OutputWindow();

        OutputWindow(const OutputWindow&) = delete;
        OutputWindow& operator=(const OutputWindow&) = delete;

        void Create(
            ID3D11Device5* d3dDevice,
            ID2D1DeviceContext5* dc,
            IDXGIFactory7* dxgiFactory,
            uint32_t nodeId,
            const std::wstring& nodeName,
            const Rendering::PipelineFormat& format);

        // Blits the worker's latest frame (Controls/OutputSinkRenderer) into
        // this window's swap chain, fitted or at the user's pan / zoom, and
        // presents. UI thread only; does nothing when nothing changed.
        void BlitAndPresent(ID2D1DeviceContext5* uiDc);

        // What the render worker draws into; it holds its own reference.
        std::shared_ptr<OutputSinkRenderState> Sink() const { return m_sink; }

        void Close();

        bool IsOpen() const { return m_isOpen; }
        bool IsReady() const { return m_swapChain != nullptr; }
        uint32_t NodeId() const { return m_nodeId; }
        void SetTitle(const std::wstring& title);
        // Push the canonical "NN fps | NN.N ms" status string from the main
        // window. Each render tick presents to all output windows
        // synchronously, so the main window's FPS *is* this window's FPS --
        // no point recomputing it per-window.
        void SetStatusText(const std::wstring& text);
        // Push the multi-line per-phase breakdown the main window shows in
        // its FPS-tooltip flyout. Used as the hover tooltip on this
        // window's status bar.
        void SetStatusTooltip(const std::wstring& tooltip);

    private:
        void CreateSwapChain();
        void ApplyInverseCompositionScale();
        void OnPanelSizeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
        winrt::fire_and_forget SaveImageAsync();

        // Shared (non-owning).
        ID3D11Device5* m_d3dDevice{ nullptr };
        IDXGIFactory7* m_dxgiFactory{ nullptr };
        // The engine's default context, which SaveImageAsync reads back with;
        // blits use the UI context passed to BlitAndPresent.
        winrt::com_ptr<ID2D1DeviceContext5> m_saveDc;

        // Owned per-window resources.
        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_fpsText{ nullptr };
        winrt::com_ptr<IDXGISwapChain3> m_swapChain;
        float m_appliedScaleX{ 0.0f };   // scale the inverse matrix was built for
        float m_appliedScaleY{ 0.0f };

        std::shared_ptr<OutputSinkRenderState> m_sink;

        // The last blitted frame, for Save.
        winrt::com_ptr<ID2D1Image> m_lastImage;

        // Expires with the window; SaveImageAsync checks it after resuming.
        std::shared_ptr<bool> m_alive{ std::make_shared<bool>(true) };
        std::wstring m_nodeName;

        Rendering::PipelineFormat m_format{ Rendering::FormatScRgbFP16 };
        uint32_t m_nodeId{ 0 };
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };
        bool m_isOpen{ false };
        bool m_needsResize{ false };
        bool m_needsFit{ true };
        bool m_autoFit{ true };  // Auto-fit until user zooms/pans

        // Pan / zoom state.
        float m_zoom{ 1.0f };
        float m_panX{ 0.0f };
        float m_panY{ 0.0f };
        bool m_isPanning{ false };
        float m_panStartX{ 0.0f };
        float m_panStartY{ 0.0f };
        float m_panOriginX{ 0.0f };
        float m_panOriginY{ 0.0f };

        // Last presented state. BlitAndPresent returns early unless the worker
        // published a new frame or the view changed: Present1 with sync
        // interval 1 blocks the UI thread until vblank, and doing it every
        // tick for every window with nothing new to show cost up to one
        // refresh period of input latency per window per tick.
        uint64_t m_lastBlittedVersion{ ~0ull };
        struct PresentedView
        {
            uint32_t w{ 0 }, h{ 0 };
            float    scale{ 0 }, zoom{ 0 }, panX{ 0 }, panY{ 0 };
            bool operator==(const PresentedView&) const = default;
        };
        PresentedView m_lastPresentedView;

        // XAML is only touched when the text actually changes; the tooltip's
        // TextBlock is created once. Rebuilding it per tick allocated a XAML
        // element per window per frame.
        std::wstring m_lastTitle;
        std::wstring m_lastStatusText;
        std::wstring m_lastTooltip;
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_tooltipBlock{ nullptr };

        // Event tokens for cleanup.
        winrt::event_token m_sizeChangedToken{};
        winrt::event_token m_closedToken{};
    };
}
