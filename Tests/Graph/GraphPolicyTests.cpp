#include "pch_engine.h"
#include "../TestSupport.h"

#include "../../Graph/GraphModel.h"
#include "../../Graph/ClockModel.h"
#include "../../Graph/PropertyWrite.h"
#include "../../Graph/ParameterModel.h"
#include "../../Rendering/GpuBindingPlan.h"
#include "../../Rendering/GraphEvaluator.h"
#include "../../Rendering/EffectGraphFile.h"
#include "../../Rendering/DisplayMonitor.h"
#include "../../Rendering/DisplayProfile.h"
#include "../../Effects/NodeCatalog.h"
#include "../../Effects/ShaderIncludes.h"
#include "../../Effects/ShaderLabEffects.h"
#include "../../Effects/ShaderVariants.h"
#include "../../Engine/Mcp/McpArgs.h"
#include "../../Engine/Mcp/McpSerialize.h"
#include "../../Engine/Mcp/McpRouter.h"
#include "../../Engine/Mcp/EngineMcpRoutes.h"
#include "Graph/GraphEdits.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

// Policies that have one owner each: the GPU binding plan, untyped property
// writes, node creation by name, custom-effect name uniqueness, the variant
// macro spellings, the MCP argument readers and the shared MCP serialisers.
// None of them needs a device.
namespace ShaderLab::Tests
{
    namespace
    {
        using namespace ShaderLab::Graph;
        namespace Numerics = winrt::Windows::Foundation::Numerics;

        ParameterDefinition Param(const wchar_t* name, const wchar_t* type, PropertyValue value, bool gpuBindable = false)
        {
            ParameterDefinition param;
            param.name = name;
            param.typeName = type;
            param.defaultValue = std::move(value);
            param.gpuBindable = gpuBindable;
            return param;
        }

        EffectNode ShaderNode(const wchar_t* name, CustomShaderType shaderType, std::vector<ParameterDefinition> params)
        {
            EffectNode node;
            node.name = name;
            node.type = shaderType == CustomShaderType::PixelShader ? NodeType::PixelShader : NodeType::ComputeShader;
            CustomEffectDefinition def;
            def.shaderType = shaderType;
            def.hlslSource = L"// " + std::wstring(name);
            def.parameters = std::move(params);
            for (const auto& param : def.parameters)
                ShaderLab::Graph::SetNodeProperty(node, param.name, param.defaultValue);
            node.customEffect = std::move(def);
            return node;
        }

