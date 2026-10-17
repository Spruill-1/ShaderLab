#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/GraphModel.h"
#include "../../Rendering/GraphEvaluator.h"
#include "../../Mcp/McpArgs.h"
#include "../../Mcp/McpJsonWriter.h"
#include "../../Mcp/McpJsonRpc.h"
#include "../../Mcp/McpRouter.h"
#include "../../Mcp/McpSessionClient.h"
#include "../../Mcp/McpToolCatalog.h"
#include "../../Mcp/EngineMcpRoutes.h"
#include "Graph/GraphEdits.h"

#include <cstdio>
#include <stdexcept>
#include <thread>

// The engine's MCP routes as a client sees them: the JSON writer every body
// goes through, error codes, argument coercion, /graph/apply's all-or-nothing
// contract, and the server faults that must not read as bad requests. The
// sink is a bare context on the shared device, with no host hooks.
namespace ShaderLab::Tests
{
    namespace
    {
        namespace WDJ = winrt::Windows::Data::Json;

        struct GraphOnlySink : Mcp::IEngineCommandSink
        {
            winrt::com_ptr<IEffectGraphContextInternal> context = CreateTestContext();
            Graph::GraphModel& graph = context->Model();
            bool throwOnNodeChanged{ false };
            int nodesAdded{ 0 };

            Mcp::Response Dispatch(std::function<Mcp::Response(Mcp::EngineContext&)> closure) override
            {
                Mcp::EngineContext engineContext(*context);
                return closure(engineContext);
            }
            void OnNodeAdded(uint32_t) override { ++nodesAdded; }
            void OnNodeChanged(uint32_t) override
            {
                if (throwOnNodeChanged) throw std::runtime_error("host \"hook\" failed");
            }
        };

        std::optional<WDJ::JsonObject> ParseBody(const Mcp::Response& response)
        {
            WDJ::JsonObject parsed{ nullptr };
            if (!WDJ::JsonObject::TryParse(winrt::to_hstring(response.body), parsed))
                return std::nullopt;
            return parsed;
        }

        std::wstring StringMember(const Mcp::Response& response, const wchar_t* key)
        {
            auto body = ParseBody(response);
            if (!body || !body->HasKey(key)) return L"(missing)";
            return std::wstring(body->GetNamedString(key));
        }

        uint32_t NodeIdNamed(const Graph::GraphModel& graph, const wchar_t* name)
        {
            for (const auto& node : graph.Nodes())
                if (node.name == name) return node.id;
            return 0;
        }

        SL_TEST_CASE(TestJsonWriter, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== MCP JsonWriter ===\n");
            Mcp::JsonWriter json;
            json.BeginObject()
                .Key("text").String("quote \" slash \\ newline \n tab \t ctl \x01")
                .Key("wide").String(std::wstring(L"café"))
                .Key("list").BeginArray().Int(1).Int(-2).Bool(true).Null().EndArray()
                .Key("nan").Float(std::numeric_limits<double>::quiet_NaN())
                .Key("inf").Fixed(std::numeric_limits<double>::infinity(), 2)
                .Key("fixed").Fixed(0.123456, 2)
                .Key("float").Float(1.5)
                .Key("shortest").Shortest(0.1f)
                .Key("nested").BeginObject().Key("empty").BeginArray().EndArray().EndObject()
                .Key("raw").Raw("{\"a\":1}")
                .EndObject();
            const std::string text = json.Take();
            WDJ::JsonObject parsed{ nullptr };
            const bool parses = WDJ::JsonObject::TryParse(winrt::to_hstring(text), parsed);
            TEST("JsonWriter_OutputParses", parses);
            TEST("JsonWriter_StringRoundTrips", parses &&
                 std::wstring(parsed.GetNamedString(L"text")) == L"quote \" slash \\ newline \n tab \t ctl \x01" &&
                 std::wstring(parsed.GetNamedString(L"wide")) == L"café");
            TEST("JsonWriter_NonFiniteIsNull", parses &&
                 parsed.GetNamedValue(L"nan").ValueType() == WDJ::JsonValueType::Null &&
                 parsed.GetNamedValue(L"inf").ValueType() == WDJ::JsonValueType::Null);
            TEST("JsonWriter_NumberFormats", text.find("\"fixed\":0.12,") != std::string::npos &&
                 text.find("\"float\":1.500000,") != std::string::npos &&
                 text.find("\"shortest\":0.1,") != std::string::npos);
            TEST("JsonWriter_CommasAndNesting", text.find("[1,-2,true,null]") != std::string::npos &&
                 text.find("{\"empty\":[]}") != std::string::npos && text.ends_with("\"raw\":{\"a\":1}}"));

            const auto error = Mcp::ErrorResponse(404, "no \"such\" node");
            TEST("ErrorResponse_EscapesAndCodes", error.statusCode == 404 &&
                 StringMember(error, L"error") == L"no \"such\" node" && StringMember(error, L"code") == L"not_found");
            TEST("ErrorResponse_CodePerStatus",
                 StringMember(Mcp::ErrorResponse(400, "x"), L"code") == L"bad_argument" &&
                 StringMember(Mcp::ErrorResponse(409, "x"), L"code") == L"not_ready" &&
                 StringMember(Mcp::ErrorResponse(503, "x"), L"code") == L"busy" &&
                 StringMember(Mcp::ErrorResponse(500, "x"), L"code") == L"internal");
        }

