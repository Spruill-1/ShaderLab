#include "pch.h"
#include "GraphCanvasInputController.h"

namespace ShaderLab::Controls
{
    namespace
    {
        using winrt::Windows::System::VirtualKey;

        bool IsKeyDown(VirtualKey key)
        {
            const auto state = winrt::Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(key);
            return (static_cast<uint32_t>(state) &
                    static_cast<uint32_t>(winrt::Windows::UI::Core::CoreVirtualKeyStates::Down)) != 0;
        }

        // A clicked node previews unless it has no image to show.
        bool IsPreviewable(const Graph::EffectNode* node)
        {
            if (!node || node->outputPins.empty() || node->IsParameterNode()) return false;
            return !(node->effectClsid.has_value() && IsEqualGUID(*node->effectClsid, CLSID_D2D1Histogram));
        }
    }

    void GraphCanvasInputController::Attach(winrt::Microsoft::UI::Xaml::UIElement const& container,
                                            NodeGraphController& canvas, GraphCanvasHost host)
    {
        m_container = container;
        m_canvas = &canvas;
        m_host = std::move(host);
        container.PointerPressed([this](auto&&, PointerArgs const& args) { OnPointerPressed(args); });
        container.PointerMoved([this](auto&&, PointerArgs const& args) { OnPointerMoved(args); });
        container.PointerReleased([this](auto&&, PointerArgs const& args) { OnPointerReleased(args); });
        container.PointerWheelChanged([this](auto&&, PointerArgs const& args) { OnPointerWheel(args); });
        container.KeyDown([this](auto&&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
        {
            OnKeyDown(args);
        });
        container.IsTabStop(true);
    }

    D2D1_POINT_2F GraphCanvasInputController::ScreenPoint(PointerArgs const& args) const
    {
        const auto position = args.GetCurrentPoint(m_container).Position();
        return { static_cast<float>(position.X), static_cast<float>(position.Y) };
    }

    // Right or middle button pans. Left button, in priority order: the play
    // button, an inline slider, a pin (connection drag), Alt + an edge
    // (remove it), a node (select and drag), empty canvas (deselect).
    void GraphCanvasInputController::OnPointerPressed(PointerArgs const& args)
    {
        const auto properties = args.GetCurrentPoint(m_container).Properties();
        const auto screen = ScreenPoint(args);
        const auto canvasPoint = m_canvas->ScreenToCanvas(screen);
        args.Handled(true);

        if (properties.IsRightButtonPressed() || properties.IsMiddleButtonPressed())
        {
            m_drag = Drag::Pan;
            m_panStart = screen;
            m_panOrigin = m_canvas->PanOffset();
            m_container.CapturePointer(args.Pointer());
            return;
        }

        if (const uint32_t playId = m_canvas->HitTestPlayButton(canvasPoint))
        {
            if (const auto playing = m_canvas->TogglePlaying(playId))
                m_host.logInfo(playId, *playing ? L"Clock started" : L"Clock paused");
            return;
        }

        if (const uint32_t sliderId = m_canvas->HitTestSlider(canvasPoint))
        {
            m_drag = Drag::Slider;
            m_canvas->BeginSliderDrag(sliderId, canvasPoint);
            m_host.selectNode(sliderId);
            m_host.requestRender();
            m_container.CapturePointer(args.Pointer());
            return;
        }

        if (const auto pin = m_canvas->HitTestPin(canvasPoint))
        {
            m_drag = Drag::Connection;
            m_canvas->BeginConnection(*pin);
            m_container.CapturePointer(args.Pointer());
            return;
        }

        if (IsKeyDown(VirtualKey::Menu))
        {
            const auto hit = m_canvas->HitTestEdge(canvasPoint, 8.0f);
            if (hit.found && m_canvas->RemoveEdge(hit))
            {
                m_host.requestRender();
                m_host.refreshProperties();
                m_host.graphEdited();
                return;
            }
        }

        if (const uint32_t nodeId = m_canvas->HitTestNode(canvasPoint))
        {
            PressNode(nodeId, canvasPoint);
            m_container.CapturePointer(args.Pointer());
            return;
        }

        m_canvas->DeselectAll();
        m_host.selectNode(0);
    }

    void GraphCanvasInputController::PressNode(uint32_t nodeId, D2D1_POINT_2F canvasPoint)
    {
        // A node already in the selection keeps the group together for the
        // drag; Shift adds to the selection, a plain click replaces it.
        if (!m_canvas->SelectedNodes().contains(nodeId))
            m_canvas->SelectNode(nodeId, IsKeyDown(VirtualKey::Shift));
        m_canvas->BeginDragNodes(canvasPoint);
        m_drag = Drag::Nodes;
        m_container.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);

        const auto snapshot = m_host.snapshot();
        m_host.nodeClicked(nodeId, IsPreviewable(snapshot ? snapshot->FindNode(nodeId) : nullptr));
    }

