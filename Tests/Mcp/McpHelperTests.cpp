#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/ShaderLabEffects.h"
#include "Graph/GraphEdits.h"
#include "Graph/GraphModel.h"
#include "Mcp/EngineMcpRoutes.h"
#include "Mcp/McpArgs.h"
#include "Mcp/McpRouter.h"
#include "Mcp/McpSerialize.h"

#include <cstdio>
#include <format>
#include <string>

// The MCP argument readers and serialisers, and the engine routes sharing one
// owner per policy with the engine.
namespace ShaderLab::Tests
{
    namespace
    {
        using namespace ShaderLab::Graph;

        SL_TEST_CASE(TestMcpHelpers, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== MCP argument readers and serialisers ===\n");
            using winrt::Windows::Data::Json::JsonObject;
            auto args = JsonObject::Parse(LR"({"a":true,"b":"off","c":"banana","n":"12.5","u":"-1","s":5})");
            std::string error;
            TEST("McpArgs_BoolFromJson", Mcp::ReadBoolArg(args, L"a", error) == true && error.empty());
            TEST("McpArgs_BoolFromWord", Mcp::ReadBoolArg(args, L"b", error) == false && error.empty());
            TEST("McpArgs_MissingIsNotAnError", !Mcp::ReadBoolArg(args, L"missing", error) && error.empty());
            TEST("McpArgs_UnknownWordIsAnError", !Mcp::ReadBoolArg(args, L"c", error) && !error.empty());
            error.clear();
            TEST("McpArgs_NumberFromString", Mcp::ReadNumberArg(args, L"n", error) == 12.5 && error.empty());
            TEST("McpArgs_NegativeUintRefused", !Mcp::ReadUintArg(args, L"u", error) && !error.empty());
            error.clear();
            TEST("McpArgs_StringMustBeString", !Mcp::ReadStringArg(args, L"s", error) && !error.empty());

            const uint8_t bytes[] = { 'M', 'a', 'n', 'y' };
            TEST("McpSerialize_Base64", Mcp::Base64Encode(bytes) == "TWFueQ==");
            AnalysisFieldValue arrayField;
            arrayField.name = L"Hist";
            arrayField.type = AnalysisFieldType::Float2Array;
            arrayField.arrayData = { 1, 2, 3, 4 };
            const std::string json = Mcp::AnalysisFieldToJson(arrayField);
            TEST("McpSerialize_ArrayFieldHasTypeAndCount", json.find("\"type\":\"float2array\"") != std::string::npos &&
                 json.find("\"count\":2") != std::string::npos);
            TEST("McpSerialize_OneNodeTypeVocabulary", std::string(Mcp::NodeTypeName(NodeType::BuiltInEffect)) == "BuiltInEffect");
        }

        // /graph/add-node and /graph/apply share CreateNodeByName and WriteProperty.
        SL_TEST_CASE(TestMcpRoutesShareOwners, Tag::Fast | Tag::Mcp)
        {
            printf("\n=== MCP routes share one owner per policy ===\n");
            struct GraphOnlySink : Mcp::IEngineCommandSink
            {
                IEffectGraphContextInternal* context{ nullptr };
                Mcp::Response Dispatch(std::function<Mcp::Response(Mcp::EngineContext&)> closure) override
                {
                    Mcp::EngineContext engineContext(*context);
                    return closure(engineContext);
                }
            };
            auto context = CreateTestContext();
            auto& graph = context->Model();
            GraphOnlySink sink;
            sink.context = context.get();
            McpRouter router;
            Mcp::RegisterEngineRoutes(router, sink);

            auto applied = router.RouteRequest(L"POST", L"/graph/apply", R"({
                "nodes": [ { "ref": "shader", "effect": "Custom Pixel Shader" }, { "ref": "out", "effect": "Output" } ] })");
            TEST("Apply_CreatesCustomShader", applied.statusCode == 200 && graph.Nodes().size() == 2 &&
                 graph.Nodes()[0].customEffect.has_value());

