#include "pch.h"
#include "McpRouteTable.h"

#include "../Engine/Mcp/McpJsonWriter.h"
#include "../Engine/Mcp/McpTimeouts.h"

namespace ShaderLab::Controls
{
    McpRouteTable::McpRouteTable(McpRouter& router, Rendering::RenderThreadDispatcher& renderDispatcher,
                                 UiInvoker invokeOnUi, BusyCheck renderBusy)
        : m_router(router), m_renderDispatcher(renderDispatcher),
          m_invokeOnUi(std::move(invokeOnUi)), m_renderBusy(std::move(renderBusy))
    {
    }

    void McpRouteTable::AddUiRoute(const std::wstring& method, const std::wstring& path, Handler handler)
    {
        m_router.AddRoute(method, path,
            [invoke = m_invokeOnUi, handler = std::move(handler)](const std::wstring& routePath,
                const std::wstring& query, const std::string& body) -> Mcp::Response
            {
                return invoke([&]() { return handler(routePath, query, body); });
            });
    }

    void McpRouteTable::AddRenderRoute(const std::wstring& method, const std::wstring& path, Handler handler)
    {
        m_router.AddRoute(method, path,
            [dispatcher = &m_renderDispatcher, busy = m_renderBusy, handler = std::move(handler)](
                const std::wstring& routePath, const std::wstring& query, const std::string& body) -> Mcp::Response
            {
                // During an adapter switch the worker is joined and the device
                // is being rebuilt, so there is nothing to run on.
                if (busy && busy())
                    return Mcp::ErrorResponse(503, "GPU adapter switch in progress; retry shortly");
                return dispatcher->DispatchSync(
                    [&]() { return handler(routePath, query, body); }, Mcp::kRenderClosureTimeout);
            });
    }

    void McpRouteTable::AddAnyThreadRoute(const std::wstring& method, const std::wstring& path, Handler handler)
    {
        m_router.AddRoute(method, path, std::move(handler));
    }
}
