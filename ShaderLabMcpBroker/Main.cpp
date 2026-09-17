// ShaderLabMcpBroker — hub + stdio shim (stdio-migration Step 5).
//
// ONE binary, two modes:
//
//   --hub    The singleton blind relay. Wins/loses the first-instance
//            election on \\.\pipe\<base>, verifies every connecting peer
//            (McpPeerIdentity pairing), answers channel-0 control ops
//            (hello / list-sessions / bye), and — from Step 6 — relays
//            sealed frames between shims and sessions by {channelId,seq}
//            without ever holding a key. Zero sessions exist in Step 5.
//
//   --stdio  The MCP front-end an MCP client launches. Owns initialize,
//            the list_sessions / use_session tools, id correlation and
//            request timeouts. Talks NDJSON on stdin/stdout (binary mode;
//            stdout carries JSON-RPC frames ONLY — logs go to
//            %LOCALAPPDATA%\ShaderLab\logs\).
//
// Deliberately NOT linked against ShaderLabEngine.dll — the hub must
// start in milliseconds and never touches a GPU. The Step 4 modules it
// needs (McpFrame, McpPeerIdentity, McpTypes' JsonEscape) are compiled
// into this exe directly.
//
// Config travels via ARGUMENTS (--pipe, --idle-exit-sec): the activated
// hub never receives the launcher's environment (spike-verified). The
// SHADERLAB_MCP_PIPE env var is honoured as a dev/CI fallback where a
// real environment exists (shim, tests); the readiness event name is
// derived from the pipe name so one override isolates every named object.

#include "pch_engine.h"
#include "../Engine/Mcp/McpFrame.h"
#include "../Engine/Mcp/McpTypes.h"
#include "../Engine/Mcp/McpPeerIdentity.h"
#include "../Engine/Mcp/McpChannel.h"
#include "../Engine/Mcp/McpTimeouts.h"
#include "../Version.h"

#include <sddl.h>
#include <aclapi.h>
#include <io.h>
#include <fcntl.h>
#include <shlobj.h>
#include <shobjidl.h>

using namespace ShaderLab::Mcp;
namespace WDJ = winrt::Windows::Data::Json;

namespace
{
    // ---- Logging (never stdout: the shim's stdout is protocol bytes) ------
    FILE* g_log = nullptr;

    void OpenLog(const wchar_t* mode)
    {
        PWSTR localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
            return;
        std::wstring dir = std::wstring(localAppData) + L"\\ShaderLab\\logs";
        CoTaskMemFree(localAppData);
        CreateDirectoryW((dir.substr(0, dir.find_last_of(L'\\'))).c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        auto path = std::format(L"{}\\broker-{}-{}.log", dir, mode, GetCurrentProcessId());
        _wfopen_s(&g_log, path.c_str(), L"a");
    }

    void Log(const std::string& line)
    {
        if (!g_log) return;
        SYSTEMTIME st; GetLocalTime(&st);
        auto stamped = std::format("{:02}:{:02}:{:02}.{:03} {}\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        fwrite(stamped.data(), 1, stamped.size(), g_log);
        fflush(g_log);
    }

    // ---- Naming ------------------------------------------------------------
    // The default pipe/event base derives from DefaultPipeBaseName()
    // (Engine/Mcp/McpPeerIdentity) so the hub, shim and every session client
    // agree on the meeting point without a --pipe override.

    struct BrokerNames
    {
        std::wstring baseName;    // e.g. ShaderLab.mcp.v1.S-1-5-21-...
        std::wstring pipePath;    // \\.\pipe\<base>
        std::wstring readyEvent;  // Local\<base>.ready
    };

    BrokerNames ResolveNames(const std::wstring& pipeArg)
    {
        BrokerNames n;
        if (!pipeArg.empty())
            n.baseName = pipeArg;
        else
        {
            wchar_t env[256]{};
            if (GetEnvironmentVariableW(L"SHADERLAB_MCP_PIPE", env, ARRAYSIZE(env)) > 0)
                n.baseName = env;
            else
                n.baseName = DefaultPipeBaseName();   // shared with the session client
        }
        n.pipePath = L"\\\\.\\pipe\\" + n.baseName;
        n.readyEvent = L"Local\\" + n.baseName + L".ready";
        return n;
    }

    // ---- Control-channel JSON ---------------------------------------------
    std::string Utf8(std::wstring_view ws) { return WideToUtf8(ws); }

    Frame MakeControl(uint64_t seq, const std::string& json)
    {
        Frame f;
        f.header.channelId = 0;
        f.header.seq = seq;
        f.body.assign(json.begin(), json.end());
        return f;
    }

    std::optional<WDJ::JsonObject> ParseControl(const Frame& f)
    {
        std::string s(f.body.begin(), f.body.end());
        WDJ::JsonObject o{ nullptr };
        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(s), o))
            return std::nullopt;
        return o;
    }

