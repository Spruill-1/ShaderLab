#pragma once

// The MCP JSON-RPC dispatcher (initialize, tools/list, tools/call,
// resources, ping) on a host's router; RegisterJsonRpcEndpoint is the whole
// integration, and both hosts use it.
//
// Rules for a line-framed stream:
//   * Every response is one JSON message with no embedded newline.
//   * A request with no id is a notification and gets Response::None():
//     zero bytes. The absent id is the test, not a "notifications/" prefix.
//   * An error answers with the request's id; only unparseable JSON, where
//     no id can be read, answers with null.
//   * protocolVersion is 2025-06-18, which removed JSON-RPC batching; a
//     batch (array) request gets -32600.

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
