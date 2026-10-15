#include "pch.h"
#include "PixelTracePanelController.h"
#include "Graph/ValueFormat.h"

namespace ShaderLab::Controls
{
    namespace
    {
        namespace MUX = winrt::Microsoft::UI::Xaml;

        constexpr wchar_t cMonoFont[] = L"Cascadia Mono, Consolas, Courier New";

        std::wstring FormatPixel(const Rendering::TracedPixel& p, uint32_t unit)
        {
            switch (unit)
            {
            case 1:
                return std::format(L"R:{}  G:{}  B:{}  A:{}", p.sR, p.sG, p.sB, p.sA);
            case 2:
                return std::format(L"R:{:.1f}  G:{:.1f}  B:{:.1f}", p.scR * 80.0f, p.scG * 80.0f, p.scB * 80.0f);
            case 3:
                return std::format(L"R:{:.4f}  G:{:.4f}  B:{:.4f}", p.pqR, p.pqG, p.pqB);
            default:
                return std::format(L"R:{:.4f}  G:{:.4f}  B:{:.4f}  A:{:.4f}", p.scR, p.scG, p.scB, p.scA);
            }
        }

        // "[pin] Name", then the node's analysis fields or its pixel.
        std::wstring LabelText(const Rendering::PixelTraceNode& node, uint32_t unit)
        {
            std::wstring text = node.pinName.empty() ? node.nodeName : L"[" + node.pinName + L"] " + node.nodeName;
            text += L"\n";
            if (node.hasAnalysisOutput)
            {
                for (const auto& field : node.analysisFields)
                    text += field.name + L": " + Graph::FormatAnalysisField(field, 0) + L"\n";
            }
            else if (node.hasPixel)
            {
                text += FormatPixel(node.pixel, unit);
            }
            else
            {
                text += L"(no output)";
            }
            return text;
        }

        std::wstring LuminanceText(const Rendering::PixelTraceNode& node)
        {
            if (node.hasAnalysisOutput || !node.hasPixel) return {};
            return std::format(L"{:.1f} cd/m²", node.pixel.luminanceNits);
        }

        template <class Fn>
        void ForEachPreOrder(const Rendering::PixelTraceNode& node, uint32_t depth, Fn&& fn)
        {
            fn(node, depth);
            for (const auto& child : node.inputs)
                ForEachPreOrder(child, depth + 1, fn);
        }

        MUX::Controls::TextBlock MonoText(const std::wstring& text)
        {
            MUX::Controls::TextBlock block;
            block.Text(text);
            block.FontFamily(MUX::Media::FontFamily(cMonoFont));
            block.FontSize(12);
            return block;
        }
    }

    void PixelTracePanelController::Attach(MUX::Controls::StackPanel const& rows,
                                           MUX::Controls::TextBlock const& positionText,
                                           MUX::Controls::ComboBox const& unitSelector,
                                           MUX::Controls::Canvas const& crosshair)
    {
        m_rows = rows;
        m_positionText = positionText;
        m_unitSelector = unitSelector;
        m_crosshair = crosshair;
        unitSelector.SelectedIndex(0);
        unitSelector.SelectionChanged([this](auto&&, auto&&)
        {
            m_unit = static_cast<uint32_t>((std::max)(0, m_unitSelector.SelectedIndex()));
            if (m_trace) UpdateRowText(*m_trace);
        });
    }

    void PixelTracePanelController::Show(const std::optional<Rendering::PixelTraceNode>& trace,
                                         const std::wstring& position, bool outOfBounds)
    {
        m_positionText.Text(position);
        m_trace = trace;
        m_rowCache.clear();
        m_rows.Children().Clear();
        if (m_trace)
            BuildRows(*m_trace, outOfBounds);
    }

    void PixelTracePanelController::BuildRows(const Rendering::PixelTraceNode& root, bool outOfBounds)
    {
        ForEachPreOrder(root, 0, [&](const Rendering::PixelTraceNode& node, uint32_t depth)
        {
            auto row = CreateRow(node);
            row.Margin({ static_cast<double>(depth) * 20.0, 0, 0, 0 });
            m_rowCache.push_back(row);
            m_rows.Children().Append(row);
        });

        if (outOfBounds)
        {
            MUX::Controls::InfoBar warning;
            warning.IsOpen(true);
            warning.IsClosable(false);
            warning.Severity(MUX::Controls::InfoBarSeverity::Warning);
            warning.Title(L"Outside image bounds");
            warning.Message(L"The selected pixel is outside the image area. Values shown are from the nearest edge pixel.");
            warning.Margin({ 0, 8, 0, 0 });
            m_rows.Children().Append(warning);
        }
    }

