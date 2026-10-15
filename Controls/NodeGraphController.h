#pragma once

#include "pch.h"
#include "NodeLayout.h"
#include "NodeGraphRenderer.h"
#include "../Graph/GraphModel.h"
#include "../Graph/GraphUiSnapshot.h"
#include "../Rendering/RenderThreadDispatcher.h"

namespace ShaderLab::Controls
{
    // The node-graph editor: laid-out node visuals, selection, node / slider
    // / connection drags, hit-testing and the graph edits they make. Painting
    // is NodeGraphRenderer's; pointer routing is GraphCanvasInputController's.
    //
    // ---- THREADING RULE: how this controller touches graph data ----
    //
    //  1. READS on the UI thread -> the published GraphUiSnapshot only. The
    //     render worker mutates the live graph continuously, so a direct read
    //     races it (an access violation inside std::map, not a compile error).
    //     Hold the snapshot for the whole read.
    //
    //  2. WRITES -> on the render thread, through the dispatcher. Every
    //     command below dispatches its own write and relays out inside the
    //     same closure.
    //
    //  3. LAYOUT (RebuildLayout / AutoLayout) reads the live graph, so it runs
    //     on the render thread, or while the worker is stopped (startup, an
    //     adapter switch). Other threads call RequestRelayout /
    //     RequestAutoLayout, which dispatch.
    class NodeGraphController
    {
    public:
        using SnapshotProvider = std::function<std::shared_ptr<const Graph::GraphUiSnapshot>()>;

        // Must precede every other call.
        void Attach(Graph::GraphModel& graph, Rendering::RenderThreadDispatcher& dispatcher,
                    SnapshotProvider snapshots);

        // Args: srcNodeId, srcPin, dstNodeId, dstPin, isDataBinding.
        using ConnectionCallback = std::function<void(uint32_t, uint32_t, uint32_t, uint32_t, bool)>;
        void SetConnectionCallback(ConnectionCallback cb) { m_connectionCallback = std::move(cb); }

        // ---- Layout ----

        // Recomputes every node's visual from the live graph. Layout thread only.
        void RebuildLayout();
        // Moves every node to its ComputeAutoLayout place, then relays out.
        // Layout thread only; a graph with a cycle keeps its positions.
        void AutoLayout();
        // The same from any other thread: run on the render thread and wait.
        void RequestRelayout();
        void RequestAutoLayout();

        // ---- View ----

        void SetPanOffset(float x, float y);
        D2D1_POINT_2F PanOffset() const { return m_panOffset; }
        void SetZoom(float zoom);   // clamped to [0.1, 5]
        float Zoom() const { return m_zoom; }
        D2D1_POINT_2F ScreenToCanvas(D2D1_POINT_2F screenPoint) const;
        // Used to place auto-positioned new nodes in the current view (DIPs).
        void SetViewportSize(float w, float h) { m_viewportW = w; m_viewportH = h; }
        // Annotates each node with its GPU cost. UI thread only.
        void SetShowNodeGpuStats(bool on);
        bool ShowNodeGpuStats() const { return m_showNodeGpuStats; }
        // Bounding box of every node visual in canvas space; {0,0,0,0} when empty.
        D2D1_RECT_F ContentBounds() const;

        // ---- Hit tests (UI thread, canvas space) ----

        uint32_t HitTestNode(D2D1_POINT_2F canvasPoint) const;
        std::optional<PinRef> HitTestPin(D2D1_POINT_2F canvasPoint) const;
        uint32_t HitTestSlider(D2D1_POINT_2F canvasPoint) const;
        uint32_t HitTestPlayButton(D2D1_POINT_2F canvasPoint) const;
        bool IsParameterNode(uint32_t nodeId) const;

        struct EdgeHit
        {
            bool found{ false };
            bool isDataBinding{ false };
            uint32_t sourceNodeId{ 0 };
            uint32_t sourcePin{ 0 };        // image edges
            uint32_t destNodeId{ 0 };
            uint32_t destPin{ 0 };          // image edges
            std::wstring destPropertyName;  // data bindings
            std::wstring sourceFieldName;   // data bindings
        };
        // The edge or binding curve closest to the point within tolerance.
        EdgeHit HitTestEdge(D2D1_POINT_2F canvasPoint, float tolerance = 6.0f) const;

        // ---- Commands (UI thread) ----

        // Removes the edge, or the binding components that read that field.
        bool RemoveEdge(const EdgeHit& hit);
        // Starts or pauses a clock; returns its new state, or nothing if it is gone.
        std::optional<bool> TogglePlaying(uint32_t nodeId);

