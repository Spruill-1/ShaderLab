// ShaderLabMcpBroker: the MCP transport, one binary with two modes.
//
//   --hub    The per-user singleton blind relay between shims and sessions
//            (Hub.h).
//   --stdio  The MCP server an MCP client launches; it forwards to the
//            session use_session pinned, through the hub (Shim.h).
//
// It does not link EffectGraph's engine: the hub must start in milliseconds
// and never touches a GPU. The transport modules it needs (McpFrame,
// McpCrypto, McpChannel, McpPeerIdentity, and McpTypes' JsonEscape) are
// compiled into this exe.
//
// Configuration travels as arguments (--pipe, --idle-exit-sec) because an
// activated hub never receives the launcher's environment. SHADERLAB_MCP_PIPE
// is honoured as a dev / CI fallback where a real environment exists (shim,
// tests); the readiness event name derives from the pipe name, so one
// override isolates every named object.

#include "pch_engine.h"
#include "BrokerPipeIo.h"
#include "Hub.h"
#include "Shim.h"

int wmain(int argc, wchar_t* argv[])
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    bool hubMode = false, stdioMode = false;
    std::wstring pipeArg;
    std::wstring hubAumid;
    uint32_t idleExitSec = 120;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--hub") hubMode = true;
        else if (a == L"--stdio") stdioMode = true;
        else if (a == L"--pipe" && i + 1 < argc) pipeArg = argv[++i];
        else if (a == L"--hub-aumid" && i + 1 < argc) hubAumid = argv[++i];
        else if (a == L"--idle-exit-sec" && i + 1 < argc)
            idleExitSec = static_cast<uint32_t>(_wtoi(argv[++i]));
        else
        {
            fwprintf(stderr, L"ShaderLabMcpBroker --hub|--stdio [--pipe NAME] "
                L"[--hub-aumid AUMID] [--idle-exit-sec N]\n");
            return 1;
        }
    }
    if (hubMode == stdioMode)
    {
        fwprintf(stderr, L"ShaderLabMcpBroker: exactly one of --hub / --stdio is required\n");
        return 1;
    }

    const auto names = ShaderLab::Broker::ResolveNames(pipeArg);
    return hubMode
        ? ShaderLab::Broker::RunHub(names, idleExitSec)
        : ShaderLab::Broker::RunStdio(names, hubAumid);
}
