#include "pch.h"
#include "NodeGraphRenderer.h"
#include "Graph/ClockModel.h"
#include "Graph/ParameterModel.h"
#include "Graph/ValueFormat.h"

namespace ShaderLab::Controls
{
    namespace
    {
        // Palette.
        constexpr UINT32 scNodeBodyColor      = 0x252528;
        constexpr UINT32 scHeaderColor        = 0x3A7BD5;
        constexpr UINT32 scEdgeColor          = 0x6B6B70;
        constexpr UINT32 scImageDragColor     = 0xFFFF00;
        constexpr UINT32 scPinColor           = 0xB0B0B8;
        constexpr UINT32 scSelectionColor     = 0x3A7BD5;
        constexpr UINT32 scTextColor          = 0xE8E8EC;
        constexpr UINT32 scDataColor          = 0xF5A623;
        constexpr UINT32 scBadgeColor         = 0x9AA0A6;
        constexpr UINT32 scParameterBody      = 0x1A2E3A;
        constexpr UINT32 scParameterHeader    = 0x00897B;
        constexpr UINT32 scDataOnlyBody       = 0x2A1E3A;
        constexpr UINT32 scDataOnlyHeader     = 0x7B1FA2;
        constexpr UINT32 scSliderTrackColor   = 0x444444;
        constexpr UINT32 scClockFillColor     = 0x4CAF50;
        constexpr UINT32 scParameterFillColor = 0x00BCD4;
        constexpr UINT32 scHandleColor        = 0xFFFFFF;
        constexpr UINT32 scPlayingColor       = 0x4CAF50;
        constexpr UINT32 scPausedColor        = 0xB0B0B8;

        D2D1_COLOR_F NodeColor(Graph::NodeType type)
        {
            switch (type)
            {
            case Graph::NodeType::Source:        return D2D1::ColorF(0x1E3A1E);
            case Graph::NodeType::BuiltInEffect: return D2D1::ColorF(0x1E2440);
            case Graph::NodeType::PixelShader:   return D2D1::ColorF(0x3A1E1E);
            case Graph::NodeType::ComputeShader: return D2D1::ColorF(0x3A2E1E);
            case Graph::NodeType::Output:        return D2D1::ColorF(0x2A2A2E);
            default:                             return D2D1::ColorF(scNodeBodyColor);
            }
        }

        D2D1_COLOR_F NodeHeaderColor(Graph::NodeType type)
        {
            switch (type)
            {
            case Graph::NodeType::Source:        return D2D1::ColorF(0x43A047);
            case Graph::NodeType::BuiltInEffect: return D2D1::ColorF(0x3A7BD5);
            case Graph::NodeType::PixelShader:   return D2D1::ColorF(0xE53935);
            case Graph::NodeType::ComputeShader: return D2D1::ColorF(0xF5A623);
            case Graph::NodeType::Output:        return D2D1::ColorF(0x78909C);
            default:                             return D2D1::ColorF(scHeaderColor);
            }
        }