        SL_TEST_CASE(TestArgumentReaders, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== MCP argument readers ===\n");
            auto args = WDJ::JsonObject::Parse(LR"({"a":"7","b":-3,"c":"x","d":2.5})");
            std::string error;
            TEST("Args_StringifiedUint", Mcp::ReadUintArg(args, L"a", error) == 7u && error.empty());
            TEST("Args_IntAcceptsNegative", Mcp::ReadIntArg(args, L"b", error) == -3 && error.empty());
            TEST("Args_RequireNamesMissingKey", !Mcp::RequireUintArg(args, L"missing", error) &&
                 error == "missing is required");
            error.clear();
            Mcp::ReadUintArg(args, L"c", error);
            Mcp::ReadUintArg(args, L"d", error);
            TEST("Args_FirstErrorKept", error == "c must be a number");
            TEST("Args_PathIdStrict", Mcp::ParseUintText(L"42") == 42u && !Mcp::ParseUintText(L"42abc") &&
                 !Mcp::ParseUintText(L"") && !Mcp::ParseUintText(L"99999999999"));
        }

        SL_TEST_CASE(TestRouteFaultsAndCoercion, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== MCP route faults and coercion ===\n");
            GraphOnlySink sink;
            McpRouter router;
            Mcp::RegisterEngineRoutes(router, sink);
            auto post = [&](const wchar_t* path, const std::string& body) { return router.RouteRequest(L"POST", path, body); };

            post(L"/graph/apply", R"({"nodes":[{"ref":"a","effect":"Gaussian Blur"},{"ref":"b","effect":"Gaussian Blur"}]})");
            const uint32_t a = sink.graph.Nodes()[0].id, b = sink.graph.Nodes()[1].id;

            // Ids may arrive as strings, like any untyped MCP argument.
            auto connected = post(L"/graph/connect", std::format(
                R"({{"srcId":"{}","srcPin":"0","dstId":{},"dstPin":0}})", a, b));
            TEST("Route_StringifiedIdsAccepted", connected.statusCode == 200 &&
                 connected.body == R"({"connected":true})" && sink.graph.Edges().size() == 1);
            auto badId = post(L"/graph/connect", R"({"srcId":"one","srcPin":0,"dstId":2,"dstPin":0})");
            TEST("Route_BadIdNamesTheField", badId.statusCode == 400 &&
                 StringMember(badId, L"error") == L"srcId must be a number" && StringMember(badId, L"code") == L"bad_argument");
            auto missing = post(L"/graph/disconnect", R"({"srcId":1})");
            TEST("Route_MissingArgNamed", missing.statusCode == 400 &&
                 StringMember(missing, L"error") == L"srcPin is required");

            // A fault after the arguments were accepted is the server's, not the client's.
            sink.throwOnNodeChanged = true;
            auto fault = post(L"/graph/set-property", std::format(R"({{"nodeId":{},"key":"StandardDeviation","value":2}})", a));
            sink.throwOnNodeChanged = false;
            TEST("Route_ServerFaultIs500", fault.statusCode == 500 &&
                 StringMember(fault, L"error") == L"host \"hook\" failed" && StringMember(fault, L"code") == L"internal");

            router.AddRoute(L"GET", L"/test/hresult",
                [](const std::wstring&, const std::wstring&, const std::string&) -> Mcp::Response
                {
                    throw winrt::hresult_error(E_ACCESSDENIED, L"no \"access\"");
                });
            auto hresult = router.RouteRequest(L"GET", L"/test/hresult", "");
            TEST("Router_HresultErrorKeepsMessage", hresult.statusCode == 500 &&
                 StringMember(hresult, L"error").find(L"no \"access\"") != std::wstring::npos &&
                 StringMember(hresult, L"error").find(L"0x80070005") != std::wstring::npos);

            auto badPath = router.RouteRequest(L"GET", L"/graph/node/12abc", "");
            TEST("Route_PathIdStrict", badPath.statusCode == 400);
            auto context = router.RouteRequest(L"GET", L"/context", "");
            TEST("Route_ContextServedByEngine", context.statusCode == 200 && ParseBody(context).has_value() &&
                 context.body.find("\"endpoints\"") == std::string::npos &&
                 context.body.find("BuiltInEffect") != std::string::npos);
        }

