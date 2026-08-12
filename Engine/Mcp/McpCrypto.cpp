#include "pch_engine.h"
#include "McpCrypto.h"

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace ShaderLab::Mcp
{
    namespace
    {
        constexpr const char* kHkdfSalt = "ShaderLab.mcp.v1";
        constexpr const char* kInfoInitiatorToAcceptor = "ShaderLab.mcp.v1 i2a";
        constexpr const char* kInfoAcceptorToInitiator = "ShaderLab.mcp.v1 a2i";

        bool Ok(NTSTATUS s) { return s >= 0; }

        std::span<const uint8_t> Bytes(const char* s)
        {
            return { reinterpret_cast<const uint8_t*>(s), strlen(s) };
        }

        // HMAC-SHA256 of (data1 || data2 || data3) under `key`.
        bool HmacSha256(std::span<const uint8_t> key,
                        std::span<const uint8_t> data1,
                        std::span<const uint8_t> data2,
                        std::span<const uint8_t> data3,
                        std::span<uint8_t> out32)
        {
            if (out32.size() != 32) return false;
            BCRYPT_ALG_HANDLE alg{};
            if (!Ok(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                    nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
                return false;
            BCRYPT_HASH_HANDLE hash{};
            bool ok = Ok(BCryptCreateHash(alg, &hash, nullptr, 0,
                    const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0));
            if (ok && !data1.empty())
                ok = Ok(BCryptHashData(hash, const_cast<PUCHAR>(data1.data()),
                        static_cast<ULONG>(data1.size()), 0));
            if (ok && !data2.empty())
                ok = Ok(BCryptHashData(hash, const_cast<PUCHAR>(data2.data()),
                        static_cast<ULONG>(data2.size()), 0));
            if (ok && !data3.empty())
                ok = Ok(BCryptHashData(hash, const_cast<PUCHAR>(data3.data()),
                        static_cast<ULONG>(data3.size()), 0));
            if (ok)
                ok = Ok(BCryptFinishHash(hash, out32.data(), 32, 0));
            if (hash) BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            return ok;
        }
    }

    // ---- HKDF-SHA256 (RFC 5869) -------------------------------------------
    bool HkdfSha256(std::span<const uint8_t> ikm,
                    std::span<const uint8_t> salt,
                    std::span<const uint8_t> info,
                    std::span<uint8_t> okm)
    {
        if (okm.empty() || okm.size() > 255 * 32)
            return false;

        // Extract: PRK = HMAC(salt, IKM). RFC: an absent salt is a
        // 32-byte zero string for SHA-256.
        std::array<uint8_t, 32> zeroSalt{};
        std::span<const uint8_t> saltUse = salt.empty()
            ? std::span<const uint8_t>(zeroSalt.data(), zeroSalt.size()) : salt;
        std::array<uint8_t, 32> prk{};
        if (!HmacSha256(saltUse, ikm, {}, {}, prk))
            return false;

        // Expand: T(i) = HMAC(PRK, T(i-1) || info || i), i from 1.
        std::array<uint8_t, 32> t{};
        size_t written = 0;
        uint8_t counter = 1;
        while (written < okm.size())
        {
            std::span<const uint8_t> prev = (counter == 1)
                ? std::span<const uint8_t>{}
                : std::span<const uint8_t>(t.data(), t.size());
            uint8_t ctrByte[1] = { counter };
            if (!HmacSha256(prk, prev, info, { ctrByte, 1 }, t))
                return false;
            const size_t take = std::min<size_t>(32, okm.size() - written);
            memcpy(okm.data() + written, t.data(), take);
            written += take;
            ++counter;
        }
        return true;
    }

    // ---- EcdhKeyPair -------------------------------------------------------
    std::optional<EcdhKeyPair> EcdhKeyPair::Generate()
    {
        EcdhKeyPair kp;
        BCRYPT_ALG_HANDLE alg{};
        if (!Ok(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0)))
            return std::nullopt;
        kp.m_alg = alg;

        BCRYPT_KEY_HANDLE key{};
        if (!Ok(BCryptGenerateKeyPair(alg, &key, 256, 0)) ||
            !Ok(BCryptFinalizeKeyPair(key, 0)))
        {
            if (key) BCryptDestroyKey(key);
            return std::nullopt;   // kp's dtor closes the provider
        }
        kp.m_key = key;

        // Export the public blob (BCRYPT_ECCKEY_BLOB header + X || Y).
        ULONG len = 0;
        if (!Ok(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &len, 0)))
            return std::nullopt;
        kp.m_publicBlob.resize(len);
        if (!Ok(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                kp.m_publicBlob.data(), len, &len, 0)))
            return std::nullopt;
        kp.m_publicBlob.resize(len);
        return kp;
    }

    EcdhKeyPair::EcdhKeyPair(EcdhKeyPair&& o) noexcept
        : m_alg(o.m_alg), m_key(o.m_key), m_publicBlob(std::move(o.m_publicBlob))
    {
        o.m_alg = nullptr;
        o.m_key = nullptr;
    }

    EcdhKeyPair& EcdhKeyPair::operator=(EcdhKeyPair&& o) noexcept
    {
        if (this != &o)
        {
            this->~EcdhKeyPair();
            m_alg = o.m_alg; m_key = o.m_key; m_publicBlob = std::move(o.m_publicBlob);
            o.m_alg = nullptr; o.m_key = nullptr;
        }
        return *this;
    }

    EcdhKeyPair::~EcdhKeyPair()
    {
        if (m_key) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_key));
        if (m_alg) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(m_alg), 0);
        m_key = nullptr;
        m_alg = nullptr;
    }

    std::optional<std::vector<uint8_t>> EcdhKeyPair::SharedSecret(
        std::span<const uint8_t> peerPublicBlob) const
    {
        if (!m_alg || !m_key || peerPublicBlob.empty())
            return std::nullopt;

        BCRYPT_KEY_HANDLE peer{};
        if (!Ok(BCryptImportKeyPair(static_cast<BCRYPT_ALG_HANDLE>(m_alg), nullptr,
                BCRYPT_ECCPUBLIC_BLOB, &peer,
                const_cast<PUCHAR>(peerPublicBlob.data()),
                static_cast<ULONG>(peerPublicBlob.size()), 0)))
            return std::nullopt;

        BCRYPT_SECRET_HANDLE secret{};
        std::optional<std::vector<uint8_t>> result;
        if (Ok(BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(m_key), peer, &secret, 0)))
        {
            ULONG len = 0;
            if (Ok(BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, nullptr, 0, &len, 0)))
            {
                std::vector<uint8_t> raw(len);
                if (Ok(BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr,
                        raw.data(), len, &len, 0)))
                {
                    raw.resize(len);
                    // CNG trap: BCRYPT_KDF_RAW_SECRET hands the shared
                    // secret back byte-REVERSED (little-endian). Flip it
                    // to the conventional big-endian form.
                    std::reverse(raw.begin(), raw.end());
                    result = std::move(raw);
                }
            }
            BCryptDestroySecret(secret);
        }
        BCryptDestroyKey(peer);
        return result;
    }

    // ---- Session key derivation -------------------------------------------
    std::optional<SessionKeys> DeriveSessionKeys(
        const EcdhKeyPair& mine,
        std::span<const uint8_t> peerPublicBlob,
        bool isInitiator)
    {
        auto secret = mine.SharedSecret(peerPublicBlob);
        if (!secret)
            return std::nullopt;

        std::array<uint8_t, kSessionKeyBytes> i2a{}, a2i{};
        if (!HkdfSha256(*secret, Bytes(kHkdfSalt), Bytes(kInfoInitiatorToAcceptor), i2a) ||
            !HkdfSha256(*secret, Bytes(kHkdfSalt), Bytes(kInfoAcceptorToInitiator), a2i))
            return std::nullopt;

        SessionKeys keys;
        keys.sendKey = isInitiator ? i2a : a2i;
        keys.recvKey = isInitiator ? a2i : i2a;
        return keys;
    }

    // ---- GcmChannel --------------------------------------------------------
    std::optional<GcmChannel> GcmChannel::Create(std::span<const uint8_t> key32)
    {
        if (key32.size() != kSessionKeyBytes)
            return std::nullopt;

        GcmChannel ch;
        BCRYPT_ALG_HANDLE alg{};
        if (!Ok(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
            return std::nullopt;
        ch.m_alg = alg;

        if (!Ok(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                sizeof(BCRYPT_CHAIN_MODE_GCM), 0)))
            return std::nullopt;

        ULONG objLen = 0, cb = 0;
        if (!Ok(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0)))
            return std::nullopt;
        ch.m_keyObject.resize(objLen);

        BCRYPT_KEY_HANDLE key{};
        if (!Ok(BCryptGenerateSymmetricKey(alg, &key,
                ch.m_keyObject.data(), objLen,
                const_cast<PUCHAR>(key32.data()), static_cast<ULONG>(key32.size()), 0)))
            return std::nullopt;
        ch.m_key = key;
        return ch;
    }

    GcmChannel::GcmChannel(GcmChannel&& o) noexcept
        : m_alg(o.m_alg), m_key(o.m_key), m_keyObject(std::move(o.m_keyObject))
    {
        o.m_alg = nullptr;
        o.m_key = nullptr;
    }

    GcmChannel& GcmChannel::operator=(GcmChannel&& o) noexcept
    {
        if (this != &o)
        {
            this->~GcmChannel();
            m_alg = o.m_alg; m_key = o.m_key; m_keyObject = std::move(o.m_keyObject);
            o.m_alg = nullptr; o.m_key = nullptr;
        }
        return *this;
    }

    GcmChannel::~GcmChannel()
    {
        if (m_key) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_key));
        if (m_alg) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(m_alg), 0);
        m_key = nullptr;
        m_alg = nullptr;
    }

    namespace
    {
        std::array<uint8_t, 12> NonceFromSeq(uint64_t seq)
        {
            // 4 zero bytes + 8-byte LE seq. Per-direction keys make this
            // safe; the seq binding makes desync an auth failure.
            std::array<uint8_t, 12> n{};
            for (int i = 0; i < 8; ++i)
                n[4 + i] = static_cast<uint8_t>(seq >> (8 * i));
            return n;
        }
    }

    bool GcmChannel::Seal(uint64_t seq,
                          std::span<const uint8_t> aad,
                          std::span<const uint8_t> plaintext,
                          std::vector<uint8_t>& out) const
    {
        if (!m_key) return false;
        auto nonce = NonceFromSeq(seq);
        std::array<uint8_t, kGcmTagBytes> tag{};

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data();
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = aad.empty() ? nullptr : const_cast<PUCHAR>(aad.data());
        info.cbAuthData = static_cast<ULONG>(aad.size());
        info.pbTag = tag.data();
        info.cbTag = static_cast<ULONG>(tag.size());

        std::vector<uint8_t> cipher(plaintext.size());
        ULONG written = 0;
        if (!Ok(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(m_key),
                const_cast<PUCHAR>(plaintext.data()), static_cast<ULONG>(plaintext.size()),
                &info, nullptr, 0,
                plaintext.empty() ? nullptr : cipher.data(),
                static_cast<ULONG>(cipher.size()), &written, 0)))
            return false;
        cipher.resize(written);
        cipher.insert(cipher.end(), tag.begin(), tag.end());
        out = std::move(cipher);
        return true;
    }

    bool GcmChannel::Open(uint64_t seq,
                          std::span<const uint8_t> aad,
                          std::span<const uint8_t> cipherWithTag,
                          std::vector<uint8_t>& out) const
    {
        if (!m_key || cipherWithTag.size() < kGcmTagBytes) return false;
        auto nonce = NonceFromSeq(seq);
        const size_t cipherLen = cipherWithTag.size() - kGcmTagBytes;

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data();
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = aad.empty() ? nullptr : const_cast<PUCHAR>(aad.data());
        info.cbAuthData = static_cast<ULONG>(aad.size());
        info.pbTag = const_cast<PUCHAR>(cipherWithTag.data() + cipherLen);
        info.cbTag = kGcmTagBytes;

        std::vector<uint8_t> plain(cipherLen);
        ULONG written = 0;
        if (!Ok(BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(m_key),
                const_cast<PUCHAR>(cipherWithTag.data()), static_cast<ULONG>(cipherLen),
                &info, nullptr, 0,
                cipherLen == 0 ? nullptr : plain.data(),
                static_cast<ULONG>(plain.size()), &written, 0)))
            return false;
        plain.resize(written);
        out = std::move(plain);
        return true;
    }
}