        SL_TEST_CASE(TestGpuBindingPlan, Tag::Fast)
        {
            printf("\n=== GPU binding plan ===\n");
            GraphModel graph;
            // A D3D11 compute producer: Mean (float), Vec (float3), Arr (8 floats, 2 pixels).
            auto producerNode = ShaderNode(L"Producer", CustomShaderType::D3D11ComputeShader, {});
            producerNode.customEffect->analysisFields = {
                { L"Mean", AnalysisFieldType::Float },
                { L"Vec",  AnalysisFieldType::Float3 },
                { L"Arr",  AnalysisFieldType::FloatArray, 8 },
            };
            producerNode.customEffect->analysisOutputType = AnalysisOutputType::Typed;
            const uint32_t producer = graph.AddNode(std::move(producerNode));

            // A parameter node is not a GPU producer.
            auto paramNode = ShaderNode(L"Param", CustomShaderType::PixelShader, {});
            paramNode.customEffect->hlslSource.clear();
            paramNode.customEffect->analysisFields = { { L"Value", AnalysisFieldType::Float } };
            paramNode.customEffect->analysisOutputType = AnalysisOutputType::Typed;
            const uint32_t parameter = graph.AddNode(std::move(paramNode));

            const uint32_t consumer = graph.AddNode(ShaderNode(L"Consumer", CustomShaderType::PixelShader, {
                Param(L"A", L"float", 0.0f, true),
                Param(L"B", L"float2", Numerics::float2{}, true),
                Param(L"C", L"float", 0.0f, true),
                Param(L"D", L"float", 0.0f, true),
                Param(L"E", L"float", 0.0f, true),
                Param(L"Plain", L"float", 0.0f),
            }));
            graph.BindProperty(consumer, L"A", producer, L"Mean");
            graph.BindProperty(consumer, L"B", producer, L"Vec");        // .xy of one element
            graph.BindProperty(consumer, L"C", producer, L"Vec", 1);     // .y into a float: not addressable
            graph.BindProperty(consumer, L"D", parameter, L"Value");     // not a compute producer
            // Element 4 of Arr sits in the field's second pixel, in .x.
            PropertyBinding arrayElement;
            arrayElement.sources = { ComponentSource{ producer, L"Arr", 4, 0 } };
            graph.FindNode(consumer)->propertyBindings[L"E"] = arrayElement;

            const auto plan = Rendering::PlanGpuBindings(*graph.FindNode(consumer), graph, true);
            const auto* a = plan.Find(L"A");
            const auto* b = plan.Find(L"B");
            const auto* e = plan.Find(L"E");
            TEST("BindingPlan_ScalarServedAtItsField", a && a->fieldIndex == 0 && a->gpuBindableIndex == 0 &&
                 a->mode == Effects::GpuBindingMode::Texture && a->producerId == producer);
            TEST("BindingPlan_MultiComponentServed", b && b->fieldIndex == 1 && b->gpuBindableIndex == 1);
            TEST("BindingPlan_ComponentMismatchNotServed", plan.Find(L"C") == nullptr);
            TEST("BindingPlan_ParameterNodeNotServed", plan.Find(L"D") == nullptr);
            TEST("BindingPlan_FloatArrayElementAddressedByPixel", e && e->fieldIndex == 2 + 1 && e->gpuBindableIndex == 4);
            const uint32_t expectedBits = Effects::WithBindingMode(Effects::WithBindingMode(
                Effects::WithBindingMode(0, 0, Effects::GpuBindingMode::Texture), 1, Effects::GpuBindingMode::Texture),
                4, Effects::GpuBindingMode::Texture);
            TEST("BindingPlan_BitsPackOneModePerParameter", plan.bits == expectedBits);

            const auto notReady = Rendering::PlanGpuBindings(*graph.FindNode(consumer), graph, true,
                [](uint32_t) { return false; });
            TEST("BindingPlan_ProducerNotReadyServesNothing", notReady.entries.empty() && notReady.bits == 0);
            TEST("BindingPlan_DisabledServesNothing",
                 Rendering::PlanGpuBindings(*graph.FindNode(consumer), graph, false).entries.empty());

            // The same bindings on a compute consumer go through lane 1.
            const uint32_t computeConsumer = graph.AddNode(ShaderNode(L"Compute Consumer",
                CustomShaderType::D3D11ComputeShader, { Param(L"A", L"float", 0.0f, true) }));
            graph.BindProperty(computeConsumer, L"A", producer, L"Mean");
            const auto lane1 = Rendering::PlanGpuBindings(*graph.FindNode(computeConsumer), graph, true);
            TEST("BindingPlan_ComputeConsumerUsesStructuredBuffer", lane1.entries.size() == 1 &&
                 lane1.bits == static_cast<uint32_t>(Effects::GpuBindingMode::StructuredBuffer));
        }

