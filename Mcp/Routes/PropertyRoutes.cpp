#include "pch_engine.h"
#include "RouteSupport.h"
#include "../McpRouter.h"

#include "Graph/GraphModel.h"
#include "Graph/PropertyWrite.h"

// Property values and property bindings.
namespace ShaderLab::Mcp::Routes
{
    namespace
    {
        // POST /graph/set-property { nodeId, key, value }. Graph::WriteProperty
        // types the value, coercing the strings untyped MCP arguments arrive
        // as ("203", "true"). An unknown key is accepted: un-seeded Direct2D
        // properties, Clock's isPlaying, shaderPath (the media path) and
        // numeric index keys are legitimately absent until first written.
        void RegisterSetProperty(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/set-property",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    const auto key = RequireStringArg(args, L"key", argError);
                    if (!argError.empty())
                        return Error(400, argError);
                    if (!HasArg(args, L"value"))
                        return Error(400, "value is required");
                    auto value = ToUntypedValue(args.GetNamedValue(L"value"), argError);
                    if (!value)
                        return Error(400, argError);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        auto* node = ctx.Model().FindNode(*nodeId);
                        if (!node)
                            return Error(404, "Node not found");
                        if (auto error = Graph::WriteProperty(*node, *key, *value); !error.empty())
                            return Error(400, WideToUtf8(error));
                        sink.OnNodeChanged(*nodeId);
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // POST /graph/bind-property { nodeId, propertyName, sourceNodeId,
        // sourceFieldName, sourceComponent? }: drive a property from an
        // analysis output field.
        void RegisterBindProperty(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/bind-property",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    const auto propertyName = RequireStringArg(args, L"propertyName", argError);
                    const auto sourceNodeId = RequireUintArg(args, L"sourceNodeId", argError);
                    const auto sourceField = RequireStringArg(args, L"sourceFieldName", argError);
                    const uint32_t component = ReadUintArg(args, L"sourceComponent", argError).value_or(0);
                    if (!argError.empty())
                        return Error(400, argError);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        const auto error = ctx.Model().BindProperty(*nodeId, *propertyName, *sourceNodeId, *sourceField, component);
                        if (!error.empty())
                            return Error(400, WideToUtf8(error));
                        sink.OnGraphStructureChanged();
                        return Json(200, R"({"ok":true})");
                    });
                });
        }

        // POST /graph/unbind-property { nodeId, propertyName }.
        void RegisterUnbindProperty(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/graph/unbind-property",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    const auto propertyName = RequireStringArg(args, L"propertyName", argError);
                    if (!argError.empty())
                        return Error(400, argError);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        if (!ctx.Model().UnbindProperty(*nodeId, *propertyName))
                            return Error(404, "No binding for that property");
                        sink.OnGraphStructureChanged();
                        return Json(200, R"({"ok":true})");
                    });
                });
        }
    }

    void RegisterPropertyRoutes(McpRouter& router, IEngineCommandSink& sink)
    {
        RegisterSetProperty(router, sink);
        RegisterBindProperty(router, sink);
        RegisterUnbindProperty(router, sink);
    }
}