    void GraphCanvasInputController::OnPointerMoved(PointerArgs const& args)
    {
        if (m_drag == Drag::None) return;
        args.Handled(true);
        const auto screen = ScreenPoint(args);
        switch (m_drag)
        {
        case Drag::Pan:
            m_canvas->SetPanOffset(m_panOrigin.x + screen.x - m_panStart.x,
                                   m_panOrigin.y + screen.y - m_panStart.y);
            break;
        case Drag::Slider:
            if (m_canvas->UpdateSliderDrag(m_canvas->ScreenToCanvas(screen)))
            {
                m_host.requestRender();
                m_host.refreshProperties();
            }
            break;
        case Drag::Nodes:
            m_canvas->UpdateDragNodes(m_canvas->ScreenToCanvas(screen));
            break;
        case Drag::Connection:
            m_canvas->UpdateConnection(m_canvas->ScreenToCanvas(screen));
            break;
        default:
            break;
        }
    }

    void GraphCanvasInputController::OnPointerReleased(PointerArgs const& args)
    {
        args.Handled(true);
        const Drag drag = std::exchange(m_drag, Drag::None);
        if (drag == Drag::None) return;
        m_container.ReleasePointerCapture(args.Pointer());
        switch (drag)
        {
        case Drag::Slider:
            m_canvas->EndSliderDrag();
            m_host.requestRender();
            m_host.refreshProperties();
            break;
        case Drag::Nodes:
            m_canvas->EndDragNodes();
            break;
        case Drag::Connection:
            if (m_canvas->EndConnection(m_canvas->ScreenToCanvas(ScreenPoint(args))))
                m_host.nodeListChanged();
            break;
        default:
            break;
        }
    }

    // Zooms toward the pointer.
    void GraphCanvasInputController::OnPointerWheel(PointerArgs const& args)
    {
        const int32_t delta = args.GetCurrentPoint(m_container).Properties().MouseWheelDelta();
        const auto p = ScreenPoint(args);
        const float oldZoom = m_canvas->Zoom();
        const float newZoom = std::clamp(oldZoom * (delta > 0 ? 1.1f : 0.9f), 0.2f, 3.0f);
        const auto pan = m_canvas->PanOffset();
        m_canvas->SetZoom(newZoom);
        m_canvas->SetPanOffset(p.x - (p.x - pan.x) * (newZoom / oldZoom),
                               p.y - (p.y - pan.y) * (newZoom / oldZoom));
        args.Handled(true);
    }

    void GraphCanvasInputController::OnKeyDown(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
    {
        const auto key = args.Key();
        if (IsKeyDown(VirtualKey::Control))
        {
            switch (key)
            {
            case VirtualKey::A: m_canvas->SelectAll(); break;
            case VirtualKey::C: Copy(); break;
            case VirtualKey::V:
                if (m_clipboardNodes.empty()) return;
                Paste();
                break;
            case VirtualKey::L:
                m_canvas->RequestAutoLayout();
                m_host.requestRender();
                break;
            default:
                return;
            }
            args.Handled(true);
            return;
        }
        if (key == VirtualKey::Delete && DeleteSelection())
            args.Handled(true);
    }

    // The selection (or the node the Properties panel shows) and the edges
    // between the selected nodes.
    void GraphCanvasInputController::Copy()
    {
        if (m_canvas->SelectedNodes().empty() && m_host.selectedNodeId() != 0)
            m_canvas->SelectNode(m_host.selectedNodeId());
        m_clipboardNodes.clear();
        m_clipboardEdges.clear();
        const auto snapshot = m_host.snapshot();
        if (!snapshot) return;
        const auto& selection = m_canvas->SelectedNodes();
        for (uint32_t id : selection)
            if (const auto* node = snapshot->FindNode(id))
                m_clipboardNodes.emplace_back(id, *node);
        for (const auto& edge : snapshot->edges)
            if (selection.contains(edge.sourceNodeId) && selection.contains(edge.destNodeId))
                m_clipboardEdges.push_back(edge);
    }

    void GraphCanvasInputController::Paste()
    {
        m_canvas->PasteNodes(m_clipboardNodes, m_clipboardEdges);
        m_host.nodeListChanged();
    }

    // Deletes the selection, or the node the Properties panel shows; the
    // Output node of a single-node delete stays.
    bool GraphCanvasInputController::DeleteSelection()
    {
        const uint32_t shown = m_host.selectedNodeId();
        if (m_canvas->SelectedNodes().empty())
        {
            if (shown == 0) return false;
            const auto snapshot = m_host.snapshot();
            const auto* node = snapshot ? snapshot->FindNode(shown) : nullptr;
            if (node && node->type == Graph::NodeType::Output) return false;
            m_canvas->SelectNode(shown);
        }
        m_host.nodesDeleting(m_canvas->SelectedNodes());
        m_canvas->DeleteSelected();
        m_host.nodesDeleted();
        return true;
    }
}
