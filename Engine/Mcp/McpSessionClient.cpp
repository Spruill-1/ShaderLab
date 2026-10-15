#include "pch_engine.h"
#include "McpSessionClient.h"
#include "McpRouter.h"
#include "McpFrame.h"
#include "McpChannel.h"
#include "McpPeerIdentity.h"
#include "McpTypes.h"

#include <winrt/Windows.Data.Json.h>
#include <unordered_map>

namespace ShaderLab::Mcp
{
    namespace WDJ = winrt::Windows::Data::Json;

    namespace
    {
        std::wstring ResolvePipePath(const std::wstring& base)
        {
            return L"\\\\.\\pipe\\" + ResolvePipeBaseName(base);
        }

        // Blocking write of a whole buffer to a byte-mode pipe.
        bool WriteAll(HANDLE pipe, const std::vector<uint8_t>& bytes)
        {
            size_t off = 0;
            while (off < bytes.size())
            {
                DWORD wrote = 0;
                if (!WriteFile(pipe, bytes.data() + off,
                        static_cast<DWORD>(bytes.size() - off), &wrote, nullptr) || wrote == 0)
                    return false;
                off += wrote;
            }
            return true;
        }

        bool SendFrame(HANDLE pipe, uint32_t channelId, uint64_t seq,
                       const std::vector<uint8_t>& body)
        {
            Frame f;
            f.header.channelId = channelId;
            f.header.seq = seq;
            f.body = body;
            std::vector<uint8_t> wire;
            if (!EncodeFrame(f, wire))
                return false;
            return WriteAll(pipe, wire);
        }

        bool SendControlJson(HANDLE pipe, uint64_t seq, const std::string& json)
        {
            return SendFrame(pipe, 0, seq, std::vector<uint8_t>(json.begin(), json.end()));
        }
    }

    struct McpSessionClient::Impl
    {
        McpRouter&           router;
        SessionClientOptions opts;
        std::atomic<bool>    stop{ false };
        HANDLE               stopEvent{ nullptr };   // manual reset; set by Stop()

        mutable std::mutex   statusMutex;
        SessionStatus        status;

        // `pipe` and `runnerThread` are guarded by ioMutex.
        //
        // The pipe is opened WITHOUT FILE_FLAG_OVERLAPPED, so every read and
        // write on it is synchronous. Per the Win32 cancellation rules that
        // means Stop() must unblock the session thread with
        // CancelSynchronousIo(<that thread>) -- CancelIo/CancelIoEx only
        // cancel ASYNCHRONOUS operations -- and it must NOT close the handle,
        // because the session thread is concurrently inside ReadFile /
        // WriteFile on it. Closing a handle out from under in-flight I/O is
        // undefined: a Debug build raises STATUS_INVALID_HANDLE (0xC0000008),
        // and once the value is recycled by any other thread in the process
        // the session thread can write MCP bytes into an unrelated object.
        // ServeOnce (the owning thread) does the close.
        //
        // ioMutex is only ever held around publishing / cancelling / closing
        // these handles -- never across blocking I/O -- so Stop() cannot be
        // delayed by an in-flight request.
        std::mutex           ioMutex;
        HANDLE               pipe{ INVALID_HANDLE_VALUE };
        HANDLE               runnerThread{ nullptr };   // owned duplicate

        // Per-channel acceptor state.
        struct Channel
        {
            SecureChannel sc;
            uint64_t      sendSeq{ 1 };  // handshake was seq 0
            uint64_t      recvSeq{ 0 };  // highest data seq opened; older ones are replays
        };
        std::unordered_map<uint32_t, Channel> channels;

