#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>

// Forward-declare to avoid pulling winsock headers into every TU.
typedef unsigned long long SOCKET_T;

namespace ShaderLab
{
    // Lightweight HTTP server using raw Winsock2 TCP sockets.
    // Runs on a background thread; handlers must dispatch UI-thread work
    // via DispatcherQueue themselves.
    class McpHttpServer
    {
    public:
        struct Response
        {
            uint16_t    statusCode{ 200 };
            std::string body;
            std::string contentType{ "application/json" };
        };

        using Handler = std::function<Response(const std::wstring& path, const std::string& body)>;

        McpHttpServer() = default;
        ~McpHttpServer();

        void AddRoute(const std::wstring& method, const std::wstring& pathPrefix, Handler handler);
        bool Start(uint16_t port = 47808);
        void Stop();
        bool IsRunning() const { return m_running.load(); }

        // Route a request programmatically (used by the MCP JSON-RPC handler).
        Response RouteRequest(const std::wstring& method, const std::wstring& path, const std::string& body);

    private:
        void ListenerThread(uint16_t port);
        void HandleConnection(SOCKET_T clientSock);

        struct Route
        {
            std::wstring method;
            std::wstring pathPrefix;
            Handler      handler;
        };

        std::vector<Route>  m_routes;
        SOCKET_T            m_listenSock{ ~0ULL };
        std::jthread        m_thread;
        std::atomic<bool>   m_running{ false };
    };
}