    void PixelTracePanelController::UpdateRowText(const Rendering::PixelTraceNode& root)
    {
        size_t index = 0;
        ForEachPreOrder(root, 0, [&](const Rendering::PixelTraceNode& node, uint32_t)
        {
            if (index >= m_rowCache.size()) return;
            auto row = m_rowCache[index++];
            // Child 0 is the label, child 1 the luminance.
            row.Children().GetAt(0).as<MUX::Controls::TextBlock>().Text(LabelText(node, m_unit));
            row.Children().GetAt(1).as<MUX::Controls::TextBlock>().Text(LuminanceText(node));
        });
    }

    MUX::Controls::Grid PixelTracePanelController::CreateRow(const Rendering::PixelTraceNode& node) const
    {
        MUX::Controls::Grid row;
        MUX::Controls::ColumnDefinition labelColumn;
        labelColumn.Width({ 1, MUX::GridUnitType::Star });
        row.ColumnDefinitions().Append(labelColumn);
        MUX::Controls::ColumnDefinition luminanceColumn;
        luminanceColumn.Width({ 0, MUX::GridUnitType::Auto });
        row.ColumnDefinitions().Append(luminanceColumn);

        auto label = MonoText(LabelText(node, m_unit));
        label.TextWrapping(MUX::TextWrapping::NoWrap);
        MUX::Controls::Grid::SetColumn(label, 0);
        row.Children().Append(label);

        auto luminance = MonoText(LuminanceText(node));
        luminance.Margin({ 8, 0, 0, 0 });
        luminance.VerticalAlignment(MUX::VerticalAlignment::Top);
        MUX::Controls::Grid::SetColumn(luminance, 1);
        row.Children().Append(luminance);
        return row;
    }

    void PixelTracePanelController::SetMarker(float dipX, float dipY, const Rendering::PreviewView& viewAtClick)
    {
        m_hasMarker = true;
        m_markerDipX = dipX;
        m_markerDipY = dipY;
        m_markerView = viewAtClick;
    }

    void PixelTracePanelController::UpdateCrosshair(const Rendering::PreviewView& current, bool visible)
    {
        m_crosshair.Children().Clear();
        MUX::Media::RectangleGeometry clip;
        clip.Rect({ 0, 0, static_cast<float>(m_crosshair.ActualWidth()), static_cast<float>(m_crosshair.ActualHeight()) });
        m_crosshair.Clip(clip);
        if (!visible || !m_hasMarker || !m_trace) return;

        // The clicked image point, projected through the current view.
        const float imageX = (m_markerDipX - m_markerView.panX) / m_markerView.zoom;
        const float imageY = (m_markerDipY - m_markerView.panY) / m_markerView.zoom;
        const float cx = imageX * current.zoom + current.panX;
        const float cy = imageY * current.zoom + current.panY;
        constexpr float cArm = 16.0f;
        constexpr float cGap = 4.0f;

        auto line = [&](float x1, float y1, float x2, float y2, winrt::Windows::UI::Color color, double thickness)
        {
            MUX::Shapes::Line l;
            l.X1(x1); l.Y1(y1); l.X2(x2); l.Y2(y2);
            l.Stroke(MUX::Media::SolidColorBrush(color));
            l.StrokeThickness(thickness);
            m_crosshair.Children().Append(l);
        };
        // A black outline under a white line.
        for (const auto& [color, thickness] : { std::pair{ winrt::Windows::UI::Color{ 230, 0, 0, 0 }, 3.0 },
                                                 std::pair{ winrt::Windows::UI::Color{ 230, 255, 255, 255 }, 1.5 } })
        {
            line(cx - cArm, cy, cx - cGap, cy, color, thickness);
            line(cx + cGap, cy, cx + cArm, cy, color, thickness);
            line(cx, cy - cArm, cx, cy - cGap, color, thickness);
            line(cx, cy + cGap, cx, cy + cArm, color, thickness);
        }
    }

    void PixelTracePanelController::Reset()
    {
        m_trace.reset();
        m_rowCache.clear();
        m_hasMarker = false;
        m_rows.Children().Clear();
        m_positionText.Text(L"Click preview to trace a pixel");
    }
}
