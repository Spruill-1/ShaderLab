#include "pch.h"
#include "CurveEditorDialog.h"
#include "DialogCoordinator.h"

namespace ShaderLab::Controls
{
    namespace
    {
        namespace XC = winrt::Microsoft::UI::Xaml::Controls;
        namespace Media = winrt::Microsoft::UI::Xaml::Media;
        namespace Shapes = winrt::Microsoft::UI::Xaml::Shapes;
        using winrt::Windows::Foundation::Point;

        constexpr float cCanvasWidth = 400.0f;
        constexpr float cCanvasHeight = 300.0f;
        constexpr float cPointRadius = 6.0f;
        constexpr uint32_t cMaxControlPoints = 16;

        // The editor's state, shared by its event handlers.
        struct CurveEditorState
        {
            CurveShape shape;
            std::vector<Shapes::Ellipse> handles;
            Shapes::Polyline polyline{ nullptr };
            int dragging{ -1 };

            void MoveHandle(size_t index)
            {
                XC::Canvas::SetLeft(handles[index], shape.points[index].X - cPointRadius);
                XC::Canvas::SetTop(handles[index], shape.points[index].Y - cPointRadius);
            }

            void RedrawCurve()
            {
                auto points = Media::PointCollection();
                for (const auto& p : shape.points)
                    points.Append(p);
                polyline.Points(points);
            }
        };

