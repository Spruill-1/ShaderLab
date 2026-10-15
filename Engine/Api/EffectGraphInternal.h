#pragma once

#include "pch_engine.h"
#include "../../EngineExport.h"
#include "EffectGraph.h"

namespace ShaderLab::Graph { class GraphModel; }
namespace ShaderLab::Effects { class SourceNodeFactory; }
namespace ShaderLab::Rendering
{
    class GraphEvaluator;
    struct DeviceStack;
    struct FrameRequest;
    struct FrameResult;
    struct DisplayProfile;
}

// The context as ShaderLab's own hosts and tests see it: the C++ objects behind
// the public interfaces, for what the public API leaves out on purpose (canvas
// layout, the UI snapshot, Effect Designer compiles, perf counters, MCP).
// Reached with QueryInterface from IEffectGraphContext; only code built with
// this engine may use it.
//
// Its IID changes with every SHADERLAB_ENGINE_ABI_VERSION, so a host built
// against another engine gets E_NOINTERFACE rather than a mismatched layout.
static_assert(SHADERLAB_ENGINE_ABI_VERSION == 7,
    "New engine ABI version: give IEffectGraphContextInternal a new IID and update this check.");

MIDL_INTERFACE("684bd71b-4d22-4ce7-b0d9-a5bf835e25c3")
IEffectGraphContextInternal : public IEffectGraphContext
{
    // The graph, evaluator, sources and devices the context owns. Same
    // threading rule as the context: one thread at a time.
    STDMETHOD_(ShaderLab::Graph::GraphModel&, Model)() PURE;
    STDMETHOD_(ShaderLab::Rendering::GraphEvaluator&, Evaluator)() PURE;
    STDMETHOD_(ShaderLab::Effects::SourceNodeFactory&, Sources)() PURE;
    STDMETHOD_(const ShaderLab::Rendering::DeviceStack&, Devices)() PURE;

    // The graph id behind a node interface; 0 when it was removed or belongs
    // to another context.
    STDMETHOD_(uint32_t, NodeId)(IEffectGraphNode* node) PURE;

    // The node interface for a graph id: the same object while anything holds
    // a reference to it. S_FALSE and null for a missing id.
    STDMETHOD(NodeFor)(uint32_t nodeId, IEffectGraphNode** node) PURE;

    // One frame as a host drives it: its own demand roots, target, GPU timer
    // and draw callback (Rendering::FrameExecutor::Run on the context's
    // device context).
    STDMETHOD_(ShaderLab::Rendering::FrameResult, RunFrame)(const ShaderLab::Rendering::FrameRequest& request) PURE;

    // The display Working Space nodes describe before every frame: a host's
    // monitor or pinned profile. The context uses the defaults until set.
    STDMETHOD_(void, SetDisplayProfile)(const ShaderLab::Rendering::DisplayProfile& profile) PURE;

    // Moves the context onto another device: an adapter switch or a removed
    // device. The graph is kept, effects and shaders are rebuilt and sources
    // reopened; node interfaces stay valid. Images from Realize do not.
    STDMETHOD(MoveToDevice)(ID3D11Device* device) PURE;
};
