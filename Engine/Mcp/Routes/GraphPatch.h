#pragma once

// /graph/apply: a batch of nodes, edges and bindings built in one call.
// Parsing checks every argument before the graph is touched; applying either
// completes or restores the graph it started from.

#include "RouteSupport.h"
#include "../../../Effects/NodeCatalog.h"
#include "../../../Graph/PropertyWrite.h"

#include <map>
#include <vector>

namespace ShaderLab::Mcp::Routes
{
    // A node named in a patch: a JSON number is a node id; a string is a ref
    // given to a node earlier in the patch, or failing that a node id written
    // as digits.
    struct PatchNodeRef
    {
        std::optional<uint32_t> id;
        std::wstring            ref;
    };

    struct PatchNode
    {
        std::wstring           effect;
        Effects::NodeCreateArgs args;
        std::vector<std::pair<std::wstring, Graph::UntypedValue>> properties;
        std::wstring           ref;   // empty: the client does not name it
    };

    struct PatchEdge
    {
        PatchNodeRef from;
        PatchNodeRef to;
        uint32_t     fromPin{ 0 };
        uint32_t     toPin{ 0 };
    };

    struct PatchBinding
    {
        PatchNodeRef node;
        std::wstring property;
        PatchNodeRef from;
        std::wstring field;
        uint32_t     component{ 0 };
    };

    struct GraphPatch
    {
        std::vector<PatchNode>    nodes;
        std::vector<PatchEdge>    edges;
        std::vector<PatchBinding> bindings;
    };

    // The patch in a /graph/apply body, or nullopt and the reason.
    std::optional<GraphPatch> ParseGraphPatch(const WDJ::JsonObject& body, std::string& error);

    struct PatchResult
    {
        std::vector<uint32_t>           nodeIds;   // in patch order
        std::map<std::wstring, uint32_t> refToId;
        std::string                     error;     // set: nothing was applied
    };

    // Creates the nodes, then connects the edges, then binds the properties.
    // On the first failure everything already applied is undone: the new
    // nodes are removed and the edges and bindings of existing nodes are
    // put back. Runs inside Dispatch; fires no sink events.
    PatchResult ApplyGraphPatch(EngineContext& ctx, const GraphPatch& patch);
}
