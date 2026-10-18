#include "pch_engine.h"
#include "Shim.h"

#include "../Mcp/McpTimeouts.h"
#include "../Mcp/McpTypes.h"
#include "Version.h"

#include <fcntl.h>
#include <io.h>
#include <shobjidl.h>

#include <string_view>

namespace ShaderLab::Broker
{
    using namespace ShaderLab::Mcp;

    namespace
    {
        using ShimReply = std::vector<std::string>;

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

        constexpr const char* cShimToolsJson =
            R"JSON({"tools":[)JSON"
            R"JSON({"name":"list_sessions","description":"List the ShaderLab sessions currently registered with the MCP hub. Each entry carries a session id usable with use_session.","inputSchema":{"type":"object","properties":{}}},)JSON"
            R"JSON({"name":"use_session","description":"Attach this MCP connection to a ShaderLab session by id (from list_sessions). Subsequent tool calls are routed to that session.","inputSchema":{"type":"object","properties":{"sessionId":{"type":"string"}},"required":["sessionId"]}})JSON"
            R"JSON(]})JSON";

        // The shim's own two tools, parsed from cShimToolsJson. nullopt (and
        // logged) only if that constant stops being valid JSON.
        std::optional<WDJ::JsonArray> ShimTools()
        {
            WDJ::JsonObject parsed{ nullptr };
            if (!WDJ::JsonObject::TryParse(winrt::to_hstring(std::string(cShimToolsJson)), parsed) ||
                !parsed.HasKey(L"tools") || parsed.GetNamedValue(L"tools").ValueType() != WDJ::JsonValueType::Array)
            {
                Log("shim: the built-in tool list is not valid JSON");
                return std::nullopt;
            }
            return parsed.GetNamedArray(L"tools");
        }

        // Activates the packaged hub by AUMID, the only way the hub survives
        // the MCP client's job object (a plain CreateProcess dies with it).
        // Activation does not inherit this environment, so the pipe base
        // travels as an argument.
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

        // Round-trips a control op that expects one control reply, skipping
        // any channel frames interleaved before it.
        std::optional<WDJ::JsonObject> HubControlRoundTrip(ShimState& st, const std::string& opJson)
        {
            if (!st.EnsureHub()) return std::nullopt;
            if (!SendControl(st.hub->pipe, st.hub->seq, opJson)) { st.hub.reset(); return std::nullopt; }
            Frame reply;
            for (int i = 0; i < 64; ++i)
            {
                if (ReadFrame(st.hub->pipe, st.hub->acc, reply, 5000) != ReadFrameStatus::Ok)
                { st.hub.reset(); return std::nullopt; }
                if (reply.header.channelId == 0)
                    return ParseControl(reply);
            }
            return std::nullopt;
        }

        bool IsSessionLost(const std::optional<WDJ::JsonObject>& control)
        {
            auto op = control ? Utf8(control->GetNamedString(L"op", L"")) : std::string();
            return op == "session-gone" || op == "channel-gone" || op == "channel-close";
        }

        // A ready secure channel to the pinned session. False (clearing the
        // pin when the session is gone) otherwise.
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
            // Our hello goes out as seq 0; then the session's hello on this channel.
            if (!SendData(st.hub->pipe, st.channelId, 0, sc->HelloBody()))
            { st.hub.reset(); st.DropChannel(); return false; }

            Frame f;
            for (int i = 0; i < 64; ++i)
            {
                if (ReadFrame(st.hub->pipe, st.hub->acc, f, 5000) != ReadFrameStatus::Ok)
                { st.hub.reset(); st.DropChannel(); return false; }
                if (f.header.channelId == 0)
                {
                    if (IsSessionLost(ParseControl(f)))
                    { st.pinnedSession.clear(); st.DropChannel(); return false; }
                    continue;
                }
                if (f.header.channelId == st.channelId && f.header.seq == 0)
                {
                    if (!sc->OnPeerHello(f.body)) { st.DropChannel(); return false; }
                    st.channel = std::move(*sc);
                    st.chanSendSeq = 1;
                    st.chanRecvSeq = 0;
                    return st.channel->Ready();
                }
            }
            st.DropChannel();
            return false;
        }