        SL_TEST_CASE(TestVariantPolicy, Tag::Fast)
        {
            printf("\n=== Variant macros and requests ===\n");
            using Effects::GpuBindingMode;
            const uint32_t bits = Effects::PackBindingModes(std::array{ GpuBindingMode::Texture, GpuBindingMode::CBuffer,
                                                                        GpuBindingMode::StructuredBuffer });
            TEST("Variant_PackRoundTrips", Effects::BindingModeAt(bits, 0) == GpuBindingMode::Texture &&
                 Effects::BindingModeAt(bits, 1) == GpuBindingMode::CBuffer &&
                 Effects::BindingModeAt(bits, 2) == GpuBindingMode::StructuredBuffer);
            TEST("Variant_UnusedModeReadsAsCBuffer", Effects::BindingModeAt(3u, 0) == GpuBindingMode::CBuffer);

            const auto macros = Effects::BuildVariantMacros({ "Gain", "Peak" }, bits, { "Mode" }, 0x3);
            const std::vector<Effects::MacroPair> expected{
                { "_SLPARAM_Gain_GPU", "2" }, { "_SLPARAM_Peak_GPU", "0" },
                { "_SLOPT_Mode_MODE", "1" }, { "_SLOPT_Mode", "2" } };
            TEST("Variant_MacroSpellings", macros == expected);

            CustomEffectDefinition def;
            def.shaderType = CustomShaderType::PixelShader;
            def.hlslSource = L"float4 main() : SV_TARGET { return 0; }\r\n";
            def.parameters = { Param(L"Gain", L"float", 1.0f, true) };
            auto mode = Param(L"Mode", L"uint", 0u);
            mode.enumLabels = { L"A", L"B" };
            mode.specialize = true;
            def.parameters.push_back(mode);
            const auto request = Effects::MakeVariantRequest(def, L"Test", 1, "ps_5_0", bits, 0x1);
            TEST("Variant_RequestCarriesBothNameLists",
                 request.gpuBindableParamNames == std::vector<std::string>{ "Gain" } &&
                 request.optionParamNames == std::vector<std::string>{ "Mode" } &&
                 request.key.paramSignatureHash == Effects::HashParamSignature({ "Gain" }, { "Mode" }));
            TEST("Variant_RequestSourceIsCanonical", request.hlslSource.find('\r') == std::string::npos &&
                 request.key.sourceHash == Effects::HashCanonicalSource(request.hlslSource));
        }

        SL_TEST_CASE(TestHlslText, Tag::Fast)
        {
            printf("\n=== HLSL text ===\n");
            // Every built-in's HLSL and every embedded header is 7-bit ASCII,
            // so it round-trips through any conversion.
            bool allAscii = true;
            for (const auto& include : Effects::EmbeddedIncludes())
            {
                for (char ch : include.text)
                {
                    if (static_cast<unsigned char>(ch) < 0x80) continue;
                    printf("  non-ASCII byte in %.*s\n", static_cast<int>(include.name.size()), include.name.data());
                    allAscii = false;
                    break;
                }
            }
            for (const auto& descriptor : Effects::ShaderLabEffects::Instance().All())
            {
                if (descriptor.IsUserEffect()) continue;
                for (unsigned char ch : descriptor.hlslSource)
                {
                    if (ch < 0x80) continue;
                    printf("  non-ASCII byte in %ls\n", descriptor.name.c_str());
                    allAscii = false;
                    break;
                }
            }
            TEST("Hlsl_BuiltInsAreAscii", allAscii);

            // Non-ASCII text survives the trip into a definition and back to UTF-8.
            const std::string utf8 = "// \xCE\xBC and \xE2\x80\x94\nfloat4 main() : SV_TARGET { return 0; }";
            const std::wstring wide = ShaderLab::Utf8ToWide(utf8);
            TEST("Hlsl_Utf8RoundTrips", wide.find(L'\u03BC') != std::wstring::npos &&
                 ShaderLab::WideToUtf8(wide) == utf8 && Effects::CanonicalizeHlslSource(wide) == utf8);
            TEST("Hlsl_CrLfIsOneLine", Effects::CanonicalizeHlslSource(std::wstring_view(L"a\r\nb\rc")) == "a\nb\nc");
        }

