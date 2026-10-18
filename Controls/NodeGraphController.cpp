#include "pch.h"
#include "NodeGraphController.h"
#include "Graph/ClockModel.h"
#include "Graph/GraphEdits.h"
#include "Graph/GraphLayout.h"
#include "Graph/ParameterModel.h"

namespace ShaderLab::Controls
{
    namespace
    {
        bool Contains(const D2D1_RECT_F& rect, D2D1_POINT_2F p, float slopX = 0.0f, float slopY = 0.0f)
        {
            return p.x >= rect.left - slopX && p.x <= rect.right + slopX &&
                   p.y >= rect.top - slopY && p.y <= rect.bottom + slopY;
        }

        // Squared distance from a point to a cubic bezier, sampled.
        float DistanceToCurveSqr(D2D1_POINT_2F p, const EdgeCurve& curve)
        {
            constexpr int cSamples = 24;
            float best = (std::numeric_limits<float>::max)();
            for (int i = 0; i <= cSamples; ++i)
            {
                const float t = static_cast<float>(i) / cSamples;
                const float u = 1.0f - t;
                const float bx = u*u*u*curve.start.x + 3*u*u*t*curve.cp1.x + 3*u*t*t*curve.cp2.x + t*t*t*curve.end.x;
                const float by = u*u*u*curve.start.y + 3*u*u*t*curve.cp1.y + 3*u*t*t*curve.cp2.y + t*t*t*curve.end.y;
                const float dx = p.x - bx, dy = p.y - by;
                best = (std::min)(best, dx*dx + dy*dy);
            }
            return best;
        }
    }

    void NodeGraphController::Attach(Graph::GraphModel& graph, Rendering::RenderThreadDispatcher& dispatcher,
                                     SnapshotProvider snapshots)
    {
        m_graph = &graph;
        m_dispatcher = &dispatcher;
        m_snapshotProvider = std::move(snapshots);
    }

    // -----------------------------------------------------------------------
    // Layout
    // -----------------------------------------------------------------------

    bool NodeGraphController::OnLayoutThread() const
    {
        return m_dispatcher && (m_dispatcher->IsConsumerThread() || !m_dispatcher->HasConsumer());
    }

    void NodeGraphController::CheckLayoutThread() const
    {
        if (OnLayoutThread()) return;
        OutputDebugStringW(L"[NodeGraph] layout called off the render thread\n");
        assert(!"NodeGraphController layout called off the render thread");
    }

    void NodeGraphController::RebuildLayout()
    {
        if (!m_graph) return;
        CheckLayoutThread();
        std::unordered_map<uint32_t, NodeVisual> visuals;
        for (const auto& node : m_graph->Nodes())
            visuals.emplace(node.id, ComputeNodeVisual(node));
        {
            std::unique_lock lock(m_visualsMutex);
            m_visuals = std::move(visuals);
        }
        m_needsRedraw = true;
    }

    void NodeGraphController::AutoLayout()
    {
        if (!m_graph) return;
        CheckLayoutThread();
        const auto positions = Graph::ComputeAutoLayout(*m_graph, [](const Graph::EffectNode& node)
        {
            const auto bounds = ComputeNodeVisual(node).bounds;
            return bounds.bottom - bounds.top;
        });
        if (positions)
        {
            for (auto& node : m_graph->MutableNodes())
                if (auto it = positions->find(node.id); it != positions->end())
                    ::ShaderLab::Graph::SetNodePosition(node, it->second);
        }
        RebuildLayout();
    }

    void NodeGraphController::RequestRelayout()
    {
        if (m_dispatcher) m_dispatcher->DispatchSync([this] { RebuildLayout(); });
    }

    void NodeGraphController::RequestAutoLayout()
    {
        if (m_dispatcher) m_dispatcher->DispatchSync([this] { AutoLayout(); });
    }

    // -----------------------------------------------------------------------
    // View
    // -----------------------------------------------------------------------

    void NodeGraphController::SetPanOffset(float x, float y)
    {
        m_panOffset = { x, y };
        m_needsRedraw = true;
    }

    void NodeGraphController::SetZoom(float zoom)
    {
        m_zoom = (std::max)(0.1f, (std::min)(zoom, 5.0f));
        m_needsRedraw = true;
    }

    D2D1_POINT_2F NodeGraphController::ScreenToCanvas(D2D1_POINT_2F screenPoint) const
    {
        return { (screenPoint.x - m_panOffset.x) / m_zoom, (screenPoint.y - m_panOffset.y) / m_zoom };
    }

