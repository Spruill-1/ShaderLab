#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Graph
{
    class GraphModel;
}

// Bringing a graph's ShaderLab effect nodes up to the registry's version.
namespace ShaderLab::Effects
{
    struct ShaderLabEffectDescriptor;

    // The registry's descriptor for `node` when it is newer than the version
    // the node was created from; nullptr otherwise.
    SHADERLAB_API const ShaderLabEffectDescriptor* NewerDescriptorFor(const Graph::EffectNode& node);

    // Replaces the node's definition, pins and properties with the
    // descriptor's, keeping the values of properties that still exist, and
    // marks it dirty. The caller invalidates the evaluator's state for it.
    // Render thread only.
    SHADERLAB_API bool UpgradeNode(Graph::GraphModel& graph, Graph::EffectNode& node,
        const ShaderLabEffectDescriptor& descriptor);

    // Upgrades every outdated node and returns the ids it upgraded.
    SHADERLAB_API std::vector<uint32_t> UpgradeOutdatedNodes(Graph::GraphModel& graph);
}
