#pragma once

#include "pch.h"
#include "PipelineFormat.h"
#include "DisplayMonitor.h"

namespace ShaderLab::Rendering
{
    // Device creation preference.
    enum class DevicePreference
    {
        Default,    // Hardware, fallback to WARP on failure
        Hardware,   // Force hardware GPU (fail if unavailable)
        Warp,       // Force WARP software renderer
    };

    // Core rendering engine: owns D3D11 device, D2D1 device context, and the
    // DXGI swap chain bound to a WinUI 3 SwapChainPanel.
    //
    // Lifecycle:
    //   1. Construct
    //   2. Initialize(hwnd, swapChainPanel) — creates device stack + swap chain
    //   3. Resize(w, h) — called on SizeChanged
    //   4. BeginDraw / EndDraw / Present — per-frame rendering
    //   5. SetPipelineFormat — switch format, recreates swap chain
    //
    // The engine does NOT own the graph evaluator — that lives at a higher level
    // and calls BeginDraw/EndDraw to render into the D2D device context.
    class RenderEngine
    {
    public:
        RenderEngine() = default;
        ~RenderEngine() = default;

        RenderEngine(const RenderEngine&) = delete;
        RenderEngine& operator=(const RenderEngine&) = delete;

        // Create D3D11 device, D2D factory/device/context, swap chain on the panel.
        void Initialize(HWND hwnd,
                        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel,
                        const PipelineFormat& format = FormatScRgbFP16,
                        DevicePreference devicePref = DevicePreference::Default);

        // Release all GPU resources.
        void Shutdown();

        // Respond to SwapChainPanel size changes.
        void Resize(uint32_t widthPixels, uint32_t heightPixels);

        // Switch the entire pipeline to a new format (recreates swap chain + RT).
        void SetPipelineFormat(const PipelineFormat& format);

        // --- Per-frame rendering ---

        // Begins a D2D drawing session on the back-buffer render target.
        // Returns the D2D device context to draw into.
        ID2D1DeviceContext5* BeginDraw();

        // Ends the D2D drawing session.
        void EndDraw();

        // Presents the back buffer to the swap chain.
        void Present(bool vsync = true);

        // --- Accessors ---

        bool IsInitialized() const { return m_d3dDevice != nullptr; }

        ID3D11Device5*          D3DDevice()       const { return m_d3dDevice.get(); }
        ID3D11DeviceContext4*   D3DContext()       const { return m_d3dContext.get(); }
        ID2D1Factory7*          D2DFactory()       const { return m_d2dFactory.get(); }
        ID2D1Device6*           D2DDevice()        const { return m_d2dDevice.get(); }
        ID2D1DeviceContext5*    D2DDeviceContext()  const { return m_d2dDeviceContext.get(); }
        IDXGISwapChain3*        SwapChain()        const { return m_swapChain.get(); }
        IDXGIFactory7*          DXGIFactory()      const { return m_dxgiFactory.get(); }

        const PipelineFormat&   ActiveFormat()     const { return m_format; }
        uint32_t                BackBufferWidth()  const { return m_width; }
        uint32_t                BackBufferHeight() const { return m_height; }

        // GPU / adapter info.
        const std::wstring&     AdapterName()      const { return m_adapterName; }
        bool                    IsWarp()           const { return m_isWarp; }

    private:
        void CreateDeviceResources(DevicePreference devicePref);
        void CreateSwapChain(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel);
        void ConfigureSwapChainColorSpace();
        void CreateRenderTarget();
        void ReleaseRenderTarget();

        // Device stack
        winrt::com_ptr<IDXGIFactory7>           m_dxgiFactory;
        winrt::com_ptr<ID3D11Device5>           m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext4>     m_d3dContext;
        winrt::com_ptr<ID2D1Factory7>           m_d2dFactory;
        winrt::com_ptr<ID2D1Device6>            m_d2dDevice;
        winrt::com_ptr<ID2D1DeviceContext5>      m_d2dDeviceContext;

        // Swap chain
        winrt::com_ptr<IDXGISwapChain3>         m_swapChain;
        winrt::com_ptr<ID2D1Bitmap1>            m_renderTarget;

        // State
        PipelineFormat  m_format{ FormatScRgbFP16 };
        uint32_t        m_width{ 0 };
        uint32_t        m_height{ 0 };
        HWND            m_hwnd{ nullptr };
        std::wstring    m_adapterName;
        bool            m_isWarp{ false };

        // Keep panel reference for swap chain recreation on format change.
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
    };
}
