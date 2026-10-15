#pragma once

#include "pch_engine.h"
#include "../EngineExport.h"
#include "GraphModel.h"

// Graph edits the GUI makes on the user's behalf, kept here so any host can
// make the same ones. All of them write the live graph: render thread only.
namespace ShaderLab::Graph
{
    // Every change to a node's settings goes through one of these, so the
    // node is marked dirty with it. Each takes the node or (graph, id); the
    // id form returns false, changing nothing, when the node is missing.
    // A node still being built, before AddNode, can use the node form too.

    // Sets a property to a typed value and marks the node dirty. A value
    // that arrives untyped (MCP, scripts) goes through WriteProperty instead.
    SHADERLAB_API void SetNodeProperty(EffectNode& node, const std::wstring& key, PropertyValue value);
    SHADERLAB_API bool SetNodeProperty(GraphModel& graph, uint32_t nodeId, const std::wstring& key, PropertyValue value);

    // Points a Source node at another file and marks it dirty. The host
    // prepares the source to open it.
    SHADERLAB_API void SetNodeMedia(EffectNode& node, std::wstring path);
    SHADERLAB_API bool SetNodeMedia(GraphModel& graph, uint32_t nodeId, std::wstring path);

    // Renames a node. Names are labels, so nothing re-renders.
    SHADERLAB_API void RenameNode(EffectNode& node, std::wstring name);
    SHADERLAB_API bool RenameNode(GraphModel& graph, uint32_t nodeId, std::wstring name);

    // Places a node on the canvas, or moves several by `delta`. Layout only.
    SHADERLAB_API void SetNodePosition(EffectNode& node, winrt::Windows::Foundation::Numerics::float2 position);
    SHADERLAB_API void MoveNodes(GraphModel& graph, std::span<const uint32_t> nodeIds,
        winrt::Windows::Foundation::Numerics::float2 delta);

    // Marks a node dirty because something it reads from outside the graph
    // changed: a re-registered capture, a reloaded file, a new frame.
    SHADERLAB_API void MarkNodeChanged(EffectNode& node);
    SHADERLAB_API bool MarkNodeChanged(GraphModel& graph, uint32_t nodeId);

    // Clears a node's error and marks it dirty, so the next frame tries it
    // again (a source's Reload).
    SHADERLAB_API bool RetryNode(GraphModel& graph, uint32_t nodeId);

    // Drops a node's output and marks it dirty, for when what produced it is
    // gone (a re-registered capture item).
    SHADERLAB_API bool ResetNodeOutput(GraphModel& graph, uint32_t nodeId);

    // Replaces a custom node's effect definition, dropping its output and
    // marking it dirty. The host then hands the node to its evaluator
    // (GraphEvaluator::UpdateNodeShader).
    SHADERLAB_API bool ReplaceCustomEffect(GraphModel& graph, uint32_t nodeId, CustomEffectDefinition definition);

    // Points a bound property at another component (x, y, z, w) of its first
    // source field. False when the property is not bound.
    SHADERLAB_API bool SetBindingComponent(GraphModel& graph, uint32_t nodeId, const std::wstring& key,
        uint32_t component);

    // Records a node's GPU time for the perf display. Not an edit: nothing
    // re-renders.
    SHADERLAB_API void RecordNodeGpuTime(EffectNode& node, double milliseconds, GpuNodeState state);

    // A new node that runs the same custom effect as `node`: its name, pins,
    // properties and definition, with a fresh shader GUID so D2D treats it as
    // a separate effect. Unlike CloneNodeForPaste it keeps no bindings.
    SHADERLAB_API EffectNode NewInstanceOf(const EffectNode& node);

    // Applies `edit` to an existing property and marks the node dirty. False,
    // leaving the node untouched, when the node or property is missing or the
    // edit returns false.
    SHADERLAB_API bool EditNodeProperty(GraphModel& graph, uint32_t nodeId, const std::wstring& key,
        const std::function<bool(PropertyValue&)>& edit);

    // A copy of `node` ready to be added as a new node: dirty, with no cached
    // output or error, and a fresh shader GUID so D2D does not confuse the
    // copy's effect with the original's.
    SHADERLAB_API EffectNode CloneNodeForPaste(const EffectNode& node);

    // Connects every edge whose two ends are both in `idMap` (old id -> new
    // id) between the new nodes. Returns the number connected.
    SHADERLAB_API uint32_t ConnectClonedEdges(GraphModel& graph, std::span<const EffectEdge> edges,
        const std::unordered_map<uint32_t, uint32_t>& idMap);

    // Adds a copy of each node in `nodeIds` (CloneNodeForPaste) with the
    // edges between them. A binding between two copied nodes reads the copy;
    // one to a node outside the set still reads the original. Returns
    // (old id, new id) in the order given, a repeated id copied once; empty,
    // with nothing added, when any id is missing.
    SHADERLAB_API std::vector<std::pair<uint32_t, uint32_t>> DuplicateNodes(GraphModel& graph,
        std::span<const uint32_t> nodeIds);

    // Numeric Expression inputs: the float parameters A, B, ... the formula reads.
    SHADERLAB_API uint32_t ExpressionInputCount(const EffectNode& node);

    // True when `node` is a Numeric Expression with more than one input, so
    // one can go.
    SHADERLAB_API bool CanRemoveExpressionInput(const EffectNode& node);

    // Adds the first unused single-letter input (A to Z) with value 0 and
    // returns its name; empty when the node is not a Numeric Expression or
    // all 26 are taken.
    SHADERLAB_API std::wstring AddExpressionInput(GraphModel& graph, uint32_t nodeId);

    // Removes an input with its property and any binding to it. Refuses the
    // Expression property itself and the last remaining input.
    SHADERLAB_API bool RemoveExpressionInput(GraphModel& graph, uint32_t nodeId, const std::wstring& name);
}
