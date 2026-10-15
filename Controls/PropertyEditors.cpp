#include "pch.h"
#include "PropertyEditors.h"

namespace ShaderLab::Controls
{
    namespace
    {
        namespace Xaml = winrt::Microsoft::UI::Xaml;
        namespace XC = winrt::Microsoft::UI::Xaml::Controls;
        namespace Media = winrt::Microsoft::UI::Xaml::Media;
        using winrt::Windows::Foundation::Numerics::float2;
        using winrt::Windows::Foundation::Numerics::float3;
        using winrt::Windows::Foundation::Numerics::float4;
        using Effects::PropertyMetadata;
        using Effects::PropertyUIHint;
        using Children = XC::UIElementCollection;

        // Writes `newValue` over the property and reports the change.
        template <class T>
        void Assign(const PropertyEditTarget& target, T newValue)
        {
            if (target.apply([newValue](Graph::PropertyValue& property) { property = newValue; return true; }))
                target.changed();
        }

        bool HasEnumLabels(const PropertyMetadata* meta)
        {
            return meta && meta->uiHint == PropertyUIHint::ComboBox && !meta->enumLabels.empty();
        }

        // A combo box over the metadata's labels; T is the property's type.
        template <class T>
        void AppendEnumEditor(Children const& children, uint32_t selected,
            const PropertyMetadata& meta, const PropertyEditTarget& target)
        {
            auto combo = XC::ComboBox();
            for (const auto& label : meta.enumLabels)
                combo.Items().Append(winrt::box_value(winrt::hstring(label)));
            if (selected < meta.enumLabels.size())
                combo.SelectedIndex(static_cast<int32_t>(selected));
            combo.HorizontalAlignment(Xaml::HorizontalAlignment::Stretch);
            combo.Margin({ 0, 0, 0, 4 });
            combo.SelectionChanged([target](auto&& sender, auto&&)
            {
                const int32_t index = sender.template as<XC::ComboBox>().SelectedIndex();
                if (index < 0) return;
                Assign(target, static_cast<T>(index));
            });
            children.Append(combo);
        }

        // Makes `box` write its value, converted to T; a cleared (NaN) box
        // writes nothing. Attached after the range is set, so clamping the
        // initial value is not taken for an edit.
        template <class T>
        void CommitNumberBox(XC::NumberBox const& box, const PropertyEditTarget& target)
        {
            box.ValueChanged([target](auto&& sender, auto&&)
            {
                const double entered = sender.template as<XC::NumberBox>().Value();
                if (std::isnan(entered)) return;
                Assign(target, static_cast<T>(entered));
            });
        }

        void AppendBoolEditor(Children const& children, bool value, const PropertyEditTarget& target)
        {
            auto toggle = XC::ToggleSwitch();
            toggle.IsOn(value);
            toggle.OnContent(winrt::box_value(L"True"));
            toggle.OffContent(winrt::box_value(L"False"));
            toggle.Margin({ 0, 0, 0, 4 });
            toggle.Toggled([target](auto&& sender, auto&&)
            {
                Assign(target, sender.template as<XC::ToggleSwitch>().IsOn());
            });
            children.Append(toggle);
        }

        void AppendUintEditor(Children const& children, uint32_t value,
            const PropertyMetadata* meta, const PropertyEditTarget& target)
        {
            if (HasEnumLabels(meta))
            {
                AppendEnumEditor<uint32_t>(children, value, *meta, target);
                return;
            }
            const float step = meta ? meta->step : 1.0f;
            auto box = XC::NumberBox();
            box.Value(static_cast<double>(value));
            box.Minimum(meta ? meta->minValue : 0.0f);
            box.Maximum(meta ? meta->maxValue : 100.0f);
            box.SmallChange(step);
            box.LargeChange(step * 10.0);
            box.SpinButtonPlacementMode(XC::NumberBoxSpinButtonPlacementMode::Inline);
            box.Margin({ 0, 0, 0, 4 });
            CommitNumberBox<uint32_t>(box, target);
            children.Append(box);
        }

