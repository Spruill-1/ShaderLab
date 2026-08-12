#include "pch_engine.h"
#include "McpJsonRpc.h"
#include "McpRouter.h"
#include "McpToolCatalog.h"
#include "../../Version.h"

#include <winrt/Windows.Data.Json.h>
#include <format>

namespace ShaderLab::Mcp
{
    namespace WDJ = winrt::Windows::Data::Json;

    namespace
    {
        // The one protocol revision this server implements. 2025-06-18
        // removed JSON-RPC batching from MCP — this server never accepted
        // batches, so it is the first revision the implementation is
        // actually conformant with (the old dispatcher pinned 2024-11-05,
        // which required batching). Always answered regardless of the
        // client's requested version; a client that cannot speak it is
        // expected to disconnect per spec.
        constexpr const char* kProtocolVersion = "2025-06-18";

        std::string WrapResult(const std::string& idStr, const std::string& result)
        {
            return std::format(
                R"JSON({{"jsonrpc":"2.0","id":{},"result":{}}})JSON", idStr, result);
        }

        std::string WrapError(const std::string& idStr, int code, std::string_view message)
        {
            return std::format(
                R"JSON({{"jsonrpc":"2.0","id":{},"error":{{"code":{},"message":"{}"}}}})JSON",
                idStr, code, JsonEscape(message));
        }

        // Tool result envelope: MCP requires content[].text to be a STRING,
        // so the route body is escaped through the shared JsonEscape (the
        // old dispatcher's ad-hoc loop passed raw control characters from
        // HLSL error text straight through, producing invalid JSON).
        std::string TextToolResult(const std::string& body, bool isError)
        {
            return std::format(
                R"JSON({{"content":[{{"type":"text","text":"{}"}}],"isError":{}}})JSON",
                JsonEscape(body), isError ? "true" : "false");
        }

        // Error surfaced as a TOOL RESULT (isError:true), not a protocol
        // error — per MCP, tool-level failures are results so the model
        // can see them.
        Response ToolErrorResult(const std::string& idStr, std::string_view message)
        {
            return { 200, WrapResult(idStr, TextToolResult(std::string(message), true)) };
        }

        // responseMode is a function of the RESPONSE, not the tool: repack
        // as MCP image content only if the tool is flagged, the caller
        // asked (inline == true), the route succeeded, the body re-parses,
        // and it carries base64 + mimeType. Anything else falls through to
        // the generic text wrapper.
        bool TryImageRepack(const ToolDef& tool, const WDJ::JsonObject& args,
                            const Response& restResp, const std::string& idStr,
                            Response& out)
        {
            if (!tool.imageInline) return false;
            if (!args.HasKey(L"inline")) return false;
            auto v = args.GetNamedValue(L"inline");
            if (v.ValueType() != WDJ::JsonValueType::Boolean || !v.GetBoolean()) return false;
            if (restResp.statusCode != 200) return false;

            WDJ::JsonObject ro{ nullptr };
            if (!WDJ::JsonObject::TryParse(winrt::to_hstring(restResp.body), ro)) return false;
            if (!ro.HasKey(L"base64") || !ro.HasKey(L"mimeType")) return false;

            auto b64 = WideToUtf8(std::wstring(ro.GetNamedString(L"base64")));
            auto mime = WideToUtf8(std::wstring(ro.GetNamedString(L"mimeType")));
            std::string content = std::format(
                R"JSON({{"content":[{{"type":"image","data":"{}","mimeType":"{}"}}],"isError":false}})JSON",
                b64, mime);
            out = { 200, WrapResult(idStr, content) };
            return true;
        }

