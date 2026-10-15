#pragma once

// Connects a ShaderLab host to the broker hub as a "session", so an MCP
// client (through the stdio shim) can select it with use_session and drive
// its graph. Both hosts use it against their McpRouter.
//
// Per shim<->session channel the client runs a SecureChannel (acceptor): it
// opens each sealed request, hands the plaintext JSON-RPC to
// router.RouteRequest(L"POST", L"/", body) and seals the response back. The
// hub relays frames by channelId and never holds a key.
//
// Threading: Run() blocks on a dedicated thread and owns the connection,
// reconnecting with capped backoff after a drop (a hub restart, an update).
// Stop() unblocks it from any thread. Requests are served one at a time,
// which keeps the graph single-writer.

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
        std::wstring pipeBaseName;   // empty -> ResolvePipeBaseName({})
        std::wstring sessionId;      // persisted per-window GUID (NOT an ordinal)
        std::wstring label;          // human label surfaced by list_sessions
    };

    enum class SessionState
    {
        Stopped,      // Run() has not started, or has returned
        Connecting,   // between attempts, or mid-handshake
        NoHub,        // nothing is listening on the pipe
        Refused,      // the hub failed identity or pairing checks
        Registered,   // paired with the hub and serving requests
    };

    struct SessionStatus
    {
        SessionState state{ SessionState::Stopped };
        std::wstring detail;   // why, for NoHub and Refused
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

        // Wakes Run() out of its current wait or blocking pipe I/O; Run()
        // then returns. Safe from any thread.
        void Stop();

        // Where the connection stands. Safe from any thread.
        SessionStatus Status() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