        // A slider and a number box kept in step; either one writes the value.
        void AppendRangedFloatEditor(Children const& children, float value,
            float minValue, float maxValue, float step, const PropertyEditTarget& target)
        {
            // Stops each control's update of the other from echoing back.
            auto syncing = std::make_shared<bool>(false);

            auto slider = XC::Slider();
            slider.Minimum(minValue);
            slider.Maximum(maxValue);
            slider.StepFrequency(step);
            slider.Value(static_cast<double>(value));
            slider.Margin({ 0, 0, 0, 0 });

            auto box = XC::NumberBox();
            box.Value(static_cast<double>(value));
            box.Minimum(minValue);
            box.Maximum(maxValue);
            box.SmallChange(step);
            box.SpinButtonPlacementMode(XC::NumberBoxSpinButtonPlacementMode::Compact);
            box.Margin({ 0, 0, 0, 4 });

            slider.ValueChanged([box, syncing, target](auto&& sender, auto&&)
            {
                if (*syncing) return;
                *syncing = true;
                const double sliderValue = sender.template as<XC::Slider>().Value();
                box.Value(sliderValue);
                Assign(target, static_cast<float>(sliderValue));
                *syncing = false;
            });
            box.ValueChanged([slider, syncing, target](auto&& sender, auto&&)
            {
                if (*syncing) return;
                *syncing = true;
                const double entered = sender.template as<XC::NumberBox>().Value();
                if (!std::isnan(entered))
                {
                    slider.Value(entered);
                    Assign(target, static_cast<float>(entered));
                }
                *syncing = false;
            });

            children.Append(slider);
            children.Append(box);
        }

        void AppendFloatEditor(Children const& children, float value,
            const PropertyEditorSpec& spec, const PropertyEditTarget& target)
        {
            const auto* meta = spec.meta;
            if (HasEnumLabels(meta))
            {
                AppendEnumEditor<float>(children, static_cast<uint32_t>(value + 0.5f), *meta, target);
                return;
            }
            const float step = meta ? meta->step : 0.01f;
            if (meta && meta->uiHint == PropertyUIHint::Slider)
            {
                const float minValue = spec.minOverride.value_or(meta->minValue);
                const float maxValue = spec.maxOverride.value_or(meta->maxValue);
                AppendRangedFloatEditor(children, value, minValue, maxValue, step, target);
                return;
            }
            auto box = XC::NumberBox();
            box.Value(static_cast<double>(value));
            box.SmallChange(step);
            box.SpinButtonPlacementMode(XC::NumberBoxSpinButtonPlacementMode::Inline);
            box.Margin({ 0, 0, 0, 4 });
            CommitNumberBox<float>(box, target);
            children.Append(box);
        }

        void AppendIntEditor(Children const& children, int32_t value,
            const PropertyMetadata* meta, const PropertyEditTarget& target)
        {
            auto box = XC::NumberBox();
            box.Value(static_cast<double>(value));
            box.Minimum(meta ? meta->minValue : static_cast<float>(INT_MIN));
            box.Maximum(meta ? meta->maxValue : static_cast<float>(INT_MAX));
            box.SmallChange(meta ? meta->step : 1.0f);
            box.SpinButtonPlacementMode(XC::NumberBoxSpinButtonPlacementMode::Inline);
            box.Margin({ 0, 0, 0, 4 });
            CommitNumberBox<int32_t>(box, target);
            children.Append(box);
        }

        // Commits on focus loss rather than per keystroke.
        void AppendStringEditor(Children const& children, const std::wstring& value,
            const PropertyEditTarget& target)
        {
            auto box = XC::TextBox();
            box.Text(winrt::hstring(value));
            box.Margin({ 0, 0, 0, 4 });
            box.LostFocus([target](auto&& sender, auto&&)
            {
                std::wstring text(sender.template as<XC::TextBox>().Text().c_str());
                if (target.apply([&text](Graph::PropertyValue& property) { property = std::move(text); return true; }))
                    target.changed();
            });
            children.Append(box);
        }

        XC::TextBlock MakeGridLabel(const wchar_t* text, double width)
        {
            auto label = XC::TextBlock();
            label.Text(text);
            label.Width(width);
            label.FontSize(11);
            return label;
        }