    // ---- Overlapped pipe I/O helpers --------------------------------------
    // One writer per pipe: callers serialize writes themselves. The
    // OVERLAPPED + buffer must outlive the completion packet even after a
    // successful CancelIoEx — every path below waits for completion via
    // GetOverlappedResult(bWait=TRUE) before the buffers go out of scope.
    bool WriteAll(HANDLE pipe, const std::vector<uint8_t>& bytes)
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;
        DWORD written = 0;
        bool ok = WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &ov)
            ? true : (GetLastError() == ERROR_IO_PENDING);
        if (ok)
            ok = GetOverlappedResult(pipe, &ov, &written, TRUE) && written == bytes.size();
        CloseHandle(ov.hEvent);
        return ok;
    }

    // Reads until one complete frame decodes, the deadline passes, or the
    // pipe dies. `acc` persists across calls so partial reads carry over.
    enum class ReadFrameStatus { Ok, Timeout, Closed, Poisoned };
    ReadFrameStatus ReadFrame(HANDLE pipe, std::vector<uint8_t>& acc, Frame& out, DWORD timeoutMs)
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            auto d = TryDecodeFrame(acc);
            if (d.status == FrameDecodeStatus::Ok)
            {
                acc.erase(acc.begin(), acc.begin() + d.consumed);
                out = std::move(d.frame);
                return ReadFrameStatus::Ok;
            }
            if (d.status == FrameDecodeStatus::Oversize || d.status == FrameDecodeStatus::Malformed)
                return ReadFrameStatus::Poisoned;   // stream is unrecoverable

            ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return ReadFrameStatus::Timeout;

            uint8_t buf[16 * 1024];
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) return ReadFrameStatus::Closed;
            DWORD got = 0;
            BOOL started = ReadFile(pipe, buf, sizeof(buf), nullptr, &ov);
            if (!started && GetLastError() != ERROR_IO_PENDING)
            {
                CloseHandle(ov.hEvent);
                return ReadFrameStatus::Closed;
            }
            DWORD wait = WaitForSingleObject(ov.hEvent, static_cast<DWORD>(deadline - now));
            if (wait != WAIT_OBJECT_0)
            {
                // Timed out: cancel, then WAIT for the completion packet —
                // ERROR_NOT_FOUND from CancelIoEx just means the read
                // completed in the race, so still drain the result before
                // buf leaves scope (the classic use-after-free otherwise).
                CancelIoEx(pipe, &ov);
                if (GetOverlappedResult(pipe, &ov, &got, TRUE) && got > 0)
                    acc.insert(acc.end(), buf, buf + got);
                CloseHandle(ov.hEvent);
                continue;   // loop re-checks decode + deadline
            }
            if (!GetOverlappedResult(pipe, &ov, &got, TRUE) || got == 0)
            {
                CloseHandle(ov.hEvent);
                return ReadFrameStatus::Closed;
            }
            acc.insert(acc.end(), buf, buf + got);
            CloseHandle(ov.hEvent);
        }
    }

    bool SendControl(HANDLE pipe, uint64_t& seq, const std::string& json)
    {
        std::vector<uint8_t> wire;
        if (!EncodeFrame(MakeControl(seq++, json), wire))
            return false;
        return WriteAll(pipe, wire);
    }

    // ---- Client-side connect + hello --------------------------------------
    struct HubConnection
    {
        HANDLE pipe{ INVALID_HANDLE_VALUE };
        uint64_t seq{ 1 };
        std::vector<uint8_t> acc;

        // Owns the pipe handle: rule-of-five matters here. Without an
        // explicit move ctor the compiler-generated COPY would duplicate
        // the raw HANDLE value and the source's destructor would close
        // it — the returned connection then holds a dead handle (this
        // exact bug shipped for about an hour; the smoke caught it as
        // "Hub connection lost" on the first post-connect request).
        HubConnection() = default;
        HubConnection(HubConnection&& o) noexcept
            : pipe(o.pipe), seq(o.seq), acc(std::move(o.acc))
        {
            o.pipe = INVALID_HANDLE_VALUE;
        }
        HubConnection& operator=(HubConnection&& o) noexcept
        {
            if (this != &o)
            {
                if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
                pipe = o.pipe; seq = o.seq; acc = std::move(o.acc);
                o.pipe = INVALID_HANDLE_VALUE;
            }
            return *this;
        }
        HubConnection(const HubConnection&) = delete;
        HubConnection& operator=(const HubConnection&) = delete;
        ~HubConnection() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }
    };

    // Connects, verifies the hub's process identity, exchanges hello.
    // role: "shim" | "hub-probe". Returns nullopt with reason logged.
    std::optional<HubConnection> ConnectToHub(const BrokerNames& names, const char* role)
    {
        HubConnection c;
        c.pipe = CreateFileW(names.pipePath.c_str(),
            FILE_READ_DATA | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
            0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (c.pipe == INVALID_HANDLE_VALUE)
        {
            Log(std::format("connect: CreateFileW failed {}", GetLastError()));
            return std::nullopt;
        }

        // Verify the peer PROCESS before trusting anything it says.
        auto self = ResolveProcessIdentity(GetCurrentProcessId());
        auto hub = ResolvePipeServerIdentity(c.pipe);
        if (!self || !hub)
        {
            Log("connect: identity resolution failed");
            return std::nullopt;
        }

        auto myBuild = Utf8(LocalBuildId());
        if (!SendControl(c.pipe, c.seq, std::format(
                R"({{"op":"hello","role":"{}","protocol":"v1","buildId":"{}","pid":{}}})",
                role, JsonEscape(myBuild), GetCurrentProcessId())))
        {
            Log("connect: hello write failed");
            return std::nullopt;
        }

        Frame reply;
        if (ReadFrame(c.pipe, c.acc, reply, 3000) != ReadFrameStatus::Ok)
        {
            Log("connect: no hello reply within 3s");
            return std::nullopt;
        }
        auto obj = ParseControl(reply);
        if (!obj || Utf8(obj->GetNamedString(L"op", L"")) != "hello-ack")
        {
            Log("connect: hub refused or malformed hello reply");
            return std::nullopt;
        }

        // The shim (and hub-probe) do NOT strictly pair the hub: a shim is
        // inherently cross-identity with a packaged hub, and a rogue hub
        // sees only sealed bytes. The strict session↔hub pairing lives in
        // McpSessionClient (a session must verify it registers with a real
        // ShaderLab hub). Identity was still resolved above so the peer PID
        // is known; the hello-ack confirms a live hub answered.
        (void)hub;
        return c;
    }

    // ========================================================================
    // HUB
    // ========================================================================
    // One connected peer (shim OR session). shared_ptr because the channel
    // and session tables reference it across threads; the last ref closes
    // the pipe. Writes come from the owning thread (control replies) AND
    // relay threads, so every write serializes through writeMx (one-writer-
    // per-pipe, enforced by the lock rather than by convention).
    struct Conn
    {
        HANDLE       pipe{ INVALID_HANDLE_VALUE };
        std::mutex   writeMx;
        uint64_t     ctrlSeq{ 1 };
        std::string  role;
        std::wstring sessionId;
        std::wstring label;

        ~Conn() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }

        bool WriteFrameLocked(const Frame& f)
        {
            std::vector<uint8_t> wire;
            if (!EncodeFrame(f, wire)) return false;
            std::lock_guard lk(writeMx);
            return WriteAll(pipe, wire);
        }
        bool SendControl(const std::string& json)
        {
            Frame f;
            f.header.channelId = 0;
            f.header.seq = ctrlSeq++;   // only the owning thread sends control
            f.body.assign(json.begin(), json.end());
            return WriteFrameLocked(f);
        }
        bool Relay(const Frame& f) { return WriteFrameLocked(f); }
    };

    struct Hub
    {
        std::atomic<int>       clients{ 0 };
        std::atomic<ULONGLONG> lastActivity{ 0 };
        PeerIdentity           self;
        std::wstring           buildId;

        std::mutex mapMx;
        std::unordered_map<std::wstring, std::shared_ptr<Conn>> sessions;   // by sessionId
        struct Pair { std::shared_ptr<Conn> shim, session; };
        std::unordered_map<uint32_t, Pair> channels;
        std::atomic<uint32_t> nextChannel{ 1 };

        std::string SessionsJson()
        {
            std::lock_guard lk(mapMx);
            std::string j = R"({"op":"sessions","sessions":[)";
            bool first = true;
            for (auto& [id, c] : sessions)
            {
                if (!first) j += ",";
                j += std::format(R"({{"id":"{}","label":"{}"}})",
                    JsonEscape(Utf8(id)), JsonEscape(Utf8(c->label)));
                first = false;
            }
            j += "]}";
            return j;
        }
    };

    void ServeConnection(std::shared_ptr<Conn> conn, Hub* hub)
    {
        std::vector<uint8_t> acc;
        bool counted = false;

        auto finish = [&] {
            // Tear down any channels + session registration this conn owned.
            std::vector<std::shared_ptr<Conn>> notifyShims;
            {
                std::lock_guard lk(hub->mapMx);
                if (!conn->sessionId.empty())
                {
                    auto it = hub->sessions.find(conn->sessionId);
                    if (it != hub->sessions.end() && it->second.get() == conn.get())
                        hub->sessions.erase(it);
                }
                for (auto it = hub->channels.begin(); it != hub->channels.end();)
                {
                    if (it->second.shim.get() == conn.get())
                    {
                        // shim gone: tell the session to free the channel.
                        if (it->second.session)
                            it->second.session->SendControl(std::format(
                                R"({{"op":"channel-close","channelId":{}}})", it->first));
                        it = hub->channels.erase(it);
                    }
                    else if (it->second.session.get() == conn.get())
                    {
                        // session gone: tell the shim, distinctly.
                        if (it->second.shim)
                            it->second.shim->SendControl(std::format(
                                R"({{"op":"session-gone","channelId":{},"sessionId":"{}"}})",
                                it->first, JsonEscape(Utf8(conn->sessionId))));
                        it = hub->channels.erase(it);
                    }
                    else ++it;
                }
            }
            FlushFileBuffers(conn->pipe);
            DisconnectNamedPipe(conn->pipe);
            if (counted) hub->clients.fetch_sub(1);
            hub->lastActivity.store(GetTickCount64());
        };

        // First frame must be a channel-0 hello.
        Frame first;
        if (ReadFrame(conn->pipe, acc, first, 5000) != ReadFrameStatus::Ok ||
            first.header.channelId != 0)
        {
            Log("conn: no hello");
            finish();
            return;
        }
        auto hello = ParseControl(first);
        if (!hello || Utf8(hello->GetNamedString(L"op", L"")) != "hello")
        {
            Log("conn: malformed hello");
            finish();
            return;
        }

        auto clientIdent = ResolvePipeClientIdentity(conn->pipe);
        auto theirBuild = std::wstring(hello->GetNamedString(L"buildId", L""));
        conn->role = Utf8(hello->GetNamedString(L"role", L""));
        conn->label = std::wstring(hello->GetNamedString(L"label", L""));
        conn->sessionId = std::wstring(hello->GetNamedString(L"sessionId", L""));
        auto protocol = Utf8(hello->GetNamedString(L"protocol", L""));

        // Role-aware pairing. The strict binary-pairing boundary is enforced
        // on SESSION registration — a session serves tool calls that mutate
        // real graphs, so it must be a genuine ShaderLab (packaged→PFN match,
        // or the gated unpackaged fallback). A SHIM is the MCP client's
        // front-end whose payloads are sealed end-to-end to the session; the
        // hub is blind to them, and in production the shim is unpackaged
        // while the hub is packaged (an expected mix). Refusing that mix
        // would make the whole shim↔hub link impossible, so a shim is
        // accepted once its identity resolves and the protocol matches.
        const char* refuse = nullptr;
        if (protocol != "v1")
            refuse = "protocol-mismatch";
        else if (!clientIdent)
            refuse = "identity-unresolved";
        else if (conn->role == "session")
        {
            if (conn->sessionId.empty())
                refuse = "missing-session-id";
            else if (EvaluatePairing(hub->self, *clientIdent, hub->buildId, theirBuild)
                     != PairingVerdict::Accept)
                refuse = "pairing-refused";
        }

        if (refuse)
        {
            Log(std::format("conn: refused ({}, role={})", refuse, conn->role));
            conn->SendControl(std::format(R"({{"op":"refused","reason":"{}"}})", refuse));
            finish();
            return;
        }

        int sessionCount;
        {
            std::lock_guard lk(hub->mapMx);
            if (conn->role == "session")
                hub->sessions[conn->sessionId] = conn;
            sessionCount = static_cast<int>(hub->sessions.size());
        }

        conn->SendControl(std::format(
            R"({{"op":"hello-ack","hubPid":{},"protocol":"v1","buildId":"{}","sessionCount":{}}})",
            GetCurrentProcessId(), JsonEscape(Utf8(hub->buildId)), sessionCount));
        Log(std::format("conn: accepted role={} pid={} sessionId={}",
            conn->role, clientIdent->pid, Utf8(conn->sessionId)));

        if (conn->role == "hub-probe")
        {
            finish();
            return;
        }

        counted = true;
        hub->clients.fetch_add(1);
        hub->lastActivity.store(GetTickCount64());

        for (;;)
        {
            Frame f;
            auto rs = ReadFrame(conn->pipe, acc, f, 60'000);
            if (rs == ReadFrameStatus::Timeout)
                continue;
            if (rs != ReadFrameStatus::Ok)
            {
                if (rs == ReadFrameStatus::Poisoned)
                    Log("conn: poisoned stream, dropping");
                break;
            }
            hub->lastActivity.store(GetTickCount64());

            if (f.header.channelId == 0)
            {
                auto obj = ParseControl(f);
                auto op = obj ? Utf8(obj->GetNamedString(L"op", L"")) : std::string();
                if (op == "list-sessions")
                {
                    conn->SendControl(hub->SessionsJson());
                }
                else if (op == "open-channel")
                {
                    // shim asks to reach a session by id. Blind: the hub
                    // only pairs the two conns; the sealed handshake runs
                    // end-to-end over the allocated channel afterwards.
                    auto sid = obj ? std::wstring(obj->GetNamedString(L"sessionId", L"")) : L"";
                    std::shared_ptr<Conn> target;
                    uint32_t cid = 0;
                    {
                        std::lock_guard lk(hub->mapMx);
                        auto it = hub->sessions.find(sid);
                        if (it != hub->sessions.end())
                        {
                            target = it->second;
                            cid = hub->nextChannel.fetch_add(1);
                            hub->channels[cid] = Hub::Pair{ conn, target };
                        }
                    }
                    if (target)
                        conn->SendControl(std::format(
                            R"({{"op":"channel-open","channelId":{},"sessionId":"{}"}})",
                            cid, JsonEscape(Utf8(sid))));
                    else
                        conn->SendControl(std::format(
                            R"({{"op":"session-gone","sessionId":"{}"}})", JsonEscape(Utf8(sid))));
                }
                else if (op == "bye")
                    break;
                else
                    conn->SendControl(R"({"op":"error","reason":"unknown-op"})");
            }
            else
            {
                // Relay: forward the (opaque, sealed) frame to the other
                // end of this channel. The hub reads ONLY the channelId.
                std::shared_ptr<Conn> other;
                {
                    std::lock_guard lk(hub->mapMx);
                    auto it = hub->channels.find(f.header.channelId);
                    if (it != hub->channels.end())
                        other = (it->second.shim.get() == conn.get())
                            ? it->second.session : it->second.shim;
                }
                if (other)
                    other->Relay(f);
                else
                    conn->SendControl(std::format(
                        R"({{"op":"channel-gone","channelId":{}}})", f.header.channelId));
            }
        }
        Log("conn: closed");
        finish();
    }

    // Election-loss proof: connect to the incumbent and complete hello.
    bool ProveHealthyHub(const BrokerNames& names)
    {
        return ConnectToHub(names, "hub-probe").has_value();
    }

    int RunHub(const BrokerNames& names, uint32_t idleExitSec)
    {
        // First thing: the activated hub gets a visible console otherwise
        // (spike-verified), and the election loser must be console-free.
        FreeConsole();
        OpenLog(L"hub");

        // Explicit-rights DACL for the current user — every bit
        // enumerated individually, never GENERIC_WRITE, so the
        // FILE_CREATE_PIPE_INSTANCE grant (it shares the
        // FILE_APPEND_DATA bit) is a visible decision instead of an
        // accident. It IS granted, deliberately: the hub itself creates
        // every subsequent instance under this same SID and the access
        // check runs against the first instance's DACL. The EA rights
        // are required too — CreateNamedPipe internally requests
        // FILE_GENERIC_READ|WRITE, which include FILE_READ_EA /
        // FILE_WRITE_EA; omit them and the hub's own next-instance
        // create fails ERROR_ACCESS_DENIED (measured). Same-user
        // instance squatting is outside the threat model (same-user
        // isolation is not a hard boundary; see McpCrypto.h) — the
        // enforced boundary is peer pairing at hello time, not the DACL.
        HANDLE tok{};
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok);
        BYTE tokBuf[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)]{};
        DWORD tokLen = sizeof(tokBuf);
        GetTokenInformation(tok, TokenUser, tokBuf, tokLen, &tokLen);
        CloseHandle(tok);
        PSID userSid = reinterpret_cast<TOKEN_USER*>(tokBuf)->User.Sid;

        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = FILE_READ_DATA | FILE_WRITE_DATA |
            FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
            FILE_READ_EA | FILE_WRITE_EA | SYNCHRONIZE |
            FILE_CREATE_PIPE_INSTANCE | READ_CONTROL;
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(userSid);

        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS)
        {
            Log("hub: SetEntriesInAcl failed");
            return 2;
        }
        SECURITY_DESCRIPTOR sd{};
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
        SECURITY_ATTRIBUTES sa{ sizeof(sa), &sd, FALSE };

        HANDLE first = CreateNamedPipeW(names.pipePath.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
        if (first == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            // ERROR_ACCESS_DENIED is NOT a bare "I lost" — it also means
            // "parameters differ from the existing instance" (the stale-
            // hub-after-update case) and "genuine DACL denial". Prove the
            // loss by completing hello against the incumbent.
            if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY)
            {
                if (ProveHealthyHub(names))
                {
                    Log("hub: lost election to a healthy incumbent, exiting 0");
                    LocalFree(acl);
                    return 0;
                }
                Log(std::format("hub: pipe exists but no healthy hub answered "
                    "(stale instance or DACL denial), err={}", err));
                LocalFree(acl);
                return 3;
            }
            Log(std::format("hub: CreateNamedPipe failed {}", err));
            LocalFree(acl);
            return 2;
        }

        // Won the election. Publish readiness. The event is manual-reset
        // and — if this process dies — STAYS SIGNALLED: waiters (Step 6
        // sessions) must pair it with a connect timeout + retry poll, or
        // they wait forever on a corpse. Graceful exits reset it below.
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, names.readyEvent.c_str());

        Hub hub;
        auto selfIdent = ResolveProcessIdentity(GetCurrentProcessId());
        if (!selfIdent)
        {
            Log("hub: cannot resolve own identity");
            LocalFree(acl);
            return 2;
        }
        hub.self = *selfIdent;
        hub.buildId = LocalBuildId();
        hub.lastActivity.store(GetTickCount64());

        Log(std::format("hub: elected, serving {} (idle-exit {}s)",
            Utf8(names.pipePath), idleExitSec));
        if (ready) SetEvent(ready);

        HANDLE instance = first;
        std::vector<std::thread> workers;
        int exitCode = 0;
        for (;;)
        {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL pending = !ConnectNamedPipe(instance, &ov);
            DWORD err = pending ? GetLastError() : ERROR_PIPE_CONNECTED;
            bool connected = (err == ERROR_PIPE_CONNECTED);

            while (!connected)
            {
                if (err != ERROR_IO_PENDING)
                {
                    Log(std::format("hub: ConnectNamedPipe failed {}", err));
                    exitCode = 2;
                    break;
                }
                DWORD w = WaitForSingleObject(ov.hEvent, 1000);
                if (w == WAIT_OBJECT_0)
                {
                    connected = true;
                    break;
                }
                // Idle check while nobody is knocking.
                if (hub.clients.load() == 0 && idleExitSec > 0 &&
                    GetTickCount64() - hub.lastActivity.load() > idleExitSec * 1000ull)
                {
                    Log("hub: idle, exiting");
                    CancelIoEx(instance, &ov);
                    DWORD dummy = 0;
                    GetOverlappedResult(instance, &ov, &dummy, TRUE);
                    CloseHandle(ov.hEvent);
                    CloseHandle(instance);
                    if (ready) { ResetEvent(ready); CloseHandle(ready); }
                    for (auto& t : workers) if (t.joinable()) t.detach();
                    LocalFree(acl);
                    return 0;
                }
            }
            CloseHandle(ov.hEvent);
            if (exitCode != 0)
                break;

            // Create the NEXT instance before serving this one, so a
            // second client never finds zero listening instances.
            HANDLE next = CreateNamedPipeW(names.pipePath.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
            if (next == INVALID_HANDLE_VALUE)
            {
                Log(std::format("hub: next-instance CreateNamedPipe failed {}", GetLastError()));
                exitCode = 2;
                CloseHandle(instance);
                break;
            }

            hub.lastActivity.store(GetTickCount64());
            auto conn = std::make_shared<Conn>();
            conn->pipe = instance;
            workers.emplace_back(ServeConnection, conn, &hub);
            instance = next;
        }

        if (ready) { ResetEvent(ready); CloseHandle(ready); }
        for (auto& t : workers) if (t.joinable()) t.detach();
        LocalFree(acl);
        return exitCode;
    }

    // ========================================================================
    // SHIM
    // ========================================================================
    void EmitLine(const std::string& line)
    {
        // stdout carries JSON-RPC frames only — one line per message,
        // written as raw bytes ('\n' stays '\n'; stdout is _O_BINARY).
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        const char nl = '\n';
        WriteFile(out, &nl, 1, &written, nullptr);
    }

    std::string WrapResult(const std::string& idStr, const std::string& result)
    {
        return std::format(R"JSON({{"jsonrpc":"2.0","id":{},"result":{}}})JSON", idStr, result);
    }

    std::string WrapError(const std::string& idStr, int code, std::string_view message)
    {
        return std::format(
            R"JSON({{"jsonrpc":"2.0","id":{},"error":{{"code":{},"message":"{}"}}}})JSON",
            idStr, code, JsonEscape(message));
    }

    std::string TextToolResult(const std::string& body, bool isError)
    {
        return std::format(
            R"JSON({{"content":[{{"type":"text","text":"{}"}}],"isError":{}}})JSON",
            JsonEscape(body), isError ? "true" : "false");
    }

    constexpr const char* kShimToolsJson =
        R"JSON({"tools":[)JSON"
        R"JSON({"name":"list_sessions","description":"List the ShaderLab sessions currently registered with the MCP hub. Each entry carries a session id usable with use_session.","inputSchema":{"type":"object","properties":{}}},)JSON"
        R"JSON({"name":"use_session","description":"Attach this MCP connection to a ShaderLab session by id (from list_sessions). Subsequent tool calls are routed to that session.","inputSchema":{"type":"object","properties":{"sessionId":{"type":"string"}},"required":["sessionId"]}})JSON"
        R"JSON(]})JSON";

    bool SendData(HANDLE pipe, uint32_t channelId, uint64_t seq,
                  const std::vector<uint8_t>& body)
    {
        Frame f;
        f.header.channelId = channelId;
        f.header.seq = seq;
        f.body = body;
        std::vector<uint8_t> wire;
        if (!EncodeFrame(f, wire)) return false;
        return WriteAll(pipe, wire);
    }

    // Activate the packaged hub via its AUMID. This is the ONLY way the hub
    // survives the MCP client's job object (spike: plain CreateProcess dies
    // with the job). A non-packaged shim activating a packaged app is proven
    // to work. Args carry the pipe base so the activated hub binds the same
    // pipe the shim will connect to (activation does NOT inherit our env).
    bool ActivateHub(const std::wstring& aumid, const std::wstring& pipeBase)
    {
        if (aumid.empty())
            return false;
        winrt::com_ptr<IApplicationActivationManager> mgr;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
            CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(mgr.put()));
        if (FAILED(hr))
        {
            Log(std::format("activate: CoCreateInstance failed 0x{:08X}", static_cast<uint32_t>(hr)));
            return false;
        }
        std::wstring args = std::format(L"--hub --pipe {}", pipeBase);
        DWORD pid = 0;
        hr = mgr->ActivateApplication(aumid.c_str(), args.c_str(), AO_NONE, &pid);
        if (FAILED(hr))
        {
            Log(std::format("activate: ActivateApplication failed 0x{:08X}", static_cast<uint32_t>(hr)));
            return false;
        }
        Log(std::format("activate: hub pid {}", pid));
        return true;
    }

    struct ShimState
    {
        BrokerNames  names;
        std::wstring hubAumid;   // packaged hub AUMID for on-demand activation
        std::optional<HubConnection> hub;   // lazy; may be nullopt (no hub)

        // Pinned session (from use_session) + its secure channel. One
        // channel, serial requests — the shim owns id correlation, so a
        // single in-flight request per session is sufficient.
        std::wstring pinnedSession;
        uint32_t     channelId{ 0 };
        std::optional<SecureChannel> channel;
        uint64_t     chanSendSeq{ 1 };

        bool EnsureHub()
        {
            if (hub && hub->pipe != INVALID_HANDLE_VALUE)
                return true;
            hub = ConnectToHub(names, "shim");
            if (hub)
                return true;

            // No hub answered. If we know the packaged hub's AUMID, activate
            // it (this is the client-driven bootstrap: the MCP client's shim
            // brings the hub up) and retry with a short poll for the pipe.
            if (hubAumid.empty() || !ActivateHub(hubAumid, names.baseName))
                return false;
            for (int i = 0; i < 20; ++i)   // ~5 s
            {
                Sleep(250);
                if (WaitNamedPipeW(names.pipePath.c_str(), 200))
                {
                    hub = ConnectToHub(names, "shim");
                    if (hub) return true;
                }
            }
            return false;
        }

        void DropChannel()
        {
            channelId = 0;
            channel.reset();
            chanSendSeq = 1;
        }
    };

    // Round-trip a control op that expects a single control reply.
    std::optional<WDJ::JsonObject> HubControlRoundTrip(ShimState& st, const std::string& opJson)
    {
        if (!st.EnsureHub()) return std::nullopt;
        if (!SendControl(st.hub->pipe, st.hub->seq, opJson)) { st.hub.reset(); return std::nullopt; }
        Frame reply;
        // Skip any interleaved channel frames; a control op answers on ch0.
        for (int i = 0; i < 64; ++i)
        {
            if (ReadFrame(st.hub->pipe, st.hub->acc, reply, 5000) != ReadFrameStatus::Ok)
            { st.hub.reset(); return std::nullopt; }
            if (reply.header.channelId == 0)
                return ParseControl(reply);
        }
        return std::nullopt;
    }

    // Ensure a ready secure channel to the pinned session. Returns false
    // (and clears the pin) if the session is gone.
    bool EnsureChannel(ShimState& st)
    {
        if (st.channelId != 0 && st.channel && st.channel->Ready())
            return true;
        st.DropChannel();
        if (st.pinnedSession.empty() || !st.EnsureHub())
            return false;

        auto reply = HubControlRoundTrip(st, std::format(
            R"({{"op":"open-channel","sessionId":"{}"}})", JsonEscape(Utf8(st.pinnedSession))));
        if (!reply) return false;
        auto op = Utf8(reply->GetNamedString(L"op", L""));
        if (op == "session-gone") { st.pinnedSession.clear(); return false; }
        if (op != "channel-open") return false;
        st.channelId = static_cast<uint32_t>(reply->GetNamedNumber(L"channelId", 0));

        auto sc = SecureChannel::Create(/*initiator=*/true);
        if (!sc) { st.DropChannel(); return false; }
        // Send our hello (seq 0), then read the session's hello on this channel.
        if (!SendData(st.hub->pipe, st.channelId, 0, sc->HelloBody()))
        { st.hub.reset(); st.DropChannel(); return false; }

        Frame f;
        for (int i = 0; i < 64; ++i)
        {
            if (ReadFrame(st.hub->pipe, st.hub->acc, f, 5000) != ReadFrameStatus::Ok)
            { st.hub.reset(); st.DropChannel(); return false; }
            if (f.header.channelId == 0)
            {
                auto o = ParseControl(f);
                auto cop = o ? Utf8(o->GetNamedString(L"op", L"")) : std::string();
                if (cop == "session-gone" || cop == "channel-gone" || cop == "channel-close")
                { st.pinnedSession.clear(); st.DropChannel(); return false; }
                continue;
            }
            if (f.header.channelId == st.channelId && f.header.seq == 0)
            {
                if (!sc->OnPeerHello(f.body)) { st.DropChannel(); return false; }
                st.channel = std::move(*sc);
                st.chanSendSeq = 1;
                return st.channel->Ready();
            }
        }
        st.DropChannel();
        return false;
    }

    // Seal `requestLine`, send it to the pinned session, return the
    // session's response line (already a full JSON-RPC response carrying
    // the request's id — the shim is a pass-through for forwarded
    // methods). nullopt means session_gone / channel failure.
    std::optional<std::string> ForwardToSession(ShimState& st, const std::string& requestLine)
    {
        if (!EnsureChannel(st))
            return std::nullopt;
        const uint32_t cid = st.channelId;
        uint64_t seq = st.chanSendSeq++;
        std::vector<uint8_t> plain(requestLine.begin(), requestLine.end());
        auto sealed = st.channel->Seal(cid, seq, plain);
        if (!sealed || !SendData(st.hub->pipe, cid, seq, *sealed))
        { st.hub.reset(); st.DropChannel(); return std::nullopt; }

        Frame f;
        const DWORD shimTimeout = static_cast<DWORD>(kShimRequestTimeout.count());
        for (int i = 0; i < 64; ++i)
        {
            if (ReadFrame(st.hub->pipe, st.hub->acc, f, shimTimeout) != ReadFrameStatus::Ok)
            { st.hub.reset(); st.DropChannel(); return std::nullopt; }
            if (f.header.channelId == 0)
            {
                auto o = ParseControl(f);
                auto cop = o ? Utf8(o->GetNamedString(L"op", L"")) : std::string();
                if (cop == "session-gone" || cop == "channel-gone" || cop == "channel-close")
                { st.pinnedSession.clear(); st.DropChannel(); return std::nullopt; }
                continue;
            }
            if (f.header.channelId == cid && f.header.seq >= 1)
            {
                auto plainResp = st.channel->Open(cid, f.header.seq, f.body);
                if (!plainResp) { st.DropChannel(); return std::nullopt; }
                return std::string(plainResp->begin(), plainResp->end());
            }
        }
        return std::nullopt;
    }

    std::string HandleShimRequest(ShimState& st, const std::string& line)
    {
        std::string idStr = "null";
        try
        {
            WDJ::JsonObject jobj{ nullptr };
            if (!WDJ::JsonObject::TryParse(winrt::to_hstring(line), jobj))
                return WrapError("null", -32700, "Parse error");

            bool hasId = jobj.HasKey(L"id");
            if (hasId)
            {
                auto id = jobj.GetNamedValue(L"id");
                if (id.ValueType() == WDJ::JsonValueType::Number)
                    idStr = std::format("{}", static_cast<int64_t>(id.GetNumber()));
                else if (id.ValueType() == WDJ::JsonValueType::String)
                    idStr = "\"" + JsonEscape(Utf8(std::wstring(id.GetString()))) + "\"";
                else
                    hasId = false;
            }
            if (!jobj.HasKey(L"method"))
                return hasId ? WrapError(idStr, -32600, "Invalid Request: missing method")
                             : std::string();
            auto method = Utf8(std::wstring(jobj.GetNamedString(L"method")));

            if (!hasId)
                return {};   // notification: zero bytes

            // ---- Shim-owned methods (never forwarded) ----
            if (method == "initialize")
            {
                return WrapResult(idStr, std::format(
                    R"JSON({{"protocolVersion":"2025-06-18","capabilities":{{"tools":{{"listChanged":true}},"resources":{{}}}},"serverInfo":{{"name":"shaderlab-shim","version":"{}"}}}})JSON",
                    JsonEscape(Utf8(::ShaderLab::VersionString))));
            }
            if (method == "ping")
                return WrapResult(idStr, "{}");
            if (method == "resources/list")
                return WrapResult(idStr, R"JSON({"resources":[]})JSON");
            if (method == "tools/list")
            {
                // Splice: the 2 shim tools + (when a session is pinned)
                // that session's catalog, merged as real JSON values —
                // never string-spliced. use_session flipping the pinned
                // session is why the shim advertises tools.listChanged.
                WDJ::JsonObject shimObj{ nullptr };
                WDJ::JsonObject::TryParse(winrt::to_hstring(std::string(kShimToolsJson)), shimObj);
                WDJ::JsonArray merged;
                auto appendAll = [&](WDJ::JsonArray const& arr) {
                    for (uint32_t i = 0; i < arr.Size(); ++i)
                    {
                        WDJ::JsonValue v{ nullptr };
                        if (WDJ::JsonValue::TryParse(arr.GetAt(i).Stringify(), v))
                            merged.Append(v);
                    }
                };
                appendAll(shimObj.GetNamedArray(L"tools"));
                if (!st.pinnedSession.empty())
                {
                    auto resp = ForwardToSession(st,
                        R"({"jsonrpc":"2.0","id":0,"method":"tools/list"})");
                    WDJ::JsonObject ro{ nullptr };
                    if (resp && WDJ::JsonObject::TryParse(winrt::to_hstring(*resp), ro)
                        && ro.HasKey(L"result"))
                    {
                        auto result = ro.GetNamedObject(L"result");
                        if (result.HasKey(L"tools"))
                            appendAll(result.GetNamedArray(L"tools"));
                    }
                }
                WDJ::JsonObject outObj;
                outObj.Insert(L"tools", merged);
                return WrapResult(idStr, Utf8(std::wstring(outObj.Stringify())));
            }

            const bool isToolCall = (method == "tools/call");
            if (isToolCall)
            {
                if (!jobj.HasKey(L"params") ||
                    jobj.GetNamedValue(L"params").ValueType() != WDJ::JsonValueType::Object)
                    return WrapError(idStr, -32602, "Invalid params");
                auto params = jobj.GetNamedObject(L"params");
                if (!params.HasKey(L"name"))
                    return WrapError(idStr, -32602, "Invalid params: missing tool name");
                auto tool = Utf8(std::wstring(params.GetNamedString(L"name")));

                if (tool == "list_sessions")
                {
                    auto reply = HubControlRoundTrip(st, R"({"op":"list-sessions"})");
                    if (!reply)
                        return WrapResult(idStr, TextToolResult(
                            "No hub is running, or it did not answer. Launch ShaderLab "
                            "(or restart your MCP client after installing) and retry.", true));
                    if (Utf8(reply->GetNamedString(L"op", L"")) != "sessions")
                        return WrapResult(idStr, TextToolResult("Malformed hub reply.", true));
                    auto arr = reply->GetNamedArray(L"sessions", WDJ::JsonArray());
                    return WrapResult(idStr, TextToolResult(
                        std::format(R"({{"sessions":{}}})", Utf8(std::wstring(arr.Stringify()))),
                        false));
                }
                if (tool == "use_session")
                {
                    auto args = params.HasKey(L"arguments") &&
                        params.GetNamedValue(L"arguments").ValueType() == WDJ::JsonValueType::Object
                        ? params.GetNamedObject(L"arguments") : WDJ::JsonObject();
                    auto sid = std::wstring(args.GetNamedString(L"sessionId", L""));
                    if (sid.empty())
                        return WrapResult(idStr, TextToolResult("use_session requires a sessionId.", true));

                    // Validate against the live registry before pinning.
                    auto reply = HubControlRoundTrip(st, R"({"op":"list-sessions"})");
                    bool found = false;
                    if (reply && Utf8(reply->GetNamedString(L"op", L"")) == "sessions")
                    {
                        auto arr = reply->GetNamedArray(L"sessions", WDJ::JsonArray());
                        for (uint32_t i = 0; i < arr.Size(); ++i)
                            if (std::wstring(arr.GetObjectAt(i).GetNamedString(L"id", L"")) == sid)
                            { found = true; break; }
                    }
                    if (!found)
                        return WrapResult(idStr, TextToolResult(
                            "Unknown session. Call list_sessions to see registered sessions.", true));
                    st.DropChannel();
                    st.pinnedSession = sid;
                    // Respond, then announce the tool set changed. Now that a session
                    // is pinned, tools/list returns its catalog (see above), so a
                    // listChanged-aware client (e.g. Claude Code) must re-fetch to see
                    // the session's tools. The shim advertises tools.listChanged in
                    // initialize for exactly this; without emitting the notification the
                    // session's tools stay invisible after attach. Emitted here rather
                    // than returned so the use_session reply goes out first.
                    EmitLine(WrapResult(idStr, TextToolResult(
                        std::format(R"({{"attached":"{}"}})", JsonEscape(Utf8(sid))), false)));
                    EmitLine(R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
                    return "";   // both frames already written above
                }
                // Any other tool: route to the pinned session.
            }
            else if (method != "resources/read")
            {
                // Unknown non-forwardable method.
                return WrapError(idStr, -32601, "Method not found: " + method);
            }

            // ---- Forwarded methods (graph tools, resources/read) ----
            if (st.pinnedSession.empty())
            {
                return isToolCall
                    ? WrapResult(idStr, TextToolResult(
                        "No session attached. Call list_sessions, then use_session, "
                        "before using graph tools.", true))
                    : WrapError(idStr, -32001, "No session attached");
            }
            auto forwarded = ForwardToSession(st, line);
            if (forwarded)
                return *forwarded;   // full JSON-RPC response, id already correct
            return isToolCall
                ? WrapResult(idStr, TextToolResult(
                    "session_gone: the pinned ShaderLab session is no longer registered "
                    "with the hub. Call list_sessions and use_session again.", true))
                : WrapError(idStr, -32001, "session_gone");
        }
        catch (...)
        {
            return WrapError(idStr, -32603, "Internal error");
        }
    }

    int RunStdio(const BrokerNames& names, const std::wstring& hubAumid)
    {
        // Binary mode: text mode would translate '\n' to "\r\n" on the
        // wire while string-level assertions still pass.
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        OpenLog(L"shim");
        Log(std::format("shim: started, pipe base {}, aumid {}",
            Utf8(names.baseName), hubAumid.empty() ? "(none)" : Utf8(hubAumid)));

        ShimState st;
        st.names = names;
        st.hubAumid = hubAumid;
        st.EnsureHub();   // best-effort; absence (+ activation) handled per-request

        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        std::string acc;
        char buf[16 * 1024];
        for (;;)
        {
            DWORD got = 0;
            if (!ReadFile(in, buf, sizeof(buf), &got, nullptr) || got == 0)
                break;   // client closed stdin: exit
            acc.append(buf, got);

            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos)
            {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;
                auto reply = HandleShimRequest(st, line);
                if (!reply.empty())
                    EmitLine(reply);
            }
        }

        if (st.hub)
            SendControl(st.hub->pipe, st.hub->seq, R"({"op":"bye"})");
        Log("shim: stdin closed, exiting");
        return 0;
    }
}

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
    if (hubMode == stdioMode)   // exactly one mode required
    {
        fwprintf(stderr, L"ShaderLabMcpBroker: exactly one of --hub / --stdio is required\n");
        return 1;
    }

    auto names = ResolveNames(pipeArg);
    return hubMode ? RunHub(names, idleExitSec) : RunStdio(names, hubAumid);
}