        // Seals `requestLine` to the pinned session and returns its response
        // line, a full JSON-RPC response carrying the request's id. nullopt:
        // the session is gone or the channel failed.
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
                    if (IsSessionLost(ParseControl(f)))
                    { st.pinnedSession.clear(); st.DropChannel(); return std::nullopt; }
                    continue;
                }
                if (f.header.channelId == cid && f.header.seq > st.chanRecvSeq)
                {
                    auto plainResp = st.channel->Open(cid, f.header.seq, f.body);
                    if (!plainResp) { st.DropChannel(); return std::nullopt; }
                    st.chanRecvSeq = f.header.seq;
                    return std::string(plainResp->begin(), plainResp->end());
                }
            }
            return std::nullopt;
        }

        // A parsed request with an id (notifications never reach a handler).
        struct ShimRequest
        {
            const std::string&     line;
            const WDJ::JsonObject& message;
            const std::string&     id;      // JSON text of the id
            const std::string&     method;
        };

        using MethodHandler = ShimReply (*)(ShimState&, const ShimRequest&);
        using ToolHandler = ShimReply (*)(ShimState&, const ShimRequest&, const WDJ::JsonObject& arguments);

        // A forwarded method: the session's response, or the failure a
        // client can act on.
        ShimReply Forward(ShimState& st, const ShimRequest& request)
        {
            const bool isToolCall = request.method == "tools/call";
            if (st.pinnedSession.empty())
            {
                return { isToolCall
                    ? WrapResult(request.id, TextToolResult(
                        "No session attached. Call list_sessions, then use_session, "
                        "before using graph tools.", true))
                    : WrapError(request.id, -32001, "No session attached") };
            }
            if (auto forwarded = ForwardToSession(st, request.line))
                return { *forwarded };   // its id is already the request's
            return { isToolCall
                ? WrapResult(request.id, TextToolResult(
                    "session_gone: the pinned ShaderLab session is no longer registered "
                    "with the hub. Call list_sessions and use_session again.", true))
                : WrapError(request.id, -32001, "session_gone") };
        }

        ShimReply Initialize(ShimState&, const ShimRequest& request)
        {
            return { WrapResult(request.id, std::format(
                R"JSON({{"protocolVersion":"2025-06-18","capabilities":{{"tools":{{"listChanged":true}},"resources":{{}}}},"serverInfo":{{"name":"shaderlab-shim","version":"{}"}}}})JSON",
                JsonEscape(Utf8(::ShaderLab::VersionString)))) };
        }

        // The shim's two tools plus, when a session is pinned, that session's
        // catalog, merged as JSON values. use_session changing the pin is
        // why initialize advertises tools.listChanged.
        ShimReply ListTools(ShimState& st, const ShimRequest& request)
        {
            auto ownTools = ShimTools();
            if (!ownTools) return { WrapError(request.id, -32603, "Internal error") };
            WDJ::JsonArray merged;
            auto appendAll = [&](WDJ::JsonArray const& arr) {
                for (uint32_t i = 0; i < arr.Size(); ++i)
                {
                    WDJ::JsonValue v{ nullptr };
                    if (WDJ::JsonValue::TryParse(arr.GetAt(i).Stringify(), v))
                        merged.Append(v);
                }
            };
            appendAll(*ownTools);
            if (!st.pinnedSession.empty())
            {
                auto resp = ForwardToSession(st, R"({"jsonrpc":"2.0","id":0,"method":"tools/list"})");
                WDJ::JsonObject ro{ nullptr };
                if (resp && WDJ::JsonObject::TryParse(winrt::to_hstring(*resp), ro) && ro.HasKey(L"result") &&
                    ro.GetNamedValue(L"result").ValueType() == WDJ::JsonValueType::Object)
                {
                    auto result = ro.GetNamedObject(L"result");
                    if (result.HasKey(L"tools") && result.GetNamedValue(L"tools").ValueType() == WDJ::JsonValueType::Array)
                        appendAll(result.GetNamedArray(L"tools"));
                }
            }
            WDJ::JsonObject outObj;
            outObj.Insert(L"tools", merged);
            return { WrapResult(request.id, Utf8(std::wstring(outObj.Stringify()))) };
        }

        ShimReply ListSessions(ShimState& st, const ShimRequest& request, const WDJ::JsonObject&)
        {
            auto reply = HubControlRoundTrip(st, R"({"op":"list-sessions"})");
            if (!reply)
                return { WrapResult(request.id, TextToolResult(
                    "No hub is running, or it did not answer. Launch ShaderLab "
                    "(or restart your MCP client after installing) and retry.", true)) };
            if (Utf8(reply->GetNamedString(L"op", L"")) != "sessions")
                return { WrapResult(request.id, TextToolResult("Malformed hub reply.", true)) };
            auto arr = reply->GetNamedArray(L"sessions", WDJ::JsonArray());
            return { WrapResult(request.id, TextToolResult(
                std::format(R"({{"sessions":{}}})", Utf8(std::wstring(arr.Stringify()))), false)) };
        }

        // Pins a session the hub has registered. The reply comes first, then
        // notifications/tools/list_changed: tools/list now includes the
        // session's catalog, and a listChanged-aware client re-fetches it.
        ShimReply UseSession(ShimState& st, const ShimRequest& request, const WDJ::JsonObject& arguments)
        {
            auto sid = std::wstring(arguments.GetNamedString(L"sessionId", L""));
            if (sid.empty())
                return { WrapResult(request.id, TextToolResult("use_session requires a sessionId.", true)) };

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
                return { WrapResult(request.id, TextToolResult(
                    "Unknown session. Call list_sessions to see registered sessions.", true)) };
            st.DropChannel();
            st.pinnedSession = sid;
            return {
                WrapResult(request.id, TextToolResult(
                    std::format(R"({{"attached":"{}"}})", JsonEscape(Utf8(sid))), false)),
                R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})",
            };
        }

        const std::pair<std::string_view, ToolHandler> cShimTools[] =
        {
            { "list_sessions", &ListSessions },
            { "use_session",   &UseSession },
        };

        // The shim's own tools; any other tool goes to the pinned session.
        ShimReply CallTool(ShimState& st, const ShimRequest& request)
        {
            if (!request.message.HasKey(L"params") ||
                request.message.GetNamedValue(L"params").ValueType() != WDJ::JsonValueType::Object)
                return { WrapError(request.id, -32602, "Invalid params") };
            auto params = request.message.GetNamedObject(L"params");
            if (!params.HasKey(L"name") || params.GetNamedValue(L"name").ValueType() != WDJ::JsonValueType::String)
                return { WrapError(request.id, -32602, "Invalid params: missing tool name") };
            const auto tool = Utf8(std::wstring(params.GetNamedString(L"name")));
            for (const auto& [name, handler] : cShimTools)
            {
                if (tool != name) continue;
                auto arguments = params.HasKey(L"arguments") &&
                    params.GetNamedValue(L"arguments").ValueType() == WDJ::JsonValueType::Object
                    ? params.GetNamedObject(L"arguments") : WDJ::JsonObject();
                return handler(st, request, arguments);
            }
            return Forward(st, request);
        }

        const std::pair<std::string_view, MethodHandler> cMethods[] =
        {
            { "initialize",     &Initialize },
            { "ping",           [](ShimState&, const ShimRequest& r) -> ShimReply { return { WrapResult(r.id, "{}") }; } },
            { "resources/list", [](ShimState&, const ShimRequest& r) -> ShimReply { return { WrapResult(r.id, R"JSON({"resources":[]})JSON") }; } },
            { "tools/list",     &ListTools },
            { "tools/call",     &CallTool },
            { "resources/read", &Forward },
        };

        // The id as JSON text; nullopt when the message has none usable
        // (a notification).
        std::optional<std::string> RequestId(const WDJ::JsonObject& message)
        {
            if (!message.HasKey(L"id")) return std::nullopt;
            auto id = message.GetNamedValue(L"id");
            if (id.ValueType() == WDJ::JsonValueType::Number)
                return std::format("{}", static_cast<int64_t>(id.GetNumber()));
            if (id.ValueType() == WDJ::JsonValueType::String)
                return "\"" + JsonEscape(Utf8(std::wstring(id.GetString()))) + "\"";
            return std::nullopt;
        }
    }

    bool ShimState::EnsureHub()
    {
        if (hub && hub->pipe != INVALID_HANDLE_VALUE)
            return true;
        hub = ConnectToHub(names, "shim");
        if (hub)
            return true;

        // No hub answered. With the packaged hub's AUMID, activate it (the
        // client-driven bootstrap) and poll for its pipe.
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

    void ShimState::DropChannel()
    {
        channelId = 0;
        channel.reset();
        chanSendSeq = 1;
        chanRecvSeq = 0;
    }

    std::vector<std::string> HandleShimRequest(ShimState& st, const std::string& line)
    {
        std::string idStr = "null";
        std::string method;
        try
        {
            WDJ::JsonObject message{ nullptr };
            if (!WDJ::JsonObject::TryParse(winrt::to_hstring(line), message))
                return { WrapError("null", -32700, "Parse error") };

            const auto id = RequestId(message);
            if (id) idStr = *id;
            if (!message.HasKey(L"method") || message.GetNamedValue(L"method").ValueType() != WDJ::JsonValueType::String)
                return id ? ShimReply{ WrapError(idStr, -32600, "Invalid Request: missing method") } : ShimReply{};
            method = Utf8(std::wstring(message.GetNamedString(L"method")));
            if (!id)
                return {};   // a notification gets no reply

            const ShimRequest request{ line, message, idStr, method };
            for (const auto& [name, handler] : cMethods)
                if (method == name) return handler(st, request);
            return { WrapError(idStr, -32601, "Method not found: " + method) };
        }
        catch (const winrt::hresult_error& ex)
        {
            Log(std::format("shim: {} failed: 0x{:08X} {}", method, static_cast<uint32_t>(ex.code()),
                Utf8(std::wstring(ex.message()))));
        }
        catch (const std::exception& ex)
        {
            Log(std::format("shim: {} failed: {}", method, ex.what()));
        }
        catch (...)
        {
            Log(std::format("shim: {} failed with an unknown exception", method));
        }
        return { WrapError(idStr, -32603, "Internal error") };
    }

    namespace
    {
        // One line per message, written as raw bytes ('\n' stays '\n';
        // stdout is _O_BINARY).
        void EmitLine(const std::string& line)
        {
            HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD written = 0;
            WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            const char nl = '\n';
            WriteFile(out, &nl, 1, &written, nullptr);
        }
    }

    int RunStdio(const BrokerNames& names, const std::wstring& hubAumid)
    {
        // Text mode would turn '\n' into "\r\n" on the wire.
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        OpenLog(L"shim");
        Log(std::format("shim: started, pipe base {}, aumid {}",
            Utf8(names.baseName), hubAumid.empty() ? "(none)" : Utf8(hubAumid)));
        ShimTools();   // logs if the built-in tool list ever stops parsing

        ShimState st;
        st.names = names;
        st.hubAumid = hubAumid;
        st.EnsureHub();   // best effort; a missing hub is handled per request

        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        std::string acc;
        char buf[16 * 1024];
        for (;;)
        {
            DWORD got = 0;
            if (!ReadFile(in, buf, sizeof(buf), &got, nullptr) || got == 0)
                break;   // the client closed stdin
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
                for (const auto& reply : HandleShimRequest(st, line))
                    EmitLine(reply);
            }
        }

        if (st.hub)
            SendControl(st.hub->pipe, st.hub->seq, R"({"op":"bye"})");
        Log("shim: stdin closed, exiting");
        return 0;
    }
}
