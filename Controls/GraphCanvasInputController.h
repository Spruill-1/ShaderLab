#pragma once

#include "pch.h"
#include "NodeGraphController.h"

namespace ShaderLab::Controls
{
    // What the canvas input asks of the window it sits in.
    struct GraphCanvasHost
    {
        std::function<std::shared_ptr<const Graph::GraphUiSnapshot>()> snapshot;
        std::function<uint32_t()> selectedNodeId;
        // The node the Properties panel shows (0 for none); refreshes the panel.
        std::function<void(uint32_t)> selectNode;
        // A node body was clicked: show its properties, and preview it when
        // `previewable`.
        std::function<void(uint32_t nodeId, bool previewable)> nodeClicked;
        std::function<void()> requestRender;
        std::function<void()> refreshProperties;
        std::function<void()> nodeListChanged;
        std::function<void()> graphEdited;   // marks the file unsaved
        std::function<void(uint32_t, const std::wstring&)> logInfo;
        // Around a Delete: before, with the ids about to go; after, once gone.
        std::function<void(const std::unordered_set<uint32_t>&)> nodesDeleting;
        std::function<void()> nodesDeleted;
    };

    // Pointer and keyboard input on the node-graph canvas: pan and zoom,
    // node / slider / connection drags, Alt+click edge removal, and the
    // Ctrl+A / C / V / L and Delete shortcuts. Edits go through the
    // NodeGraphController. UI thread only.
    class GraphCanvasInputController
    {
    public:
        void Attach(winrt::Microsoft::UI::Xaml::UIElement const& container,
                    NodeGraphController& canvas, GraphCanvasHost host);

    private:
        using PointerArgs = winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs;

        D2D1_POINT_2F ScreenPoint(PointerArgs const& args) const;
        void OnPointerPressed(PointerArgs const& args);
        void OnPointerMoved(PointerArgs const& args);
        void OnPointerReleased(PointerArgs const& args);
        void OnPointerWheel(PointerArgs const& args);
        void OnKeyDown(winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);

        void PressNode(uint32_t nodeId, D2D1_POINT_2F canvasPoint);
        void Copy();
        void Paste();
        bool DeleteSelection();   // true when something was deleted

        enum class Drag { None, Pan, Slider, Nodes, Connection };

        winrt::Microsoft::UI::Xaml::UIElement m_container{ nullptr };
        NodeGraphController* m_canvas{ nullptr };
        GraphCanvasHost m_host;
        Drag m_drag{ Drag::None };
        D2D1_POINT_2F m_panStart{};
        D2D1_POINT_2F m_panOrigin{};

        // Ctrl+C keeps (original id, node) and the edges between them.
        std::vector<std::pair<uint32_t, Graph::EffectNode>> m_clipboardNodes;
        std::vector<Graph::EffectEdge> m_clipboardEdges;
    };
}