        // A 5x4 grid of number boxes, rows R G B A and the offset row.
        void AppendMatrixEditor(Children const& children, const D2D1_MATRIX_5X4_F& value,
            const PropertyMetadata* meta, const PropertyEditTarget& target)
        {
            static constexpr const wchar_t* scRowLabels[] = { L"R", L"G", L"B", L"A", L"Ofs" };
            static constexpr const wchar_t* scColumnLabels[] = { L"R", L"G", L"B", L"A" };

            auto grid = XC::StackPanel();
            grid.Orientation(XC::Orientation::Vertical);
            grid.Margin({ 0, 0, 0, 4 });

            auto headerRow = XC::StackPanel();
            headerRow.Orientation(XC::Orientation::Horizontal);
            auto spacer = XC::TextBlock();
            spacer.Width(32);
            headerRow.Children().Append(spacer);
            for (const wchar_t* column : scColumnLabels)
            {
                auto header = MakeGridLabel(column, 70);
                header.HorizontalTextAlignment(Xaml::TextAlignment::Center);
                headerRow.Children().Append(header);
            }
            grid.Children().Append(headerRow);

            const float* cells = &value._11;
            for (int r = 0; r < 5; ++r)
            {
                auto row = XC::StackPanel();
                row.Orientation(XC::Orientation::Horizontal);
                row.Margin({ 0, 1, 0, 1 });
                auto rowLabel = MakeGridLabel(scRowLabels[r], 32);
                rowLabel.VerticalAlignment(Xaml::VerticalAlignment::Center);
                row.Children().Append(rowLabel);

                for (int c = 0; c < 4; ++c)
                {
                    const int cell = r * 4 + c;
                    auto box = XC::NumberBox();
                    box.Value(static_cast<double>(cells[cell]));
                    box.SmallChange(meta ? meta->step : 0.01);
                    box.Width(70);
                    box.SpinButtonPlacementMode(XC::NumberBoxSpinButtonPlacementMode::Compact);
                    box.ValueChanged([target, cell](auto&& sender, auto&&)
                    {
                        const double entered = sender.template as<XC::NumberBox>().Value();
                        if (std::isnan(entered)) return;
                        const auto cellValue = static_cast<float>(entered);
                        const bool applied = target.apply([cell, cellValue](Graph::PropertyValue& property)
                        {
                            auto* matrix = std::get_if<D2D1_MATRIX_5X4_F>(&property);
                            if (!matrix) return false;
                            (&matrix->_11)[cell] = cellValue;
                            return true;
                        });
                        if (applied) target.changed();
                    });
                    row.Children().Append(box);
                }
                grid.Children().Append(row);
            }
            children.Append(grid);
        }

        // A small plot of the curve and a button that opens the full editor.
        void AppendCurveEditor(Children const& children, const std::vector<float>& value,
            const PropertyEditorSpec& spec)
        {
            constexpr float cWidth = 200.0f;
            constexpr float cHeight = 60.0f;

            auto container = XC::StackPanel();
            container.Orientation(XC::Orientation::Vertical);
            container.Margin({ 0, 0, 0, 4 });

            auto preview = XC::Canvas();
            preview.Width(cWidth);
            preview.Height(cHeight);
            preview.Background(Media::SolidColorBrush(winrt::Microsoft::UI::ColorHelper::FromArgb(255, 30, 30, 30)));

            auto polyline = Xaml::Shapes::Polyline();
            polyline.Stroke(Media::SolidColorBrush(winrt::Microsoft::UI::Colors::CornflowerBlue()));
            polyline.StrokeThickness(1.5);
            auto points = Media::PointCollection();
            if (!value.empty())
            {
                const uint32_t stride = (std::max)(1u, static_cast<uint32_t>(value.size()) / 50u);
                for (uint32_t i = 0; i < value.size(); i += stride)
                {
                    const float x = static_cast<float>(i) / static_cast<float>(value.size() - 1) * cWidth;
                    const float y = cHeight - std::clamp(value[i], 0.0f, 1.0f) * cHeight;
                    points.Append(winrt::Windows::Foundation::Point(x, y));
                }
            }
            polyline.Points(points);
            preview.Children().Append(polyline);
            container.Children().Append(preview);

            auto editButton = XC::Button();
            editButton.Content(winrt::box_value(L"Edit Curve..."));
            editButton.Margin({ 0, 4, 0, 0 });
            editButton.HorizontalAlignment(Xaml::HorizontalAlignment::Stretch);
            editButton.Click([open = spec.openCurveEditor](auto&&, auto&&) { if (open) open(); });
            container.Children().Append(editButton);
            children.Append(container);
        }

