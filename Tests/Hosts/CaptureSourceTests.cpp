#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/NodeCatalog.h"
#include "Effects/SourceNodeFactory.h"
#include "Graph/GraphModel.h"
#include "Graph/SourceKind.h"
#include "Sources/CaptureSources.h"

#include <cstdio>
#include <winrt/Windows.Graphics.Capture.h>
#include <windows.graphics.capture.interop.h>

// ShaderLab's capture Sources, registered with EffectGraph as live source
// kinds by RegisterShaderLabSupport (SL_TEST_SETUP).
namespace
{
    using namespace ShaderLab::Tests;
    using ShaderLab::Effects::SourceNodeFactory;
    using ShaderLab::Graph::SourceKind;

    bool HasLiveFields(const ShaderLab::Graph::EffectNode& node)
    {
        const auto& names = SourceNodeFactory::scLiveFields;
        if (node.analysisOutput.fields.size() != names.size()) return false;
        for (size_t i = 0; i < names.size(); ++i)
            if (node.analysisOutput.fields[i].name != names[i]) return false;
        return true;
    }

    SL_TEST_CASE(TestCaptureSourceKinds, Tag::Fast)
    {
        std::printf("\n=== ShaderLab: capture source kinds ===\n");
        const auto duplication = ShaderLab::Sources::CreateDesktopDuplicationNode(0, 1);
        const auto graphicsCapture = ShaderLab::Sources::CreateGraphicsCaptureNode();
        TEST("Capture_NodesAreLive", ShaderLab::Graph::ClassifySource(duplication) == SourceKind::Live &&
             ShaderLab::Graph::ClassifySource(graphicsCapture) == SourceKind::Live);
        TEST("Capture_FieldsSeededAtCreation", HasLiveFields(duplication) && HasLiveFields(graphicsCapture));
        TEST("Capture_DuplicationRecognised", ShaderLab::Sources::IsDesktopDuplication(duplication) &&
             !ShaderLab::Sources::IsDesktopDuplication(graphicsCapture) &&
             std::get<uint32_t>(duplication.properties.at(L"OutputIndex")) == 1);

        // A graph saved before the split carries the same marker properties.
        const auto loaded = ShaderLab::Graph::GraphModel::FromJson(LR"({"nodes":[
            {"id":1,"name":"Screen","type":"Source","position":[0,0],
             "properties":[{"name":"IsDxgiDuplicateOutput","type":"bool","value":true},
                           {"name":"AdapterIndex","type":"uint","value":0},
                           {"name":"OutputIndex","type":"uint","value":0}],
             "inputPins":[],"outputPins":[{"name":"Frame","index":0}]}],"edges":[],"nextId":2})");
        const auto* screen = loaded.FindNode(1);
        TEST("Capture_SavedGraphStillCaptures", screen && ShaderLab::Graph::ClassifySource(*screen) == SourceKind::Live &&
             ShaderLab::Sources::IsDesktopDuplication(*screen));

        // By name, as MCP creates it: a missing output is refused, naming the
        // outputs there are.
        ShaderLab::Graph::GraphModel graph;
        ShaderLab::Effects::NodeCreateArgs args;
        args.uints[L"adapterIndex"] = 97;
        args.uints[L"outputIndex"] = 3;
        auto missing = ShaderLab::Effects::CreateNodeByName(L"Desktop Duplication", args, graph);
        std::printf("  [info] missing output: '%ls'\n", missing.error.c_str());
        TEST("Capture_MissingOutputRefused", !missing.node &&
             missing.error.starts_with(L"No DXGI output 97:3. Available adapter:output pairs: "));
    }

    // Windows Graphics Capture opens only on a picked item, handed over as
    // the node's live-source target.
    SL_TEST_CASE(TestGraphicsCaptureTarget, Tag::Gpu)
    {
        std::printf("\n=== ShaderLab: Windows Graphics Capture target ===\n");
        const std::wstring notPicked = L"Choose a window or monitor for this Windows Graphics Capture source.";
        ShaderLab::Graph::GraphModel graph;
        SourceNodeFactory factory;
        const uint32_t id = graph.AddNode(ShaderLab::Sources::CreateGraphicsCaptureNode());
        factory.PrepareSourceNode(*graph.FindNode(id), g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        TEST("Capture_NoItemAsksForOne", graph.FindNode(id)->runtimeError == notPicked &&
             graph.FindNode(id)->cachedOutput == nullptr);

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        try
        {
            auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                                         IGraphicsCaptureItemInterop>();
            HMONITOR monitor = ::MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            winrt::check_hresult(interop->CreateForMonitor(monitor,
                winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(item)));
        }
        catch (const winrt::hresult_error&)
        {
            item = nullptr;   // no capture here (session 0, CI): reported as a skip below
        }
        if (!item)
        {
            SkipMissingFixture(L"a capture item for the primary monitor (no desktop session)");
            return;
        }

        // The picked item is not a device resource: releasing the device's
        // objects keeps it, and the capture reopens.
        factory.SetLiveSourceTarget(id, item);
        factory.PrepareSourceNode(*graph.FindNode(id), g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        const std::wstring opened = graph.FindNode(id)->runtimeError;
        factory.ReleaseCache();
        factory.PrepareSourceNode(*graph.FindNode(id), g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        std::printf("  [info] after pick: '%ls'; after release: '%ls'\n", opened.c_str(),
                    graph.FindNode(id)->runtimeError.c_str());
        TEST("Capture_PickedItemOpens", opened != notPicked);
        TEST("Capture_PickedItemSurvivesRelease", graph.FindNode(id)->runtimeError != notPicked);
    }
}
