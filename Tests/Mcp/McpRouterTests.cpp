#include "pch_engine.h"
#include "../TestSupport.h"
#include "Engine/Mcp/EngineMcpRoutes.h"
#include "Engine/Mcp/McpJsonRpc.h"
#include "Engine/Mcp/McpRouter.h"
#include "Engine/Mcp/McpToolCatalog.h"
#include "Engine/Utf8.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

// The MCP router, JSON-RPC dispatch and engine routes on a graph-only sink.
namespace
{
    using namespace ShaderLab::Tests;

    // McpRouter owns the query split, so ?since= on /node/{id}/logs reaches the
    // handler. Pins the (path, query, body) handler contract, HasRoute and the
    // Response noReply discriminator.
    SL_TEST_CASE(TestMcpRouter, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== McpRouter ===\n");

        ShaderLab::McpRouter router;
        std::wstring gotPath, gotQuery;
        int calls = 0;
        router.AddRoute(L"GET", L"/probe/",
            [&](const std::wstring& path, const std::wstring& query, const std::string&)
                -> ShaderLab::Mcp::Response {
                ++calls; gotPath = path; gotQuery = query;
                return { 200, "{}" };
            });
        // Catch-all, to prove longest-prefix still wins when a query is present.
        router.AddRoute(L"GET", L"/",
            [&](const std::wstring&, const std::wstring&, const std::string&)
                -> ShaderLab::Mcp::Response {
                return { 200, R"({"catchall":true})" };
            });

        auto r1 = router.RouteRequest(L"GET", L"/probe/7/logs?since=3", "");
        TEST("Router_MatchIgnoresQuery", r1.statusCode == 200 && calls == 1);
        TEST("Router_QueryReachesHandler", gotQuery == L"since=3");
        TEST("Router_PathStrippedForHandler", gotPath == L"/probe/7/logs");

        auto r2 = router.RouteRequest(L"GET", L"/probe/7/logs", "");
        TEST("Router_EmptyQueryIsEmpty", r2.statusCode == 200 && gotQuery.empty());

        auto r3 = router.RouteRequest(L"GET", L"/elsewhere", "");
        TEST("Router_CatchAllStillMatches",
            r3.statusCode == 200 && r3.body.find("catchall") != std::string::npos);

        TEST("Router_HasRouteIgnoresQuery", router.HasRoute(L"GET", L"/probe/x?y=1"));
        TEST("Router_HasRouteMethodMiss", !router.HasRoute(L"POST", L"/probe/x"));
        TEST("Router_HasSpecificRouteTrue", router.HasSpecificRoute(L"GET", L"/probe/x?y=1"));
        TEST("Router_HasSpecificRouteExcludesCatchAll", !router.HasSpecificRoute(L"GET", L"/elsewhere"));

        ShaderLab::McpRouter bare;
        TEST("Router_NoMatch404", bare.RouteRequest(L"GET", L"/nope", "").statusCode == 404);

        // A handler's exception text is escaped into the error body.
        router.AddRoute(L"GET", L"/throws",
            [](const std::wstring&, const std::wstring&, const std::string&) -> ShaderLab::Mcp::Response {
                throw std::runtime_error("bad \"quoted\" \\ input");
            });
        auto thrown = router.RouteRequest(L"GET", L"/throws", "");
        winrt::Windows::Data::Json::JsonObject thrownBody{ nullptr };
        TEST("Router_ExceptionBodyIsJson", thrown.statusCode == 500 &&
            winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(thrown.body), thrownBody) &&
            std::wstring(thrownBody.GetNamedString(L"error")) == L"bad \"quoted\" \\ input");

