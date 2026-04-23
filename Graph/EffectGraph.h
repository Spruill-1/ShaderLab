#pragma once

#include "pch.h"
#include "EffectNode.h"
#include "EffectEdge.h"

namespace ShaderLab::Graph
{
    // DAG-based effect graph.
    // Owns nodes and edges; provides topological ordering and JSON round-trip.
    class EffectGraph
    {
    public:
        EffectGraph() = default;

        // --- Node management ---

        // Adds a node and returns its assigned ID.
        uint32_t AddNode(EffectNode node);

        // Removes a node and all edges connected to it.
        void RemoveNode(uint32_t nodeId);

        // Returns a pointer to the node, or nullptr if not found.
        EffectNode* FindNode(uint32_t nodeId);
        const EffectNode* FindNode(uint32_t nodeId) const;

        // --- Edge management ---

        // Connects an output pin of the source node to an input pin of the dest node.
        // Returns false if the connection would create a cycle.
        bool Connect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin);

        // Removes a specific edge. Returns true if found and removed.
        bool Disconnect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin);

        // Removes all edges connected to a given input pin (ensures single input).
        void DisconnectInput(uint32_t dstId, uint32_t dstPin);

        // --- Queries ---

        // Returns edges that feed into a given node.
        std::vector<const EffectEdge*> GetInputEdges(uint32_t nodeId) const;

        // Returns edges that originate from a given node.
        std::vector<const EffectEdge*> GetOutputEdges(uint32_t nodeId) const;

        // Returns node IDs in topological order (sources first, output last).
        // Throws std::logic_error if the graph contains a cycle.
        std::vector<uint32_t> TopologicalSort() const;

        // Returns true if adding an edge from srcId to dstId would create a cycle.
        bool WouldCreateCycle(uint32_t srcId, uint32_t dstId) const;

        // Marks all nodes as dirty (forces full re-evaluation).
        void MarkAllDirty();

        // Returns true if any node has its dirty flag set.
        bool HasDirtyNodes() const;

        // Clears all cached outputs.
        void ClearCachedOutputs();

        // --- Property bindings ---

        // Bind a downstream node's property to an upstream analysis output field.
        // Validates type compatibility, rejects cycles, replaces existing binding.
        // Returns empty string on success, or an error message on failure.
        std::wstring BindProperty(
            uint32_t destNodeId,
            const std::wstring& propertyName,
            uint32_t sourceNodeId,
            const std::wstring& sourceFieldName,
            uint32_t sourceComponent = 0);

        // Remove a property binding. Returns true if a binding was removed.
        bool UnbindProperty(uint32_t nodeId, const std::wstring& propertyName);

        // Returns true if the property type is bindable (float, float2, float3, float4).
        static bool IsBindablePropertyType(const PropertyValue& value);

        // --- Accessors ---

        const std::vector<EffectNode>& Nodes() const { return m_nodes; }
        const std::vector<EffectEdge>& Edges() const { return m_edges; }
        bool IsEmpty() const { return m_nodes.empty(); }

        // --- Serialization (Windows.Data.Json) ---

        winrt::hstring ToJson() const;
        static EffectGraph FromJson(winrt::hstring const& json);

        // --- Bulk operations ---

        void Clear();

    private:
        std::vector<EffectNode> m_nodes;
        std::vector<EffectEdge> m_edges;
        uint32_t m_nextId{ 1 };
    };
}
