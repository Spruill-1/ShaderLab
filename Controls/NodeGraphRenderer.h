#pragma once

#include "pch.h"
#include "NodeLayout.h"
#include "../Graph/GraphUiSnapshot.h"

namespace ShaderLab::Controls
{
    // The connection being dragged from a pin, if any.
    struct ConnectionDrag
    {
        bool active{ false };
        PinRef from;
        D2D1_POINT_2F currentPos{};   // pointer, canvas space
    };

    // Paints the node-graph canvas from a snapshot and the laid-out visuals.
    // UI thread only; owns its D2D brushes, geometries and text formats.
    class NodeGraphRenderer
    {
    public:
        struct Scene
        {
            const Graph::GraphUiSnapshot& snapshot;
            const std::unordered_map<uint32_t, NodeVisual>& visuals;
            const std::unordered_set<uint32_t>& selection;
            const ConnectionDrag& drag;
            D2D1_POINT_2F pan{};
            float zoom{ 1.0f };
            bool showGpuStats{ false };
        };

        void Draw(ID2D1DeviceContext* dc, const Scene& scene);

        // Drops every device-bound resource (after a device change).
        void ReleaseDeviceResources();

    private:
        bool EnsureResources(ID2D1DeviceContext* dc);
        HRESULT CreateResources(ID2D1DeviceContext* dc);

        void DrawEdges(ID2D1DeviceContext* dc, const Scene& scene);
        void DrawNode(ID2D1DeviceContext* dc, const NodeVisual& visual, const Graph::EffectNode& node, bool selected);
        void DrawNodeChrome(ID2D1DeviceContext* dc, const NodeVisual& visual, const Graph::EffectNode& node, bool selected);
        void DrawGpuBadge(ID2D1DeviceContext* dc, const NodeVisual& visual, const Graph::EffectNode& node);
        void DrawExpression(ID2D1DeviceContext* dc, const NodeVisual& visual, const Graph::EffectNode& node);
        void DrawImagePins(ID2D1DeviceContext* dc, const NodeVisual& visual, const Graph::EffectNode& node);
        void DrawDataPins(ID2D1DeviceContext* dc, const NodeVisual& visual, const Graph::EffectNode& node);
        void DrawClockControl(ID2D1DeviceContext* dc, const NodeVisual& visual, const Graph::EffectNode& node);
        void DrawParameterSlider(ID2D1DeviceContext* dc, const NodeVisual& visual, const Graph::EffectNode& node);
        void DrawSliderTrack(ID2D1DeviceContext* dc, const D2D1_RECT_F& slider, float t,
                             D2D1_COLOR_F fillColor, float handleRadius);
        void DrawLabel(ID2D1DeviceContext* dc, const std::wstring& text, const D2D1_RECT_F& rect,
                       DWRITE_TEXT_ALIGNMENT alignment, ID2D1Brush* brush);
        void FillAt(ID2D1DeviceContext* dc, ID2D1Geometry* geometry, D2D1_POINT_2F at, ID2D1Brush* brush);
        void DrawConnectionDrag(ID2D1DeviceContext* dc, const Scene& scene);

        winrt::com_ptr<ID2D1Factory> m_factory;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushNode;        // retinted per node
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushHeader;      // retinted per node
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushDragLine;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushPin;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushSelection;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushText;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushDataPin;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushDataEdge;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushGpuBadge;    // retinted per GPU state
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushTrack;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushFill;        // retinted per slider
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushHandle;
        winrt::com_ptr<ID2D1SolidColorBrush> m_brushPlay;        // retinted per play state
        // Drawn translated to each pin / button centre.
        winrt::com_ptr<ID2D1PathGeometry> m_dataPinDiamond;
        winrt::com_ptr<ID2D1PathGeometry> m_playTriangle;
        winrt::com_ptr<IDWriteTextFormat> m_titleFormat;
        winrt::com_ptr<IDWriteTextFormat> m_labelFormat;
        bool m_resourcesCreated{ false };
        HRESULT m_lastResourceError{ S_OK };
    };
}