        Impl(McpRouter& r, SessionClientOptions o)
            : router(r), opts(std::move(o)), stopEvent(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
        ~Impl() { if (stopEvent) ::CloseHandle(stopEvent); }

        void SetStatus(SessionState state, std::wstring detail = {})
        {
            std::lock_guard lock(statusMutex);
            status = { state, std::move(detail) };
        }

        // One connect → register → serve cycle. Returns when the pipe
        // dies or Stop() fires.
        void ServeOnce()
        {
            channels.clear();
            const std::wstring path = ResolvePipePath(opts.pipeBaseName);

            SetStatus(SessionState::Connecting);
            HANDLE h = CreateFileW(path.c_str(),
                FILE_READ_DATA | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                SetStatus(SessionState::NoHub, std::format(L"no hub on {} (Win32 error {})", path, ::GetLastError()));
                return;
            }
            {
                // Publish under the lock, and bail if Stop() fired while we
                // were connecting -- otherwise this handle is one Stop() never
                // saw and the session keeps serving after being disabled.
                std::lock_guard lock(ioMutex);
                if (stop.load())
                {
                    CloseHandle(h);
                    return;
                }
                pipe = h;
            }

            // Verify hub identity + pairing before registering.
            auto self = ResolveProcessIdentity(GetCurrentProcessId());
            auto hub = ResolvePipeServerIdentity(h);
            uint64_t outSeq = 1;
            // `stop` is checked at each handshake step, not just in the serve
            // loop below. Disabling MCP on a freshly launched window lands
            // here -- the client is still connecting / exchanging hello -- and
            // without these checks the handshake ran to completion against a
            // cancelled pipe before anyone noticed the toggle.
            bool ok = self && hub && !stop.load();
            if (!self || !hub)
                SetStatus(SessionState::Refused, L"could not verify the hub's process identity");
            if (ok)
            {
                auto myBuild = WideToUtf8(LocalBuildId());
                ok = SendControlJson(h, outSeq++, std::format(
                    R"({{"op":"hello","role":"session","sessionId":"{}","label":"{}","protocol":"v1","buildId":"{}","pid":{}}})",
                    JsonEscape(WideToUtf8(opts.sessionId)),
                    JsonEscape(WideToUtf8(opts.label)),
                    JsonEscape(myBuild), GetCurrentProcessId()));
            }

            std::vector<uint8_t> acc;
            if (ok && !stop.load())
            {
                Frame ack;
                if (ReadFrameBlocking(h, acc, ack) && ack.header.channelId == 0)
                {
                    auto obj = ParseControl(ack);
                    auto hubBuild = obj ? std::wstring(obj->GetNamedString(L"buildId", L"")) : L"";
                    const bool acked = obj && WideToUtf8(obj->GetNamedString(L"op", L"")) == "hello-ack";
                    const auto verdict = EvaluatePairing(*self, *hub, LocalBuildId(), hubBuild);
                    ok = acked && verdict == PairingVerdict::Accept;
                    if (!acked)
                        SetStatus(SessionState::Refused, L"the hub did not acknowledge the session");
                    else if (!ok)
                        SetStatus(SessionState::Refused, std::format(
                            L"the hub is a different ShaderLab build or package (pairing verdict {})",
                            static_cast<int>(verdict)));
                }
                else ok = false;
            }

            if (ok)
            {
                SetStatus(SessionState::Registered);
                for (;;)
                {
                    if (stop.load()) break;
                    Frame f;
                    if (!ReadFrameBlocking(h, acc, f))
                        break;
                    HandleFrame(h, f);
                }
                SetStatus(SessionState::Connecting, L"the connection to the hub closed");
            }

            // This thread owns the close -- see the ioMutex note above.
            HANDLE cur = INVALID_HANDLE_VALUE;
            {
                std::lock_guard lock(ioMutex);
                cur = pipe;
                pipe = INVALID_HANDLE_VALUE;
            }
            if (cur != INVALID_HANDLE_VALUE)
                CloseHandle(cur);
        }

        void HandleFrame(HANDLE h, const Frame& f)
        {
            if (f.header.channelId == 0)
            {
                auto obj = ParseControl(f);
                auto op = obj ? WideToUtf8(obj->GetNamedString(L"op", L"")) : std::string();
                if (op == "channel-close" && obj)
                    channels.erase(static_cast<uint32_t>(obj->GetNamedNumber(L"channelId", 0)));
                // Other control ops (ping etc.) need no reply from a session.
                return;
            }

            const uint32_t cid = f.header.channelId;
            if (f.header.seq == 0)
            {
                // Handshake: peer (shim) public blob. Acceptor side.
                auto sc = SecureChannel::Create(/*initiator=*/false);
                if (!sc || !sc->OnPeerHello(f.body))
                    return;
                auto helloBody = sc->HelloBody();
                channels.insert_or_assign(cid, Channel{ std::move(*sc), 1 });
                SendFrame(h, cid, 0, helloBody);   // our hello, seq 0
                return;
            }

            auto it = channels.find(cid);
            if (it == channels.end() || !it->second.sc.Ready())
                return;   // data before handshake; drop

            // The shim numbers each channel's requests upward from 1, so a seq
            // at or below the last one opened is a replay.
            if (f.header.seq <= it->second.recvSeq)
                return;
            auto plain = it->second.sc.Open(cid, f.header.seq, f.body);
            if (!plain)
                return;   // tamper / desync: drop the frame
            it->second.recvSeq = f.header.seq;

            std::string requestJson(plain->begin(), plain->end());
            Response resp = router.RouteRequest(L"POST", L"/", requestJson);
            if (resp.noReply)
                return;   // JSON-RPC notification: no response frame

            std::vector<uint8_t> respBytes(resp.body.begin(), resp.body.end());
            uint64_t seq = it->second.sendSeq++;
            auto sealed = it->second.sc.Seal(cid, seq, respBytes);
            if (sealed)
                SendFrame(h, cid, seq, *sealed);
        }

        static std::optional<WDJ::JsonObject> ParseControl(const Frame& f)
        {
            std::string s(f.body.begin(), f.body.end());
            WDJ::JsonObject o{ nullptr };
            if (!WDJ::JsonObject::TryParse(winrt::to_hstring(s), o))
                return std::nullopt;
            return o;
        }

        // Blocking read that accumulates until one frame decodes. Returns
        // false when the pipe closes (or Stop() closed our handle).
        bool ReadFrameBlocking(HANDLE h, std::vector<uint8_t>& acc, Frame& out)
        {
            for (;;)
            {
                auto d = TryDecodeFrame(acc);
                if (d.status == FrameDecodeStatus::Ok)
                {
                    acc.erase(acc.begin(), acc.begin() + d.consumed);
                    out = std::move(d.frame);
                    return true;
                }
                if (d.status == FrameDecodeStatus::Oversize ||
                    d.status == FrameDecodeStatus::Malformed)
                    return false;   // poisoned stream

                uint8_t buf[16 * 1024];
                DWORD got = 0;
                if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0)
                    return false;
                acc.insert(acc.end(), buf, buf + got);
            }
        }
    };

