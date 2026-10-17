#include "pch_engine.h"
#include "McpRouter.h"
#include "McpJsonWriter.h"

#include <format>

namespace ShaderLab
{
    void McpRouter::AddRoute(const std::wstring& method, const std::wstring& pathPrefix, Handler handler)
    {
        m_routes.push_back({ method, pathPrefix, std::move(handler) });
    }

    void McpRouter::SetActivityCallback(ActivityCallback cb)
    {
        std::lock_guard lock(m_activityMutex);
        m_activityCallback = std::move(cb);
    }

    const McpRouter::Route* McpRouter::FindRoute(
        const std::wstring& method, const std::wstring& matchPath) const
    {
        const Route* bestRoute = nullptr;
        size_t bestLen = 0;
        for (const auto& route : m_routes)
        {
            if (route.method != method)
                continue;
            if (matchPath.starts_with(route.pathPrefix) && route.pathPrefix.size() > bestLen)
            {
                bestRoute = &route;
                bestLen = route.pathPrefix.size();
            }
        }
        return bestRoute;
    }

    bool McpRouter::HasRoute(const std::wstring& method, const std::wstring& path) const
    {
        auto qpos = path.find(L'?');
        return FindRoute(method,
            qpos == std::wstring::npos ? path : path.substr(0, qpos)) != nullptr;
    }

    bool McpRouter::HasSpecificRoute(const std::wstring& method, const std::wstring& path) const
    {
        auto qpos = path.find(L'?');
        const Route* best = FindRoute(method,
            qpos == std::wstring::npos ? path : path.substr(0, qpos));
        return best && best->pathPrefix != L"/";
    }

    Mcp::Response McpRouter::RouteRequest(
        const std::wstring& method, const std::wstring& path, const std::string& body)
    {
        // Split the query off: matching happens against the bare path, and
        // the query reaches the handler as its own argument.
        std::wstring matchPath = path;
        std::wstring query;
        if (auto qpos = path.find(L'?'); qpos != std::wstring::npos)
        {
            matchPath = path.substr(0, qpos);
            query = path.substr(qpos + 1);
        }

        Mcp::Response resp;
        if (const Route* bestRoute = FindRoute(method, matchPath))
        {
            try
            {
                resp = bestRoute->handler(matchPath, query, body);
            }
            catch (const std::exception& ex)
            {
                resp = Mcp::ErrorResponse(500, ex.what());
            }
            catch (const winrt::hresult_error& ex)
            {
                resp = Mcp::ErrorResponse(500, std::format("{} (0x{:08X})",
                    winrt::to_string(ex.message()), static_cast<uint32_t>(ex.code().value)));
            }
            catch (...)
            {
                // SEH translations and anything else: a handler must never
                // let an exception escape onto a transport thread.
                resp = Mcp::ErrorResponse(500, "Unhandled non-standard exception in route handler");
            }
        }
        else
        {
            resp = Mcp::ErrorResponse(404, "Not found");
        }

        // Activity ping for the host's UI indicator — once per top-level
        // dispatcher request (POST /), not per internal sub-route the
        // dispatcher itself routes. clientId is a generic label: the broker
        // pipe carries no peer address.
        if (method == L"POST" && matchPath == L"/")
        {
            ActivityCallback cb;
            {
                std::lock_guard lock(m_activityMutex);
                cb = m_activityCallback;
            }
            if (cb)
            {
                try { cb("POST", L"/", resp.statusCode, "session"); }
                catch (...)
                {
                    // Deliberate swallow: this callback only drives a host UI
                    // activity indicator. A throwing indicator must never turn
                    // an otherwise-successful MCP request into a failure, and
                    // the response is already built at this point.
                }
            }
        }
        return resp;
    }
}
