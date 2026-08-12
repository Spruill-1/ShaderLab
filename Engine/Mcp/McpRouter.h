#pragma once

#include "../../EngineExport.h"
#include "McpTypes.h"
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <cstdint>

namespace ShaderLab
{
    // MCP route registry. The embedded Winsock HTTP listener was removed in
    // stdio-migration Step 9 (point of no return) — the ONLY transport now is
    // the broker (shim → hub → session over named pipes; McpSessionClient
    // routes each sealed request through RouteRequest). What remains is the
    // pure routing surface every transport is written against: AddRoute,
    // RouteRequest (longest-prefix, owns the query split), HasRoute.
    class SHADERLAB_API McpRouter
    {
    public:
        // Handler receives the query-stripped path, the raw query string
        // (text after the first '?', empty if none), and the body. The
        // router owns the split, so matching happens on the bare path and
        // the query reaches the handler regardless of caller.
        using Handler = std::function<Mcp::Response(
            const std::wstring& path,
            const std::wstring& query,
            const std::string& body)>;

        // Fired once per top-level dispatcher request (POST /) so a host can
        // pulse a UI activity indicator. `clientId` identifies the caller —
        // a session/channel label now that HTTP peer addresses are gone
        // (formerly `peerAddress`). Must be cheap + thread-safe.
        using ActivityCallback = std::function<void(
            const std::string& method,
            const std::wstring& path,
            uint16_t statusCode,
            const std::string& clientId)>;

        McpRouter() = default;
        ~McpRouter() = default;

        void AddRoute(const std::wstring& method, const std::wstring& pathPrefix, Handler handler);

        // Register a callback invoked after each top-level POST / request.
        // Set to nullptr to clear. Safe to call any time.
        void SetActivityCallback(ActivityCallback cb);

        // Route a request. `path` may carry a query string; the router
        // splits it before matching. Fires the activity callback when this
        // is the top-level dispatcher call (POST /).
        Mcp::Response RouteRequest(const std::wstring& method, const std::wstring& path,
                                   const std::string& body);

        // True if any registered route would match (longest-prefix, query
        // stripped). Catch-alls ("POST /") match every path of their method.
        bool HasRoute(const std::wstring& method, const std::wstring& path) const;

        // Like HasRoute, but a bare "/" catch-all does not count — the
        // dispatcher asks this before forwarding a tools/call so an absent
        // backing route fails legibly instead of falling into POST /.
        bool HasSpecificRoute(const std::wstring& method, const std::wstring& path) const;

    private:
        struct Route
        {
            std::wstring method;
            std::wstring pathPrefix;
            Handler      handler;
        };

        const Route* FindRoute(const std::wstring& method, const std::wstring& matchPath) const;

        std::vector<Route>  m_routes;
        std::mutex          m_activityMutex;
        ActivityCallback    m_activityCallback;
    };
}