        // A slider drag writes the latest position on the render thread
        // without waiting; EndSliderDrag waits for the last write. Update
        // returns true when the position moved.
        void BeginSliderDrag(uint32_t nodeId, D2D1_POINT_2F canvasPoint);
        bool UpdateSliderDrag(D2D1_POINT_2F canvasPoint);
        void EndSliderDrag();
        uint32_t SliderDragNodeId() const { return m_sliderDrag.nodeId; }

        // Moves the selection the same way: visuals at once, the graph's
        // positions coalesced into one queued write.
        void BeginDragNodes(D2D1_POINT_2F startPoint);
        void UpdateDragNodes(D2D1_POINT_2F currentPoint);
        void EndDragNodes();

        void BeginConnection(const PinRef& from);
        void UpdateConnection(D2D1_POINT_2F currentPoint);
        // Connects output to input of the same pin type; true when it did.
        bool EndConnection(D2D1_POINT_2F dropPoint);
        void CancelConnection();

        void SelectNode(uint32_t nodeId, bool addToSelection = false);
        void DeselectAll();
        void SelectAll();
        const std::unordered_set<uint32_t>& SelectedNodes() const { return m_selection; }
        void DeleteSelected();

        // Adds a node at a canvas position, or in the current view when the
        // position is (0, 0). Returns its id.
        uint32_t AddNode(Graph::EffectNode node, D2D1_POINT_2F canvasPos);

        // Adds a copy of each (original id, node) in the current view and
        // reconnects the edges between the copies.
        void PasteNodes(const std::vector<std::pair<uint32_t, Graph::EffectNode>>& nodes,
                        std::span<const Graph::EffectEdge> edges);

        // ---- Paint (UI thread) ----

        void Render(ID2D1DeviceContext* dc);
        void ReleaseDeviceResources();
        void SetNeedsRedraw() { m_needsRedraw.store(true); }
        bool NeedsRedraw() const { return m_needsRedraw.load(); }

    private:
        bool OnLayoutThread() const;
        void CheckLayoutThread() const;
        std::shared_ptr<const Graph::GraphUiSnapshot> Snapshot() const
        {
            return m_snapshotProvider ? m_snapshotProvider() : nullptr;
        }

        // Applies the queued drag writes. Render thread.
        void FlushDragWrites();
        // Queues one FlushDragWrites unless one is already queued.
        void ScheduleDragFlush();

        Graph::GraphModel* m_graph{ nullptr };
        Rendering::RenderThreadDispatcher* m_dispatcher{ nullptr };
        SnapshotProvider m_snapshotProvider;
        ConnectionCallback m_connectionCallback;

        // Guards m_visuals, which RebuildLayout writes on the render thread
        // while the UI paints and hit-tests from it.
        //
        // Never hold this across a DispatchSync (a dispatched closure that
        // relayouts takes it on the render thread, so waiting on that closure
        // while holding this deadlocks), never read
        // m_visuals inside a dispatched closure, and copy values out rather
        // than keep references across a dispatch. RebuildLayout takes it
        // itself, so no holder may call it.
        mutable std::shared_mutex m_visualsMutex;
        std::unordered_map<uint32_t, NodeVisual> m_visuals;

        // UI-thread state.
        std::unordered_set<uint32_t> m_selection;
        bool m_draggingNodes{ false };
        D2D1_POINT_2F m_dragStart{};
        D2D1_POINT_2F m_dragOffset{};
        struct SliderDrag
        {
            uint32_t nodeId{ 0 };
            bool isClock{ false };
            D2D1_RECT_F rect{};
            double lastT{ -1.0 };
        } m_sliderDrag;
        ConnectionDrag m_connectionDrag;

        // Drag writes waiting for the render thread; FlushDragWrites takes them.
        struct DragWrites
        {
            std::mutex mutex;
            bool queued{ false };
            std::vector<uint32_t> moveIds;
            float dx{ 0.0f };
            float dy{ 0.0f };
            uint32_t sliderNodeId{ 0 };
            bool sliderIsClock{ false };
            std::optional<double> sliderT;
        } m_dragWrites;

        D2D1_POINT_2F m_panOffset{ 0.0f, 0.0f };
        float m_zoom{ 1.0f };
        int m_autoStaggerCount{ 0 };
        float m_viewportW{ 800.0f };
        float m_viewportH{ 600.0f };
        std::atomic<bool> m_showNodeGpuStats{ false };

        NodeGraphRenderer m_renderer;
        // Set from the render thread too (layout rebuilds, compute results).
        std::atomic<bool> m_needsRedraw{ true };
    };
}
