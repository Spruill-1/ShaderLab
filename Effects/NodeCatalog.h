#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Graph
{
    class GraphModel;
}

// Creating a node from the name a client gives it, the one lookup behind
// /graph/add-node and /graph/apply.
namespace ShaderLab::Effects
{
    // What a node is created with besides its name.
    struct NodeCreateArgs
    {
        std::wstring filePath;        // Video / Image Source
        uint32_t     adapterIndex{ 0 };   // desktop duplication
        uint32_t     outputIndex{ 0 };
    };

    struct NodeCreateResult
    {
        std::optional<Graph::EffectNode> node;
        std::wstring error;           // why there is no node
        // A Source that needs SourceNodeFactory::PrepareSourceNode once added:
        // a file-backed Video / Image Source, or a desktop duplication.
        bool needsPrepare{ false };
        bool desktopDuplication{ false };
    };

    // Resolves, in order: "Custom Pixel Shader" / "Custom Compute Shader" /
    // "Custom D3D11 Compute Shader" (an empty shader), a ShaderLab effect, a
    // Direct2D built-in, "Output" (numbered "Output 2", ... after the first in
    // `graph`), "Desktop Duplication" / "DXGI Duplicate Output" / "Display
    // Capture" (fails, listing the outputs, when the adapter:output pair does
    // not exist), and "Video Source" / "Video" / "Image Source" / "Image".
    NodeCreateResult CreateNodeByName(
        std::wstring_view name, const NodeCreateArgs& args, const Graph::GraphModel& graph);

    // A node running an authored custom effect: one input pin per input name,
    // an output pin and the custom effect's CLSID unless it is a D3D11 compute
    // (data only), each parameter at its default, and contracts derived from
    // those properties as a loaded graph's would be.
    Graph::EffectNode CreateCustomEffectNode(Graph::CustomEffectDefinition definition, std::wstring name);
}