        SL_TEST_CASE(TestPropertyWrites, Tag::Fast)
        {
            printf("\n=== Property writes ===\n");
            auto node = ShaderNode(L"Writer", CustomShaderType::PixelShader, {
                Param(L"Gain", L"float", 1.0f), Param(L"Mode", L"uint", 0u), Param(L"Offset", L"int", int32_t{ 0 }),
                Param(L"Enabled", L"bool", false), Param(L"Tint", L"float3", Numerics::float3{}) });
            node.dirty = false;

            TEST("Write_NumberToUint", WriteProperty(node, L"Mode", 3.0).empty() &&
                 std::get<uint32_t>(node.properties[L"Mode"]) == 3u && node.dirty);
            TEST("Write_NumberToInt", WriteProperty(node, L"Offset", -2.0).empty() &&
                 std::get<int32_t>(node.properties[L"Offset"]) == -2);
            TEST("Write_StringToFloat", WriteProperty(node, L"Gain", std::wstring(L" 203 ")).empty() &&
                 std::get<float>(node.properties[L"Gain"]) == 203.0f);
            TEST("Write_StringToBool", WriteProperty(node, L"Enabled", std::wstring(L"On")).empty() &&
                 std::get<bool>(node.properties[L"Enabled"]));
            TEST("Write_ArrayToFloat3", WriteProperty(node, L"Tint", std::vector<double>{ 1, 2, 3 }).empty() &&
                 std::get<Numerics::float3>(node.properties[L"Tint"]).z == 3.0f);
            TEST("Write_StringArrayToFloat3", WriteProperty(node, L"Tint", std::wstring(L"[4, 5, 6]")).empty() &&
                 std::get<Numerics::float3>(node.properties[L"Tint"]).x == 4.0f);

            // Refused, and the previous value kept.
            TEST("Write_PartialNumberRefused", !WriteProperty(node, L"Gain", std::wstring(L"12abc")).empty() &&
                 std::get<float>(node.properties[L"Gain"]) == 203.0f);
            TEST("Write_UnknownBoolWordRefused", !WriteProperty(node, L"Enabled", std::wstring(L"banana")).empty() &&
                 std::get<bool>(node.properties[L"Enabled"]));
            TEST("Write_NegativeUintRefused", !WriteProperty(node, L"Mode", -1.0).empty() &&
                 std::get<uint32_t>(node.properties[L"Mode"]) == 3u);
            TEST("Write_WrongArrayLengthRefused", !WriteProperty(node, L"Tint", std::vector<double>{ 1, 2 }).empty());
            TEST("Write_ArrayIntoScalarRefused", !WriteProperty(node, L"Gain", std::vector<double>{ 1, 2 }).empty());

            // A key with no declared or existing type keeps the value's own type.
            TEST("Write_NewKeyKeepsValueType", WriteProperty(node, L"Note", std::wstring(L"hello")).empty() &&
                 std::get<std::wstring>(node.properties[L"Note"]) == L"hello");
            // The media path has one store: the field, not a property.
            TEST("Write_ShaderPathSetsMediaPath", WriteProperty(node, L"shaderPath", std::wstring(L"x.png")).empty() &&
                 node.mediaPath == L"x.png" && !node.properties.contains(L"shaderPath"));
            TEST("Write_MediaPathNeedsString", !WriteProperty(node, L"mediaPath", 3.0).empty());

            // Playing a finished non-looping clock rewinds it.
            auto clock = Effects::ShaderLabEffects::CreateNode(*Effects::ShaderLabEffects::Instance().FindByName(L"Clock"));
            ShaderLab::Graph::SetNodeProperty(clock, L"Loop", 0.0f);
            clock.clockTime = 10.0;
            TEST("Write_PlayRewindsFinishedClock", WriteProperty(clock, L"isPlaying", std::wstring(L"true")).empty() &&
                 clock.isPlaying && clock.clockTime == 0.0);
            TEST("Write_BadPlayRefused", !WriteProperty(clock, L"IsPlaying", std::wstring(L"maybe")).empty() &&
                 clock.isPlaying);
            SeekClock(clock, 0.5);
            TEST("Clock_SeekIsNormalized", clock.clockTime == 0.5 * ReadClockParams(clock).Duration());

            // A parameter slider: Min / Max properties win over the declared range, and the step snaps.
            auto parameter = ShaderNode(L"Float Parameter", CustomShaderType::PixelShader, { Param(L"Value", L"float", 0.0f) });
            parameter.customEffect->parameters[0].minValue = 0.0f;
            parameter.customEffect->parameters[0].maxValue = 1.0f;
            parameter.customEffect->parameters[0].step = 0.5f;
            ShaderLab::Graph::SetNodeProperty(parameter, L"Max", 10.0f);
            const auto range = ReadParameterRange(parameter);
            TEST("Parameter_MinMaxPropertiesWin", range.min == 0.0f && range.max == 10.0f && range.step == 0.5f);
            TEST("Parameter_SetNormalizedSnaps", SetParameterNormalized(parameter, 0.33) &&
                 std::get<float>(parameter.properties[L"Value"]) == 3.5f);
            TEST("Parameter_UnchangedReportsFalse", !SetParameterNormalized(parameter, 0.34));
            TEST("Parameter_NormalizedValue", std::abs(ParameterNormalizedValue(parameter) - 0.35f) < 1e-6f);
        }

