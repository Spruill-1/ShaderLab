#pragma once

// The shim: the MCP server an MCP client launches (--stdio). It owns
// initialize, the list_sessions / use_session tools, id correlation and
// request timeouts, and forwards everything else, sealed, to the session
// use_session pinned. NDJSON on stdin/stdout; stdout carries JSON-RPC
// messages only.

#include "BrokerPipeIo.h"

#include "../Engine/Mcp/McpChannel.h"

#include <optional>
#include <string>
#include <vector>

namespace ShaderLab::Broker
{
    struct ShimState
    {
        BrokerNames  names;
        std::wstring hubAumid;                // packaged hub AUMID for on-demand activation
        std::optional<HubConnection> hub;     // connected on first use; nullopt: no hub

        // The pinned session and its secure channel. One channel and serial
        // requests: the shim owns id correlation.
        std::wstring pinnedSession;
        uint32_t     channelId{ 0 };
        std::optional<Mcp::SecureChannel> channel;
        uint64_t     chanSendSeq{ 1 };
        uint64_t     chanRecvSeq{ 0 };   // highest response seq opened; older ones are replays

        // Connects to the hub, activating the packaged one when none answers
        // and an AUMID is known.
        bool EnsureHub();
        void DropChannel();
    };

    // Answers one JSON-RPC line: the lines to write to stdout, in order.
    // Empty for a notification. Never throws: an internal failure is logged
    // with its cause and answered with -32603.
    std::vector<std::string> HandleShimRequest(ShimState& state, const std::string& line);

    // Reads stdin until the client closes it.
    int RunStdio(const BrokerNames& names, const std::wstring& hubAumid);
}
