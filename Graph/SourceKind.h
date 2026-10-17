#pragma once

#include "pch_engine.h"

namespace ShaderLab::Graph
{
    struct EffectNode;

    // Where a Source node's image comes from. Saved graphs record it as the
    // bool properties IsDxgiDuplicateOutput / IsWindowsGraphicsCapture /
    // IsVideo, a media path, or the Flood CLSID; ClassifySource reads them.
    enum class SourceKind : uint8_t
    {
        None,                // not a Source node, or nothing to produce yet
        Image,               // a still image file at mediaPath
        Flood,               // a D2D Flood of its Color property
        Video,               // a video file at mediaPath
        DesktopDuplication,  // a DXGI output capture
        GraphicsCapture,     // a Windows Graphics Capture item
    };

    // The marker properties, in the order ClassifySource tests them.
    inline constexpr const wchar_t* cDesktopDuplicationMarker = L"IsDxgiDuplicateOutput";
    inline constexpr const wchar_t* cGraphicsCaptureMarker    = L"IsWindowsGraphicsCapture";
    inline constexpr const wchar_t* cVideoMarker              = L"IsVideo";

    // The kind of a node. A marker counts only when it is a true bool. An
    // Image node with an empty path is still an Image: it has no file yet.
    SourceKind ClassifySource(const EffectNode& node);

    // DesktopDuplication or GraphicsCapture.
    inline bool IsLiveCapture(SourceKind kind)
    {
        return kind == SourceKind::DesktopDuplication || kind == SourceKind::GraphicsCapture;
    }
}
