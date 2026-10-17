#pragma once

#include "pch.h"
#include "Rendering/PixelTrace.h"
#include "../Rendering/PreviewViewState.h"

namespace ShaderLab::Controls
{
    // The Pixel Trace tab: one row per node of a trace (name, pin, value in
    // the chosen unit, luminance) and the crosshair on the preview marking
    // the traced point. The trace itself is Rendering::TracePixel's, run by
    // the host on the render thread. UI thread only.
    class PixelTracePanelController
    {
    public:
        void Attach(winrt::Microsoft::UI::Xaml::Controls::StackPanel const& rows,
                    winrt::Microsoft::UI::Xaml::Controls::TextBlock const& positionText,
                    winrt::Microsoft::UI::Xaml::Controls::ComboBox const& unitSelector,
                    winrt::Microsoft::UI::Xaml::Controls::Canvas const& crosshair);

        // Shows a trace (nothing when the read failed). `position` labels it;
        // `outOfBounds` adds the nearest-edge warning.
        void Show(const std::optional<Rendering::PixelTraceNode>& trace, const std::wstring& position,
                  bool outOfBounds);
        // Remembers the traced point: where the click was, under which view.
        void SetMarker(float dipX, float dipY, const Rendering::PreviewView& viewAtClick);
        // Draws the crosshair through `current` when `visible` and a trace is shown.
        void UpdateCrosshair(const Rendering::PreviewView& current, bool visible);
        // Back to "click to trace" (a new graph).
        void Reset();

    private:
        void BuildRows(const Rendering::PixelTraceNode& root, bool outOfBounds);
        void UpdateRowText(const Rendering::PixelTraceNode& root);
        winrt::Microsoft::UI::Xaml::Controls::Grid CreateRow(const Rendering::PixelTraceNode& node) const;

        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_rows{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_positionText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::ComboBox m_unitSelector{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Canvas m_crosshair{ nullptr };

        std::optional<Rendering::PixelTraceNode> m_trace;
        uint32_t m_unit{ 0 };   // 0 scRGB, 1 sRGB, 2 nits, 3 PQ
        // Rows in pre-order, so a unit change only rewrites their text.
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Grid> m_rowCache;
        bool m_hasMarker{ false };
        float m_markerDipX{ 0.0f };
        float m_markerDipY{ 0.0f };
        Rendering::PreviewView m_markerView;
    };
}