        void DrawCurve(ID2D1DeviceContext* dc, ID2D1Factory* factory, const EdgeCurve& curve,
                       ID2D1Brush* brush, float width)
        {
            winrt::com_ptr<ID2D1PathGeometry> path;
            if (!factory || FAILED(factory->CreatePathGeometry(path.put()))) return;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(path->Open(sink.put()))) return;
            sink->BeginFigure(curve.start, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddBezier({ curve.cp1, curve.cp2, curve.end });
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close())) return;
            dc->DrawGeometry(path.get(), brush, width);
        }

        // A closed, filled polygon around the origin.
        HRESULT CreatePolygon(ID2D1Factory* factory, std::initializer_list<D2D1_POINT_2F> points,
                              winrt::com_ptr<ID2D1PathGeometry>& out)
        {
            winrt::com_ptr<ID2D1PathGeometry> path;
            HRESULT hr = factory->CreatePathGeometry(path.put());
            if (FAILED(hr)) return hr;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            hr = path->Open(sink.put());
            if (FAILED(hr)) return hr;
            auto it = points.begin();
            sink->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
            for (++it; it != points.end(); ++it)
                sink->AddLine(*it);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            hr = sink->Close();
            if (SUCCEEDED(hr)) out = std::move(path);
            return hr;
        }

        // The value a data input pin shows: an enum label, a vector's
        // components or the float.
        std::wstring FormatPinValue(const Graph::EffectNode& node, const std::wstring& name,
                                    const Graph::PropertyValue& value)
        {
            using namespace winrt::Windows::Foundation::Numerics;
            const float* scalar = std::get_if<float>(&value);
            if (auto label = Graph::EnumLabelFor(node, name, scalar ? *scalar : 0.0f))
                return *label;
            if (auto* v2 = std::get_if<float2>(&value))
                return std::format(L"{:.3g}, {:.3g}", v2->x, v2->y);
            if (auto* v3 = std::get_if<float3>(&value))
                return std::format(L"{:.3g}, {:.3g}, {:.3g}", v3->x, v3->y, v3->z);
            if (auto* v4 = std::get_if<float4>(&value))
                return std::format(L"{:.3g}, {:.3g}, {:.3g}, {:.3g}", v4->x, v4->y, v4->z, v4->w);
            return std::format(L"{:.4g}", scalar ? *scalar : 0.0f);
        }
    }

    void NodeGraphRenderer::ReleaseDeviceResources()
    {
        *this = NodeGraphRenderer{};
    }

    HRESULT NodeGraphRenderer::CreateResources(ID2D1DeviceContext* dc)
    {
        dc->GetFactory(m_factory.put());
        if (!m_factory) return E_POINTER;

        const std::pair<winrt::com_ptr<ID2D1SolidColorBrush>*, D2D1_COLOR_F> brushes[] = {
            { &m_brushNode,      D2D1::ColorF(scNodeBodyColor) },
            { &m_brushHeader,    D2D1::ColorF(scHeaderColor) },
            { &m_brushEdge,      D2D1::ColorF(scEdgeColor) },
            { &m_brushDragLine,  D2D1::ColorF(scImageDragColor, 0.7f) },
            { &m_brushPin,       D2D1::ColorF(scPinColor) },
            { &m_brushSelection, D2D1::ColorF(scSelectionColor, 0.3f) },
            { &m_brushText,      D2D1::ColorF(scTextColor) },
            { &m_brushDataPin,   D2D1::ColorF(scDataColor) },
            { &m_brushDataEdge,  D2D1::ColorF(scDataColor, 0.75f) },
            { &m_brushGpuBadge,  D2D1::ColorF(scBadgeColor) },
            { &m_brushTrack,     D2D1::ColorF(scSliderTrackColor) },
            { &m_brushFill,      D2D1::ColorF(scParameterFillColor) },
            { &m_brushHandle,    D2D1::ColorF(scHandleColor) },
            { &m_brushPlay,      D2D1::ColorF(scPausedColor) },
        };
        for (const auto& [brush, color] : brushes)
            if (const HRESULT hr = dc->CreateSolidColorBrush(color, brush->put()); FAILED(hr))
                return hr;

        constexpr float r = cPinRadius * 0.85f;
        if (const HRESULT hr = CreatePolygon(m_factory.get(),
                { { 0, -r }, { r, 0 }, { 0, r }, { -r, 0 } }, m_dataPinDiamond); FAILED(hr))
            return hr;
        if (const HRESULT hr = CreatePolygon(m_factory.get(),
                { { -4, -6 }, { -4, 6 }, { 5, 0 } }, m_playTriangle); FAILED(hr))
            return hr;

        winrt::com_ptr<IDWriteFactory> dwrite;
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(dwrite.put()));
        if (FAILED(hr)) return hr;
        hr = dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", m_titleFormat.put());
        if (FAILED(hr)) return hr;
        m_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        hr = dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.0f, L"en-US", m_labelFormat.put());
        if (FAILED(hr)) return hr;
        m_labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return S_OK;
    }

    bool NodeGraphRenderer::EnsureResources(ID2D1DeviceContext* dc)
    {
        if (m_resourcesCreated) return true;
        const HRESULT hr = CreateResources(dc);
        if (FAILED(hr))
        {
            // Retried next paint; reported once per distinct failure.
            if (hr != m_lastResourceError)
                OutputDebugStringW(std::format(L"[NodeGraph] canvas resources failed: 0x{:08X}\n",
                    static_cast<uint32_t>(hr)).c_str());
            const HRESULT error = hr;
            ReleaseDeviceResources();
            m_lastResourceError = error;
            return false;
        }
        m_resourcesCreated = true;
        m_lastResourceError = S_OK;
        return true;
    }

    void NodeGraphRenderer::Draw(ID2D1DeviceContext* dc, const Scene& scene)
    {
        if (!dc || !EnsureResources(dc)) return;

        dc->SetTransform(D2D1::Matrix3x2F::Scale(scene.zoom, scene.zoom) *
                         D2D1::Matrix3x2F::Translation(scene.pan.x, scene.pan.y));

        DrawEdges(dc, scene);
        for (const auto& [nodeId, visual] : scene.visuals)
        {
            if (const auto* node = scene.snapshot.FindNode(nodeId))
            {
                DrawNode(dc, visual, *node, scene.selection.contains(nodeId));
                if (scene.showGpuStats)
                    DrawGpuBadge(dc, visual, *node);
            }
        }
        DrawConnectionDrag(dc, scene);

        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    void NodeGraphRenderer::DrawEdges(ID2D1DeviceContext* dc, const Scene& scene)
    {
        for (const auto& edge : scene.snapshot.edges)
        {
            auto src = scene.visuals.find(edge.sourceNodeId);
            auto dst = scene.visuals.find(edge.destNodeId);
            if (src == scene.visuals.end() || dst == scene.visuals.end()) continue;
            const auto start = src->second.Pin(PinKind::ImageOut, edge.sourcePin).value_or(D2D1_POINT_2F{});
            const auto end = dst->second.Pin(PinKind::ImageIn, edge.destPin).value_or(D2D1_POINT_2F{});
            DrawCurve(dc, m_factory.get(), EdgeCurve::Between(start, end), m_brushEdge.get(), 2.0f);
        }

        ForEachDataEdge(scene.snapshot.nodes, scene.visuals,
            [&](const DataEdge& edge)
            {
                DrawCurve(dc, m_factory.get(), EdgeCurve::Between(edge.start, edge.end), m_brushDataEdge.get(), 1.5f);
            });
    }

    void NodeGraphRenderer::DrawNode(ID2D1DeviceContext* dc, const NodeVisual& visual,
                                     const Graph::EffectNode& node, bool selected)
    {
        DrawNodeChrome(dc, visual, node, selected);
        DrawExpression(dc, visual, node);
        DrawImagePins(dc, visual, node);
        DrawDataPins(dc, visual, node);
        if (visual.isParameterNode && node.customEffect.has_value())
        {
            if (visual.isClockNode) DrawClockControl(dc, visual, node);
            else                    DrawParameterSlider(dc, visual, node);
        }
    }

    void NodeGraphRenderer::DrawNodeChrome(ID2D1DeviceContext* dc, const NodeVisual& visual,
                                           const Graph::EffectNode& node, bool selected)
    {
        const D2D1_ROUNDED_RECT body = { visual.bounds, cNodeCornerRadius, cNodeCornerRadius };
        m_brushNode->SetColor(visual.isParameterNode ? D2D1::ColorF(scParameterBody)
            : visual.isDataOnly ? D2D1::ColorF(scDataOnlyBody)
            : NodeColor(node.type));
        dc->FillRoundedRectangle(body, m_brushNode.get());
        if (selected)
            dc->DrawRoundedRectangle(body, m_brushSelection.get(), 2.0f);

        const D2D1_RECT_F header = { visual.bounds.left, visual.bounds.top,
                                     visual.bounds.right, visual.bounds.top + visual.headerHeight };
        m_brushHeader->SetColor(visual.isParameterNode ? D2D1::ColorF(scParameterHeader)
            : visual.isDataOnly ? D2D1::ColorF(scDataOnlyHeader)
            : NodeHeaderColor(node.type));
        dc->FillRoundedRectangle({ header, cNodeCornerRadius, cNodeCornerRadius }, m_brushHeader.get());
        dc->DrawText(node.name.c_str(), static_cast<UINT32>(node.name.size()),
                     m_titleFormat.get(), header, m_brushText.get());
    }

    // Drawn above the node, so it never collides with the title or pins and
    // turning it on never reflows the graph. The state says as much as the
    // number: a node can cost nothing, or have its cost counted elsewhere.
    void NodeGraphRenderer::DrawGpuBadge(ID2D1DeviceContext* dc, const NodeVisual& visual,
                                         const Graph::EffectNode& node)
    {
        std::wstring badge;
        UINT32 color = scEdgeColor;
        switch (node.gpuState)
        {
        case Graph::GpuNodeState::Measured:
            badge = std::format(L"{:.3f} ms  measured", node.lastGpuMs);
            color = 0x7CD97C;
            break;
        case Graph::GpuNodeState::Cached:  badge = L"cached";               color = 0x6EA8D8; break;
        case Graph::GpuNodeState::Fused:   badge = L"fused downstream";     color = scBadgeColor; break;
        case Graph::GpuNodeState::Idle:    badge = L"idle · no consumer"; color = 0xC9A227; break;
        case Graph::GpuNodeState::CpuOnly: badge = L"cpu only";             break;
        default:                           badge = L"not measured";         break;
        }
        m_brushGpuBadge->SetColor(D2D1::ColorF(color));
        DrawLabel(dc, badge, { visual.bounds.left, visual.bounds.top - 14.0f,
                               visual.bounds.right, visual.bounds.top - 1.0f },
                  DWRITE_TEXT_ALIGNMENT_TRAILING, m_brushGpuBadge.get());
    }

    // A Numeric Expression shows its formula under the header.
    void NodeGraphRenderer::DrawExpression(ID2D1DeviceContext* dc, const NodeVisual& visual,
                                           const Graph::EffectNode& node)
    {
        if (!node.customEffect.has_value() || node.customEffect->nodeKind != Graph::NodeKind::Expression)
            return;
        auto it = node.properties.find(L"Expression");
        const auto* expression = it != node.properties.end() ? std::get_if<std::wstring>(&it->second) : nullptr;
        if (!expression) return;
        const float headerBottom = visual.bounds.top + visual.headerHeight;
        DrawLabel(dc, L"= " + *expression,
                  { visual.bounds.left + 6.0f, headerBottom + 1.0f, visual.bounds.right - 6.0f, headerBottom + 18.0f },
                  DWRITE_TEXT_ALIGNMENT_LEADING, m_brushText.get());
    }

    void NodeGraphRenderer::DrawImagePins(ID2D1DeviceContext* dc, const NodeVisual& visual,
                                          const Graph::EffectNode& node)
    {
        for (uint32_t i = 0; i < visual.imageInputPins.size(); ++i)
        {
            const auto& p = visual.imageInputPins[i];
            dc->FillEllipse({ p, cPinRadius, cPinRadius }, m_brushPin.get());
            if (node.customEffect.has_value() && i < node.customEffect->inputNames.size())
                DrawLabel(dc, node.customEffect->inputNames[i],
                          { p.x + cPinRadius + 3.0f, p.y - 7.0f, visual.bounds.right - 4.0f, p.y + 7.0f },
                          DWRITE_TEXT_ALIGNMENT_LEADING, m_brushPin.get());
        }
        for (const auto& p : visual.imageOutputPins)
            dc->FillEllipse({ p, cPinRadius, cPinRadius }, m_brushPin.get());
    }

    // Data pins are diamonds labelled with their name and current value.
    void NodeGraphRenderer::DrawDataPins(ID2D1DeviceContext* dc, const NodeVisual& visual,
                                         const Graph::EffectNode& node)
    {
        for (uint32_t i = 0; i < visual.dataInputPins.size(); ++i)
        {
            const auto& p = visual.dataInputPins[i];
            FillAt(dc, m_dataPinDiamond.get(), p, m_brushDataPin.get());
            if (i >= visual.dataInputNames.size()) continue;
            const auto& name = visual.dataInputNames[i];
            std::wstring label = name;
            if (auto it = node.properties.find(name); it != node.properties.end())
                label += L" = " + FormatPinValue(node, name, it->second);
            DrawLabel(dc, label, { p.x + cPinRadius + 3.0f, p.y - 7.0f, visual.bounds.right - 4.0f, p.y + 7.0f },
                      DWRITE_TEXT_ALIGNMENT_LEADING, m_brushDataPin.get());
        }

        // Parameter and data-only nodes show their output values beside the names.
        const bool showValues = node.analysisOutput.type == Graph::AnalysisOutputType::Typed &&
            !node.analysisOutput.fields.empty() && (visual.isParameterNode || node.outputPins.empty());
        for (uint32_t i = 0; i < visual.dataOutputPins.size(); ++i)
        {
            const auto& p = visual.dataOutputPins[i];
            FillAt(dc, m_dataPinDiamond.get(), p, m_brushDataPin.get());
            if (i >= visual.dataOutputNames.size()) continue;
            std::wstring label = visual.dataOutputNames[i];
            if (showValues)
            {
                for (const auto& field : node.analysisOutput.fields)
                    if (field.name == visual.dataOutputNames[i])
                    {
                        label += std::format(L" = {:.4g}", field.components[0]);
                        break;
                    }
            }
            DrawLabel(dc, label, { visual.bounds.left + 4.0f, p.y - 7.0f, p.x - cPinRadius - 3.0f, p.y + 7.0f },
                      DWRITE_TEXT_ALIGNMENT_TRAILING, m_brushDataPin.get());
        }
    }

    // Play / pause button, a seek slider and the time.
    void NodeGraphRenderer::DrawClockControl(ID2D1DeviceContext* dc, const NodeVisual& visual,
                                             const Graph::EffectNode& node)
    {
        const Graph::ClockParams clock = Graph::ReadClockParams(node);
        const float t = std::clamp(static_cast<float>(node.clockTime / clock.Duration()), 0.0f, 1.0f);

        const auto& button = visual.playButtonRect;
        const D2D1_POINT_2F centre = { (button.left + button.right) * 0.5f, (button.top + button.bottom) * 0.5f };
        m_brushPlay->SetColor(D2D1::ColorF(node.isPlaying ? scPlayingColor : scPausedColor));
        if (node.isPlaying)
        {
            dc->FillRectangle({ centre.x - 4, centre.y - 5, centre.x - 1, centre.y + 5 }, m_brushPlay.get());
            dc->FillRectangle({ centre.x + 1, centre.y - 5, centre.x + 4, centre.y + 5 }, m_brushPlay.get());
        }
        else
        {
            FillAt(dc, m_playTriangle.get(), centre, m_brushPlay.get());
        }

        DrawSliderTrack(dc, visual.sliderRect, t, D2D1::ColorF(scClockFillColor), 4.0f);

        const auto time = std::format(L"{:.1f}s / {:.1f}s",
            static_cast<float>(clock.startTime + node.clockTime), static_cast<float>(clock.stopTime));
        const auto& s = visual.sliderRect;
        DrawLabel(dc, time, { s.left, s.bottom, s.right, s.bottom + 14.0f },
                  DWRITE_TEXT_ALIGNMENT_CENTER, m_brushText.get());
    }

    void NodeGraphRenderer::DrawParameterSlider(ID2D1DeviceContext* dc, const NodeVisual& visual,
                                                const Graph::EffectNode& node)
    {
        auto it = node.properties.find(L"Value");
        if (it == node.properties.end()) return;
        const float* scalar = std::get_if<float>(&it->second);
        const float value = scalar ? *scalar : 0.0f;

        DrawSliderTrack(dc, visual.sliderRect, Graph::ParameterNormalizedValue(node),
                        D2D1::ColorF(scParameterFillColor), 5.0f);

        const auto text = Graph::EnumLabelFor(node, L"Value", value).value_or(std::format(L"{:.2f}", value));
        const auto& s = visual.sliderRect;
        DrawLabel(dc, text, { s.left, s.bottom - 2.0f, s.right, s.bottom + 12.0f },
                  DWRITE_TEXT_ALIGNMENT_CENTER, m_brushText.get());
    }

    void NodeGraphRenderer::DrawSliderTrack(ID2D1DeviceContext* dc, const D2D1_RECT_F& slider, float t,
                                            D2D1_COLOR_F fillColor, float handleRadius)
    {
        constexpr float cTrackHeight = 4.0f;
        const float y = (slider.top + slider.bottom) * 0.5f;
        const float fillRight = slider.left + t * (slider.right - slider.left);
        dc->FillRoundedRectangle({ { slider.left, y - cTrackHeight * 0.5f, slider.right, y + cTrackHeight * 0.5f },
                                   2.0f, 2.0f }, m_brushTrack.get());
        m_brushFill->SetColor(fillColor);
        dc->FillRoundedRectangle({ { slider.left, y - cTrackHeight * 0.5f, fillRight, y + cTrackHeight * 0.5f },
                                   2.0f, 2.0f }, m_brushFill.get());
        dc->FillEllipse({ { fillRight, y }, handleRadius, handleRadius }, m_brushHandle.get());
    }

    void NodeGraphRenderer::DrawLabel(ID2D1DeviceContext* dc, const std::wstring& text, const D2D1_RECT_F& rect,
                                      DWRITE_TEXT_ALIGNMENT alignment, ID2D1Brush* brush)
    {
        m_labelFormat->SetTextAlignment(alignment);
        dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), m_labelFormat.get(), rect, brush);
    }

    void NodeGraphRenderer::FillAt(ID2D1DeviceContext* dc, ID2D1Geometry* geometry, D2D1_POINT_2F at,
                                   ID2D1Brush* brush)
    {
        D2D1_MATRIX_3X2_F saved{};
        dc->GetTransform(&saved);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(at.x, at.y) * saved);
        dc->FillGeometry(geometry, brush);
        dc->SetTransform(saved);
    }

    void NodeGraphRenderer::DrawConnectionDrag(ID2D1DeviceContext* dc, const Scene& scene)
    {
        if (!scene.drag.active) return;
        auto it = scene.visuals.find(scene.drag.from.nodeId);
        if (it == scene.visuals.end()) return;
        const auto start = it->second.Pin(scene.drag.from.kind, scene.drag.from.index).value_or(D2D1_POINT_2F{});
        auto* brush = IsDataPin(scene.drag.from.kind) ? m_brushDataEdge.get() : m_brushDragLine.get();
        dc->DrawLine(start, scene.drag.currentPos, brush, 2.0f);
    }
}
