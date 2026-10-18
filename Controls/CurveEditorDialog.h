#pragma once

#include "pch.h"

#include <functional>

namespace ShaderLab::Controls
{
    class DialogCoordinator;

    // A curve drawn as control points on the editor's canvas (y down, the
    // canvas height being output 0).
    struct CurveShape
    {
        std::vector<winrt::Windows::Foundation::Point> points;
        float width{ 1.0f };
        float height{ 1.0f };

        // Fills every entry of `lut` by linear interpolation between the
        // points, clamped to [0, 1].
        void WriteTo(std::vector<float>& lut) const;
    };

    // Opens the curve editor on `lut` (16 draggable points sampled from it,
    // ends pinned to the edges, plus Reset to Identity). `apply` receives the
    // edited curve when the user presses Apply.
    void ShowCurveEditorDialog(
        DialogCoordinator& dialogs,
        winrt::Microsoft::UI::Xaml::XamlRoot const& root,
        const std::wstring& propertyKey,
        const std::vector<float>& lut,
        std::function<void(const CurveShape&)> apply);
}
