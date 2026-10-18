#pragma once

#include "pch_engine.h"
#include "Graph/EffectNode.h"

// ShaderLab's live capture Sources -- DXGI Desktop Duplication of a monitor
// and Windows Graphics Capture of a picked window or monitor -- plugged into
// EffectGraph as live source kinds (Effects/LiveSource.h).
namespace ShaderLab::Sources
{
    // The marker properties saved graphs carry.
    inline constexpr const wchar_t* cDesktopDuplicationMarker = L"IsDxgiDuplicateOutput";
    inline constexpr const wchar_t* cGraphicsCaptureMarker    = L"IsWindowsGraphicsCapture";

    // Registers both kinds, and the "Desktop Duplication" / "DXGI Duplicate
    // Output" / "Display Capture" node names, which take the uint arguments
    // adapterIndex and outputIndex and fail, listing the outputs, when that
    // pair does not exist. RegisterShaderLabSupport calls it.
    void RegisterCaptureSources();

    // A Source capturing DXGI output `outputIndex` of adapter `adapterIndex`.
    Graph::EffectNode CreateDesktopDuplicationNode(
        uint32_t adapterIndex,
        uint32_t outputIndex,
        const std::wstring& displayName = L"");

    // A Windows Graphics Capture Source. It captures nothing until its item
    // is handed to SourceNodeFactory::SetLiveSourceTarget.
    Graph::EffectNode CreateGraphicsCaptureNode(
        const std::wstring& displayName = L"Windows Graphics Capture");

    bool IsDesktopDuplication(const Graph::EffectNode& node);
}