        void AppendGridLines(XC::Canvas const& canvas)
        {
            const auto brush = Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 60, 60, 60));
            for (int i = 1; i < 4; ++i)
            {
                const float y = cCanvasHeight * i / 4.0f;
                auto horizontal = Shapes::Line();
                horizontal.X1(0); horizontal.X2(cCanvasWidth); horizontal.Y1(y); horizontal.Y2(y);
                horizontal.Stroke(brush);
                horizontal.StrokeThickness(0.5);
                canvas.Children().Append(horizontal);

                const float x = cCanvasWidth * i / 4.0f;
                auto vertical = Shapes::Line();
                vertical.X1(x); vertical.X2(x); vertical.Y1(0); vertical.Y2(cCanvasHeight);
                vertical.Stroke(brush);
                vertical.StrokeThickness(0.5);
                canvas.Children().Append(vertical);
            }
        }

        // Evenly spaced control points sampled from the LUT.
        std::vector<Point> SampleControlPoints(const std::vector<float>& lut)
        {
            const auto lutSize = static_cast<uint32_t>(lut.size());
            const uint32_t count = (std::max)(2u, (std::min)(cMaxControlPoints, lutSize));
            std::vector<Point> points;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(count - 1);
                const uint32_t index = (std::min)(static_cast<uint32_t>(t * (lutSize - 1)), lutSize - 1);
                points.push_back({ t * cCanvasWidth, cCanvasHeight - std::clamp(lut[index], 0.0f, 1.0f) * cCanvasHeight });
            }
            return points;
        }
    }

    void CurveShape::WriteTo(std::vector<float>& lut) const
    {
        const auto lutSize = static_cast<uint32_t>(lut.size());
        const int last = static_cast<int>(points.size()) - 1;
        for (uint32_t i = 0; i < lutSize; ++i)
        {
            const float x = static_cast<float>(i) / static_cast<float>(lutSize - 1) * width;

            // The segment [segment, segment + 1] holding x.
            int segment = 0;
            for (int j = 1; j <= last; ++j)
            {
                if (points[j].X >= x)
                {
                    segment = j - 1;
                    break;
                }
                segment = j;
            }

            float value;
            if (segment >= last)
            {
                value = 1.0f - points.back().Y / height;
            }
            else
            {
                const float x0 = points[segment].X, x1 = points[segment + 1].X;
                const float y0 = points[segment].Y, y1 = points[segment + 1].Y;
                const float t = (x1 > x0) ? (x - x0) / (x1 - x0) : 0.0f;
                value = 1.0f - (y0 + t * (y1 - y0)) / height;
            }
            lut[i] = std::clamp(value, 0.0f, 1.0f);
        }
    }

    void ShowCurveEditorDialog(
        DialogCoordinator& dialogs,
        winrt::Microsoft::UI::Xaml::XamlRoot const& root,
        const std::wstring& propertyKey,
        const std::vector<float>& lut,
        std::function<void(const CurveShape&)> apply)
    {
        if (lut.empty()) return;

        auto state = std::make_shared<CurveEditorState>();
        state->shape = { SampleControlPoints(lut), cCanvasWidth, cCanvasHeight };

        auto canvas = XC::Canvas();
        canvas.Width(cCanvasWidth);
        canvas.Height(cCanvasHeight);
        canvas.Background(Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));
        AppendGridLines(canvas);

        state->polyline = Shapes::Polyline();
        state->polyline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
        state->polyline.StrokeThickness(2.0);
        canvas.Children().Append(state->polyline);
        state->RedrawCurve();

        for (size_t i = 0; i < state->shape.points.size(); ++i)
        {
            auto handle = Shapes::Ellipse();
            handle.Width(cPointRadius * 2);
            handle.Height(cPointRadius * 2);
            handle.Fill(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::White()));
            canvas.Children().Append(handle);
            state->handles.push_back(handle);
            state->MoveHandle(i);
        }

        // Positions are taken relative to the canvas the points live in.
        canvas.PointerPressed([state](auto&& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            const auto pos = args.GetCurrentPoint(sender.template as<winrt::Microsoft::UI::Xaml::UIElement>()).Position();
            const auto& points = state->shape.points;
            for (int i = static_cast<int>(points.size()) - 1; i >= 0; --i)
            {
                const float dx = pos.X - points[i].X;
                const float dy = pos.Y - points[i].Y;
                if (dx * dx + dy * dy < (cPointRadius * 3) * (cPointRadius * 3))
                {
                    state->dragging = i;
                    break;
                }
            }
        });

        canvas.PointerMoved([state](auto&& sender, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            const int index = state->dragging;
            if (index < 0) return;
            const auto pos = args.GetCurrentPoint(sender.template as<winrt::Microsoft::UI::Xaml::UIElement>()).Position();
            auto& points = state->shape.points;

            // The end points stay on the edges; the rest keep their order.
            float x = pos.X;
            const float y = std::clamp(pos.Y, 0.0f, cCanvasHeight);
            if (index == 0)
                x = 0.0f;
            else if (index == static_cast<int>(points.size()) - 1)
                x = cCanvasWidth;
            else
                x = std::clamp(x, points[index - 1].X + 1.0f, points[index + 1].X - 1.0f);

            points[index] = { x, y };
            state->MoveHandle(static_cast<size_t>(index));
            state->RedrawCurve();
        });

        canvas.PointerReleased([state](auto&&, auto&&) { state->dragging = -1; });

        auto dialog = XC::ContentDialog();
        dialog.Title(winrt::box_value(L"Curve Editor — " + propertyKey));
        dialog.PrimaryButtonText(L"Apply");
        dialog.CloseButtonText(L"Cancel");
        dialog.XamlRoot(root);

        auto content = XC::StackPanel();
        content.Children().Append(canvas);

        auto resetButton = XC::Button();
        resetButton.Content(winrt::box_value(L"Reset to Identity"));
        resetButton.Margin({ 0, 8, 0, 0 });
        resetButton.Click([state](auto&&, auto&&)
        {
            auto& points = state->shape.points;
            for (size_t i = 0; i < points.size(); ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(points.size() - 1);
                points[i] = { t * cCanvasWidth, cCanvasHeight - t * cCanvasHeight };
                state->MoveHandle(i);
            }
            state->RedrawCurve();
        });
        content.Children().Append(resetButton);
        dialog.Content(content);

        dialog.PrimaryButtonClick([state, apply = std::move(apply)](auto&&, auto&&)
        {
            if (apply) apply(state->shape);
        });

        dialogs.ShowAsync(dialog);
    }
}