        auto none = ShaderLab::Mcp::Response::None();
        TEST("Router_ResponseNoneIsNoReply", none.noReply && none.statusCode == 202);
        TEST("Router_ResponseDefaultReplies", ShaderLab::Mcp::Response{}.noReply == false);
    }

    // McpJsonRpc over the route registry, driven through RouteRequest with no
    // HTTP: single-line messages, id echo on every error path, notifications
    // keyed off an absent id, batch rejection, guarded params, and a tool error
    // for an absent route.
    SL_TEST_CASE(TestMcpJsonRpc, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== McpJsonRpc dispatcher ===\n");

        ShaderLab::McpRouter router;
        router.AddRoute(L"GET", L"/graph/overview",
            [](const std::wstring&, const std::wstring&, const std::string&) -> ShaderLab::Mcp::Response {
                return { 200, R"({"previewNodeId":0,"nodes":[],"edges":[]})" };
            });
        ShaderLab::Mcp::JsonRpcOptions opts;
        opts.hostKind = "headless";
        ShaderLab::Mcp::RegisterJsonRpcEndpoint(router, std::move(opts));

        auto post = [&](const std::string& body) {
            return router.RouteRequest(L"POST", L"/", body);
        };

        auto init = post(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})");
        TEST("Rpc_InitializeSingleLine", init.body.find('\n') == std::string::npos);
        TEST("Rpc_InitializeVersion2025_06_18", init.body.find("\"2025-06-18\"") != std::string::npos);
        TEST("Rpc_InitializeHostKind", init.body.find("shaderlab-headless") != std::string::npos);

        auto tl = post(R"({"jsonrpc":"2.0","id":"abc","method":"tools/list"})");
        TEST("Rpc_ToolsListSingleLine", tl.body.find('\n') == std::string::npos);
        TEST("Rpc_ToolsListEchoesStringId", tl.body.find("\"id\":\"abc\"") != std::string::npos);
        // tools/list is filtered by route presence, the same predicate tools/call
        // enforces -- this fixture registers ONLY GET /graph/overview, so that is
        // the one tool it may advertise. Before the filter the whole 39-entry
        // catalog was emitted regardless of host, so a headless session advertised
        // the GUI-only tools and then refused them on call.
        TEST("Rpc_ToolsListHasCatalog", tl.body.find("\"graph_overview\"") != std::string::npos);
        TEST("Rpc_ToolsListOmitsUnroutedTool", tl.body.find("\"graph_add_node\"") == std::string::npos);

        auto noid = post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        TEST("Rpc_NotificationNoReply", noid.noReply && noid.body.empty());

        auto missing = post(R"({"jsonrpc":"2.0","id":7})");
        TEST("Rpc_MissingMethodKeepsId", missing.body.find("\"id\":7") != std::string::npos);

        auto batch = post(R"([{"jsonrpc":"2.0","id":9,"method":"ping"}])");
        TEST("Rpc_BatchRejected", batch.body.find("-32600") != std::string::npos);

        auto call = post(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"graph_overview","arguments":{}}})");
        TEST("Rpc_ToolCallRoutes", call.body.find("previewNodeId") != std::string::npos
            && call.body.find("\"isError\":false") != std::string::npos);

        auto absent = post(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"graph_snapshot","arguments":{}}})");
        TEST("Rpc_AbsentRouteToolErrors", absent.body.find("\"isError\":true") != std::string::npos
            && absent.body.find("not available") != std::string::npos);

        auto unk = post(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"nope","arguments":{}}})");
        TEST("Rpc_UnknownToolErrors", unk.body.find("\"isError\":true") != std::string::npos);

        auto arrParams = post(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":[1,2]})");
        TEST("Rpc_ArrayParamsGuarded", arrParams.body.find("-32602") != std::string::npos
            && arrParams.body.find("\"id\":5") != std::string::npos);

        TEST("Rpc_EscaperControlChars",   // literal split: \x is greedy, \x01b would be ESC
            ShaderLab::Mcp::JsonEscape("a\x01" "b\nc") == "a\\u0001b\\nc");

        // Adding or removing an MCP tool changes this; update the tool tables
        // in docs/ with it.
        constexpr size_t cExpectedToolCount = 50;
        TEST_EQ("Rpc_CatalogCount", ShaderLab::Mcp::ToolCatalog().size(), cExpectedToolCount);
        bool catalogSingleLine = true;
        for (const auto& t : ShaderLab::Mcp::ToolCatalog())
            if (std::string_view(t.listJson).find('\n') != std::string_view::npos)
                catalogSingleLine = false;
        TEST("Rpc_CatalogEntriesSingleLine", catalogSingleLine);
    }

    // /graph/apply with per-node properties, run through the real engine routes
    // on a graph-only sink (no device, so sources are not prepared).
    SL_TEST_CASE(TestGraphApplyProperties, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== /graph/apply properties ===\n");

        struct GraphOnlySink : ShaderLab::Mcp::IEngineCommandSink
        {
            ShaderLab::Graph::GraphModel* graph{ nullptr };
            ShaderLab::Mcp::Response Dispatch(
                std::function<ShaderLab::Mcp::Response(ShaderLab::Mcp::EngineContext&)> closure) override
            {
                ShaderLab::Mcp::EngineContext context;
                context.graph = graph;
                return closure(context);
            }
        };

        ShaderLab::Graph::GraphModel graph;
        GraphOnlySink sink;
        sink.graph = &graph;
        ShaderLab::McpRouter router;
        ShaderLab::Mcp::RegisterEngineRoutes(router, sink);

        auto applied = router.RouteRequest(L"POST", L"/graph/apply", R"({
            "nodes": [
                { "ref": "clock", "effect": "Clock", "properties": { "StartTime": 10, "StopTime": 180 } },
                { "ref": "split", "effect": "Split Comparison", "properties": { "OutputW": 3840, "Rotation": "45" } },
                { "ref": "video", "effect": "Video", "properties": { "Loop": "true" } }
            ],
            "edges": [ { "from": "video", "to": "split", "toPin": 0 } ]
        })");
        TEST("GraphApply_PropertiesSucceed", applied.statusCode == 200);
        TEST("GraphApply_AddsEveryNode", graph.Nodes().size() == 3);

        auto floatProperty = [&](const wchar_t* nodeName, const wchar_t* key) -> float
        {
            for (const auto& node : graph.Nodes())
            {
                if (node.name != nodeName) continue;
                auto it = node.properties.find(key);
                if (it == node.properties.end()) return -1.0f;
                if (auto* value = std::get_if<float>(&it->second)) return *value;
            }
            return -1.0f;
        };
        TEST("GraphApply_NumberWritten", floatProperty(L"Clock", L"StopTime") == 180.0f);
        TEST("GraphApply_StringCoercedToFloat", floatProperty(L"Split Comparison", L"Rotation") == 45.0f);

        // A value that cannot be coerced to the property's type is a 400, not a crash.
        auto rejected = router.RouteRequest(L"POST", L"/graph/apply", R"({
            "nodes": [ { "effect": "Video", "properties": { "Loop": "maybe" } } ]
        })");
        TEST("GraphApply_BadBoolRejected", rejected.statusCode == 400 &&
            rejected.body.find("boolean") != std::string::npos);
        TEST("GraphApply_RejectedNodeNotAdded", graph.Nodes().size() == 3);

        // Error text that carries user input is escaped, so the body stays JSON.
        auto errorText = [](const ShaderLab::Mcp::Response& response) -> std::wstring
        {
            winrt::Windows::Data::Json::JsonObject parsed{ nullptr };
            if (!winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(response.body), parsed) ||
                !parsed.HasKey(L"error"))
                return L"(unparseable)";
            return std::wstring(parsed.GetNamedString(L"error"));
        };
        auto badEffect = router.RouteRequest(L"POST", L"/graph/apply",
            R"({ "nodes": [ { "effect": "No \"Such\" \\ Effect" } ] })");
        TEST("GraphApply_UnknownEffectErrorIsJson", badEffect.statusCode == 400 &&
            errorText(badEffect).find(L"No \"Such\" \\ Effect") != std::wstring::npos);
        auto badRef = router.RouteRequest(L"POST", L"/graph/apply",
            R"({ "edges": [ { "from": "a\"b", "to": 1 } ] })");
        TEST("GraphApply_UnknownRefErrorIsJson", badRef.statusCode == 400 &&
            errorText(badRef).find(L"a\"b") != std::wstring::npos);

        // A readback that fails while something upstream is compiling says so.
        uint32_t splitId = 0, videoId = 0;
        for (const auto& node : graph.Nodes())
        {
            if (node.name == L"Split Comparison") splitId = node.id;
            if (node.type == ShaderLab::Graph::NodeType::Source) videoId = node.id;
        }
        const std::string regionBody = "{\"nodeId\":" + std::to_string(splitId) + ",\"x\":0,\"y\":0,\"w\":1,\"h\":1}";
        const std::string captureBody = "{\"nodeId\":" + std::to_string(splitId) + "}";

        auto notCompiling = router.RouteRequest(L"POST", L"/render/pixel-region", regionBody);
        TEST("ReadWhileCompiling_PlainNotReadyWithoutCompile", notCompiling.statusCode != 200 &&
            notCompiling.body.find("compiling") == std::string::npos);

        graph.FindNode(videoId)->compilePending = true;
        auto upstream = router.RouteRequest(L"POST", L"/render/pixel-region", regionBody);
        TEST("ReadWhileCompiling_UpstreamReported", upstream.statusCode == 409 &&
            upstream.body.find("\"compiling\":[\"") != std::string::npos &&
            upstream.body.find("retry") != std::string::npos);
        auto capture = router.RouteRequest(L"POST", L"/render/capture-node", captureBody);
        TEST("ReadWhileCompiling_CaptureReported", capture.statusCode == 409 &&
            capture.body.find("\"compiling\"") != std::string::npos);
        graph.FindNode(videoId)->compilePending = false;
    }

    // /graph/load-file and /graph/save-file through the real engine routes on a
    // graph-only sink: bare JSON, a package with embedded media, the media
    // directory hand-off, and the 400s.
    SL_TEST_CASE(TestGraphLoadSaveFile, Tag::Fast | Tag::Mcp)
    {
        printf("\n=== /graph/load-file + /graph/save-file ===\n");
        namespace fs = std::filesystem;

        // Deletes the previous media directory on each change, as the hosts do.
        struct FileSink : ShaderLab::Mcp::IEngineCommandSink
        {
            ShaderLab::Graph::GraphModel* graph{ nullptr };
            ShaderLab::Rendering::GraphEvaluator* evaluator{ nullptr };
            std::wstring mediaDir;
            int loads{ 0 };
            ShaderLab::Mcp::Response Dispatch(
                std::function<ShaderLab::Mcp::Response(ShaderLab::Mcp::EngineContext&)> closure) override
            {
                ShaderLab::Mcp::EngineContext context;
                context.graph = graph;
                context.evaluator = evaluator;
                return closure(context);
            }
            void OnGraphLoaded() override { ++loads; }
            void OnGraphMediaDirChanged(const std::wstring& extractDir) override
            {
                std::error_code ec;
                if (!mediaDir.empty() && mediaDir != extractDir) fs::remove_all(mediaDir, ec);
                mediaDir = extractDir;
            }
        };

        ShaderLab::Graph::GraphModel graph;
        ShaderLab::Rendering::GraphEvaluator evaluator;
        FileSink sink;
        sink.graph = &graph;
        sink.evaluator = &evaluator;
        ShaderLab::McpRouter router;
        ShaderLab::Mcp::RegisterEngineRoutes(router, sink);

        const fs::path dir = fs::temp_directory_path() / std::format(L"ShaderLabFileRouteTest-{}", ::GetCurrentProcessId());
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        auto readFile = [](const fs::path& path) {
            std::ifstream file(path, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        };
        auto writeFile = [](const fs::path& path, const std::string& bytes) {
            std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
        };
        auto pathBody = [](const fs::path& path, const char* extra = "") {
            return std::format(R"({{"path":"{}"{}}})", ShaderLab::Mcp::JsonEscape(ShaderLab::Mcp::WideToUtf8(path.wstring())), extra);
        };
        auto post = [&](const wchar_t* route, const std::string& body) { return router.RouteRequest(L"POST", route, body); };

        // Bare JSON fixture.
        const std::wstring fixture = FindFixture(L"test_cli_basic.json");
        if (fixture.empty())
        {
            SkipMissingFixture(L"test_cli_basic.json");
            return;
        }
        auto loadedJson = post(L"/graph/load-file", pathBody(fixture));
        TEST("LoadFile_BareJsonLoads", loadedJson.statusCode == 200 && loadedJson.body.find("\"format\":\"json\"") != std::string::npos);
        TEST("LoadFile_ReportsNodeCount", !graph.Nodes().empty() &&
            loadedJson.body.find(std::format("\"nodeCount\":{}", graph.Nodes().size())) != std::string::npos);
        TEST("LoadFile_FiresGraphLoaded", sink.loads == 1 && sink.mediaDir.empty());

        // A package with one embedded media file.
        std::string mediaBytes(256 * 1024, '\0');
        uint32_t rngState = 0x2545F491u;
        for (auto& value : mediaBytes)
        {
            rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
            value = static_cast<char>(rngState);
        }
        const fs::path mediaPath = dir / L"clip.png";
        writeFile(mediaPath, mediaBytes);
        auto addSource = post(L"/graph/add-node", std::format(R"({{"effectName":"Image","filePath":"{}"}})",
            ShaderLab::Mcp::JsonEscape(ShaderLab::Mcp::WideToUtf8(mediaPath.wstring()))));
        TEST("SaveFile_SourceAdded", addSource.statusCode == 200);
        const size_t nodeCount = graph.Nodes().size();

        const fs::path package = dir / L"round trip.effectgraph";
        auto saved = post(L"/graph/save-file", pathBody(package));
        const auto packageBytes = readFile(package);
        TEST("SaveFile_PackageWritten", saved.statusCode == 200 && saved.body.find("\"format\":\"package\"") != std::string::npos
            && saved.body.find("\"mediaEmbedded\":1") != std::string::npos
            && packageBytes.size() > mediaBytes.size() && packageBytes[0] == 'P' && packageBytes[1] == 'K');
        auto resaved = post(L"/graph/save-file", pathBody(package));
        TEST("SaveFile_UnchangedMediaReused", resaved.statusCode == 200 && resaved.body.find("\"mediaUnchanged\":1") != std::string::npos);

        const fs::path bareJson = dir / L"graph.json";
        auto savedJson = post(L"/graph/save-file", pathBody(bareJson));
        const auto jsonBytes = readFile(bareJson);
        TEST("SaveFile_BareJsonWritten", savedJson.statusCode == 200 && savedJson.body.find("\"format\":\"json\"") != std::string::npos
            && !jsonBytes.empty() && jsonBytes[0] == '{');

        // Load the package back: the source must point at an extracted copy of the media.
        post(L"/graph/clear", "");
        auto loadedPackage = post(L"/graph/load-file", pathBody(package));
        std::wstring sourcePath;
        for (const auto& node : graph.Nodes())
            if (node.type == ShaderLab::Graph::NodeType::Source && node.mediaPath.has_value())
                sourcePath = *node.mediaPath;
        const std::wstring extractDir = sink.mediaDir;
        TEST("LoadFile_PackageLoads", loadedPackage.statusCode == 200 && loadedPackage.body.find("\"format\":\"package\"") != std::string::npos
            && loadedPackage.body.find("\"mediaFiles\":1") != std::string::npos && graph.Nodes().size() == nodeCount);
        TEST("LoadFile_SourcePointsAtExtractedMedia", !extractDir.empty() && sourcePath.starts_with(extractDir)
            && fs::exists(sourcePath) && readFile(sourcePath) == std::vector<uint8_t>(mediaBytes.begin(), mediaBytes.end()));

        // Replacing the graph hands over an empty directory, so the extracted one goes.
        auto reloadedJson = post(L"/graph/load-file", pathBody(bareJson));
        TEST("LoadFile_ReplacingGraphReleasesMedia", reloadedJson.statusCode == 200 && sink.mediaDir.empty() && !fs::exists(extractDir));

        // Errors leave the graph alone.
        writeFile(dir / L"garbage.json", "this is not a graph");
        writeFile(dir / L"garbage.effectgraph", std::string("PK\x03\x04", 4) + "truncated");
        const int loadsBefore = sink.loads;
        auto missing = post(L"/graph/load-file", pathBody(dir / L"missing.effectgraph"));
        auto garbage = post(L"/graph/load-file", pathBody(dir / L"garbage.json"));
        auto badZip = post(L"/graph/load-file", pathBody(dir / L"garbage.effectgraph"));
        auto relative = post(L"/graph/load-file", R"({"path":"relative\\graph.json"})");
        auto noPath = post(L"/graph/load-file", "{}");
        TEST("LoadFile_MissingFile400", missing.statusCode == 400 && missing.body.find("not found") != std::string::npos);
        TEST("LoadFile_GarbageFile400", garbage.statusCode == 400 && garbage.body.find("not a valid graph") != std::string::npos);
        TEST("LoadFile_BadPackage400", badZip.statusCode == 400 && badZip.body.find("package") != std::string::npos);
        TEST("LoadFile_RelativeAndMissingPath400", relative.statusCode == 400 && noPath.statusCode == 400);
        TEST("LoadFile_ErrorsKeepGraph", sink.loads == loadsBefore && graph.Nodes().size() == nodeCount);

        auto embedIntoJson = post(L"/graph/save-file", pathBody(dir / L"x.json", R"(,"embedMedia":"true")"));
        auto noFolder = post(L"/graph/save-file", pathBody(dir / L"no such folder" / L"x.effectgraph"));
        auto badFlag = post(L"/graph/save-file", pathBody(dir / L"x.effectgraph", R"(,"embedMedia":"maybe")"));
        TEST("SaveFile_EmbedIntoJson400", embedIntoJson.statusCode == 400);
        TEST("SaveFile_MissingFolder400", noFolder.statusCode == 400 && noFolder.body.find("folder") != std::string::npos);
        TEST("SaveFile_BadEmbedFlag400", badFlag.statusCode == 400);

        fs::remove_all(dir, ec);
    }
}
