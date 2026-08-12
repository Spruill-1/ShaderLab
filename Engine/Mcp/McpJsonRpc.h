#pragma once

// Engine-side MCP JSON-RPC dispatcher (stdio-migration Step 3).
//
// Moved out of MainWindow.McpRoutes.cpp so BOTH hosts serve the MCP
// protocol surface (initialize / tools/list / tools/call / resources /
// ping) over whatever transport fronts the router — the legacy HTTP
// listener today, the stdio session client from Step 6 on. The GUI's
// former inline dispatcher is deleted; RegisterJsonRpcEndpoint on the
// host's router is the whole integration.
//
// stdio-conformance rules enforced here (all were violated by the old
// GUI dispatcher; invisible over HTTP, fatal over a line-framed stream):
//   * Every response is ONE JSON message with no embedded newlines.
//   * Notifications (requests with an ABSENT id — that is the detection,
//     not a "notifications/" name prefix) produce Response::None(): the
//     HTTP transport sends 202-empty, the stdio transport emits zero
//     bytes.
//   * `id: null` never appears on an error path when the request carried
//     an id — a null id matches no pending request on a multiplexed
//     stream and hangs the client until its own timeout. The one
//     remaining null is the unparseable-JSON case, where no id is
//     recoverable (the Step 5 shim owns that failure class).
//   * protocolVersion is pinned to 2025-06-18, the revision that REMOVED
//     JSON-RPC batching — this server never supported batching, and the
//     previously-pinned 2024-11-05 required it. Batch (array) requests
//     get an explicit -32600.

#include "pch_engine.h"
#include "../../EngineExport.h"
#include "McpTypes.h"

#include <string>
#include <vector>

namespace ShaderLab
{
    class McpRouter;
}

namespace ShaderLab::Mcp
{
    struct ResourceDef
    {
        std::string  uri;          // "shaderlab://graph"
        std::string  name;         // display name
        std::string  description;
        std::wstring routePath;    // GET route serving the content
    };

    struct JsonRpcOptions
    {
        // Surfaced in GET / health and serverInfo so callers (and the test
        // suite) can tell which host answered: "gui" or "headless".
        std::string hostKind{ "gui" };

        // Resource URI -> route table. Empty selects DefaultResources().
        // Note shaderlab://context maps to the GUI-only /context route and
        // 404s on a headless host — accepted; the resource list is shared.
        std::vector<ResourceDef> resources;
    };

    SHADERLAB_API std::vector<ResourceDef> DefaultResources();

    // Registers GET / (exact-match health probe) and POST / (JSON-RPC
    // dispatcher) on the router. Call after RegisterEngineRoutes; the
    // dispatcher forwards tools/call to whatever routes the host has
    // registered, so tools whose backing route is absent on this host
    // return an isError text result rather than protocol failures.
    SHADERLAB_API void RegisterJsonRpcEndpoint(McpRouter& router, JsonRpcOptions options);
}
