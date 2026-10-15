#include "pch_engine.h"
#include "../TestSupport.h"
#include "Engine/Mcp/McpChannel.h"
#include "Engine/Mcp/McpCrypto.h"
#include "Engine/Mcp/McpFrame.h"
#include "Engine/Mcp/McpPeerIdentity.h"
#include "Engine/Mcp/McpRouter.h"
#include "Engine/Mcp/McpSessionClient.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <thread>
#include <vector>

// The MCP transport: session client, frame codec, crypto, peer identity and
// the secure channel.
namespace
{
    using namespace ShaderLab::Tests;

    // Session-client start and stop. Stop() never closes the pipe while the
    // session thread is blocked on it: it sets the stop flag and cancels that
    // thread's synchronous I/O, and the owning thread closes the handle. No hub
    // runs here, so the client sits in its connect/backoff loop, the window of a
    // freshly launched app. Pins: Stop() is safe before the pipe is published,
    // the duplicated thread handle is published and retired, and Run() returns
    // promptly.
    SL_TEST_CASE(TestMcpSessionClientLifecycle, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== McpSessionClient lifecycle ===\n");
        using namespace ShaderLab::Mcp;

        ShaderLab::McpRouter router;

        // Point at a pipe name nothing is serving so CreateFileW fails fast and
        // the client stays in the pre-registration window.
        auto makeOpts = [] {
            SessionClientOptions o;
            o.pipeBaseName = L"ShaderLab.mcp.unittest.nohub." +
                std::to_wstring(GetCurrentProcessId());
            o.sessionId = L"{00000000-0000-0000-0000-00000000TEST}";
            o.label = L"unit-test-session";
            return o;
        };

        // Stop() immediately after launch, repeatedly. Any handle misuse here is
        // what took the app down on a toggle click.
        bool allJoined = true;
        for (int i = 0; i < 25 && allJoined; ++i)
        {
            McpSessionClient client(router, makeOpts());
            std::thread t([&client] { client.Run(); });
            client.Stop();                       // races the connect attempt
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (t.joinable() && std::chrono::steady_clock::now() < deadline)
            {
                t.join();
                break;
            }
            if (t.joinable()) { allJoined = false; t.detach(); }
        }
        TEST("Stop() during connect returns promptly (25x, no hang)", allJoined);

        // Stop() before Run() ever starts, and Stop() called twice, must both be
        // no-ops rather than touching a handle that was never published.
        bool safeEdges = true;
        try
        {
            McpSessionClient neverRan(router, makeOpts());
            neverRan.Stop();
            neverRan.Stop();

            McpSessionClient client(router, makeOpts());
            std::thread t([&client] { client.Run(); });
            client.Stop();
            client.Stop();                       // second Stop after the first
            t.join();
        }
        catch (...) { safeEdges = false; }
        TEST("Stop() is safe before Run() and when called twice", safeEdges);

        // The discriminating case. The two checks above never publish a pipe
        // handle (nothing is listening), so closing the handle from the
        // caller would pass them too. Here a stub pipe server accepts the
        // connection and never answers the hello, leaving the session thread
        // parked in a blocking synchronous ReadFile: the state in which
        // closing the handle from another thread is undefined.
        {
            auto opts = makeOpts();
            opts.pipeBaseName = L"ShaderLab.mcp.unittest.stub." +
                std::to_wstring(GetCurrentProcessId());
            const std::wstring pipePath = L"\\\\.\\pipe\\" + opts.pipeBaseName;

            std::atomic<bool> serverReady{ false };
            std::atomic<bool> serverStop{ false };
            std::thread server([&] {
                HANDLE p = CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                    1, 64 * 1024, 64 * 1024, 0, nullptr);
                serverReady.store(true);
                if (p == INVALID_HANDLE_VALUE) return;
                ConnectNamedPipe(p, nullptr);
                while (!serverStop.load()) Sleep(20);   // never reply
                CloseHandle(p);
            });
            while (!serverReady.load()) Sleep(5);

            McpSessionClient client(router, opts);
            std::thread t([&client] { client.Run(); });
            Sleep(400);   // connect + send hello + block reading the ack

            const auto t0 = std::chrono::steady_clock::now();
            client.Stop();
            t.join();
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();

            serverStop.store(true);
            // Release a server still parked in ConnectNamedPipe (only possible if
            // the client never got there) so this join cannot hang the suite.
            HANDLE poke = CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);
            server.join();

