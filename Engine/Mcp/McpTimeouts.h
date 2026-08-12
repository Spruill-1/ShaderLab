#pragma once

// The one place the MCP timeout ladder lives (stdio-migration Step 7).
//
// Each hop must give the hop below it time to fail on its OWN terms and
// report a real error, rather than the outer hop timing out first and
// leaving the inner one orphaned:
//
//     render-thread closure  <  MainWindow::DispatchSync  <  shim  <  client
//
//   * kRenderClosure  — a single engine route body on the render worker.
//   * kDispatchSync   — MainWindow::DispatchSync waiting on the worker.
//                       Strictly greater, so a wedged closure surfaces as
//                       the closure's failure, not a bare dispatch timeout.
//   * kShimRequest    — the shim's wait for a session's sealed response
//                       (McpSessionClient forwards a request and waits).
//                       Greater than kDispatchSync so the session can turn
//                       a slow route into a real JSON-RPC error first.
//   * kMcpClient      — informational: the MCP client's own request budget.
//                       We never enforce it; it must exceed kShimRequest or
//                       the client gives up before the shim can answer. The
//                       ecosystem default is ~60 s; kept here so the ladder
//                       is legible in one place.
//
// These are the render/DispatchSync/shim rungs the GUI enforces; the shim
// (a separate binary) reads the same numbers via the shared source of
// truth this header is. Adjust here, nowhere else.

#include <chrono>

namespace ShaderLab::Mcp
{
    inline constexpr std::chrono::milliseconds kRenderClosureTimeout{ 20'000 };
    inline constexpr std::chrono::milliseconds kDispatchSyncTimeout{ 25'000 };
    inline constexpr std::chrono::milliseconds kShimRequestTimeout{ 30'000 };
    inline constexpr std::chrono::milliseconds kMcpClientBudget{ 60'000 };

    static_assert(kRenderClosureTimeout < kDispatchSyncTimeout,
        "render closure must fail before the DispatchSync waiting on it");
    static_assert(kDispatchSyncTimeout < kShimRequestTimeout,
        "DispatchSync must fail before the shim waiting on the session");
    static_assert(kShimRequestTimeout < kMcpClientBudget,
        "the shim must answer before the MCP client's own budget expires");
}
