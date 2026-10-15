#include "pch_engine.h"
#include "PreviewViewState.h"

namespace ShaderLab::Rendering
{
    std::optional<PreviewView> FitPreviewView(const D2D1_RECT_F& bounds, float viewportW, float viewportH)
    {
        if (viewportW <= 0.0f || viewportH <= 0.0f) return std::nullopt;
        const float imageW = bounds.right - bounds.left;
        const float imageH = bounds.bottom - bounds.top;
        if (imageW <= 0.0f || imageH <= 0.0f) return std::nullopt;
        if (imageW > 100000.0f || imageH > 100000.0f) return PreviewView{};

        PreviewView view;
        view.zoom = (std::min)(viewportW / imageW, viewportH / imageH) * 0.95f;
        view.panX = (viewportW - imageW * view.zoom) * 0.5f - bounds.left * view.zoom;
        view.panY = (viewportH - imageH * view.zoom) * 0.5f - bounds.top * view.zoom;
        return view;
    }

    PreviewView ZoomPreviewAbout(const PreviewView& view, float x, float y, float factor)
    {
        const float zoom = std::clamp(view.zoom * factor, 0.01f, 100.0f);
        return { zoom,
                 x - (x - view.panX) * (zoom / view.zoom),
                 y - (y - view.panY) * (zoom / view.zoom) };
    }

    PreviewFrame PreviewViewState::Frame() const
    {
        std::scoped_lock lock(m_mutex);
        return { m_nodeId, m_selectedNodeId, m_view, m_fitPending || m_autoFit, m_version };
    }

    uint32_t PreviewViewState::NodeId() const
    {
        std::scoped_lock lock(m_mutex);
        return m_nodeId;
    }

    uint32_t PreviewViewState::SelectedNodeId() const
    {
        std::scoped_lock lock(m_mutex);
        return m_selectedNodeId;
    }

    PreviewView PreviewViewState::View() const
    {
        std::scoped_lock lock(m_mutex);
        return m_view;
    }

    bool PreviewViewState::AutoFit() const
    {
        std::scoped_lock lock(m_mutex);
        return m_autoFit;
    }

    bool PreviewViewState::FitPending() const
    {
        std::scoped_lock lock(m_mutex);
        return m_fitPending;
    }

    void PreviewViewState::SetSelectedNode(uint32_t nodeId)
    {
        std::scoped_lock lock(m_mutex);
        m_selectedNodeId = nodeId;
    }

    void PreviewViewState::SetView(const PreviewView& view)
    {
        std::scoped_lock lock(m_mutex);
        m_view = view;
        m_autoFit = false;
        EditedLocked();
    }

    void PreviewViewState::ZoomAbout(float x, float y, float factor)
    {
        std::scoped_lock lock(m_mutex);
        m_view = ZoomPreviewAbout(m_view, x, y, factor);
        m_autoFit = false;
        EditedLocked();
    }

    void PreviewViewState::RequestFit(bool keepFitted)
    {
        std::scoped_lock lock(m_mutex);
        if (keepFitted) m_autoFit = true;
        m_fitPending = true;
        EditedLocked();
    }

    void PreviewViewState::FitNow(const D2D1_RECT_F& bounds)
    {
        std::scoped_lock lock(m_mutex);
        m_autoFit = true;
        if (const auto fitted = FitPreviewView(bounds, m_viewportW, m_viewportH))
        {
            m_view = *fitted;
            m_fitPending = false;
        }
        else
        {
            m_fitPending = true;
        }
        EditedLocked();
    }

    void PreviewViewState::SelectNode(uint32_t nodeId)
    {
        std::scoped_lock lock(m_mutex);
        if (nodeId == m_nodeId) return;
        if (m_nodeId != 0)
            m_remembered[m_nodeId] = { m_view, m_autoFit };
        m_nodeId = nodeId;
        auto it = nodeId != 0 ? m_remembered.find(nodeId) : m_remembered.end();
        if (it != m_remembered.end())
        {
            m_view = it->second.view;
            m_autoFit = it->second.autoFit;
            m_fitPending = it->second.autoFit;
        }
        else
        {
            m_autoFit = true;
            m_fitPending = true;
        }
        EditedLocked();
    }

    void PreviewViewState::Reset()
    {
        std::scoped_lock lock(m_mutex);
        m_nodeId = 0;
        m_remembered.clear();
        m_fitPending = true;
        EditedLocked();
    }

    void PreviewViewState::ClearNode()
    {
        std::scoped_lock lock(m_mutex);
        m_nodeId = 0;
        EditedLocked();
    }

    void PreviewViewState::Restore(uint32_t nodeId, const PreviewView& view)
    {
        std::scoped_lock lock(m_mutex);
        m_nodeId = nodeId;
        m_view = view;
        m_fitPending = false;
        EditedLocked();
    }

    void PreviewViewState::SetViewport(float width, float height)
    {
        std::scoped_lock lock(m_mutex);
        m_viewportW = width;
        m_viewportH = height;
    }

    bool PreviewViewState::ApplyFit(const PreviewFrame& frame, const PreviewView& fitted)
    {
        std::scoped_lock lock(m_mutex);
        // An edit since the frame began wins: its own frame will fit if it wants one.
        if (frame.version != m_version) return false;
        m_fitPending = false;
        if (fitted == m_view) return false;
        m_view = fitted;
        EditedLocked();
        return true;
    }

    bool PreviewViewState::FitTo(const PreviewFrame& frame, const D2D1_RECT_F& bounds)
    {
        float viewportW = 0.0f, viewportH = 0.0f;
        {
            std::scoped_lock lock(m_mutex);
            viewportW = m_viewportW;
            viewportH = m_viewportH;
        }
        const auto fitted = FitPreviewView(bounds, viewportW, viewportH);
        return fitted && ApplyFit(frame, *fitted);
    }
}