        SL_TEST_CASE(TestNodeCatalogAndNames, Tag::Fast)
        {
            printf("\n=== Node creation and names ===\n");
            GraphModel graph;
            const Effects::NodeCreateArgs noArgs;
            auto custom = Effects::CreateNodeByName(L"Custom Pixel Shader", noArgs, graph);
            TEST("CreateByName_CustomShader", custom.node && custom.node->customEffect.has_value() &&
                 custom.node->customEffect->shaderType == CustomShaderType::PixelShader);
            auto builtIn = Effects::CreateNodeByName(L"Gaussian Blur", noArgs, graph);
            TEST("CreateByName_Direct2DEffect", builtIn.node && builtIn.node->type == NodeType::BuiltInEffect);
            auto shaderLab = Effects::CreateNodeByName(L"Clock", noArgs, graph);
            TEST("CreateByName_ShaderLabEffect", shaderLab.node && shaderLab.node->IsClock());

            auto first = Effects::CreateNodeByName(L"Output", noArgs, graph);
            graph.AddNode(std::move(*first.node));
            auto second = Effects::CreateNodeByName(L"Output", noArgs, graph);
            TEST("CreateByName_OutputsNumbered", second.node && second.node->name == L"Output 2");

            Effects::NodeCreateArgs fileArgs;
            fileArgs.filePath = L"C:\\media\\clip.mp4";
            auto video = Effects::CreateNodeByName(L"Video", fileArgs, graph);
            TEST("CreateByName_VideoNeedsPrepare", video.node && video.needsPrepare && video.node->name == L"clip.mp4");
            auto unknown = Effects::CreateNodeByName(L"No Such Effect", noArgs, graph);
            TEST("CreateByName_UnknownSaysWhy", !unknown.node && unknown.error.find(L"No Such Effect") != std::wstring::npos);

            // Name uniqueness: same name, different HLSL -> renamed.
            GraphModel names;
            auto a = ShaderNode(L"Glow", CustomShaderType::PixelShader, {});
            auto b = ShaderNode(L"Glow", CustomShaderType::PixelShader, {});
            b.customEffect->hlslSource = L"// different";
            const uint32_t idA = names.AddNode(std::move(a));
            const uint32_t idB = names.AddNode(std::move(b));
            TEST("UniqueName_RenamesOnConflict", names.EnsureUniqueCustomEffectName(idB) && names.FindNode(idB)->name == L"Glow (2)");
            TEST("UniqueName_NoConflictNoRename", !names.EnsureUniqueCustomEffectName(idA) && names.FindNode(idA)->name == L"Glow");
            auto c = ShaderNode(L"Glow", CustomShaderType::PixelShader, {});
            c.customEffect->hlslSource = L"// third";
            const uint32_t idC = names.AddNode(std::move(c));
            TEST("UniqueName_SkipsTakenSuffix", names.EnsureUniqueCustomEffectName(idC) && names.FindNode(idC)->name == L"Glow (3)");
        }