            // An injected value a custom shader gains after creation is
            // injected once the shader is recompiled.
            {
                auto& shader = graph.MutableNodes()[0];
                ShaderLab::Graph::SetNodeProperty(shader, L"InputMask", 0.0f);
                const std::string hlsl = "Texture2D t0 : register(t0); SamplerState s0 : register(s0); "
                    "cbuffer C : register(b0) { float InputMask; }; "
                    "float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv0 : TEXCOORD0)"
                    " : SV_TARGET { return t0.Sample(s0, uv0.xy) * InputMask; }";
                const auto compiled = router.RouteRequest(L"POST", L"/effect/compile",
                    std::format(R"({{"nodeId":{},"hlsl":"{}"}})", shader.id, hlsl));
                TEST("Compile_RederivesAuthoredContracts", compiled.statusCode == 200 &&
                     compiled.body.find("\"compiled\":true") != std::string::npos &&
                     HasInjected(graph.Nodes()[0].customEffect->injectedConstants, InjectedConstants::InputMask));

                // SL_FIELD_ offsets come from the fields sent with the compile,
                // not the node's previous list.
                graph.MutableNodes()[0].customEffect->analysisFields =
                    { { L"Lo", AnalysisFieldType::Float }, { L"Hi", AnalysisFieldType::Float } };
                const std::string fieldHlsl = "#if SL_FIELD_Lo != 2 || SL_FIELD_Mid != 1\\n#error stale field offsets\\n#endif\\n"
                    "Texture2D t0 : register(t0); SamplerState s0 : register(s0); "
                    "float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv0 : TEXCOORD0)"
                    " : SV_TARGET { return t0.Sample(s0, uv0.xy); }";
                const auto reordered = router.RouteRequest(L"POST", L"/effect/compile",
                    std::format(R"({{"nodeId":{},"hlsl":"{}","analysisFields":[)"
                        R"({{"name":"Hi","type":"float"}},{{"name":"Mid","type":"float"}},{{"name":"Lo","type":"float"}}]}})",
                        graph.Nodes()[0].id, fieldHlsl));
                TEST("Compile_FieldMacrosFollowRequestFields", reordered.statusCode == 200 &&
                     reordered.body.find("\"compiled\":true") != std::string::npos &&
                     graph.Nodes()[0].customEffect->analysisFields.size() == 3);
            }

            // A source's file reads back under the name set-property takes.
            ShaderLab::Graph::SetNodeMedia(graph.MutableNodes()[1], L"clip.mp4");
            const auto nodeJson = router.RouteRequest(L"GET",
                std::format(L"/graph/node/{}", graph.Nodes()[1].id), "");
            TEST("NodeJson_ReportsMediaPathAsShaderPath", nodeJson.statusCode == 200 &&
                 nodeJson.body.find("\"shaderPath\":\"clip.mp4\"") != std::string::npos);
            graph.MutableNodes()[1].mediaPath.reset();

            auto added = router.RouteRequest(L"POST", L"/graph/add-node", R"({"effectName":"Output"})");
            TEST("AddNode_NumbersOutputs", added.statusCode == 200 && graph.Nodes().back().name == L"Output 2");

            const uint32_t clockId = [&]
            {
                auto created = router.RouteRequest(L"POST", L"/graph/add-node", R"({"effectName":"Clock"})");
                return graph.Nodes().back().id;
            }();
            auto setBad = router.RouteRequest(L"POST", L"/graph/set-property",
                std::format(R"({{"nodeId":{},"key":"StopTime","value":"12abc"}})", clockId));
            TEST("SetProperty_UnparseableNumberRefused", setBad.statusCode == 400);
            auto setNull = router.RouteRequest(L"POST", L"/graph/set-property",
                std::format(R"({{"nodeId":{},"key":"StopTime","value":null}})", clockId));
            TEST("SetProperty_NullRefused", setNull.statusCode == 400);
            auto applyPlay = router.RouteRequest(L"POST", L"/graph/apply",
                R"({"nodes":[{"effect":"Clock","properties":{"isPlaying":"banana"}}]})");
            TEST("Apply_BadIsPlayingRefusedLikeSetProperty", applyPlay.statusCode == 400);
        }
    }
}
