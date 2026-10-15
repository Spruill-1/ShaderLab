#include "pch_engine.h"
#include "RouteSupport.h"
#include "../McpRouter.h"

#include "../../../Graph/GraphModel.h"
#include "../../../Rendering/GraphEvaluator.h"
#include "../../../Effects/ShaderCompiler.h"
#include "../../../Effects/ShaderVariants.h"
#include "../../../Graph/GraphEdits.h"

// Custom-effect HLSL: reading it back and recompiling it.
namespace ShaderLab::Mcp::Routes
{
    namespace
    {
        // GET /effect/hlsl/{nodeId}. A ShaderLab library effect reports
        // isLibraryEffect=true: it ships with the app and is not recompiled.
        void RegisterEffectHlsl(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"GET", L"/effect/hlsl/",
                [&sink](const std::wstring& path, const std::wstring&, const std::string&) -> Response
                {
                    uint32_t nodeId = 0;
                    if (auto bad = ReadPathNodeId(path, L"/effect/hlsl/", nodeId))
                        return *bad;

                    return sink.Dispatch([nodeId](EngineContext& ctx) -> Response
                    {
                        const auto* node = ctx.graph->FindNode(nodeId);
                        if (!node)
                            return NodeNotFound(nodeId);

                        JsonWriter json;
                        json.BeginObject().Key("nodeId").Int(nodeId);
                        if (!node->customEffect.has_value())
                        {
                            json.Key("hasCustomEffect").Bool(false)
                                .Key("name").String(node->name)
                                .Key("runtimeError").String(node->runtimeError);
                            return Json(200, json.EndObject().Take());
                        }

                        const auto& def = *node->customEffect;
                        json.Key("hasCustomEffect").Bool(true)
                            .Key("name").String(node->name)
                            .Key("shaderType").String(def.shaderType == Graph::CustomShaderType::PixelShader
                                ? "PixelShader" : "ComputeShader")
                            .Key("hlslSource").String(def.hlslSource)
                            .Key("inputNames").BeginArray();
                        for (const auto& name : def.inputNames)
                            json.String(name);
                        json.EndArray().Key("parameters").BeginArray();
                        for (const auto& parameter : def.parameters)
                            json.BeginObject().Key("name").String(parameter.name).EndObject();
                        json.EndArray()
                            .Key("bytecodeSize").Int(def.compiledBytecode.size())
                            .Key("isCompiled").Bool(def.isCompiled())
                            .Key("runtimeError").String(node->runtimeError)
                            .Key("isLibraryEffect").Bool(!def.shaderLabEffectId.empty());
                        if (!def.shaderLabEffectId.empty())
                            json.Key("shaderLabEffectId").String(def.shaderLabEffectId)
                                .Key("shaderLabEffectVersion").Int(def.shaderLabEffectVersion);
                        return Json(200, json.EndObject().Take());
                    });
                });
        }

        // The analysisFields of a compile request, or nullopt and the reason.
        std::optional<std::vector<Graph::AnalysisFieldDescriptor>> ReadAnalysisFields(
            const WDJ::JsonArray& list, std::string& error)
        {
            std::vector<Graph::AnalysisFieldDescriptor> fields;
            for (uint32_t i = 0; i < list.Size(); ++i)
            {
                const std::string where = std::format("analysisFields[{}]", i);
                const auto value = list.GetAt(i);
                if (value.ValueType() != WDJ::JsonValueType::Object)
                {
                    error = where + " must be an object";
                    return std::nullopt;
                }
                const auto entry = value.GetObject();
                std::string fieldError;
                auto name = RequireStringArg(entry, L"name", fieldError);
                auto typeTag = RequireStringArg(entry, L"type", fieldError);
                const uint32_t length = ReadUintArg(entry, L"length", fieldError).value_or(0);
                if (!fieldError.empty())
                {
                    error = where + " " + fieldError;
                    return std::nullopt;
                }
                auto type = Graph::AnalysisFieldTypeFromTag(*typeTag);
                if (!type)
                {
                    error = where + " has unknown type '" + WideToUtf8(*typeTag) + "'";
                    return std::nullopt;
                }
                Graph::AnalysisFieldDescriptor field;
                field.name = *name;
                field.type = *type;
                field.arrayLength = length;
                fields.push_back(std::move(field));
            }
            return fields;
        }