        // tools/call → route request, per the catalog row.
        Response DispatchToolCall(McpRouter& router, const std::string& idStr,
                                  const WDJ::JsonObject& params)
        {
            if (!params.HasKey(L"name") ||
                params.GetNamedValue(L"name").ValueType() != WDJ::JsonValueType::String)
                return { 200, WrapError(idStr, -32602, "Invalid params: missing tool name") };

            auto toolName = WideToUtf8(std::wstring(params.GetNamedString(L"name")));
            WDJ::JsonObject args;
            if (params.HasKey(L"arguments"))
            {
                auto av = params.GetNamedValue(L"arguments");
                if (av.ValueType() != WDJ::JsonValueType::Object)
                    return { 200, WrapError(idStr, -32602, "Invalid params: arguments must be an object") };
                args = av.GetObject();
            }

            const ToolDef* tool = FindTool(toolName);
            if (!tool)
                return ToolErrorResult(idStr, "Unknown tool: " + toolName);

            std::wstring path = tool->pathTemplate;
            std::string  body;
            switch (tool->argMode)
            {
            case ToolArgMode::BodyPassthrough:
                body = WideToUtf8(std::wstring(args.Stringify()));
                break;
            case ToolArgMode::NoBody:
                break;
            case ToolArgMode::PathNumber:
            {
                if (!args.HasKey(tool->argKey))
                    return ToolErrorResult(idStr,
                        "Missing required argument: " + WideToUtf8(tool->argKey));
                auto num = static_cast<uint32_t>(args.GetNamedNumber(tool->argKey));
                path = std::vformat(tool->pathTemplate, std::make_wformat_args(num));
                break;
            }
            case ToolArgMode::PathString:
            {
                if (!args.HasKey(tool->argKey))
                    return ToolErrorResult(idStr,
                        "Missing required argument: " + WideToUtf8(tool->argKey));
                path = std::wstring(tool->pathTemplate) +
                       std::wstring(args.GetNamedString(tool->argKey));
                break;
            }
            case ToolArgMode::NodeLogs:
            {
                if (!args.HasKey(tool->argKey))
                    return ToolErrorResult(idStr,
                        "Missing required argument: " + WideToUtf8(tool->argKey));
                auto nodeId = static_cast<uint32_t>(args.GetNamedNumber(tool->argKey));
                uint64_t sinceSeq = 0;
                if (args.HasKey(L"sinceSeq"))
                    sinceSeq = static_cast<uint64_t>(args.GetNamedNumber(L"sinceSeq"));
                path = std::vformat(tool->pathTemplate, std::make_wformat_args(nodeId, sinceSeq));
                break;
            }
            case ToolArgMode::UnwrapField:
            {
                if (!args.HasKey(tool->argKey))
                    return ToolErrorResult(idStr,
                        "Missing required argument: " + WideToUtf8(tool->argKey));
                body = WideToUtf8(std::wstring(args.GetNamedString(tool->argKey)));
                break;
            }
            }

            // A tool whose backing route is absent on this host must fail
            // legibly. Without this check the request falls through
            // longest-prefix matching into this dispatcher's own POST /
            // catch-all and reads as a bogus notification — the retired
            // image_stats silent-success bug, resurrected.
            if (!router.HasSpecificRoute(tool->method, path))
                return ToolErrorResult(idStr,
                    "Tool not available on this host: " + toolName);

            Response restResp = router.RouteRequest(tool->method, path, body);

            Response repacked;
            if (TryImageRepack(*tool, args, restResp, idStr, repacked))
                return repacked;

            bool isError = restResp.statusCode >= 400;
            return { 200, WrapResult(idStr, TextToolResult(restResp.body, isError)) };
        }
    }

    std::vector<ResourceDef> DefaultResources()
    {
        return {
            { "shaderlab://context", "ShaderLab Context",
              "System prompt: pipeline format, shader conventions, API reference", L"/context" },
            { "shaderlab://graph", "Effect Graph",
              "Full graph state with nodes, edges, properties, custom effect definitions", L"/graph" },
            { "shaderlab://registry/effects", "Built-in Effects",
              "All 48+ built-in D2D effects with property metadata", L"/registry/effects" },
            { "shaderlab://custom-effects", "Custom Effects",
              "Custom effects in graph with HLSL source and compile status", L"/custom-effects" },
        };
    }

