#include "pch_engine.h"
#include "McpChannel.h"

namespace ShaderLab::Mcp
{
    std::array<uint8_t, 12> ChannelAad(uint32_t channelId, uint64_t seq)
    {
        std::array<uint8_t, 12> aad{};
        for (int i = 0; i < 4; ++i) aad[i] = static_cast<uint8_t>(channelId >> (8 * i));
        for (int i = 0; i < 8; ++i) aad[4 + i] = static_cast<uint8_t>(seq >> (8 * i));
        return aad;
    }

    std::optional<SecureChannel> SecureChannel::Create(bool initiator)
    {
        auto keys = EcdhKeyPair::Generate();
        if (!keys)
            return std::nullopt;
        SecureChannel ch;
        ch.m_initiator = initiator;
        ch.m_helloBody = keys->PublicBlob();
        ch.m_keys = std::move(*keys);
        return ch;
    }

    bool SecureChannel::OnPeerHello(std::span<const uint8_t> peerHelloBody)
    {
        if (Ready())
            return true;
        if (!m_keys || peerHelloBody.empty())
            return false;

        auto sk = DeriveSessionKeys(*m_keys, peerHelloBody, m_initiator);
        if (!sk)
            return false;
        auto send = GcmChannel::Create(sk->sendKey);
        auto recv = GcmChannel::Create(sk->recvKey);
        if (!send || !recv)
            return false;
        m_send = std::move(*send);
        m_recv = std::move(*recv);
        return true;
    }

    std::optional<std::vector<uint8_t>> SecureChannel::Seal(
        uint32_t channelId, uint64_t seq, std::span<const uint8_t> plaintext) const
    {
        if (!m_send)
            return std::nullopt;
        auto aad = ChannelAad(channelId, seq);
        std::vector<uint8_t> out;
        if (!m_send->Seal(seq, aad, plaintext, out))
            return std::nullopt;
        return out;
    }

    std::optional<std::vector<uint8_t>> SecureChannel::Open(
        uint32_t channelId, uint64_t seq, std::span<const uint8_t> body) const
    {
        if (!m_recv)
            return std::nullopt;
        auto aad = ChannelAad(channelId, seq);
        std::vector<uint8_t> out;
        if (!m_recv->Open(seq, aad, body, out))
            return std::nullopt;
        return out;
    }
}
