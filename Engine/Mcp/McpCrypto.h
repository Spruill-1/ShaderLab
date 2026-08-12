#pragma once

// Session crypto for the MCP broker pipe (stdio-migration Step 4).
//
// Ephemeral P-256 ECDH -> HKDF-SHA256 -> AES-256-GCM, all via BCrypt —
// no new dependency. X25519 was rejected: CNG named-curve support is
// unverified at the manifest's declared 10.0.17763 floor and buys
// nothing against an empty threat model.
//
// WHY ENCRYPT AT ALL: not defence against a local attacker (same-user
// isolation is not a hard boundary on Windows, and this must never be
// claimed otherwise). It is architectural: the hub relays frames whose
// bodies it cannot log, cache, dump or inspect because it never holds a
// key — and that stays true when someone later adds diagnostics to the
// relay. A ~33 MB inline capture never enters the address space of the
// component most likely to be crash-dumping.
//
// Direction separation: the handshake derives TWO keys (initiator->
// acceptor and acceptor->initiator) via HKDF info labels, so the GCM
// nonce can simply be the frame sequence number without cross-direction
// reuse. The frame's clear header travels as AAD, which is what turns a
// header tamper or a sequence desync into an authentication failure at
// Open() rather than silent misdelivery.

#include "pch_engine.h"
#include "../../EngineExport.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ShaderLab::Mcp
{
    inline constexpr size_t kGcmTagBytes = 16;
    inline constexpr size_t kSessionKeyBytes = 32;

    // Ephemeral P-256 key pair. Move-only RAII over the CNG handles; the
    // private key never leaves the object.
    class SHADERLAB_API EcdhKeyPair
    {
    public:
        static std::optional<EcdhKeyPair> Generate();

        EcdhKeyPair(EcdhKeyPair&&) noexcept;
        EcdhKeyPair& operator=(EcdhKeyPair&&) noexcept;
        EcdhKeyPair(const EcdhKeyPair&) = delete;
        EcdhKeyPair& operator=(const EcdhKeyPair&) = delete;
        ~EcdhKeyPair();

        // CNG BCRYPT_ECCPUBLIC_BLOB bytes. NOTE: the blob carries an
        // 8-byte BCRYPT_ECCKEY_BLOB header ahead of the raw X||Y curve
        // points — never size buffers against the bare curve width.
        const std::vector<uint8_t>& PublicBlob() const { return m_publicBlob; }

        // Raw ECDH shared secret with a peer's public blob, already
        // corrected for the CNG trap: BCryptDeriveKey with
        // BCRYPT_KDF_RAW_SECRET returns the secret byte-REVERSED
        // (little-endian); this returns the conventional big-endian
        // form so the HKDF input matches every other ECDH stack.
        std::optional<std::vector<uint8_t>> SharedSecret(
            std::span<const uint8_t> peerPublicBlob) const;

    private:
        EcdhKeyPair() = default;
        void* m_alg{ nullptr };   // BCRYPT_ALG_HANDLE
        void* m_key{ nullptr };   // BCRYPT_KEY_HANDLE
        std::vector<uint8_t> m_publicBlob;
    };

    // HKDF-SHA256 (RFC 5869) over BCrypt HMAC primitives. Exposed so the
    // unit tests can pin the RFC test vectors.
    SHADERLAB_API bool HkdfSha256(
        std::span<const uint8_t> ikm,
        std::span<const uint8_t> salt,
        std::span<const uint8_t> info,
        std::span<uint8_t> okm);

    struct SessionKeys
    {
        std::array<uint8_t, kSessionKeyBytes> sendKey{};
        std::array<uint8_t, kSessionKeyBytes> recvKey{};
    };

    // Full key agreement: ECDH(mine, peer) -> HKDF-SHA256 with the
    // protocol salt -> two direction keys. `isInitiator` resolves which
    // derived key is send vs recv; the two sides of a handshake call
    // this with opposite values and end up with mirrored keys.
    SHADERLAB_API std::optional<SessionKeys> DeriveSessionKeys(
        const EcdhKeyPair& mine,
        std::span<const uint8_t> peerPublicBlob,
        bool isInitiator);

    // One AES-256-GCM direction. Nonce = 4 zero bytes + 8-byte LE seq —
    // safe because each direction has its own key, and binding the seq
    // into the nonce (plus the clear frame header into the AAD) makes a
    // desynchronized or replayed sequence fail authentication.
    class SHADERLAB_API GcmChannel
    {
    public:
        static std::optional<GcmChannel> Create(std::span<const uint8_t> key32);

        GcmChannel(GcmChannel&&) noexcept;
        GcmChannel& operator=(GcmChannel&&) noexcept;
        GcmChannel(const GcmChannel&) = delete;
        GcmChannel& operator=(const GcmChannel&) = delete;
        ~GcmChannel();

        // out = ciphertext || 16-byte tag.
        bool Seal(uint64_t seq,
                  std::span<const uint8_t> aad,
                  std::span<const uint8_t> plaintext,
                  std::vector<uint8_t>& out) const;

        // Fails (returns false, out untouched) on any tamper: body,
        // tag, AAD, or a seq that differs from the sealing seq.
        bool Open(uint64_t seq,
                  std::span<const uint8_t> aad,
                  std::span<const uint8_t> cipherWithTag,
                  std::vector<uint8_t>& out) const;

    private:
        GcmChannel() = default;
        void* m_alg{ nullptr };   // BCRYPT_ALG_HANDLE
        void* m_key{ nullptr };   // BCRYPT_KEY_HANDLE
        std::vector<uint8_t> m_keyObject;   // CNG key object storage
    };
}
