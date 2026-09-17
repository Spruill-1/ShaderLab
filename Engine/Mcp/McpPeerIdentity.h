#pragma once

// Peer identity + binary-pairing policy for the MCP broker pipe
// (stdio-migration Step 4).
//
// The one enforced boundary: verify the peer PROCESS, not its claims,
// and keep it version-tolerant.
//   * Identity  = package family name only. NOT the install root — that
//     changes per version and isn't reliably under WindowsApps.
//   * Compatibility = protocol version only (carried in the pipe name),
//     bumped on wire-format breaks, never per release. App/engine
//     versions are informational — comparing them would reject the
//     old-shim/new-hub pairing on every routine upgrade and destroy the
//     benefit of making the shim update-immune.
//   * Unpackaged fallback (dev + CI only): when BOTH peers lack package
//     identity, "same image directory + matching build id" — but only
//     when SHADERLAB_MCP_ALLOW_UNPACKAGED=1, so it can never silently
//     engage in an installed configuration. Mixed packaged/unpackaged
//     is always refused.

#include "pch_engine.h"
#include "../../EngineExport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ShaderLab::Mcp
{
    enum class PeerIdentityKind : uint8_t
    {
        Packaged,     // has a package family name
        Unpackaged,   // APPMODEL_ERROR_NO_PACKAGE
    };

    struct PeerIdentity
    {
        PeerIdentityKind kind{ PeerIdentityKind::Unpackaged };
        uint32_t         pid{ 0 };
        std::wstring     packageFamilyName;   // Packaged only
        std::wstring     imageDirectory;      // directory of the exe (no trailing slash)
        // Process creation time, captured with the identity. A PID-reuse
        // sanity check for long-lived channels — a SANITY CHECK, not a
        // guarantee: nothing stops the kernel recycling a PID between
        // our query and any later use of it.
        FILETIME         creationTime{};
    };

    SHADERLAB_API std::optional<PeerIdentity> ResolveProcessIdentity(uint32_t pid);

    // Resolve the process on the other end of a named pipe. The server-
    // from-client-handle call is documented ambiguously (the docs imply
    // server-side handles only) but works — the spike measured it and
    // the unit tests keep it covered.
    SHADERLAB_API std::optional<PeerIdentity> ResolvePipeClientIdentity(HANDLE serverPipeHandle);
    SHADERLAB_API std::optional<PeerIdentity> ResolvePipeServerIdentity(HANDLE clientPipeHandle);

    // "Same build" string for the unpackaged fallback: app version +
    // engine ABI, both compile-time constants of the calling binary.
    SHADERLAB_API std::wstring LocalBuildId();

    // The default broker pipe base name, per-user isolated by SID:
    // "ShaderLab.mcp.v1.<user-SID>". THE single source of truth so the hub,
    // the shim, and every session client agree on where to meet when no
    // --pipe / SHADERLAB_MCP_PIPE override is given (stdio-migration Step 6/7).
    SHADERLAB_API std::wstring DefaultPipeBaseName();

    enum class PairingVerdict : uint8_t
    {
        Accept,
        RefusedMismatch,               // packaged: different PFN; unpackaged: different dir/build
        RefusedMixed,                  // one packaged, one not — always refused
        RefusedUnpackagedNotAllowed,   // both unpackaged but the env gate is absent
    };

    // Pure policy (env var read aside): no OS calls, so the full verdict
    // matrix is unit-testable with synthesized identities. The
    // unpackaged fallback is the weak point of binary pairing — the one
    // path where "same build" is asserted rather than proven by the OS —
    // which is exactly why it hides behind SHADERLAB_MCP_ALLOW_UNPACKAGED=1.
    SHADERLAB_API PairingVerdict EvaluatePairing(
        const PeerIdentity& self,
        const PeerIdentity& peer,
        std::wstring_view selfBuildId,
        std::wstring_view peerBuildId);
}