        // Component labels: the metadata's, else R/G/B/A for a key containing
        // "Color" (or "color"), else X/Y/Z/W.
        std::vector<std::wstring> ComponentLabels(const PropertyEditorSpec& spec, size_t count)
        {
            if (spec.meta && spec.meta->componentLabels.size() >= count)
                return { spec.meta->componentLabels.begin(), spec.meta->componentLabels.begin() + count };
            const bool isColor = spec.key.find(L"Color") != std::wstring::npos ||
                                 spec.key.find(L"color") != std::wstring::npos;
            static const std::wstring scColor[] = { L"R", L"G", L"B", L"A" };
            static const std::wstring scAxis[] = { L"X", L"Y", L"Z", L"W" };
            const std::wstring* labels = isColor ? scColor : scAxis;
            return { labels, labels + count };
        }

        // Sets component `index` of a float2/3/4 property; other types are left alone.
        void SetVectorComponent(Graph::PropertyValue& property, int index, float value)
        {
            std::visit([index, value](auto& vec)
            {
                using V = std::decay_t<decltype(vec)>;
                if constexpr (std::is_same_v<V, float2> || std::is_same_v<V, float3> || std::is_same_v<V, float4>)
                {
                    float* components = &vec.x;
                    if (index >= 0 && index < static_cast<int>(sizeof(V) / sizeof(float)))
                        components[index] = value;
                }
            }, property);
        }

        // One labelled number box per component.
        template <class V>
        void AppendVectorEditor(Children const& children, const V& value,
            const PropertyEditorSpec& spec, const PropertyEditTarget& target)
        {
            constexpr int cCount = static_cast<int>(sizeof(V) / sizeof(float));
            const auto labels = ComponentLabels(spec, cCount);
            const float* components = &value.x;
            const auto* meta = spec.meta;
            const float minValue = meta ? meta->minValue : -FLT_MAX;
            const float maxValue = meta ? meta->maxValue : FLT_MAX;
            const float step = meta ? meta->step : 0.01f;

            auto column = XC::StackPanel();
            column.Orientation(XC::Orientation::Vertical);
            column.Margin({ 0, 0, 0, 4 });

            for (int i = 0; i < cCount; ++i)
            {
                auto row = XC::StackPanel();
                row.Orientation(XC::Orientation::Horizontal);
                row.Margin({ 0, 1, 0, 1 });

                auto label = XC::TextBlock();
                label.Text(winrt::hstring(labels[i]));
                label.FontSize(11);
                label.Width(20);
                label.VerticalAlignment(Xaml::VerticalAlignment::Center);
                row.Children().Append(label);

                auto box = XC::NumberBox();
                box.Value(static_cast<double>(components[i]));
                box.Minimum((meta && static_cast<int>(meta->componentMin.size()) > i) ? meta->componentMin[i] : minValue);
                box.Maximum((meta && static_cast<int>(meta->componentMax.size()) > i) ? meta->componentMax[i] : maxValue);
                box.SmallChange(step);
                box.SpinButtonPlacementMode(XC::NumberBoxSpinButtonPlacementMode::Compact);
                box.HorizontalAlignment(Xaml::HorizontalAlignment::Stretch);
                box.ValueChanged([target, i](auto&& sender, auto&&)
                {
                    const double entered = sender.template as<XC::NumberBox>().Value();
                    if (std::isnan(entered)) return;
                    const auto componentValue = static_cast<float>(entered);
                    const bool applied = target.apply([i, componentValue](Graph::PropertyValue& property)
                    {
                        SetVectorComponent(property, i, componentValue);
                        return true;
                    });
                    if (applied) target.changed();
                });
                row.Children().Append(box);
                column.Children().Append(row);
            }
            children.Append(column);
        }
    }

    void AppendPropertyEditor(
        winrt::Microsoft::UI::Xaml::Controls::UIElementCollection const& children,
        const Graph::PropertyValue& value,
        const PropertyEditorSpec& spec,
        const PropertyEditTarget& target)
    {
        std::visit([&](const auto& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                AppendBoolEditor(children, v, target);
            else if constexpr (std::is_same_v<T, uint32_t>)
                AppendUintEditor(children, v, spec.meta, target);
            else if constexpr (std::is_same_v<T, float>)
                AppendFloatEditor(children, v, spec, target);
            else if constexpr (std::is_same_v<T, int32_t>)
                AppendIntEditor(children, v, spec.meta, target);
            else if constexpr (std::is_same_v<T, std::wstring>)
                AppendStringEditor(children, v, target);
            else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                AppendMatrixEditor(children, v, spec.meta, target);
            else if constexpr (std::is_same_v<T, std::vector<float>>)
                AppendCurveEditor(children, v, spec);
            else
                AppendVectorEditor(children, v, spec, target);
        }, value);
    }
}
