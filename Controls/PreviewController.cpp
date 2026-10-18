#include "pch.h"
#include "PreviewController.h"

namespace ShaderLab::Controls
{
    namespace
    {
        constexpr float cDragThresholdDips = 5.0f;

        bool IsUnbounded(float w, float h)
        {
            return w <= 0.0f || h <= 0.0f || w > 100000.0f || h > 100000.0f;
        }
    }

    void PreviewController::Attach(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel, PreviewHost host)
    {
        m_panel = panel;
        m_host = std::move(host);
        panel.PointerPressed([this](auto&&, PointerArgs const& args) { OnPointerPressed(args); });
        panel.PointerMoved([this](auto&&, PointerArgs const& args) { OnPointerMoved(args); });
        panel.PointerReleased([this](auto&&, PointerArgs const& args) { OnPointerReleased(args); });
        panel.PointerWheelChanged([this](auto&&, PointerArgs const& args) { OnPointerWheel(args); });
    }

    D2D1_SIZE_F PreviewController::ViewportDips() const
    {
        // The preview renders at 96 DPI, so the D2D viewport matches WinUI DIPs.
        float w = static_cast<float>(m_panel.ActualWidth());
        float h = static_cast<float>(m_panel.ActualHeight());
        const auto backBuffer = m_host.backBufferSize();
        if (w <= 0) w = backBuffer.width;
        if (h <= 0) h = backBuffer.height;
        return { w, h };
    }

    D2D1_RECT_F PreviewController::ImageRect() const
    {
        const auto bounds = m_host.imageBounds();
        if (!IsUnbounded(bounds.right - bounds.left, bounds.bottom - bounds.top))
            return bounds;
        const auto viewport = ViewportDips();
        return { 0.0f, 0.0f, viewport.width, viewport.height };
    }

    PreviewClick PreviewController::ClickAt(float dipX, float dipY) const
    {
        PreviewClick click;
        click.dipX = dipX;
        click.dipY = dipY;
        click.view = m_state.View();
        click.imageRect = ImageRect();
        // The image is drawn at identity in its own DIPs under the view.
        const float imageX = (dipX - click.view.panX) / click.view.zoom;
        const float imageY = (dipY - click.view.panY) / click.view.zoom;
        const float w = click.imageRect.right - click.imageRect.left;
        const float h = click.imageRect.bottom - click.imageRect.top;
        const float rawX = w > 0.0f ? (imageX - click.imageRect.left) / w : 0.0f;
        const float rawY = h > 0.0f ? (imageY - click.imageRect.top) / h : 0.0f;
        click.outOfBounds = rawX < 0.0f || rawX > 1.0f || rawY < 0.0f || rawY > 1.0f;
        click.normX = std::clamp(rawX, 0.0f, 1.0f);
        click.normY = std::clamp(rawY, 0.0f, 1.0f);
        return click;
    }

    void PreviewController::OnPointerPressed(PointerArgs const& args)
    {
        const auto point = args.GetCurrentPoint(m_panel);
        const auto properties = point.Properties();
        const float x = static_cast<float>(point.Position().X);
        const float y = static_cast<float>(point.Position().Y);

        // A left double-click re-fits: the fit itself waits for the node's
        // bounds, so it is requested rather than done here. The OS
        // thresholds are physical pixels; positions are DIPs.
        if (properties.IsLeftButtonPressed())
        {
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const float dpiScale = (std::max)(1.0f, m_panel.CompositionScaleX());
            const float slopX = static_cast<float>(::GetSystemMetrics(SM_CXDOUBLECLK)) / dpiScale;
            const float slopY = static_cast<float>(::GetSystemMetrics(SM_CYDOUBLECLK)) / dpiScale;
            const bool isDoubleClick =
                (nowMs - m_lastClickMs) <= static_cast<int64_t>(::GetDoubleClickTime()) &&
                std::abs(x - m_lastClick.x) <= slopX && std::abs(y - m_lastClick.y) <= slopY;
            m_lastClickMs = nowMs;
            m_lastClick = { x, y };
            if (isDoubleClick)
            {
                m_state.RequestFit();
                m_host.requestRender();
                m_lastClickMs = 0;   // a third click must not re-trigger off the second
                args.Handled(true);
                return;
            }
        }

        if (properties.IsMiddleButtonPressed() || properties.IsLeftButtonPressed())
        {
            m_panning = true;
            m_dragMoved = false;
            m_panStart = { x, y };
            m_panOrigin = m_state.View();
            m_panel.CapturePointer(args.Pointer());
            args.Handled(true);
        }
    }

    // A pan applies once the pointer has moved past the click threshold.
    void PreviewController::OnPointerMoved(PointerArgs const& args)
    {
        if (!m_panning) return;
        const auto position = args.GetCurrentPoint(m_panel).Position();
        const float dx = static_cast<float>(position.X) - m_panStart.x;
        const float dy = static_cast<float>(position.Y) - m_panStart.y;
        if (std::abs(dx) > cDragThresholdDips || std::abs(dy) > cDragThresholdDips)
            m_dragMoved = true;
        if (m_dragMoved)
        {
            m_state.SetView({ m_panOrigin.zoom, m_panOrigin.panX + dx, m_panOrigin.panY + dy });
            m_host.requestRender();
        }
        args.Handled(true);
    }

    void PreviewController::OnPointerReleased(PointerArgs const& args)
    {
        if (!m_panning) return;
        m_panning = false;
        m_panel.ReleasePointerCapture(args.Pointer());
        if (m_dragMoved) return;
        const auto position = args.GetCurrentPoint(m_panel).Position();
        m_host.clicked(ClickAt(static_cast<float>(position.X), static_cast<float>(position.Y)));
    }

    void PreviewController::OnPointerWheel(PointerArgs const& args)
    {
        const auto point = args.GetCurrentPoint(m_panel);
        const int delta = point.Properties().MouseWheelDelta();
        m_state.ZoomAbout(static_cast<float>(point.Position().X), static_cast<float>(point.Position().Y),
                          delta > 0 ? 1.1f : 1.0f / 1.1f);
        m_host.requestRender();
        args.Handled(true);
    }
}
