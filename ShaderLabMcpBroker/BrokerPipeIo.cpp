#include "pch_engine.h"
#include "BrokerPipeIo.h"

#include "../Engine/Mcp/McpPeerIdentity.h"
#include "../Engine/Mcp/McpTypes.h"

#include <shlobj.h>

namespace ShaderLab::Broker
{
    using namespace ShaderLab::Mcp;

    namespace
    {
        FILE* s_log = nullptr;
    }

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
        _wfopen_s(&s_log, path.c_str(), L"a");
    }

    void Log(const std::string& line)
    {
        if (!s_log) return;
        SYSTEMTIME st; GetLocalTime(&st);
        auto stamped = std::format("{:02}:{:02}:{:02}.{:03} {}\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        fwrite(stamped.data(), 1, stamped.size(), s_log);
        fflush(s_log);
    }

    std::string Utf8(std::wstring_view text) { return WideToUtf8(text); }

    BrokerNames ResolveNames(const std::wstring& pipeArg)
    {
        BrokerNames n;
        n.baseName = ResolvePipeBaseName(pipeArg);   // shared with the session client
        n.pipePath = L"\\\\.\\pipe\\" + n.baseName;
        n.readyEvent = L"Local\\" + n.baseName + L".ready";
        return n;
    }

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
                return ReadFrameStatus::Poisoned;

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
                // Timed out: cancel, then wait for the completion packet. An
                // ERROR_NOT_FOUND from CancelIoEx means the read completed in
                // the race, so its bytes are still drained before buf leaves
                // scope.
                CancelIoEx(pipe, &ov);
                if (GetOverlappedResult(pipe, &ov, &got, TRUE) && got > 0)
                    acc.insert(acc.end(), buf, buf + got);
                CloseHandle(ov.hEvent);
                continue;   // re-checks decode and deadline
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

    bool SendData(HANDLE pipe, uint32_t channelId, uint64_t seq, const std::vector<uint8_t>& body)
    {
        Frame f;
        f.header.channelId = channelId;
        f.header.seq = seq;
        f.body = body;
        std::vector<uint8_t> wire;
        if (!EncodeFrame(f, wire)) return false;
        return WriteAll(pipe, wire);
    }

    HubConnection::HubConnection(HubConnection&& o) noexcept
        : pipe(o.pipe), seq(o.seq), acc(std::move(o.acc))
    {
        o.pipe = INVALID_HANDLE_VALUE;
    }

    HubConnection& HubConnection::operator=(HubConnection&& o) noexcept
    {
        if (this != &o)
        {
            if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
            pipe = o.pipe; seq = o.seq; acc = std::move(o.acc);
            o.pipe = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HubConnection::~HubConnection()
    {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    }

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

        // Verify the peer process before trusting anything it says.
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

        // A shim does not strictly pair the hub: it is cross-identity with a
        // packaged hub by design, and a rogue hub sees only sealed bytes. The
        // strict session-to-hub pairing lives in McpSessionClient. The
        // identity was still resolved above, and the hello-ack confirms a
        // live hub answered.
        return c;
    }
}