        SL_TEST_CASE(TestGraphModelHelpers, Tag::Fast)
        {
            printf("\n=== Graph model helpers ===\n");
            bool tagsRoundTrip = true;
            for (auto type : { AnalysisFieldType::Float, AnalysisFieldType::Float2, AnalysisFieldType::Float3,
                               AnalysisFieldType::Float4, AnalysisFieldType::FloatArray, AnalysisFieldType::Float2Array,
                               AnalysisFieldType::Float3Array, AnalysisFieldType::Float4Array })
                tagsRoundTrip = tagsRoundTrip && AnalysisFieldTypeFromTag(AnalysisFieldTypeTag(type)) == type;
            TEST("FieldTag_RoundTrips", tagsRoundTrip && !AnalysisFieldTypeFromTag(L"float5"));
            TEST("ComponentCount_Shapes", PropertyComponentCount(1.0f) == 1 &&
                 PropertyComponentCount(Numerics::float3{}) == 3 && PropertyComponentCount(std::vector<float>{}) == 0 &&
                 PropertyComponentCount(true) == 1);

            // An unknown field type in a saved graph is reported, not silently float4.
            GraphModel saved;
            auto weird = ShaderNode(L"N", CustomShaderType::D3D11ComputeShader, {});
            weird.customEffect->analysisFields = { { L"Weird", AnalysisFieldType::Float4 } };
            weird.customEffect->analysisOutputType = AnalysisOutputType::Typed;
            const uint32_t weirdId = saved.AddNode(std::move(weird));
            std::wstring json(saved.ToJson());
            const size_t tag = json.find(L"\"float4\"");
            if (tag != std::wstring::npos) json.replace(tag, 8, L"\"float5\"");
            auto graph = GraphModel::FromJson(winrt::hstring(json));
            const auto* node = graph.FindNode(weirdId);
            TEST("FieldTag_UnknownReportedOnLoad", node && node->runtimeError.find(L"Weird") != std::wstring::npos);

            // Binding traversal sees whole-array and per-component sources alike.
            PropertyBinding whole;
            whole.wholeArray = true;
            whole.wholeArraySourceNodeId = 7;
            PropertyBinding perComponent;
            perComponent.sources = { ComponentSource{ 3 }, std::nullopt, ComponentSource{ 5 } };
            std::vector<uint32_t> seen;
            perComponent.ForEachSourceNode([&](uint32_t id) { seen.push_back(id); });
            TEST("BindingSources_WholeArray", whole.ReadsFrom(7) && !whole.ReadsFrom(3));
            TEST("BindingSources_PerComponent", seen == std::vector<uint32_t>{ 3, 5 } && perComponent.ReadsFrom(5));

            // Removing one source of a two-source binding keeps the other; removing the last drops it.
            GraphModel bound;
            auto stats = ShaderNode(L"Stats", CustomShaderType::D3D11ComputeShader, {});
            stats.customEffect->analysisFields = { { L"Lo", AnalysisFieldType::Float }, { L"Hi", AnalysisFieldType::Float } };
            stats.customEffect->analysisOutputType = AnalysisOutputType::Typed;
            const uint32_t statsId = bound.AddNode(std::move(stats));
            const uint32_t userId = bound.AddNode(ShaderNode(L"User", CustomShaderType::PixelShader,
                { Param(L"Range", L"float2", Numerics::float2{}) }));
            PropertyBinding twoFields;
            twoFields.sources = { ComponentSource{ statsId, L"Lo" }, ComponentSource{ statsId, L"Hi" } };
            bound.FindNode(userId)->propertyBindings[L"Range"] = twoFields;
            TEST("UnbindSource_KeepsOtherComponent", bound.UnbindPropertySource(userId, L"Range", statsId, L"Lo") &&
                 bound.FindNode(userId)->propertyBindings.count(L"Range") == 1);
            TEST("UnbindSource_LastComponentDropsBinding", bound.UnbindPropertySource(userId, L"Range", statsId, L"Hi") &&
                 bound.FindNode(userId)->propertyBindings.empty());
        }

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
                GraphModel* graph{ nullptr };
                Rendering::GraphEvaluator evaluator;
                Mcp::Response Dispatch(std::function<Mcp::Response(Mcp::EngineContext&)> closure) override
                {
                    Mcp::EngineContext context;
                    context.graph = graph;
                    context.evaluator = &evaluator;
                    return closure(context);
                }
            };
            GraphModel graph;
            GraphOnlySink sink;
            sink.graph = &graph;
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