        SL_TEST_CASE(TestApplyAllOrNothing, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== /graph/apply all or nothing ===\n");
            GraphOnlySink sink;
            McpRouter router;
            Mcp::RegisterEngineRoutes(router, sink);
            auto apply = [&](const std::string& body) { return router.RouteRequest(L"POST", L"/graph/apply", body); };

            auto built = apply(R"({
                "nodes": [ { "ref": "clock", "effect": "Clock" },
                           { "ref": "expr", "effect": "Numeric Expression" },
                           { "ref": "a", "effect": "Gaussian Blur" },
                           { "ref": "b", "effect": "Gaussian Blur" },
                           { "ref": "c", "effect": "Gaussian Blur" } ],
                "edges": [ { "from": "a", "to": "c" } ],
                "bindings": [ { "node": "expr", "property": "A", "from": "clock.Time" } ] })");
            TEST("Apply_BuildsGraph", built.statusCode == 200 && sink.graph.Nodes().size() == 5 &&
                 sink.graph.Edges().size() == 1 && sink.nodesAdded == 5);
            const uint32_t clockId = NodeIdNamed(sink.graph, L"Clock");
            const uint32_t exprId = NodeIdNamed(sink.graph, L"Numeric Expression");
            const auto& nodes = sink.graph.Nodes();
            const uint32_t a = nodes[2].id, b = nodes[3].id, c = nodes[4].id;
            const auto edgesBefore = sink.graph.Edges();
            const auto bindingsBefore = sink.graph.FindNode(exprId)->propertyBindings;
            const size_t nodeCount = nodes.size();
            const int addedBefore = sink.nodesAdded;

