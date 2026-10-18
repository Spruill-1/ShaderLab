#pragma once

#include "pch_engine.h"

#include <mutex>
#include <unordered_map>

// Which node the preview shows and how (pan / zoom / auto-fit). The UI and
// MCP edit it; the render worker takes one consistent copy per frame and
// posts its fits back, so no field is written by two threads unguarded.
namespace ShaderLab::Rendering
{
    // Pan and zoom in DIPs: screen = image * zoom + pan.
    struct PreviewView
    {
        float zoom{ 1.0f };
        float panX{ 0.0f };
        float panY{ 0.0f };

        bool operator==(const PreviewView&) const = default;
    };

    // The view that shows `bounds` (image DIPs) whole and centred in a
    // viewport, at 95% of the tighter axis. An unbounded image (wider or
    // taller than 100000) gets zoom 1 at the origin. Nothing while the
    // viewport or the image is empty.
    std::optional<PreviewView> FitPreviewView(const D2D1_RECT_F& bounds,
                                                            float viewportW, float viewportH);

    // The view after zooming by `factor` about a screen point, zoom clamped
    // to [0.01, 100].
    PreviewView ZoomPreviewAbout(const PreviewView& view, float x, float y, float factor);

    // What one preview frame is drawn with, read once at its start.
    struct PreviewFrame
    {
        uint32_t nodeId{ 0 };
        uint32_t selectedNodeId{ 0 };   // the Properties panel's node: its readbacks stay live
        PreviewView view;
        bool fitWanted{ false };        // a fit is pending, or auto-fit is on
        uint64_t version{ 0 };          // bumped by every edit
    };

    class PreviewViewState
    {
    public:
        PreviewFrame Frame() const;
        uint32_t NodeId() const;
        uint32_t SelectedNodeId() const;
        PreviewView View() const;
        bool AutoFit() const;
        bool FitPending() const;

        void SetSelectedNode(uint32_t nodeId);

        // An explicit view (MCP, pan, wheel) ends auto-fit.
        void SetView(const PreviewView& view);
        void ZoomAbout(float x, float y, float factor);
        // Asks for a fit on the next frame; `keepFitted` also turns auto-fit on.
        void RequestFit(bool keepFitted = true);
        // Turns auto-fit on and fits to `bounds` now; a fit that cannot be
        // computed yet is left to the next frame.
        void FitNow(const D2D1_RECT_F& bounds);
        // Shows nodeId: the outgoing node's view is remembered, the incoming
        // node's restored, or fitted when it has never been shown.
        void SelectNode(uint32_t nodeId);
        // No node and no remembered views; the next node shown fits.
        void Reset();
        // No node; views and auto-fit are kept.
        void ClearNode();
        // Restores a node and view captured before a device rebuild.
        void Restore(uint32_t nodeId, const PreviewView& view);

        // The panel's size in DIPs, cached by the UI for the worker's fit.
        void SetViewport(float width, float height);

        // Render thread: applies a fit computed for `frame` unless the view
        // was edited since. Returns true when the view moved.
        bool ApplyFit(const PreviewFrame& frame, const PreviewView& fitted);
        // Render thread: fits to `bounds` for `frame` (FitPreviewView with the
        // cached viewport). Returns true when the view moved; a fit that
        // cannot be computed yet stays pending.
        bool FitTo(const PreviewFrame& frame, const D2D1_RECT_F& bounds);

    private:
        struct Remembered
        {
            PreviewView view;
            bool autoFit{ true };
        };

        void EditedLocked() { ++m_version; }

        mutable std::mutex m_mutex;
        uint32_t m_nodeId{ 0 };
        uint32_t m_selectedNodeId{ 0 };
        PreviewView m_view;
        bool m_autoFit{ true };
        bool m_fitPending{ false };
        uint64_t m_version{ 0 };
        float m_viewportW{ 0.0f };
        float m_viewportH{ 0.0f };
        std::unordered_map<uint32_t, Remembered> m_remembered;
    };
}