    void NodeGraphController::SetShowNodeGpuStats(bool on)
    {
        if (m_showNodeGpuStats == on) return;
        m_showNodeGpuStats = on;
        m_needsRedraw = true;
    }

    D2D1_RECT_F NodeGraphController::ContentBounds() const
    {
        std::shared_lock lock(m_visualsMutex);
        std::optional<D2D1_RECT_F> acc;
        for (const auto& [id, v] : m_visuals)
        {
            const auto& b = v.bounds;
            if (b.right <= b.left || b.bottom <= b.top) continue;
            if (!acc)
            {
                acc = b;
                continue;
            }
            acc->left   = (std::min)(acc->left,   b.left);
            acc->top    = (std::min)(acc->top,    b.top);
            acc->right  = (std::max)(acc->right,  b.right);
            acc->bottom = (std::max)(acc->bottom, b.bottom);
        }
        return acc.value_or(D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f));
    }

    // -----------------------------------------------------------------------
    // Hit tests
    // -----------------------------------------------------------------------

    uint32_t NodeGraphController::HitTestNode(D2D1_POINT_2F canvasPoint) const
    {
        std::shared_lock lock(m_visualsMutex);
        uint32_t hitId = 0;
        for (const auto& [id, v] : m_visuals)
            if (Contains(v.bounds, canvasPoint))
                hitId = id;
        return hitId;
    }

    std::optional<PinRef> NodeGraphController::HitTestPin(D2D1_POINT_2F canvasPoint) const
    {
        constexpr float cHitRadius = cPinRadius * 2.5f;
        std::shared_lock lock(m_visualsMutex);
        for (const auto& [id, visual] : m_visuals)
        {
            for (PinKind kind : { PinKind::ImageOut, PinKind::ImageIn, PinKind::DataOut, PinKind::DataIn })
            {
                const auto& pins = visual.Pins(kind);
                for (uint32_t i = 0; i < pins.size(); ++i)
                {
                    const float dx = canvasPoint.x - pins[i].x;
                    const float dy = canvasPoint.y - pins[i].y;
                    if (dx * dx + dy * dy <= cHitRadius * cHitRadius)
                        return PinRef{ id, kind, i, visual.PinName(kind, i) };
                }
            }
        }
        return std::nullopt;
    }

    uint32_t NodeGraphController::HitTestSlider(D2D1_POINT_2F canvasPoint) const
    {
        std::shared_lock lock(m_visualsMutex);
        for (const auto& [id, v] : m_visuals)
            if (v.isParameterNode && Contains(v.sliderRect, canvasPoint, 0.0f, 6.0f))
                return id;
        return 0;
    }

    uint32_t NodeGraphController::HitTestPlayButton(D2D1_POINT_2F canvasPoint) const
    {
        std::shared_lock lock(m_visualsMutex);
        for (const auto& [id, v] : m_visuals)
            if (v.isClockNode && Contains(v.playButtonRect, canvasPoint, 4.0f, 4.0f))
                return id;
        return 0;
    }

    bool NodeGraphController::IsParameterNode(uint32_t nodeId) const
    {
        std::shared_lock lock(m_visualsMutex);
        auto it = m_visuals.find(nodeId);
        return it != m_visuals.end() && it->second.isParameterNode;
    }

    NodeGraphController::EdgeHit NodeGraphController::HitTestEdge(D2D1_POINT_2F canvasPoint, float tolerance) const
    {
        EdgeHit best{};
        const auto snapshot = Snapshot();
        if (!snapshot) return best;
        std::shared_lock lock(m_visualsMutex);

        float bestDist = tolerance * tolerance;
        for (const auto& edge : snapshot->edges)
        {
            auto src = m_visuals.find(edge.sourceNodeId);
            auto dst = m_visuals.find(edge.destNodeId);
            if (src == m_visuals.end() || dst == m_visuals.end()) continue;
            const auto start = src->second.Pin(PinKind::ImageOut, edge.sourcePin);
            const auto end = dst->second.Pin(PinKind::ImageIn, edge.destPin);
            if (!start || !end) continue;
            const float d2 = DistanceToCurveSqr(canvasPoint, EdgeCurve::Between(*start, *end));
            if (d2 < bestDist)
            {
                bestDist = d2;
                best = EdgeHit{ true, false, edge.sourceNodeId, edge.sourcePin, edge.destNodeId, edge.destPin, {}, {} };
            }
        }

        ForEachDataEdge(snapshot->nodes, m_visuals, [&](const DataEdge& edge)
        {
            const float d2 = DistanceToCurveSqr(canvasPoint, EdgeCurve::Between(edge.start, edge.end));
            if (d2 < bestDist)
            {
                bestDist = d2;
                best = EdgeHit{ true, true, edge.sourceNodeId, 0, edge.destNodeId, 0, edge.property, edge.field };
            }
        });
        return best;
    }

    // -----------------------------------------------------------------------
    // Commands
    // -----------------------------------------------------------------------

    bool NodeGraphController::RemoveEdge(const EdgeHit& hit)
    {
        if (!hit.found || !m_graph || !m_dispatcher) return false;
        const bool removed = m_dispatcher->DispatchSync([this, &hit]
        {
            const bool ok = hit.isDataBinding
                ? m_graph->UnbindPropertySource(hit.destNodeId, hit.destPropertyName,
                                                hit.sourceNodeId, hit.sourceFieldName)
                : m_graph->Disconnect(hit.sourceNodeId, hit.sourcePin, hit.destNodeId, hit.destPin);
            if (ok) RebuildLayout();
            return ok;
        });
        if (removed) m_needsRedraw = true;
        return removed;
    }

    std::optional<bool> NodeGraphController::TogglePlaying(uint32_t nodeId)
    {
        if (!m_graph || !m_dispatcher) return std::nullopt;
        const auto playing = m_dispatcher->DispatchSync([this, nodeId]() -> std::optional<bool>
        {
            auto* node = m_graph->FindNode(nodeId);
            if (!node) return std::nullopt;
            Graph::SetClockPlaying(*node, !node->isPlaying);
            return node->isPlaying;
        });
        if (playing) m_needsRedraw = true;
        return playing;
    }

    void NodeGraphController::FlushDragWrites()
    {
        std::vector<uint32_t> moveIds;
        float dx = 0.0f, dy = 0.0f;
        uint32_t sliderNodeId = 0;
        bool sliderIsClock = false;
        std::optional<double> sliderT;
        {
            std::scoped_lock lock(m_dragWrites.mutex);
            m_dragWrites.queued = false;
            moveIds = std::move(m_dragWrites.moveIds);
            dx = std::exchange(m_dragWrites.dx, 0.0f);
            dy = std::exchange(m_dragWrites.dy, 0.0f);
            sliderNodeId = m_dragWrites.sliderNodeId;
            sliderIsClock = m_dragWrites.sliderIsClock;
            sliderT = std::exchange(m_dragWrites.sliderT, std::nullopt);
        }
        if (!m_graph) return;
        if (dx != 0.0f || dy != 0.0f)
            ::ShaderLab::Graph::MoveNodes(*m_graph, moveIds, { dx, dy });
        if (sliderT)
        {
            auto* node = m_graph->FindNode(sliderNodeId);
            if (node && node->customEffect.has_value())
            {
                if (sliderIsClock) Graph::SeekClock(*node, *sliderT);
                else               Graph::SetParameterNormalized(*node, *sliderT);
            }
        }
    }

    void NodeGraphController::ScheduleDragFlush()
    {
        {
            std::scoped_lock lock(m_dragWrites.mutex);
            if (m_dragWrites.queued) return;
            m_dragWrites.queued = true;
        }
        // The dispatcher outlives no closure: shutdown cancels queued ones.
        m_dispatcher->DispatchAsync([this] { FlushDragWrites(); });
    }

    void NodeGraphController::BeginSliderDrag(uint32_t nodeId, D2D1_POINT_2F canvasPoint)
    {
        {
            std::shared_lock lock(m_visualsMutex);
            auto it = m_visuals.find(nodeId);
            if (it == m_visuals.end() || !it->second.isParameterNode) return;
            m_sliderDrag = { nodeId, it->second.isClockNode, it->second.sliderRect, -1.0 };
        }
        UpdateSliderDrag(canvasPoint);
    }

    bool NodeGraphController::UpdateSliderDrag(D2D1_POINT_2F canvasPoint)
    {
        if (m_sliderDrag.nodeId == 0 || !m_dispatcher) return false;
        const auto& rect = m_sliderDrag.rect;
        const double t = std::clamp((canvasPoint.x - rect.left) / (rect.right - rect.left), 0.0f, 1.0f);
        if (t == m_sliderDrag.lastT) return false;
        m_sliderDrag.lastT = t;
        {
            std::scoped_lock lock(m_dragWrites.mutex);
            m_dragWrites.sliderNodeId = m_sliderDrag.nodeId;
            m_dragWrites.sliderIsClock = m_sliderDrag.isClock;
            m_dragWrites.sliderT = t;
        }
        ScheduleDragFlush();
        m_needsRedraw = true;
        return true;
    }

    void NodeGraphController::EndSliderDrag()
    {
        if (m_sliderDrag.nodeId == 0) return;
        m_sliderDrag = {};
        if (m_dispatcher) m_dispatcher->DispatchSync([this] { FlushDragWrites(); });
        m_needsRedraw = true;
    }

    void NodeGraphController::BeginDragNodes(D2D1_POINT_2F startPoint)
    {
        m_draggingNodes = true;
        m_dragStart = startPoint;
        m_dragOffset = { 0.0f, 0.0f };
    }

    void NodeGraphController::UpdateDragNodes(D2D1_POINT_2F currentPoint)
    {
        if (!m_draggingNodes || !m_dispatcher) return;
        const float dx = currentPoint.x - m_dragStart.x;
        const float dy = currentPoint.y - m_dragStart.y;
        const float stepX = dx - m_dragOffset.x;
        const float stepY = dy - m_dragOffset.y;
        m_dragOffset = { dx, dy };

        {
            std::scoped_lock lock(m_dragWrites.mutex);
            m_dragWrites.moveIds.assign(m_selection.begin(), m_selection.end());
            m_dragWrites.dx += stepX;
            m_dragWrites.dy += stepY;
        }
        ScheduleDragFlush();

        {
            std::unique_lock lock(m_visualsMutex);
            for (uint32_t id : m_selection)
                if (auto it = m_visuals.find(id); it != m_visuals.end())
                    it->second.Translate(stepX, stepY);
        }
        m_needsRedraw = true;
    }

    void NodeGraphController::EndDragNodes()
    {
        if (!m_draggingNodes) return;
        m_draggingNodes = false;
        if (m_dispatcher) m_dispatcher->DispatchSync([this] { FlushDragWrites(); });
        m_needsRedraw = true;
    }

    void NodeGraphController::BeginConnection(const PinRef& from)
    {
        m_connectionDrag.active = true;
        m_connectionDrag.from = from;
        std::shared_lock lock(m_visualsMutex);
        if (auto it = m_visuals.find(from.nodeId); it != m_visuals.end())
            if (const auto pin = it->second.Pin(from.kind, from.index))
                m_connectionDrag.currentPos = *pin;
    }

    void NodeGraphController::UpdateConnection(D2D1_POINT_2F currentPoint)
    {
        if (!m_connectionDrag.active) return;
        m_connectionDrag.currentPos = currentPoint;
        m_needsRedraw = true;
    }

    bool NodeGraphController::EndConnection(D2D1_POINT_2F dropPoint)
    {
        const PinRef from = m_connectionDrag.from;
        const bool active = m_connectionDrag.active;
        CancelConnection();
        if (!active || !m_graph || !m_dispatcher) return false;

        const auto target = HitTestPin(dropPoint);
        // Output to input, image to image or data to data.
        if (!target || IsDataPin(target->kind) != IsDataPin(from.kind) ||
            IsOutputPin(target->kind) == IsOutputPin(from.kind))
            return false;
        const PinRef& source = IsOutputPin(from.kind) ? from : *target;
        const PinRef& dest = IsOutputPin(from.kind) ? *target : from;
        const bool isData = IsDataPin(from.kind);
        // A data pin is resolved by the name recorded when the drag began,
        // so a relayout during the drag cannot retarget it.
        if (isData && (source.name.empty() || dest.name.empty())) return false;

        const bool connected = m_dispatcher->DispatchSync([this, &source, &dest, isData]
        {
            const bool ok = isData
                ? m_graph->BindProperty(dest.nodeId, dest.name, source.nodeId, source.name, 0).empty()
                : m_graph->Connect(source.nodeId, source.index, dest.nodeId, dest.index);
            if (ok) RebuildLayout();
            return ok;
        });
        if (connected && m_connectionCallback)
            m_connectionCallback(source.nodeId, source.index, dest.nodeId, dest.index, isData);
        return connected;
    }

    void NodeGraphController::CancelConnection()
    {
        if (m_connectionDrag.active) m_needsRedraw = true;
        m_connectionDrag.active = false;
    }

    // -----------------------------------------------------------------------
    // Selection
    // -----------------------------------------------------------------------

    void NodeGraphController::SelectNode(uint32_t nodeId, bool addToSelection)
    {
        if (!addToSelection)
            m_selection.clear();
        m_selection.insert(nodeId);
        m_needsRedraw = true;
    }

    void NodeGraphController::DeselectAll()
    {
        m_selection.clear();
        m_needsRedraw = true;
    }

    void NodeGraphController::SelectAll()
    {
        if (const auto snapshot = Snapshot())
            for (const auto& node : snapshot->nodes)
                m_selection.insert(node.id);
        m_needsRedraw = true;
    }

    void NodeGraphController::DeleteSelected()
    {
        if (!m_graph || !m_dispatcher) return;
        const auto ids = m_selection;
        m_dispatcher->DispatchSync([this, &ids]
        {
            for (uint32_t id : ids)
                m_graph->RemoveNode(id);
            RebuildLayout();
        });
        m_selection.clear();
        m_needsRedraw = true;
    }

    uint32_t NodeGraphController::AddNode(Graph::EffectNode node, D2D1_POINT_2F canvasPos)
    {
        if (!m_graph || !m_dispatcher) return 0;

        if (canvasPos.x == 0.0f && canvasPos.y == 0.0f)
        {
            // The centre of the current view, staggered so successive adds
            // do not stack, and nudged off any node already there.
            const float zoom = m_zoom > 0.0f ? m_zoom : 1.0f;
            const float cx = (-m_panOffset.x + m_viewportW * 0.5f) / zoom;
            const float cy = (-m_panOffset.y + m_viewportH * 0.5f) / zoom;
            const float stagger = static_cast<float>(m_autoStaggerCount++ % 8) * 24.0f;
            canvasPos = { cx - cNodeWidth * 0.5f + stagger, cy - 30.0f + stagger };

            if (const auto snapshot = Snapshot())
            {
                for (int attempt = 0; attempt < 8; ++attempt)
                {
                    const bool overlap = std::any_of(snapshot->nodes.begin(), snapshot->nodes.end(),
                        [&](const Graph::EffectNode& n)
                        {
                            return std::abs(n.position.x - canvasPos.x) < 8.0f &&
                                   std::abs(n.position.y - canvasPos.y) < 8.0f;
                        });
                    if (!overlap) break;
                    canvasPos.x += 24.0f;
                    canvasPos.y += 24.0f;
                }
            }
        }

        ::ShaderLab::Graph::SetNodePosition(node, { canvasPos.x, canvasPos.y });
        return m_dispatcher->DispatchSync([this, &node]
        {
            const uint32_t id = m_graph->AddNode(std::move(node));
            RebuildLayout();
            return id;
        });
    }

    void NodeGraphController::PasteNodes(const std::vector<std::pair<uint32_t, Graph::EffectNode>>& nodes,
                                         std::span<const Graph::EffectEdge> edges)
    {
        if (!m_graph || !m_dispatcher || nodes.empty()) return;
        std::unordered_map<uint32_t, uint32_t> idMap;
        for (const auto& [originalId, node] : nodes)
            idMap[originalId] = AddNode(Graph::CloneNodeForPaste(node), { 0.0f, 0.0f });
        m_dispatcher->DispatchSync([this, &idMap, edges]
        {
            Graph::ConnectClonedEdges(*m_graph, edges, idMap);
            RebuildLayout();
        });
    }

    // -----------------------------------------------------------------------
    // Paint
    // -----------------------------------------------------------------------

    void NodeGraphController::Render(ID2D1DeviceContext* dc)
    {
        if (!dc) return;
        // Cleared first, so a request raised mid-paint survives.
        m_needsRedraw.store(false);
        // One snapshot for the whole paint, so edges and nodes agree.
        const auto snapshot = Snapshot();
        if (!snapshot) return;
        // Nothing in the paint dispatches, so holding this cannot invert the lock order.
        std::shared_lock lock(m_visualsMutex);
        m_renderer.Draw(dc, { *snapshot, m_visuals, m_selection, m_connectionDrag,
                              m_panOffset, m_zoom, m_showNodeGpuStats });
    }

    void NodeGraphController::ReleaseDeviceResources()
    {
        m_renderer.ReleaseDeviceResources();
        m_needsRedraw = true;
    }
}
