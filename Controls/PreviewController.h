#pragma once

#include "pch.h"
#include "../Rendering/PreviewViewState.h"

namespace ShaderLab::Controls
{
    // A click on the preview, in panel DIPs and as a position in the image.
    struct PreviewClick
    {
        float dipX{ 0.0f };
        float dipY{ 0.0f };
        Rendering::PreviewView view;   // the view when it happened
        D2D1_RECT_F imageRect{};       // ImageRect() when it happened
        float normX{ 0.0f };           // clamped to [0, 1]
        float normY{ 0.0f };
        bool outOfBounds{ false };     // the click itself fell outside the image
    };

    struct PreviewHost
    {
        // The previewed image's bounds the render worker last measured.
        std::function<D2D1_RECT_F()> imageBounds;
        // The preview's back-buffer size, used before the panel is laid out.
        std::function<D2D1_SIZE_F()> backBufferSize;
        std::function<void()> requestRender;
        std::function<void(const PreviewClick&)> clicked;
    };

    // Pointer input on the preview panel: left or middle drag pans, the wheel
    // zooms about the pointer, a left double-click re-fits, and a left click
    // without a drag reports the image position under it. The view itself
    // lives in the engine's PreviewViewState, shared with the render worker
    // and MCP. UI thread only.
    class PreviewController
    {
    public:
        void Attach(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel, PreviewHost host);

        Rendering::PreviewViewState& State() { return m_state; }
        const Rendering::PreviewViewState& State() const { return m_state; }

        // The panel's size in DIPs (the back buffer's before layout).
        D2D1_SIZE_F ViewportDips() const;
        // The image's bounds in image DIPs, or the viewport's rect when the
        // image is empty or unbounded (a Flood).
        D2D1_RECT_F ImageRect() const;

    private:
        using PointerArgs = winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs;

        void OnPointerPressed(PointerArgs const& args);
        void OnPointerMoved(PointerArgs const& args);
        void OnPointerReleased(PointerArgs const& args);
        void OnPointerWheel(PointerArgs const& args);
        PreviewClick ClickAt(float dipX, float dipY) const;

        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
        PreviewHost m_host;
        Rendering::PreviewViewState m_state;

        bool m_panning{ false };
        bool m_dragMoved{ false };
        D2D1_POINT_2F m_panStart{};
        Rendering::PreviewView m_panOrigin;
        // Left-button double-click, detected by hand: the press is marked
        // Handled to drive panning, which suppresses XAML's DoubleTapped.
        int64_t m_lastClickMs{ 0 };
        D2D1_POINT_2F m_lastClick{};
    };
}
