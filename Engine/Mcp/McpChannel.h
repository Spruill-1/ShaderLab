#pragma once

// Per-channel secure state for the broker relay (stdio-migration Step 6).
//
// A "channel" is one shim↔session conversation multiplexed over the hub
// by {channelId}. The hub relays channel frames blindly; the two
// ENDPOINTS run a SecureChannel each to seal/open bodies, so the hub
// never holds a key. This class is the single home of that handshake +
// AEAD framing so the shim (broker, initiator) and the session client
// (engine, acceptor) cannot drift — both compile this TU.
//
// Frame body sub-protocol on a channel (channelId > 0):
//   seq == 0 : HANDSHAKE. body = the sender's P-256 public blob, CLEAR.
//              Both ends send one; keys derive once both are seen.
//   seq >= 1 : DATA. body = AES-256-GCM(seq, aad = {channelId,seq}, plaintext).
// Each direction has its own key + its own seq counter, so seq values
// repeat across directions harmlessly (that is why two keys are derived).

#include "pch_engine.h"
#include "../../EngineExport.h"
#include "McpCrypto.h"

#include <optional>
#include <span>
#include <vector>

namespace ShaderLab::Mcp
{
    class SHADERLAB_API SecureChannel
    {
    public:
        // initiator = the side that opened the channel (the shim).
        static std::optional<SecureChannel> Create(bool initiator);

        SecureChannel(SecureChannel&&) noexcept = default;
        SecureChannel& operator=(SecureChannel&&) noexcept = default;

        // Our handshake frame body (public blob) — send it at seq 0.
        const std::vector<uint8_t>& HelloBody() const { return m_helloBody; }

        // Feed the peer's handshake body (their public blob). Derives the
        // two direction keys. Idempotent-safe: a second call is ignored.
        bool OnPeerHello(std::span<const uint8_t> peerHelloBody);

        bool Ready() const { return m_send.has_value() && m_recv.has_value(); }

        // Seal `plaintext` for a DATA frame at (channelId, seq>=1).
        std::optional<std::vector<uint8_t>> Seal(
            uint32_t channelId, uint64_t seq, std::span<const uint8_t> plaintext) const;

        // Open a received DATA frame body at (channelId, seq>=1). Fails
        // (nullopt) on any tamper or a seq that doesn't match the sender's.
        std::optional<std::vector<uint8_t>> Open(
            uint32_t channelId, uint64_t seq, std::span<const uint8_t> body) const;

    private:
        SecureChannel() = default;
        bool                    m_initiator{ false };
        std::optional<EcdhKeyPair> m_keys;
        std::vector<uint8_t>    m_helloBody;
        std::optional<GcmChannel> m_send;
        std::optional<GcmChannel> m_recv;
    };

    // AAD for a channel DATA frame: channelId (LE u32) then seq (LE u64).
    // Exposed so callers can keep the seal/open AAD identical to the wire
    // header without duplicating the byte layout.
    SHADERLAB_API std::array<uint8_t, 12> ChannelAad(uint32_t channelId, uint64_t seq);
}
