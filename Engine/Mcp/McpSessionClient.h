#pragma once

// Session client (stdio-migration Step 6).
//
// Connects a ShaderLab host to the broker hub as a "session", so an MCP
// client (via the stdio shim) can select it with use_session and drive
// its graph. Written ONCE against McpRouter& and reused by both hosts —
// headless wires it now (`--mcp-session`), the GUI in Step 7.
//
// Per shim↔session channel the client runs a SecureChannel (acceptor):
// it opens each sealed request, hands the plaintext JSON-RPC to
// `router.RouteRequest(L"POST", L"/", body)` — i.e. the same engine-side
// dispatcher the HTTP transport uses — and seals the response back. The
// hub relays frames blindly by channelId and never holds a key.
//
// Threading: Run() blocks on a dedicated thread and owns the connection.
// It reconnects with capped backoff after a drop (a hub restart, an
// update). Stop() unblocks it. Requests are served serially — one
// in-flight per session, which matches HeadlessSink's synchronous
// Dispatch and keeps the graph single-writer.

#include "pch_engine.h"
#include "../../EngineExport.h"

#include <atomic>
#include <string>

namespace ShaderLab
{
    class McpRouter;
}

namespace ShaderLab::Mcp
{
    struct SessionClientOptions
    {
        std::wstring pipeBaseName;   // empty -> SHADERLAB_MCP_PIPE / default
        std::wstring sessionId;      // persisted per-window GUID (NOT an ordinal)
        std::wstring label;          // human label surfaced by list_sessions
    };

    class SHADERLAB_API McpSessionClient
    {
    public:
        McpSessionClient(McpRouter& router, SessionClientOptions options);
        ~McpSessionClient();

        McpSessionClient(const McpSessionClient&) = delete;
        McpSessionClient& operator=(const McpSessionClient&) = delete;

        // Blocks: connect → register → serve, reconnecting with backoff
        // until Stop(). Returns when stopped.
        void Run();

        // Wakes Run() out of its current wait and closes the connection;
        // Run() then observes the stop flag and returns. Safe from any
        // thread.
        void Stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