    void RegisterJsonRpcEndpoint(McpRouter& router, JsonRpcOptions options)
    {
        if (options.resources.empty())
            options.resources = DefaultResources();

        // The GET / health route was removed with the HTTP transport
        // (stdio-migration Step 9) — nothing probes it now; the shim owns the
        // client-facing handshake and callers reach the dispatcher via the
        // sealed session channel. host kind still rides in initialize's
        // serverInfo below.

        // POST / — the JSON-RPC dispatcher.
        router.AddRoute(L"POST", L"/",
            [&router, options = std::move(options)]
            (const std::wstring&, const std::wstring&, const std::string& body) -> Response
        {
            // Extracted as early as possible so every later error path can
            // echo it. "null" survives only for unparseable JSON.
            std::string idStr = "null";
            try
            {
                WDJ::JsonObject jobj{ nullptr };
                if (!WDJ::JsonObject::TryParse(winrt::to_hstring(body), jobj))
                {
                    // Distinguish a batch (JSON array) from garbage for a
                    // clearer message; both are -32600/-32700 class.
                    WDJ::JsonArray arr{ nullptr };
                    if (WDJ::JsonArray::TryParse(winrt::to_hstring(body), arr))
                        return { 200, WrapError("null", -32600,
                            "Batch requests are not supported (protocol 2025-06-18)") };
                    return { 200, WrapError("null", -32700, "Parse error") };
                }

                bool hasId = jobj.HasKey(L"id");
                if (hasId)
                {
                    auto id = jobj.GetNamedValue(L"id");
                    if (id.ValueType() == WDJ::JsonValueType::Number)
                        idStr = std::format("{}", static_cast<int64_t>(id.GetNumber()));
                    else if (id.ValueType() == WDJ::JsonValueType::String)
                        idStr = "\"" + JsonEscape(WideToUtf8(std::wstring(id.GetString()))) + "\"";
                    else
                        hasId = false;   // null / other id types: treat as notification
                }

                if (!jobj.HasKey(L"method") ||
                    jobj.GetNamedValue(L"method").ValueType() != WDJ::JsonValueType::String)
                {
                    if (!hasId) return Response::None();
                    return { 200, WrapError(idStr, -32600, "Invalid Request: missing method") };
                }
                auto method = WideToUtf8(std::wstring(jobj.GetNamedString(L"method")));

                // Notification: absent id, no reply bytes. That is the
                // whole detection — NOT a "notifications/" name prefix.
                if (!hasId)
                    return Response::None();

                // Guarded params accessor (the old dispatcher's unchecked
                // GetNamedObject(L"params") threw winrt::hresult_error on
                // array/absent params and surfaced as a generic 500).
                auto getParams = [&](WDJ::JsonObject& out) -> bool {
                    if (!jobj.HasKey(L"params")) return false;
                    auto pv = jobj.GetNamedValue(L"params");
                    if (pv.ValueType() != WDJ::JsonValueType::Object) return false;
                    out = pv.GetObject();
                    return true;
                };

                if (method == "initialize")
                {
                    auto verStr = WideToUtf8(std::wstring(::ShaderLab::VersionString));
                    std::string result = std::format(
                        R"JSON({{"protocolVersion":"{}","capabilities":{{"tools":{{}},"resources":{{}}}},"serverInfo":{{"name":"shaderlab-{}","version":"{}"}}}})JSON",
                        kProtocolVersion, options.hostKind, verStr);
                    return { 200, WrapResult(idStr, result) };
                }

                if (method == "tools/list")
                {
                    std::string tools = R"({"tools":[)";
                    bool first = true;
                    for (const auto& t : ToolCatalog())
                    {
                        if (!first) tools += ",";
                        tools += t.listJson;
                        first = false;
                    }
                    tools += "]}";
                    return { 200, WrapResult(idStr, tools) };
                }

                if (method == "tools/call")
                {
                    WDJ::JsonObject params;
                    if (!getParams(params))
                        return { 200, WrapError(idStr, -32602, "Invalid params") };
                    return DispatchToolCall(router, idStr, params);
                }

                if (method == "resources/list")
                {
                    std::string res = R"({"resources":[)";
                    bool first = true;
                    for (const auto& r : options.resources)
                    {
                        if (!first) res += ",";
                        res += std::format(
                            R"JSON({{"uri":"{}","name":"{}","description":"{}","mimeType":"application/json"}})JSON",
                            JsonEscape(r.uri), JsonEscape(r.name), JsonEscape(r.description));
                        first = false;
                    }
                    res += "]}";
                    return { 200, WrapResult(idStr, res) };
                }

                if (method == "resources/read")
                {
                    WDJ::JsonObject params;
                    if (!getParams(params) ||
                        !params.HasKey(L"uri") ||
                        params.GetNamedValue(L"uri").ValueType() != WDJ::JsonValueType::String)
                        return { 200, WrapError(idStr, -32602, "Invalid params: missing uri") };
                    auto uri = WideToUtf8(std::wstring(params.GetNamedString(L"uri")));

                    const ResourceDef* match = nullptr;
                    for (const auto& r : options.resources)
                        if (r.uri == uri) { match = &r; break; }
                    if (!match)
                        return { 200, WrapResult(idStr, R"JSON({"contents":[]})JSON") };

                    auto restResp = router.RouteRequest(L"GET", match->routePath, "");
                    std::string result = std::format(
                        R"JSON({{"contents":[{{"uri":"{}","mimeType":"application/json","text":"{}"}}]}})JSON",
                        JsonEscape(uri), JsonEscape(restResp.body));
                    return { 200, WrapResult(idStr, result) };
                }

                if (method == "ping")
                    return { 200, WrapResult(idStr, "{}") };

                return { 200, WrapError(idStr, -32601, "Method not found: " + method) };
            }
            catch (const std::exception& ex)
            {
                return { 200, WrapError(idStr, -32603,
                    std::string("Internal error: ") + ex.what()) };
            }
            catch (...)
            {
                // winrt::hresult_error and friends: answer with the id we
                // extracted rather than letting the router's barrier turn
                // this into an uncorrelatable 500.
                return { 200, WrapError(idStr, -32603, "Internal error") };
            }
        });
    }
}