            printf("  Stop() while blocked in ReadFile returned in %lld ms\n",
                   static_cast<long long>(elapsedMs));
            TEST("Stop() unblocks a session parked in a synchronous read",
                 elapsedMs < 5000);
        }
    }

    // McpFrame and McpCrypto as pure units: the wire codec and the P-256 ECDH ->
    // HKDF-SHA256 -> AES-256-GCM session stack.
    SL_TEST_CASE(TestMcpFrameCrypto, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== McpFrame + McpCrypto ===\n");
        using namespace ShaderLab::Mcp;

        {
            Frame f;
            f.header.channelId = 7;
            f.header.seq = 0x1122334455667788ull;
            f.body = { 1, 2, 3, 4, 5 };
            std::vector<uint8_t> wire;
            TEST("Frame_EncodeSmall", EncodeFrame(f, wire) && wire.size() == 4 + 12 + 5);

            auto d = TryDecodeFrame(wire);
            TEST("Frame_RoundTripSmall",
                d.status == FrameDecodeStatus::Ok &&
                d.consumed == wire.size() &&
                d.frame.header.channelId == 7 &&
                d.frame.header.seq == 0x1122334455667788ull &&
                d.frame.body == f.body);

            bool truncOk = true;
            for (size_t cut : { size_t(0), size_t(3), size_t(4), size_t(10), wire.size() - 1 })
            {
                auto t = TryDecodeFrame(std::span(wire.data(), cut));
                if (t.status != FrameDecodeStatus::NeedMoreData || t.consumed != 0) truncOk = false;
            }
            TEST("Frame_TruncatedNeedsMore", truncOk);

            std::vector<uint8_t> two = wire;
            Frame g = f; g.header.seq = 9; g.body = { 42 };
            EncodeFrame(g, two);
            auto d1 = TryDecodeFrame(two);
            auto d2 = TryDecodeFrame(std::span(two.data() + d1.consumed, two.size() - d1.consumed));
            TEST("Frame_SequentialDecode",
                d1.status == FrameDecodeStatus::Ok && d2.status == FrameDecodeStatus::Ok &&
                d2.frame.header.seq == 9 && d2.frame.body.size() == 1 &&
                d1.consumed + d2.consumed == two.size());
        }
        {
            // 40 MB round-trip — the plan's own number.
            Frame f;
            f.header.channelId = 1;
            f.header.seq = 42;
            f.body.resize(40ull * 1024 * 1024);
            for (size_t i = 0; i < f.body.size(); i += 4096)
                f.body[i] = static_cast<uint8_t>(i >> 12);
            std::vector<uint8_t> wire;
            bool enc = EncodeFrame(f, wire);
            auto d = TryDecodeFrame(wire);
            TEST("Frame_40MBRoundTrip",
                enc && d.status == FrameDecodeStatus::Ok && d.frame.body == f.body);
        }
        {
            // Declared length over the 64 MB cap: explicit Oversize, zero
            // consumed — the stream is poisoned, never resynchronized.
            std::vector<uint8_t> over = { 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0 };  // 0x04000001
            auto d = TryDecodeFrame(over);
            TEST("Frame_OversizeExplicit", d.status == FrameDecodeStatus::Oversize && d.consumed == 0);

            Frame f; f.body.resize(kMaxFrameBytes);  // + header pushes past the cap
            std::vector<uint8_t> out;
            TEST("Frame_EncodeRefusesOversize", !EncodeFrame(f, out) && out.empty());

            std::vector<uint8_t> bad = { 4, 0, 0, 0, 1, 2, 3, 4 };  // len < header size
            TEST("Frame_MalformedShortLen",
                TryDecodeFrame(bad).status == FrameDecodeStatus::Malformed);
        }

        {
            // RFC 5869 test case A.1 (SHA-256).
            std::vector<uint8_t> ikm(22, 0x0b);
            std::vector<uint8_t> salt = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c };
            std::vector<uint8_t> info = { 0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9 };
            uint8_t okm[42]{};
            bool ok = HkdfSha256(ikm, salt, info, okm);
            static const uint8_t expect[42] = {
                0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,
                0x2f,0x2a,0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,
                0xec,0xc4,0xc5,0xbf,0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65 };
            TEST("Crypto_HkdfRfc5869A1", ok && memcmp(okm, expect, sizeof(expect)) == 0);
        }

        {
            auto a = EcdhKeyPair::Generate();
            auto b = EcdhKeyPair::Generate();
            TEST("Crypto_KeyPairGenerate", a && b && !a->PublicBlob().empty());
            if (!a || !b) return;

            auto ka = DeriveSessionKeys(*a, b->PublicBlob(), /*isInitiator=*/true);
            auto kb = DeriveSessionKeys(*b, a->PublicBlob(), /*isInitiator=*/false);
            TEST("Crypto_HandshakeMirrors",
                ka && kb && ka->sendKey == kb->recvKey && ka->recvKey == kb->sendKey &&
                ka->sendKey != ka->recvKey);
            if (!ka || !kb) return;

            auto sealer = GcmChannel::Create(ka->sendKey);
            auto opener = GcmChannel::Create(kb->recvKey);
            TEST("Crypto_ChannelCreate", sealer.has_value() && opener.has_value());
            if (!sealer || !opener) return;

            std::vector<uint8_t> plain(1024 * 1024);
            for (size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<uint8_t>(i * 31);
            std::vector<uint8_t> aad = { 9, 9, 9, 1, 2, 3 };   // stand-in clear frame header
            std::vector<uint8_t> box, opened;
            bool s = sealer->Seal(5, aad, plain, box);
            bool o = opener->Open(5, aad, box, opened);
            TEST("Crypto_SealOpenRoundTrip", s && o && opened == plain &&
                box.size() == plain.size() + kGcmTagBytes);

            std::vector<uint8_t> dump;
            std::vector<uint8_t> tampered = box; tampered[100] ^= 0x01;
            TEST("Crypto_TamperedCiphertextFails", !opener->Open(5, aad, tampered, dump));

            std::vector<uint8_t> tamperedTag = box; tamperedTag.back() ^= 0x01;
            TEST("Crypto_TamperedTagFails", !opener->Open(5, aad, tamperedTag, dump));

            std::vector<uint8_t> aad2 = aad; aad2[0] ^= 1;
            TEST("Crypto_TamperedAadFails", !opener->Open(5, aad2, box, dump));

            TEST("Crypto_SequenceDesyncFails", !opener->Open(6, aad, box, dump));

            auto wrongDir = GcmChannel::Create(kb->sendKey);
            TEST("Crypto_WrongDirectionKeyFails",
                wrongDir && !wrongDir->Open(5, aad, box, dump));
        }
    }

    // McpPeerIdentity: identity resolution against a loopback pipe (the server
    // PID from the client handle) and the pairing-policy matrix with
    // synthesized identities.
    SL_TEST_CASE(TestMcpPeerIdentity, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== McpPeerIdentity ===\n");
        using namespace ShaderLab::Mcp;

        auto self = ResolveProcessIdentity(::GetCurrentProcessId());
        TEST("Peer_SelfResolves", self.has_value());
        if (!self) return;
        TEST("Peer_SelfIsUnpackaged", self->kind == PeerIdentityKind::Unpackaged);
        TEST("Peer_SelfHasImageDir", !self->imageDirectory.empty());

        {
            auto pipeName = std::format(L"\\\\.\\pipe\\ShaderLabTest.{}.{}",
                ::GetCurrentProcessId(), ::GetTickCount64());
            HANDLE server = ::CreateNamedPipeW(pipeName.c_str(),
                PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1, 4096, 4096, 0, nullptr);
            HANDLE client = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            bool connected = false;
            if (server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE)
                connected = ::ConnectNamedPipe(server, nullptr) || ::GetLastError() == ERROR_PIPE_CONNECTED;
            TEST("Peer_LoopbackPipeConnected", connected);

            ULONG clientPid = 0, serverPid = 0;
            bool cq = connected && ::GetNamedPipeClientProcessId(server, &clientPid);
            bool sq = connected && ::GetNamedPipeServerProcessId(client, &serverPid);
            TEST("Peer_PipeClientPidFromServerHandle", cq && clientPid == ::GetCurrentProcessId());
            TEST("Peer_PipeServerPidFromClientHandle", sq && serverPid == ::GetCurrentProcessId());

            auto viaServer = connected ? ResolvePipeClientIdentity(server) : std::nullopt;
            auto viaClient = connected ? ResolvePipeServerIdentity(client) : std::nullopt;
            TEST("Peer_PipeIdentitiesResolve",
                viaServer && viaClient &&
                viaServer->pid == ::GetCurrentProcessId() &&
                viaClient->pid == ::GetCurrentProcessId());

            if (client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
            if (server != INVALID_HANDLE_VALUE) ::CloseHandle(server);
        }

        {
            auto buildId = LocalBuildId();
            TEST("Peer_BuildIdNonEmpty", !buildId.empty());

            PeerIdentity unpackA = *self, unpackB = *self;

            ::SetEnvironmentVariableW(L"SHADERLAB_MCP_ALLOW_UNPACKAGED", nullptr);
            TEST("Peer_UnpackagedRefusedWithoutGate",
                EvaluatePairing(unpackA, unpackB, buildId, buildId)
                    == PairingVerdict::RefusedUnpackagedNotAllowed);

            ::SetEnvironmentVariableW(L"SHADERLAB_MCP_ALLOW_UNPACKAGED", L"1");
            TEST("Peer_UnpackagedAcceptedWithGate",
                EvaluatePairing(unpackA, unpackB, buildId, buildId) == PairingVerdict::Accept);
            TEST("Peer_UnpackagedBuildMismatchRefused",
                EvaluatePairing(unpackA, unpackB, buildId, L"other#abi9")
                    == PairingVerdict::RefusedMismatch);

            PeerIdentity otherDir = unpackB;
            otherDir.imageDirectory = L"C:\\somewhere\\else";
            TEST("Peer_UnpackagedDirMismatchRefused",
                EvaluatePairing(unpackA, otherDir, buildId, buildId)
                    == PairingVerdict::RefusedMismatch);

            // Shared-parent relaxation: sibling per-project out dirs
            // under the same <Platform>\<Config> root pair, exact-dir does not.
            PeerIdentity sib = *self;
            sib.imageDirectory = L"C:\\build\\ARM64\\Debug\\ShaderLabHeadless";
            PeerIdentity sib2 = *self;
            sib2.imageDirectory = L"C:\\build\\ARM64\\Debug\\ShaderLabMcpBroker";
            TEST("Peer_UnpackagedSiblingDirAccepted",
                EvaluatePairing(sib, sib2, buildId, buildId) == PairingVerdict::Accept);
            PeerIdentity farDir = sib2;
            farDir.imageDirectory = L"C:\\build\\x64\\Release\\Elsewhere";
            TEST("Peer_UnpackagedDifferentRootRefused",
                EvaluatePairing(sib, farDir, buildId, buildId) == PairingVerdict::RefusedMismatch);

            PeerIdentity packagedA;
            packagedA.kind = PeerIdentityKind::Packaged;
            packagedA.packageFamilyName = L"ShaderLab_9v3yd384n9j18";
            PeerIdentity packagedB = packagedA;
            TEST("Peer_PackagedSamePfnAccepted",
                EvaluatePairing(packagedA, packagedB, buildId, L"anything") == PairingVerdict::Accept);

            PeerIdentity packagedC = packagedA;
            packagedC.packageFamilyName = L"SomeoneElse_abc123";
            TEST("Peer_PackagedPfnMismatchRefused",
                EvaluatePairing(packagedA, packagedC, buildId, buildId)
                    == PairingVerdict::RefusedMismatch);

            // Mixed is refused even while the unpackaged gate is SET.
            TEST("Peer_MixedAlwaysRefused",
                EvaluatePairing(packagedA, unpackA, buildId, buildId) == PairingVerdict::RefusedMixed);

            ::SetEnvironmentVariableW(L"SHADERLAB_MCP_ALLOW_UNPACKAGED", nullptr);
        }
    }

    // McpChannel: two SecureChannels handshaking and sealing in process, the
    // same code the shim (initiator) and the session (acceptor) run.
    SL_TEST_CASE(TestMcpChannel, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== McpChannel ===\n");
        using namespace ShaderLab::Mcp;

        auto shim = SecureChannel::Create(/*initiator=*/true);
        auto session = SecureChannel::Create(/*initiator=*/false);
        TEST("Channel_Create", shim.has_value() && session.has_value());
        if (!shim || !session) return;

        TEST("Channel_NotReadyBeforeHandshake", !shim->Ready() && !session->Ready());

        // Exchange hello bodies (the seq-0 handshake frames).
        bool h1 = session->OnPeerHello(shim->HelloBody());
        bool h2 = shim->OnPeerHello(session->HelloBody());
        TEST("Channel_HandshakeReady", h1 && h2 && shim->Ready() && session->Ready());

        const uint32_t cid = 5;

        // Shim -> session request at seq 1.
        std::string reqStr = R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
        std::vector<uint8_t> req(reqStr.begin(), reqStr.end());
        auto sealedReq = shim->Seal(cid, 1, req);
        TEST("Channel_ShimSeals", sealedReq.has_value());
        auto openedReq = sealedReq ? session->Open(cid, 1, *sealedReq) : std::nullopt;
        TEST("Channel_SessionOpens", openedReq.has_value() && *openedReq == req);

        // Session -> shim response at its own seq 1 (separate direction key).
        std::string respStr = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})";
        std::vector<uint8_t> resp(respStr.begin(), respStr.end());
        auto sealedResp = session->Seal(cid, 1, resp);
        auto openedResp = sealedResp ? shim->Open(cid, 1, *sealedResp) : std::nullopt;
        TEST("Channel_ResponseRoundTrip", openedResp.has_value() && *openedResp == resp);

        // A tampered sealed body fails to open.
        if (sealedReq)
        {
            auto bad = *sealedReq; bad[0] ^= 0x40;
            TEST("Channel_TamperRejected", !session->Open(cid, 1, bad).has_value());
        }

        // AAD binds the channelId: opening the same bytes on a different
        // channelId fails (a misrouted frame can't be replayed onto another
        // channel).
        if (sealedReq)
            TEST("Channel_WrongChannelIdRejected", !session->Open(cid + 1, 1, *sealedReq).has_value());

        // Wrong seq (desync / replay) fails.
        if (sealedReq)
            TEST("Channel_WrongSeqRejected", !session->Open(cid, 2, *sealedReq).has_value());

        // Distinct channels derive distinct keys (fresh ephemerals): a frame
        // sealed on one channel's shim doesn't open on a different pairing.
        auto shim2 = SecureChannel::Create(true);
        auto session2 = SecureChannel::Create(false);
        if (shim2 && session2)
        {
            session2->OnPeerHello(shim2->HelloBody());
            shim2->OnPeerHello(session2->HelloBody());
            auto s = shim2->Seal(cid, 1, req);
            TEST("Channel_CrossChannelKeyIsolation",
                s.has_value() && !session->Open(cid, 1, *s).has_value());
        }
    }
}