    namespace
    {
        // Stale media folders: a fresh heartbeat keeps a folder, an old one does not.
        SL_TEST_CASE(TestStaleMediaDirs, Tag::Fast)
        {
            printf("\n=== Stale media folders ===\n");
            namespace fs = std::filesystem;
            const fs::path root = fs::temp_directory_path() / std::format(L"ShaderLabReapTest-{}", ::GetCurrentProcessId());
            std::error_code ec;
            fs::remove_all(root, ec);
            const fs::path fresh = root / L"ShaderLab-fresh";
            const fs::path old = root / L"ShaderLab-old";
            const fs::path other = root / L"NotShaderLab";
            fs::create_directories(fresh);
            fs::create_directories(old);
            fs::create_directories(other);
            Rendering::TouchMediaHeartbeat(fresh.wstring());
            Rendering::TouchMediaHeartbeat(old.wstring());
            std::ofstream(old / L"clip.mp4", std::ios::binary) << "0123456789";

            // Age the old folder's heartbeat by an hour.
            FILETIME now{};
            ::GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER aged{};
            aged.LowPart = now.dwLowDateTime;
            aged.HighPart = now.dwHighDateTime;
            aged.QuadPart -= 3600ull * 10'000'000ull;
            const FILETIME agedTime{ aged.LowPart, aged.HighPart };
            HANDLE beat = ::CreateFileW((old / L".heartbeat").c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (beat != INVALID_HANDLE_VALUE)
            {
                ::SetFileTime(beat, nullptr, nullptr, &agedTime);
                ::CloseHandle(beat);
            }

            const auto stale = Rendering::FindStaleMediaDirs(root.wstring() + L"\\", Rendering::cStaleMediaDirSec, true);
            TEST("StaleMedia_OnlyTheOldFolder", stale.size() == 1 && fs::path(stale[0].path).filename() == L"ShaderLab-old");
            TEST("StaleMedia_CountsBytes", !stale.empty() && stale[0].bytes >= 10);
            const auto reaped = Rendering::ReapMediaDirs(stale);
            TEST("StaleMedia_Reaped", reaped.deleted == 1 && !fs::exists(old) && fs::exists(fresh) && fs::exists(other));
            fs::remove_all(root, ec);
        }
    }

    namespace
    {
        // A simulated profile's caps carry its primaries, and the live
        // profile is built from the caps in one place.
        SL_TEST_CASE(TestDisplayColorimetry, Tag::Fast)
        {
            printf("\n=== Display profile colorimetry ===\n");
            Rendering::DisplayMonitor monitor;
            Rendering::DisplayCapabilities seen{};
            monitor.SetCallback([&](const Rendering::DisplayCapabilities& caps) { seen = caps; });
            monitor.SetSimulatedProfile(Rendering::PresetP3_600());
            const auto caps = monitor.CachedCapabilities();
            TEST("Profile_SimulatedCapsCarryPrimaries", caps.redPrimaryX == 0.680f && caps.greenPrimaryY == 0.690f);
            TEST("Profile_CallbackSeesPrimaries", seen.redPrimaryX == 0.680f);

            Rendering::DisplayCapabilities live{};
            live.redPrimaryX = 0.708f; live.redPrimaryY = 0.292f;
            live.greenPrimaryX = 0.170f; live.greenPrimaryY = 0.797f;
            live.bluePrimaryX = 0.131f; live.bluePrimaryY = 0.046f;
            const auto profile = Rendering::ProfileFromCaps(live);
            TEST("Profile_FromCapsDetectsGamut", profile.gamut == Rendering::GamutId::BT2020 &&
                 profile.primaryGreen.y == 0.797f && !profile.isSimulated);
        }
    }
}
