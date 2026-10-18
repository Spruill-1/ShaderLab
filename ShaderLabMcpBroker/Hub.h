#pragma once

// The hub: the per-user singleton blind relay. It wins or loses the
// first-instance election on the pipe, verifies every connecting peer
// (McpPeerIdentity pairing), answers channel-0 control ops (hello,
// list-sessions, open-channel, bye), and relays sealed frames between shims
// and sessions by channel id without ever holding a key.

#include "BrokerPipeIo.h"

#include <cstdint>

namespace ShaderLab::Broker
{
    // RunHub's exit codes.
    constexpr int cHubExitOk = 0;              // idle exit, or lost the election to a healthy hub
    constexpr int cHubExitFailed = 2;          // a Win32 call the hub needs failed (logged)
    constexpr int cHubExitStaleIncumbent = 3;  // the pipe exists but no hub answered on it

    // Serves until no peer has been connected for idleExitSec (0: never).
    int RunHub(const BrokerNames& names, uint32_t idleExitSec);
}