        // POST /effect/compile { nodeId, hlsl, analysisFields? }: compiles the
        // generic build for the node's shader type, then installs the bytecode
        // under a fresh shader GUID with the new fields. The evaluator builds
        // the option variants afterwards. A compile error answers 200 with
        // compiled=false. OnCustomEffectRecompiled lets the host rebuild the
        // canvas (pins may have changed); this is the Effect Designer's
        // "Update in Graph" path.
        void RegisterCompileEffect(McpRouter& router, IEngineCommandSink& sink)
        {
            router.AddRoute(L"POST", L"/effect/compile",
                [&sink](const std::wstring&, const std::wstring&, const std::string& body) -> Response
                {
                    WDJ::JsonObject args{ nullptr };
                    if (!ParseObjectBody(body, args))
                        return Error(400, "Invalid JSON");
                    std::string argError;
                    const auto nodeId = RequireUintArg(args, L"nodeId", argError);
                    const auto hlsl = RequireStringArg(args, L"hlsl", argError);
                    const auto fieldList = ReadArrayArg(args, L"analysisFields", argError);
                    if (!argError.empty())
                        return Error(400, argError);
                    std::optional<std::vector<Graph::AnalysisFieldDescriptor>> newFields;
                    if (fieldList)
                    {
                        newFields = ReadAnalysisFields(*fieldList, argError);
                        if (!newFields)
                            return Error(400, argError);
                    }
                    const std::string hlslUtf8 = Effects::CanonicalizeHlslSource(*hlsl);

                    return sink.Dispatch([&](EngineContext& ctx) -> Response
                    {
                        auto* node = ctx.graph->FindNode(*nodeId);
                        if (!node || !node->customEffect.has_value())
                            return Error(404, "Custom effect node not found");

                        // SL_FIELD_ macros come from the fields this request
                        // applies, so the baseline and the variants agree.
                        auto compileDef = *node->customEffect;
                        if (newFields)
                            compileDef.analysisFields = *newFields;
                        const auto macroDefs = Effects::GenericVariantMacros(compileDef);
                        std::vector<Effects::ShaderCompiler::MacroDef> macros;
                        for (const auto& [name, value] : macroDefs)
                            macros.push_back({ name.c_str(), value.c_str() });
                        auto result = Effects::ShaderCompiler::CompileFromString(
                            hlslUtf8, "McpCompile", "main", Effects::ShaderTargetFor(compileDef), macros);
                        if (!result.succeeded)
                            return Json(200, JsonWriter().BeginObject()
                                .Key("compiled").Bool(false)
                                .Key("error").String(result.ErrorMessage())
                                .EndObject().Take());

                        auto* blob = result.bytecode.get();
                        const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
                        auto& def = *node->customEffect;
                        def.hlslSource = *hlsl;
                        def.compiledBytecode.assign(bytes, bytes + blob->GetBufferSize());
                        ::CoCreateGuid(&def.shaderGuid);
                        if (newFields)
                        {
                            def.analysisFields = std::move(*newFields);
                            def.analysisOutputType = def.analysisFields.empty()
                                ? Graph::AnalysisOutputType::None
                                : Graph::AnalysisOutputType::Typed;
                        }
                        // An authored effect's contracts follow its current HLSL
                        // and properties; a built-in keeps what it declares.
                        if (def.shaderLabEffectId.empty())
                            Graph::DeriveContracts(def, node->properties);

                        Graph::MarkNodeChanged(*node);
                        if (ctx.evaluator)
                            ctx.evaluator->UpdateNodeShader(*nodeId, *node);

                        // A name stands for its shader: rename if another node
                        // has this name with different HLSL.
                        ctx.graph->EnsureUniqueCustomEffectName(*nodeId);
                        sink.OnCustomEffectRecompiled(*nodeId);

                        const auto* updated = ctx.graph->FindNode(*nodeId);
                        return Json(200, JsonWriter().BeginObject()
                            .Key("compiled").Bool(true)
                            .Key("bytecodeSize").Int(updated ? updated->customEffect->compiledBytecode.size() : size_t{ 0 })
                            .EndObject().Take());
                    });
                });
        }
    }

    void RegisterEffectRoutes(McpRouter& router, IEngineCommandSink& sink)
    {
        RegisterEffectHlsl(router, sink);
        RegisterCompileEffect(router, sink);
    }
}
