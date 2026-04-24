#pragma once

#include "pch.h"
#include "../Rendering/PipelineFormat.h"
#include "../Rendering/ToneMapper.h"

namespace ShaderLab::Controls
{
    // A lightweight output window that presents a single graph node's output
    // in its own OS window. Shares the D2D device context from the main
    // RenderEngine but owns its own swap chain and render target.
    //
    // Lifecycle:
    //   1. Create(renderEngine, nodeId, nodeName)
    //   2. Per-frame: Present(dc, nodeOutput)  — called from main render loop
    //   3. Close() or destroyed
    class OutputWindow
    {
    public:
        OutputWindow() = default;
        ~OutputWindow();

        OutputWindow(const OutputWindow&) = delete;
        OutputWindow& operator=(const OutputWindow&) = delete;

        // Create the WinUI 3 window and swap chain panel.
        // Swap chain creation is deferred until the panel is loaded.
        void Create(
            ID3D11Device5* d3dDevice,
            ID2D1DeviceContext5* dc,
            IDXGIFactory7* dxgiFactory,
            uint32_t nodeId,
            const std::wstring& nodeName,
            const Rendering::PipelineFormat& format);

        // Render a node's output image to this window's swap chain.
        // Must be called between the main window's EndDraw and before
        // any other BeginDraw call (we temporarily retarget the shared DC).
        void Present(ID2D1DeviceContext5* dc, ID2D1Image* image);

        // Close the window and release resources.
        void Close();

        // Whether the window has been created and not closed.
        bool IsOpen() const { return m_isOpen; }

        // Whether the swap chain is ready for rendering.
        bool IsReady() const { return m_swapChain != nullptr; }

        // The graph node ID this window displays.
        uint32_t NodeId() const { return m_nodeId; }

        // Update the title bar text.
        void SetTitle(const std::wstring& title);

    private:
        void CreateSwapChain();
        void CreateRenderTarget();
        void ReleaseRenderTarget();
        void OnPanelSizeChanged(
            winrt::Windows::Foundation::IInspectable const& sender,
            winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& args);

        // Shared (non-owning).
        ID3D11Device5* m_d3dDevice{ nullptr };
        IDXGIFactory7* m_dxgiFactory{ nullptr };
        ID2D1DeviceContext5* m_dc{ nullptr };

        // Owned per-window resources.
        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
        winrt::com_ptr<IDXGISwapChain3> m_swapChain;
        winrt::com_ptr<ID2D1Bitmap1> m_renderTarget;

        Rendering::PipelineFormat m_format{ Rendering::FormatScRgbFP16 };
        uint32_t m_nodeId{ 0 };
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };
        bool m_isOpen{ false };
        bool m_needsResize{ false };

        // Event tokens for cleanup.
        winrt::event_token m_sizeChangedToken{};
        winrt::event_token m_closedToken{};
    };
}