    McpSessionClient::McpSessionClient(McpRouter& router, SessionClientOptions options)
        : m_impl(std::make_unique<Impl>(router, std::move(options)))
    {
    }

    McpSessionClient::~McpSessionClient() = default;

    void McpSessionClient::Run()
    {
        // Publish a real handle to this thread so Stop() can cancel our
        // blocking synchronous pipe I/O. GetCurrentThread() is a pseudo-handle
        // that only means "me" in the thread that calls it, so it has to be
        // duplicated into something another thread can pass to
        // CancelSynchronousIo (which needs THREAD_TERMINATE access --
        // DUPLICATE_SAME_ACCESS on the pseudo-handle grants it).
        {
            HANDLE dup = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                                GetCurrentProcess(), &dup,
                                0, FALSE, DUPLICATE_SAME_ACCESS))
            {
                std::lock_guard lock(m_impl->ioMutex);
                m_impl->runnerThread = dup;
            }
        }

        uint32_t backoffMs = 250;
        while (!m_impl->stop.load())
        {
            m_impl->ServeOnce();
            if (m_impl->stop.load())
                break;
            // Reconnect with capped exponential backoff after a drop; Stop()
            // ends the wait.
            if (m_impl->stopEvent)
                ::WaitForSingleObject(m_impl->stopEvent, backoffMs);
            else
                ::Sleep(backoffMs);
            backoffMs = std::min<uint32_t>(backoffMs * 2, 4000);
        }
        m_impl->SetStatus(SessionState::Stopped);

        // Retire the thread handle before returning: once we are gone a
        // late Stop() must not hand a dead thread to CancelSynchronousIo.
        HANDLE dup = nullptr;
        {
            std::lock_guard lock(m_impl->ioMutex);
            dup = m_impl->runnerThread;
            m_impl->runnerThread = nullptr;
        }
        if (dup)
            CloseHandle(dup);
    }

    void McpSessionClient::Stop()
    {
        m_impl->stop.store(true);
        if (m_impl->stopEvent)
            ::SetEvent(m_impl->stopEvent);

        // Unblock the session thread's pending SYNCHRONOUS pipe I/O. The pipe
        // is opened without FILE_FLAG_OVERLAPPED, so CancelIo / CancelIoEx do
        // not apply -- those cancel asynchronous operations. CancelSynchronousIo
        // takes the handle of the *blocked thread*, which Run() publishes.
        //
        // We deliberately do NOT close the pipe here: the session thread is
        // concurrently inside ReadFile / WriteFile on that handle. See the
        // ioMutex note on Impl for why closing it from this thread is a crash.
        //
        // A 0 return with ERROR_NOT_FOUND just means nothing was pending --
        // the thread will observe `stop` at its next check either way.
        std::lock_guard lock(m_impl->ioMutex);
        if (m_impl->runnerThread)
            CancelSynchronousIo(m_impl->runnerThread);
    }

    SessionStatus McpSessionClient::Status() const
    {
        std::lock_guard lock(m_impl->statusMutex);
        return m_impl->status;
    }
}
