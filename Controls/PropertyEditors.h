#pragma once

#include "pch.h"
#include "../Graph/PropertyValue.h"
#include "../Effects/PropertyMetadata.h"

#include <functional>
#include <optional>

// The Properties panel's editors, one per PropertyValue alternative. Each
// builder appends its controls to the panel; the controls write the property
// through the PropertyEditTarget they are given.
namespace ShaderLab::Controls
{
    // Where an editor's changes go. `apply` runs the edit on the render
    // thread and returns whether it was applied; `changed` runs on the UI
    // thread after an applied edit.
    struct PropertyEditTarget
    {
        std::function<bool(const std::function<bool(Graph::PropertyValue&)>&)> apply;
        std::function<void()> changed;
    };

    // What decides an editor's shape besides the value's type.
    struct PropertyEditorSpec
    {
        std::wstring key;
        const Effects::PropertyMetadata* meta{ nullptr };   // nullptr: type defaults
        // A float's slider range from other properties (a parameter node's
        // Min / Max), overriding the metadata's.
        std::optional<float> minOverride;
        std::optional<float> maxOverride;
        // Opens the full editor for a curve (std::vector<float>) value.
        std::function<void()> openCurveEditor;
    };

    // Appends the editor for `value`: a toggle for bool, a combo box for an
    // enum (uint or float with ComboBox labels), slider + number box for a
    // Slider float, number boxes for other numbers and per vector component,
    // a text box for a string, a 5x4 grid for a matrix and a preview with an
    // Edit Curve button for a curve.
    void AppendPropertyEditor(
        winrt::Microsoft::UI::Xaml::Controls::UIElementCollection const& children,
        const Graph::PropertyValue& value,
        const PropertyEditorSpec& spec,
        const PropertyEditTarget& target);
}
