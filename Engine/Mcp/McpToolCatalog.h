#pragma once

// Declarative MCP tool catalog (stdio-migration Step 3).
//
// One row per tool: the single-line tools/list JSON entry plus how a
// tools/call invocation maps onto the route registry. The dispatcher
// (McpJsonRpc.cpp) is generic; everything tool-specific lives here.
//
// Response shaping deliberately does NOT live in this table:
// `responseMode` is a function of the response, not the tool. Tools
// flagged `imageInline` repack to MCP image content only when
// inline == true AND status == 200 AND the body re-parses AND it has
// both `base64` and `mimeType`; anything else falls through to the
// generic text wrapper in the dispatcher.

#include "pch_engine.h"
#include "../../EngineExport.h"

#include <string_view>
#include <vector>
#include <cstdint>

namespace ShaderLab::Mcp
{
    enum class ToolArgMode : uint8_t
    {
        BodyPassthrough,  // arguments object serialized as the request body
        NoBody,           // empty body; arguments ignored
        PathNumber,       // pathTemplate has one {} slot, filled with numeric argKey
        PathString,       // argKey's raw string value appended to pathTemplate
        NodeLogs,         // /node/{nodeId}/logs?since={sinceSeq}; sinceSeq defaults 0
        UnwrapField,      // body = raw string content of argKey (graph_load_json)
    };

    struct ToolDef
    {
        const char*    name;          // "graph_add_node"
        const char*    listJson;      // single-line {"name":...,"description":...,"inputSchema":...}
        const wchar_t* method;        // L"GET" / L"POST"
        const wchar_t* pathTemplate;  // route path (format slot for Path* / NodeLogs modes)
        ToolArgMode    argMode{ ToolArgMode::BodyPassthrough };
        const wchar_t* argKey{ nullptr };  // Path* / NodeLogs / UnwrapField primary argument
        bool           imageInline{ false };
    };

    SHADERLAB_API const std::vector<ToolDef>& ToolCatalog();
    SHADERLAB_API const ToolDef* FindTool(std::string_view name);
}
