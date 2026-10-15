#include "pch_engine.h"
#include "../TestCommon.h"
#include "../../ShaderLabMcpBroker/Shim.h"

#include <winrt/Windows.Data.Json.h>

// The broker shim's JSON-RPC handling with no hub running: what it answers
// itself, and what a client sees when a forward has nowhere to go. The hub
// side is covered end to end by Tests/RunBrokerSmoke.ps1.
namespace
{
    using namespace ShaderLab::Tests;
    using namespace ShaderLab::Broker;
    namespace WDJ = winrt::Windows::Data::Json;

    ShimState NoHubState()
    {
        ShimState state;
        state.names = ResolveNames(L"ShaderLab.mcp.unittest.shim.nohub." + std::to_wstring(GetCurrentProcessId()));
        return state;   // no AUMID: nothing is activated
    }

    WDJ::JsonObject ParseLine(const std::string& line)
    {
        WDJ::JsonObject parsed{ nullptr };
        if (!WDJ::JsonObject::TryParse(winrt::to_hstring(line), parsed)) return WDJ::JsonObject{};
        return parsed;
    }

    int ErrorCode(const std::vector<std::string>& reply)
    {
        if (reply.size() != 1) return 0;
        auto message = ParseLine(reply[0]);
        if (!message.HasKey(L"error")) return 0;
        return static_cast<int>(message.GetNamedObject(L"error").GetNamedNumber(L"code"));
    }

    // The text of a tools/call result and whether it is an error.
    std::pair<std::wstring, bool> ToolText(const std::vector<std::string>& reply)
    {
        if (reply.size() != 1) return { L"", false };
        auto message = ParseLine(reply[0]);
        if (!message.HasKey(L"result")) return { L"", false };
        auto result = message.GetNamedObject(L"result");
        auto text = result.GetNamedArray(L"content").GetObjectAt(0).GetNamedString(L"text");
        return { std::wstring(text), result.GetNamedBoolean(L"isError") };
    }

    SL_TEST_CASE(TestBrokerShimOwnMethods, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== Broker shim: its own methods ===\n");
        auto state = NoHubState();

        auto init = HandleShimRequest(state, R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
        TEST("Shim_InitializeAnswers", init.size() == 1 &&
             ParseLine(init[0]).GetNamedObject(L"result").GetNamedObject(L"serverInfo").GetNamedString(L"name") == L"shaderlab-shim");
        auto ping = HandleShimRequest(state, R"({"jsonrpc":"2.0","id":"p","method":"ping"})");
        TEST("Shim_PingKeepsStringId", ping.size() == 1 && ParseLine(ping[0]).GetNamedString(L"id") == L"p");

        auto tools = HandleShimRequest(state, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        TEST("Shim_ToolsListHasOwnTwo", tools.size() == 1 &&
             ParseLine(tools[0]).GetNamedObject(L"result").GetNamedArray(L"tools").Size() == 2);

        TEST("Shim_NotificationSilent",
             HandleShimRequest(state, R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
        TEST("Shim_ParseError", ErrorCode(HandleShimRequest(state, "{not json")) == -32700);
        TEST("Shim_MissingMethod", ErrorCode(HandleShimRequest(state, R"({"jsonrpc":"2.0","id":3})")) == -32600);
        TEST("Shim_NonStringMethod", ErrorCode(HandleShimRequest(state, R"({"jsonrpc":"2.0","id":3,"method":7})")) == -32600);
        TEST("Shim_UnknownMethod", ErrorCode(HandleShimRequest(state, R"({"jsonrpc":"2.0","id":4,"method":"prompts/list"})")) == -32601);
    }

    SL_TEST_CASE(TestBrokerShimToolCalls, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== Broker shim: tool calls with no hub ===\n");
        auto state = NoHubState();

        TEST("Shim_ToolCallNeedsParams",
             ErrorCode(HandleShimRequest(state, R"({"jsonrpc":"2.0","id":1,"method":"tools/call"})")) == -32602);
        TEST("Shim_ToolNameMustBeString",
             ErrorCode(HandleShimRequest(state, R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":5}})")) == -32602);

        auto [listText, listIsError] = ToolText(HandleShimRequest(state,
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_sessions"}})"));
        TEST("Shim_ListSessionsNoHub", listIsError && listText.find(L"No hub is running") != std::wstring::npos);

        auto [useText, useIsError] = ToolText(HandleShimRequest(state,
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"use_session","arguments":{}}})"));
        TEST("Shim_UseSessionNeedsId", useIsError && useText.find(L"requires a sessionId") != std::wstring::npos);

        auto [graphText, graphIsError] = ToolText(HandleShimRequest(state,
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"graph_overview"}})"));
        TEST("Shim_GraphToolNeedsSession", graphIsError && graphText.find(L"No session attached") != std::wstring::npos);
        TEST("Shim_ResourceReadNeedsSession",
             ErrorCode(HandleShimRequest(state, R"({"jsonrpc":"2.0","id":5,"method":"resources/read"})")) == -32001);

        // A wrong-typed argument inside a shim tool is an internal error the
        // shim logs; the client still gets a well-formed -32603.
        TEST("Shim_ThrowAnswersInternalError", ErrorCode(HandleShimRequest(state,
            R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"use_session","arguments":{"sessionId":7}}})")) == -32603);
    }
}
