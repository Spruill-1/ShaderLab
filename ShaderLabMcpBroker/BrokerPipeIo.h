#pragma once

// What the hub and the shim share: the broker log, the pipe and event names,
// framed overlapped pipe I/O, channel-0 control messages, and the client side
// of connecting to a hub.

#include "../Engine/Mcp/McpFrame.h"

#include <winrt/Windows.Data.Json.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ShaderLab::Broker
{
    namespace WDJ = winrt::Windows::Data::Json;

    // Opens %LOCALAPPDATA%\ShaderLab\logs\broker-<mode>-<pid>.log. Never
    // stdout: the shim's stdout carries protocol bytes only.
    void OpenLog(const wchar_t* mode);
    // A timestamped line; a no-op when no log is open.
    void Log(const std::string& line);

    std::string Utf8(std::wstring_view text);

    // The meeting point every broker process and session agrees on, from
    // --pipe, SHADERLAB_MCP_PIPE or the per-user default.
    struct BrokerNames
    {
        std::wstring baseName;    // e.g. ShaderLab.mcp.v1.S-1-5-21-...
        std::wstring pipePath;    // \\.\pipe\<base>
        std::wstring readyEvent;  // Local\<base>.ready
    };
    BrokerNames ResolveNames(const std::wstring& pipeArg);

    // A channel-0 control frame carrying `json`, and its parse.
    Mcp::Frame MakeControl(uint64_t seq, const std::string& json);
    std::optional<WDJ::JsonObject> ParseControl(const Mcp::Frame& frame);

    // Overlapped pipe I/O. One writer per pipe: callers serialize writes.
    // Every path waits for its I/O to complete before its buffers go out of
    // scope, including after a CancelIoEx.
    bool WriteAll(HANDLE pipe, const std::vector<uint8_t>& bytes);

    enum class ReadFrameStatus { Ok, Timeout, Closed, Poisoned };
    // Reads until one complete frame decodes, the deadline passes, or the
    // pipe dies. `acc` persists across calls so partial reads carry over.
    // Poisoned: the stream cannot be decoded any further.
    ReadFrameStatus ReadFrame(HANDLE pipe, std::vector<uint8_t>& acc, Mcp::Frame& out, DWORD timeoutMs);

    bool SendControl(HANDLE pipe, uint64_t& seq, const std::string& json);
    bool SendData(HANDLE pipe, uint32_t channelId, uint64_t seq, const std::vector<uint8_t>& body);

    // A client's connection to the hub. Owns the pipe handle, so it moves
    // but never copies: a copy would close the handle the original holds.
    struct HubConnection
    {
        HANDLE pipe{ INVALID_HANDLE_VALUE };
        uint64_t seq{ 1 };
        std::vector<uint8_t> acc;

        HubConnection() = default;
        HubConnection(HubConnection&& o) noexcept;
        HubConnection& operator=(HubConnection&& o) noexcept;
        HubConnection(const HubConnection&) = delete;
        HubConnection& operator=(const HubConnection&) = delete;
        ~HubConnection();
    };

    // Connects, resolves the hub's process identity and exchanges hello.
    // role: "shim" or "hub-probe". nullopt, with the reason logged, on failure.
    std::optional<HubConnection> ConnectToHub(const BrokerNames& names, const char* role);
}
