#include "pch_engine.h"
#include "Hub.h"

#include "../Mcp/McpPeerIdentity.h"
#include "../Mcp/McpTypes.h"

#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ShaderLab::Broker
{
    using namespace ShaderLab::Mcp;

    namespace
    {
        // One connected peer (shim or session). Shared because the channel
        // and session tables reference it across threads; the last owner
        // closes the pipe. Control replies come from the owning thread and
        // relays from other peers' threads, so every write takes writeMx.
        struct Conn
        {
            HANDLE       pipe{ INVALID_HANDLE_VALUE };
            std::mutex   writeMx;
            uint64_t     ctrlSeq{ 1 };
            std::string  role;
            std::wstring sessionId;
            std::wstring label;

            ~Conn() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }

            bool Relay(const Frame& f)
            {
                std::vector<uint8_t> wire;
                if (!EncodeFrame(f, wire)) return false;
                std::lock_guard lk(writeMx);
                return WriteAll(pipe, wire);
            }

            // Any thread: a peer's teardown sends control frames here too, so
            // the sequence number is taken under the write lock.
            bool SendControl(const std::string& json)
            {
                Frame f;
                f.header.channelId = 0;
                f.body.assign(json.begin(), json.end());
                std::lock_guard lk(writeMx);
                f.header.seq = ctrlSeq++;
                std::vector<uint8_t> wire;
                if (!EncodeFrame(f, wire)) return false;
                return WriteAll(pipe, wire);
            }
        };

        // The hub's shared state. Each connection thread holds it by
        // shared_ptr, so it outlives a thread that RunHub could not join.
        struct Hub
        {
            // Connections from accept until their thread finishes, hello
            // included, so idle exit never runs under a live connection.
            std::atomic<int>       clients{ 0 };
            std::atomic<ULONGLONG> lastActivity{ 0 };
            std::atomic<bool>      stopping{ false };
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

        // A connection thread and what RunHub needs to stop and join it.
        struct Worker
        {
            std::shared_ptr<Conn> conn;
            std::shared_ptr<std::atomic<bool>> done;
            std::thread thread;
        };

        // A DACL granting the current user, and nobody else, the pipe. Every
        // right is enumerated, never GENERIC_WRITE, so the
        // FILE_CREATE_PIPE_INSTANCE grant (it shares FILE_APPEND_DATA's bit)
        // is a decision: the hub creates every later instance under this SID
        // and the access check runs against the first instance's DACL. The EA
        // rights are needed because CreateNamedPipe requests
        // FILE_GENERIC_READ|WRITE, which include them; without them the
        // hub's own next instance fails with ERROR_ACCESS_DENIED. Same-user
        // instance squatting is outside the threat model (McpCrypto.h): the
        // enforced boundary is peer pairing at hello, not the DACL.
        class UserOnlyPipeSecurity
        {
        public:
            UserOnlyPipeSecurity() = default;
            UserOnlyPipeSecurity(const UserOnlyPipeSecurity&) = delete;
            UserOnlyPipeSecurity& operator=(const UserOnlyPipeSecurity&) = delete;
            ~UserOnlyPipeSecurity() { if (m_acl) LocalFree(m_acl); }

            // False, with the failing call logged, on error.
            bool Create()
            {
                HANDLE token{};
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                {
                    Log(std::format("hub: OpenProcessToken failed {}", GetLastError()));
                    return false;
                }
                BYTE tokenBuffer[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)]{};
                DWORD tokenLength = sizeof(tokenBuffer);
                const BOOL gotUser = GetTokenInformation(token, TokenUser, tokenBuffer, tokenLength, &tokenLength);
                const DWORD tokenError = gotUser ? ERROR_SUCCESS : GetLastError();
                CloseHandle(token);
                if (!gotUser)
                {
                    Log(std::format("hub: GetTokenInformation(TokenUser) failed {}", tokenError));
                    return false;
                }
                PSID userSid = reinterpret_cast<TOKEN_USER*>(tokenBuffer)->User.Sid;

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
                if (SetEntriesInAclW(1, &ea, nullptr, &m_acl) != ERROR_SUCCESS)
                {
                    Log("hub: SetEntriesInAcl failed");
                    return false;
                }
                InitializeSecurityDescriptor(&m_descriptor, SECURITY_DESCRIPTOR_REVISION);
                SetSecurityDescriptorDacl(&m_descriptor, TRUE, m_acl, FALSE);
                m_attributes = { sizeof(m_attributes), &m_descriptor, FALSE };
                return true;
            }

            SECURITY_ATTRIBUTES* Attributes() { return &m_attributes; }

        private:
            PACL                m_acl{ nullptr };
            SECURITY_DESCRIPTOR m_descriptor{};
            SECURITY_ATTRIBUTES m_attributes{};
        };

        HANDLE CreatePipeInstance(const BrokerNames& names, UserOnlyPipeSecurity& security, bool first)
        {
            return CreateNamedPipeW(names.pipePath.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, security.Attributes());
        }

        // Drops `conn`'s session registration and channels, and tells each
        // channel's other end. The notices go out after mapMx is released, so
        // a write to a stalled peer cannot block every other lookup.
        void Unregister(Hub& hub, const std::shared_ptr<Conn>& conn)
        {
            std::vector<std::pair<std::shared_ptr<Conn>, std::string>> notices;
            {
                std::lock_guard lk(hub.mapMx);
                if (!conn->sessionId.empty())
                {
                    auto it = hub.sessions.find(conn->sessionId);
                    if (it != hub.sessions.end() && it->second.get() == conn.get())
                        hub.sessions.erase(it);
                }
                for (auto it = hub.channels.begin(); it != hub.channels.end();)
                {
                    if (it->second.shim.get() == conn.get())
                    {
                        if (it->second.session)
                            notices.emplace_back(it->second.session, std::format(
                                R"({{"op":"channel-close","channelId":{}}})", it->first));
                        it = hub.channels.erase(it);
                    }
                    else if (it->second.session.get() == conn.get())
                    {
                        if (it->second.shim)
                            notices.emplace_back(it->second.shim, std::format(
                                R"({{"op":"session-gone","channelId":{},"sessionId":"{}"}})",
                                it->first, JsonEscape(Utf8(conn->sessionId))));
                        it = hub.channels.erase(it);
                    }
                    else ++it;
                }
            }
            for (const auto& [peer, message] : notices)
                peer->SendControl(message);
        }

        // Reads and checks the hello. Returns the refusal reason, or nullptr
        // when the peer is accepted (and registered, for a session).
        const char* AcceptHello(Hub& hub, const std::shared_ptr<Conn>& conn, std::vector<uint8_t>& acc,
                                std::optional<PeerIdentity>& clientIdent)
        {
            Frame first;
            if (ReadFrame(conn->pipe, acc, first, 5000) != ReadFrameStatus::Ok || first.header.channelId != 0)
                return "no-hello";
            auto hello = ParseControl(first);
            if (!hello || Utf8(hello->GetNamedString(L"op", L"")) != "hello")
                return "malformed-hello";

            clientIdent = ResolvePipeClientIdentity(conn->pipe);
            auto theirBuild = std::wstring(hello->GetNamedString(L"buildId", L""));
            conn->role = Utf8(hello->GetNamedString(L"role", L""));
            conn->label = std::wstring(hello->GetNamedString(L"label", L""));
            conn->sessionId = std::wstring(hello->GetNamedString(L"sessionId", L""));
            auto protocol = Utf8(hello->GetNamedString(L"protocol", L""));

            // Pairing is enforced on session registration: a session serves
            // tool calls that change real graphs, so it must be a genuine
            // ShaderLab. A shim's payloads are sealed end to end, the hub is
            // blind to them, and in production the shim is unpackaged while
            // the hub is packaged, so a shim is accepted once its identity
            // resolves and the protocol matches.
            if (protocol != "v1") return "protocol-mismatch";
            if (!clientIdent) return "identity-unresolved";
            if (conn->role == "session")
            {
                if (conn->sessionId.empty()) return "missing-session-id";
                if (EvaluatePairing(hub.self, *clientIdent, hub.buildId, theirBuild) != PairingVerdict::Accept)
                    return "pairing-refused";
            }
            return nullptr;
        }

        // Answers one channel-0 op. False ends the connection (bye).
        bool HandleControl(Hub& hub, const std::shared_ptr<Conn>& conn, const Frame& f)
        {
            auto obj = ParseControl(f);
            auto op = obj ? Utf8(obj->GetNamedString(L"op", L"")) : std::string();
            if (op == "list-sessions")
            {
                conn->SendControl(hub.SessionsJson());
            }
            else if (op == "open-channel")
            {
                // Blind: the hub only pairs the two connections; the sealed
                // handshake runs end to end over the channel afterwards.
                auto sid = obj ? std::wstring(obj->GetNamedString(L"sessionId", L"")) : L"";
                std::shared_ptr<Conn> target;
                uint32_t cid = 0;
                {
                    std::lock_guard lk(hub.mapMx);
                    auto it = hub.sessions.find(sid);
                    if (it != hub.sessions.end())
                    {
                        target = it->second;
                        cid = hub.nextChannel.fetch_add(1);
                        hub.channels[cid] = Hub::Pair{ conn, target };
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
            {
                return false;
            }
            else
            {
                conn->SendControl(R"({"op":"error","reason":"unknown-op"})");
            }
            return true;
        }

        // Forwards an opaque sealed frame to the other end of its channel;
        // the hub reads only the channel id.
        void RelayData(Hub& hub, const std::shared_ptr<Conn>& conn, const Frame& f)
        {
            std::shared_ptr<Conn> other;
            {
                std::lock_guard lk(hub.mapMx);
                auto it = hub.channels.find(f.header.channelId);
                if (it != hub.channels.end())
                    other = (it->second.shim.get() == conn.get()) ? it->second.session : it->second.shim;
            }
            if (other)
                other->Relay(f);
            else
                conn->SendControl(std::format(R"({{"op":"channel-gone","channelId":{}}})", f.header.channelId));
        }

        void ServeConnection(std::shared_ptr<Conn> conn, std::shared_ptr<Hub> hub)
        {
            std::vector<uint8_t> acc;
            std::optional<PeerIdentity> clientIdent;
            if (const char* refuse = AcceptHello(*hub, conn, acc, clientIdent))
            {
                // A peer that never sent a well-formed hello gets no reply.
                const std::string_view reason(refuse);
                if (reason == "no-hello" || reason == "malformed-hello")
                {
                    Log(std::format("conn: {}", reason == "no-hello" ? "no hello" : "malformed hello"));
                }
                else
                {
                    Log(std::format("conn: refused ({}, role={})", refuse, conn->role));
                    conn->SendControl(std::format(R"({{"op":"refused","reason":"{}"}})", refuse));
                }
            }
            else
            {
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
                hub->lastActivity.store(GetTickCount64());

                // A hub-probe only proves the hub is alive.
                while (conn->role != "hub-probe")
                {
                    Frame f;
                    auto rs = ReadFrame(conn->pipe, acc, f, 60'000);
                    if (hub->stopping.load()) break;
                    if (rs == ReadFrameStatus::Timeout) continue;
                    if (rs != ReadFrameStatus::Ok)
                    {
                        if (rs == ReadFrameStatus::Poisoned)
                            Log("conn: poisoned stream, dropping");
                        break;
                    }
                    hub->lastActivity.store(GetTickCount64());
                    if (f.header.channelId == 0)
                    {
                        if (!HandleControl(*hub, conn, f)) break;
                    }
                    else
                    {
                        RelayData(*hub, conn, f);
                    }
                }
                Log("conn: closed");
            }

            Unregister(*hub, conn);
            FlushFileBuffers(conn->pipe);
            DisconnectNamedPipe(conn->pipe);
            hub->clients.fetch_sub(1);
            hub->lastActivity.store(GetTickCount64());
        }

        // Joins every connection thread that has finished.
        void JoinFinished(std::vector<Worker>& workers)
        {
            for (auto it = workers.begin(); it != workers.end();)
            {
                if (it->done->load())
                {
                    it->thread.join();
                    it = workers.erase(it);
                }
                else ++it;
            }
        }

        // Stops the remaining connections by cancelling their pipe I/O, and
        // joins them. A thread still running after the grace period is
        // detached; it owns its Conn and Hub, so nothing it reads is freed.
        void StopWorkers(Hub& hub, std::vector<Worker>& workers)
        {
            hub.stopping.store(true);
            const ULONGLONG deadline = GetTickCount64() + 5000;
            for (;;)
            {
                JoinFinished(workers);
                if (workers.empty() || GetTickCount64() >= deadline) break;
                for (auto& worker : workers)
                    CancelIoEx(worker.conn->pipe, nullptr);
                Sleep(20);
            }
            for (auto& worker : workers)
            {
                Log(std::format("hub: connection thread (role={}) did not stop; detaching", worker.conn->role));
                worker.thread.detach();
            }
            workers.clear();
        }

        // The election loser proves the loss by completing hello with the incumbent.
        bool ProveHealthyHub(const BrokerNames& names)
        {
            return ConnectToHub(names, "hub-probe").has_value();
        }
    }

    int RunHub(const BrokerNames& names, uint32_t idleExitSec)
    {
        // An activated hub would otherwise get a visible console, and the
        // election loser must be console-free too.
        FreeConsole();
        OpenLog(L"hub");

        UserOnlyPipeSecurity security;
        if (!security.Create())
            return cHubExitFailed;

        HANDLE instance = CreatePipeInstance(names, security, /*first=*/true);
        if (instance == INVALID_HANDLE_VALUE)
        {
            DWORD err = GetLastError();
            // ERROR_ACCESS_DENIED is not only "lost the election": it also
            // means the existing instance has different parameters (a stale
            // hub after an update) or a genuine DACL denial.
            if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY)
            {
                if (ProveHealthyHub(names))
                {
                    Log("hub: lost election to a healthy incumbent, exiting 0");
                    return cHubExitOk;
                }
                Log(std::format("hub: pipe exists but no healthy hub answered "
                    "(stale instance or DACL denial), err={}", err));
                return cHubExitStaleIncumbent;
            }
            Log(std::format("hub: CreateNamedPipe failed {}", err));
            return cHubExitFailed;
        }

        auto hub = std::make_shared<Hub>();
        auto selfIdent = ResolveProcessIdentity(GetCurrentProcessId());
        if (!selfIdent)
        {
            Log("hub: cannot resolve own identity");
            CloseHandle(instance);
            return cHubExitFailed;
        }
        hub->self = *selfIdent;
        hub->buildId = LocalBuildId();
        hub->lastActivity.store(GetTickCount64());

        // Won the election: publish readiness. The event is manual-reset and
        // stays signalled if this process dies, so waiters pair it with a
        // connect timeout; graceful exits reset it.
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, names.readyEvent.c_str());
        Log(std::format("hub: elected, serving {} (idle-exit {}s)", Utf8(names.pipePath), idleExitSec));
        if (ready) SetEvent(ready);

        std::vector<Worker> workers;
        int exitCode = cHubExitOk;
        for (;;)
        {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL pending = !ConnectNamedPipe(instance, &ov);
            DWORD err = pending ? GetLastError() : ERROR_PIPE_CONNECTED;
            bool connected = (err == ERROR_PIPE_CONNECTED);
            bool idle = false;

            while (!connected)
            {
                if (err != ERROR_IO_PENDING)
                {
                    Log(std::format("hub: ConnectNamedPipe failed {}", err));
                    exitCode = cHubExitFailed;
                    break;
                }
                if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0)
                {
                    connected = true;
                    break;
                }
                JoinFinished(workers);
                if (hub->clients.load() == 0 && idleExitSec > 0 &&
                    GetTickCount64() - hub->lastActivity.load() > idleExitSec * 1000ull)
                {
                    Log("hub: idle, exiting");
                    CancelIoEx(instance, &ov);
                    DWORD transferred = 0;
                    GetOverlappedResult(instance, &ov, &transferred, TRUE);
                    idle = true;
                    break;
                }
            }
            CloseHandle(ov.hEvent);
            if (idle || exitCode != cHubExitOk)
            {
                CloseHandle(instance);
                break;
            }

            // Create the next instance before serving this one, so a second
            // client never finds zero listening instances.
            HANDLE next = CreatePipeInstance(names, security, /*first=*/false);
            if (next == INVALID_HANDLE_VALUE)
            {
                Log(std::format("hub: next-instance CreateNamedPipe failed {}", GetLastError()));
                exitCode = cHubExitFailed;
                CloseHandle(instance);
                break;
            }

            hub->lastActivity.store(GetTickCount64());
            hub->clients.fetch_add(1);
            Worker worker;
            worker.conn = std::make_shared<Conn>();
            worker.conn->pipe = instance;
            worker.done = std::make_shared<std::atomic<bool>>(false);
            worker.thread = std::thread([conn = worker.conn, hub, done = worker.done] {
                ServeConnection(conn, hub);
                done->store(true);
            });
            workers.push_back(std::move(worker));
            instance = next;
            JoinFinished(workers);
        }

        if (ready) { ResetEvent(ready); CloseHandle(ready); }
        StopWorkers(*hub, workers);
        return exitCode;
    }
}
