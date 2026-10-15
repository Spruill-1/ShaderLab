#pragma once

#include "pch.h"
#include "../Graph/GraphUiSnapshot.h"
#include "../Effects/PropertyMetadata.h"

#include <functional>
#include <map>
#include <memory>

namespace ShaderLab::Graph
{
    class GraphModel;
}

namespace ShaderLab::Controls
{
    class DialogCoordinator;

    // What a panel edit may touch, handed to it on the render thread.
    struct PanelGraphEdit
    {
        Graph::GraphModel& graph;
        std::function<void()> rebuildLayout;                    // the canvas layout
        std::function<void(uint32_t)> invalidateNode;           // the evaluator's state for a node
        std::function<void(Graph::EffectNode&)> prepareSource;  // (re)loads a source's media
    };

    // The window services the panel uses. Everything is called on the UI
    // thread; editGraph blocks while its closure runs on the render thread.
    struct PropertiesPanelHost
    {
        std::function<std::shared_ptr<const Graph::GraphUiSnapshot>()> snapshot;
        std::function<uint32_t()> selectedNodeId;
        std::function<bool(const std::function<bool(PanelGraphEdit&)>&)> editGraph;
        std::function<void()> requestRender;
        std::function<void()> requestCanvasRedraw;
        std::function<void()> refreshNodeList;      // preview selector and outdated-effects button
        std::function<void()> fitPreview;
        std::function<void(uint32_t, const Graph::CustomEffectDefinition&, const std::wstring&)> openInDesigner;
        std::function<void(uint32_t)> openLog;
        std::function<void(uint32_t, const std::wstring&)> logInfo;
        std::function<HWND()> windowHandle;          // owner of the file pickers
        std::function<bool()> isShuttingDown;
        // A reference that keeps the window alive across a file picker.
        std::function<winrt::Windows::Foundation::IInspectable()> keepAlive;
    };

    // Builds the Properties panel for the selected node from the published
    // graph snapshot, and turns the user's edits into render-thread graph
    // writes. The panel is rebuilt from scratch on every change.
    class PropertiesPanelController
    {
    public:
        explicit PropertiesPanelController(DialogCoordinator& dialogs) : m_dialogs(dialogs) {}

        void Attach(winrt::Microsoft::UI::Xaml::Controls::StackPanel panel, PropertiesPanelHost host);

        // Rebuilds the panel for the host's selected node.
        void Rebuild();

        // Rebuilds when the selected node has bound properties, so their live
        // values show, unless the user is editing inside the panel.
        void RefreshBoundValues();

        // True while a control inside the panel has keyboard focus.
        bool IsInteracting() const;

    private:
        using Children = winrt::Microsoft::UI::Xaml::Controls::UIElementCollection;
        using MetadataMap = std::map<std::wstring, Effects::PropertyMetadata>;

        void AppendHeader(Children const& children, const Graph::EffectNode& node);
        void AppendErrorBanner(Children const& children, const Graph::EffectNode& node);
        void AppendImageSource(Children const& children, const Graph::EffectNode& node);
        void AppendVideoSource(Children const& children, const Graph::GraphUiSnapshot& snapshot,
            const Graph::EffectNode& node);
        void AppendProperties(Children const& children, const Graph::GraphUiSnapshot& snapshot,
            const Graph::EffectNode& node);
        void AppendExpressionEditor(Children const& children, const Graph::EffectNode& node);
        void AppendProperty(Children const& children, const Graph::GraphUiSnapshot& snapshot,
            const Graph::EffectNode& node, const std::wstring& key, const Graph::PropertyValue& value,
            const Effects::PropertyMetadata* meta);
        void AppendBindingInfo(Children const& row, const Graph::GraphUiSnapshot& snapshot,
            const Graph::EffectNode& node, const std::wstring& key, const Graph::PropertyValue& value,
            const Graph::PropertyBinding& binding);
        void AppendBindMenu(Children const& row, const Graph::GraphUiSnapshot& snapshot,
            uint32_t nodeId, const std::wstring& key);
        void AppendGammaCurvePreview(Children const& children, const Graph::EffectNode& node);
        void AppendHistogram(Children const& children, const Graph::EffectNode& node);
        void AppendAnalysisResults(Children const& children, const Graph::EffectNode& node);
        void AppendInputCount(Children const& children, const Graph::EffectNode& node);
        void AppendUpgradeBanner(Children const& children, const Graph::EffectNode& node);
        void AppendFooter(Children const& children, const Graph::EffectNode& node);

        // Runs `edit` on the render thread; false when the host has no graph.
        bool EditGraph(const std::function<bool(PanelGraphEdit&)>& edit);
        // Binds a property to an analysis field, then rebuilds.
        void Bind(uint32_t nodeId, const std::wstring& key, uint32_t sourceId,
            const std::wstring& field, uint32_t component);
        // Logs a property change and, when it can change the panel or the
        // node's pins, schedules a rebuild of both.
        void OnPropertyChanged(uint32_t nodeId, const std::wstring& key);
        void OpenCurveEditor(uint32_t nodeId, const std::wstring& key);
        void AddExpressionInput(uint32_t nodeId);
        void RemoveExpressionInput(uint32_t nodeId, const std::wstring& name);
        void UpgradeNode(uint32_t nodeId);
        winrt::fire_and_forget BrowseImage(uint32_t nodeId);
        winrt::fire_and_forget BrowseVideo(uint32_t nodeId);

        static MetadataMap CustomParameterMetadata(const Graph::EffectNode& node);

        DialogCoordinator& m_dialogs;
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_panel{ nullptr };
        PropertiesPanelHost m_host;
    };
}