            // Adds a node, replaces c's input edge, rebinds expr.A, then fails.
            auto failed = apply(std::format(R"({{
                "nodes": [ {{ "ref": "extra", "effect": "Gaussian Blur" }} ],
                "edges": [ {{ "from": {}, "to": {} }}, {{ "from": "extra", "to": {}, "toPin": 0 }} ],
                "bindings": [ {{ "node": {}, "property": "A", "from": "{}.Progress" }},
                              {{ "node": {}, "property": "A", "from": "nosuch.Time" }} ] }})",
                b, c, a, exprId, clockId, exprId));
            TEST("Apply_FailureReported", failed.statusCode == 400 &&
                 StringMember(failed, L"error").find(L"nosuch") != std::wstring::npos);
            TEST("Apply_FailureRemovesNewNodes", sink.graph.Nodes().size() == nodeCount && sink.nodesAdded == addedBefore);
            const auto& edgesAfter = sink.graph.Edges();
            TEST("Apply_FailureRestoresEdges", edgesAfter.size() == edgesBefore.size() &&
                 std::ranges::all_of(edgesBefore, [&](const auto& e) { return std::ranges::find(edgesAfter, e) != edgesAfter.end(); }));
            const auto* expr = sink.graph.FindNode(exprId);
            TEST("Apply_FailureRestoresBindings", expr && expr->propertyBindings.size() == bindingsBefore.size() &&
                 expr->propertyBindings.at(L"A").sources.size() == 1 &&
                 expr->propertyBindings.at(L"A").sources[0]->sourceFieldName == L"Time");

            // A parse error touches nothing.
            auto parseError = apply(R"({"nodes":[{"effect":"Gaussian Blur"}],"edges":[{"from":"x"}]})");
            TEST("Apply_ParseErrorTouchesNothing", parseError.statusCode == 400 && sink.graph.Nodes().size() == nodeCount);

            // A node id written as a string works wherever a ref does.
            auto numeric = apply(std::format(R"({{
                "edges": [ {{ "from": "{}", "to": "{}" }} ],
                "bindings": [ {{ "node": "{}", "property": "A", "from": "{}.Progress" }} ] }})",
                b, a, exprId, clockId));
            TEST("Apply_NumericStringRefs", numeric.statusCode == 200 &&
                 std::ranges::find(sink.graph.Edges(), Graph::EffectEdge{ b, 0, a, 0 }) != sink.graph.Edges().end() &&
                 sink.graph.FindNode(exprId)->propertyBindings.at(L"A").sources[0]->sourceFieldName == L"Progress");
        }

        SL_TEST_CASE(TestNodeEditRoutes, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== MCP duplicate, upgrade and expression-input routes ===\n");
            GraphOnlySink sink;
            McpRouter router;
            Mcp::RegisterEngineRoutes(router, sink);
            auto post = [&](const wchar_t* path, const std::string& body) { return router.RouteRequest(L"POST", path, body); };

            post(L"/graph/apply", R"({
                "nodes": [ { "ref": "clock", "effect": "Clock" },
                           { "ref": "expr", "effect": "Numeric Expression" },
                           { "ref": "a", "effect": "Gaussian Blur" },
                           { "ref": "b", "effect": "Gaussian Blur" },
                           { "ref": "stats", "effect": "Luminance Statistics" } ],
                "edges": [ { "from": "a", "to": "b" } ],
                "bindings": [ { "node": "expr", "property": "A", "from": "clock.Time" } ] })");
            const uint32_t clockId = NodeIdNamed(sink.graph, L"Clock");
            const uint32_t exprId = NodeIdNamed(sink.graph, L"Numeric Expression");
            const uint32_t statsId = NodeIdNamed(sink.graph, L"Luminance Statistics");
            const uint32_t a = sink.graph.Nodes()[2].id, b = sink.graph.Nodes()[3].id;

            // Duplicate: the edge between copies is copied; a binding to a
            // copied source reads the copy, one to an outside source the original.
            const size_t nodesBefore = sink.graph.Nodes().size();
            auto dup = post(L"/graph/duplicate-nodes", std::format(R"({{"nodeIds":[{},"{}",{},{}]}})", a, b, a, exprId));
            const auto dupBody = ParseBody(dup);
            const bool dupOk = dup.statusCode == 200 && dupBody && dupBody->GetNamedArray(L"nodeIds").Size() == 3;
            TEST("Duplicate_CopiesEachIdOnce", dupOk && sink.graph.Nodes().size() == nodesBefore + 3);
            if (dupOk)
            {
                const auto idMap = dupBody->GetNamedObject(L"idMap");
                const auto newId = [&](uint32_t oldId) { return static_cast<uint32_t>(idMap.GetNamedNumber(winrt::to_hstring(oldId))); };
                TEST("Duplicate_CopiesInnerEdge",
                     std::ranges::find(sink.graph.Edges(), Graph::EffectEdge{ newId(a), 0, newId(b), 0 }) != sink.graph.Edges().end());
                const auto* exprCopy = sink.graph.FindNode(newId(exprId));
                TEST("Duplicate_OutsideBindingReadsOriginal", exprCopy &&
                     exprCopy->propertyBindings.at(L"A").sources[0]->sourceNodeId == clockId);
            }
            auto dupClockToo = post(L"/graph/duplicate-nodes", std::format(R"({{"nodeIds":[{},{}]}})", clockId, exprId));
            if (const auto body = ParseBody(dupClockToo); dupClockToo.statusCode == 200 && body)
            {
                const auto idMap = body->GetNamedObject(L"idMap");
                const auto* exprCopy = sink.graph.FindNode(static_cast<uint32_t>(idMap.GetNamedNumber(winrt::to_hstring(exprId))));
                TEST("Duplicate_InnerBindingReadsCopy", exprCopy &&
                     exprCopy->propertyBindings.at(L"A").sources[0]->sourceNodeId ==
                         static_cast<uint32_t>(idMap.GetNamedNumber(winrt::to_hstring(clockId))));
            }
            else
            {
                TEST("Duplicate_InnerBindingReadsCopy", false);
            }
            const size_t nodesNow = sink.graph.Nodes().size();
            auto dupMissing = post(L"/graph/duplicate-nodes", std::format(R"({{"nodeIds":[{},999999]}})", a));
            TEST("Duplicate_MissingIdAddsNothing", dupMissing.statusCode == 404 && sink.graph.Nodes().size() == nodesNow);
            TEST("Duplicate_BadIdIs400", post(L"/graph/duplicate-nodes", R"({"nodeIds":[1.5]})").statusCode == 400 &&
                 post(L"/graph/duplicate-nodes", R"({"nodeIds":[]})").statusCode == 400);

            // Upgrade: a current node is not upgraded; an outdated one is,
            // keeping a property value that still exists.
            auto current = post(L"/graph/upgrade-nodes", std::format(R"({{"nodeId":{}}})", statsId));
            TEST("Upgrade_CurrentNodeUntouched", current.statusCode == 200 && current.body == R"({"upgraded":[]})");
            auto* stats = sink.graph.FindNode(statsId);
            const uint32_t currentVersion = stats->customEffect->shaderLabEffectVersion;
            stats->customEffect->shaderLabEffectVersion = 1;
            ShaderLab::Graph::SetNodeProperty(*stats, L"ClipNits", 123.0f);
            auto upgraded = post(L"/graph/upgrade-nodes", "{}");
            stats = sink.graph.FindNode(statsId);
            TEST("Upgrade_AllUpgradesOutdated", upgraded.statusCode == 200 &&
                 upgraded.body == std::format(R"({{"upgraded":[{}]}})", statsId) &&
                 stats->customEffect->shaderLabEffectVersion == currentVersion &&
                 std::get<float>(stats->properties.at(L"ClipNits")) == 123.0f);
            TEST("Upgrade_UnknownNodeIs404", post(L"/graph/upgrade-nodes", R"({"nodeId":999999})").statusCode == 404);

            // Expression inputs: add the next free letter, remove one, and
            // refuse the last input and non-expression nodes.
            auto added = post(L"/graph/expression-input", std::format(R"({{"nodeId":{},"action":"add"}})", exprId));
            TEST("ExprInput_AddsNextLetter", added.statusCode == 200 && StringMember(added, L"name") == L"B" &&
                 sink.graph.FindNode(exprId)->properties.contains(L"B"));
            auto removed = post(L"/graph/expression-input", std::format(R"({{"nodeId":{},"action":"remove","name":"A"}})", exprId));
            const auto* expr = sink.graph.FindNode(exprId);
            TEST("ExprInput_RemoveDropsBinding", removed.statusCode == 200 && !expr->properties.contains(L"A") &&
                 !expr->propertyBindings.contains(L"A"));
            TEST("ExprInput_LastInputStays",
                 post(L"/graph/expression-input", std::format(R"({{"nodeId":{},"action":"remove","name":"B"}})", exprId)).statusCode == 400 &&
                 sink.graph.FindNode(exprId)->properties.contains(L"B"));
            TEST("ExprInput_RefusesOtherNodes",
                 post(L"/graph/expression-input", std::format(R"({{"nodeId":{},"action":"add"}})", a)).statusCode == 400 &&
                 post(L"/graph/expression-input", std::format(R"({{"nodeId":{},"action":"rename"}})", exprId)).statusCode == 400);
        }

        SL_TEST_CASE(TestCatalogAndRpcArgs, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== MCP catalog and tool arguments ===\n");
            bool namesMatch = true;
            for (const auto& tool : Mcp::ToolCatalog())
            {
                WDJ::JsonObject entry{ nullptr };
                if (!WDJ::JsonObject::TryParse(winrt::to_hstring(tool.listJson), entry) ||
                    WideToUtf8(std::wstring(entry.GetNamedString(L"name", L""))) != tool.name ||
                    !entry.HasKey(L"inputSchema"))
                {
                    printf("  catalog row '%s' does not match its listJson\n", tool.name);
                    namesMatch = false;
                }
            }
            TEST("Catalog_ListJsonNameMatchesRow", namesMatch);

            GraphOnlySink sink;
            McpRouter router;
            Mcp::RegisterEngineRoutes(router, sink);
            Mcp::JsonRpcOptions options;
            options.hostKind = "headless";
            Mcp::RegisterJsonRpcEndpoint(router, std::move(options));
            router.RouteRequest(L"POST", L"/graph/add-node", R"({"effectName":"Gaussian Blur"})");
            const uint32_t id = sink.graph.Nodes()[0].id;

            auto call = [&](const std::string& arguments)
            {
                return router.RouteRequest(L"POST", L"/", std::format(
                    R"({{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{{"name":"graph_get_node","arguments":{}}}}})", arguments));
            };
            auto stringId = call(std::format(R"({{"nodeId":"{}"}})", id));
            TEST("Rpc_PathNumberAcceptsString", stringId.body.find("\"isError\":false") != std::string::npos &&
                 stringId.body.find("Gaussian Blur") != std::string::npos);
            auto badId = call(R"({"nodeId":"seven"})");
            TEST("Rpc_PathNumberBadArgIsToolError", badId.body.find("\"isError\":true") != std::string::npos &&
                 badId.body.find("nodeId must be a number") != std::string::npos &&
                 badId.body.find("-32603") == std::string::npos);

            auto registry = router.RouteRequest(L"POST", L"/",
                R"({"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"shaderlab://registry/effects"}})");
            TEST("Rpc_RegistryResourceReads", registry.body.find("Gaussian Blur") != std::string::npos);
        }

        // A client with no hub reports NoHub, and Stop() ends its reconnect
        // wait at once rather than after the backoff.
        SL_TEST_CASE(TestSessionClientStatus, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== MCP session client status ===\n");
            McpRouter router;
            Mcp::SessionClientOptions options;
            options.pipeBaseName = L"ShaderLab.mcp.unittest.status." + std::to_wstring(::GetCurrentProcessId());
            options.sessionId = L"{00000000-0000-0000-0000-0000000STATS}";
            Mcp::McpSessionClient client(router, options);
            TEST("Session_StoppedBeforeRun", client.Status().state == Mcp::SessionState::Stopped);

            std::thread runner([&client] { client.Run(); });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (client.Status().state != Mcp::SessionState::NoHub && std::chrono::steady_clock::now() < deadline)
                ::Sleep(10);
            const auto status = client.Status();
            TEST("Session_NoHubReported", status.state == Mcp::SessionState::NoHub &&
                 status.detail.find(options.pipeBaseName) != std::wstring::npos);

            // Let the backoff grow past a second, then stop inside a wait.
            ::Sleep(1800);
            const auto stopAt = std::chrono::steady_clock::now();
            client.Stop();
            runner.join();
            const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stopAt).count();
            printf("  [info] Stop() to Run() returning: %lld ms\n", static_cast<long long>(stopMs));
            TEST("Session_StopEndsBackoff", stopMs < 500);
            TEST("Session_StoppedAfterRun", client.Status().state == Mcp::SessionState::Stopped);
        }

        // POST /render/video is opt-in (headless registers it) and reads every
        // argument before anything is dispatched. No export runs here.
        SL_TEST_CASE(TestVideoExportRouteArguments, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== /render/video arguments ===\n");
            GraphOnlySink sink;
            McpRouter engineOnly;
            Mcp::RegisterEngineRoutes(engineOnly, sink);
            TEST("Video_NotAnEngineRoute", engineOnly.RouteRequest(L"POST", L"/render/video", "{}").statusCode == 404);

            McpRouter router;
            Mcp::RegisterEngineRoutes(router, sink);
            Mcp::RegisterVideoExportRoute(router, sink);
            auto post = [&](const std::string& body) { return router.RouteRequest(L"POST", L"/render/video", body); };

            auto notObject = post("[1]");
            TEST("Video_BodyMustBeObject", notObject.statusCode == 400 &&
                 StringMember(notObject, L"error") == L"body must be a JSON object");
            auto missing = post(R"({"nodeId":1})");
            TEST("Video_OutputPathRequired", missing.statusCode == 400 &&
                 StringMember(missing, L"error") == L"nodeId and outputPath are required");
            auto badCodec = post(R"({"nodeId":"1","outputPath":"x.mp4","codec":"vp9"})");
            TEST("Video_StringIdAcceptedCodecNamed", badCodec.statusCode == 400 &&
                 StringMember(badCodec, L"error").find(L"hevc, av1 or h264") != std::wstring::npos);
            auto badFps = post(R"({"nodeId":1,"outputPath":"x.mp4","fps":"fast"})");
            TEST("Video_BadNumberNamed", badFps.statusCode == 400 &&
                 StringMember(badFps, L"error") == L"fps must be a number");
        }
    }
}
