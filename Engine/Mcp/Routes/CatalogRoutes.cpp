#include "pch_engine.h"
#include "RouteSupport.h"
#include "../McpRouter.h"
#include "../McpSerialize.h"

#include "../../../Effects/EffectRegistry.h"
#include "../../../Effects/ShaderLabEffects.h"

// Static catalogs and conventions: the effect registry, the effect list and
// the /context onboarding document. Both catalogs are immutable after
// startup, so these routes read them without a dispatch.
namespace ShaderLab::Mcp::Routes
{
    namespace
    {
        // GET /registry lists every Direct2D effect; GET /registry/effect/<name>
        // details one.
        void RegisterRegistry(McpRouter& router)
        {
            router.AddRoute(L"GET", L"/registry",
                [](const std::wstring& path, const std::wstring&, const std::string&) -> Response
                {
                    auto& registry = Effects::EffectRegistry::Instance();
                    constexpr std::wstring_view cEffectPrefix = L"/registry/effect/";
                    if (path.starts_with(cEffectPrefix))
                    {
                        const auto* desc = registry.FindByName(path.substr(cEffectPrefix.size()));
                        if (!desc)
                            return Error(404, "Effect not found");

                        WDJ::JsonObject effect;
                        effect.Insert(L"name", WDJ::JsonValue::CreateStringValue(desc->name));
                        effect.Insert(L"category", WDJ::JsonValue::CreateStringValue(desc->category));
                        effect.Insert(L"inputCount", WDJ::JsonValue::CreateNumberValue(
                            static_cast<double>(desc->inputPins.size())));
                        WDJ::JsonObject properties;
                        for (const auto& [key, meta] : desc->propertyMetadata)
                        {
                            WDJ::JsonObject property;
                            property.Insert(L"min", WDJ::JsonValue::CreateNumberValue(meta.minValue));
                            property.Insert(L"max", WDJ::JsonValue::CreateNumberValue(meta.maxValue));
                            property.Insert(L"step", WDJ::JsonValue::CreateNumberValue(meta.step));
                            if (!meta.enumLabels.empty())
                            {
                                WDJ::JsonArray labels;
                                for (const auto& label : meta.enumLabels)
                                    labels.Append(WDJ::JsonValue::CreateStringValue(label));
                                property.Insert(L"enumLabels", labels);
                            }
                            properties.Insert(key, property);
                        }
                        effect.Insert(L"properties", properties);
                        return Json(200, WideToUtf8(std::wstring(effect.Stringify())));
                    }

                    WDJ::JsonArray effects;
                    for (const auto& desc : registry.All())
                    {
                        WDJ::JsonObject effect;
                        effect.Insert(L"name", WDJ::JsonValue::CreateStringValue(desc.name));
                        effect.Insert(L"category", WDJ::JsonValue::CreateStringValue(desc.category));
                        effect.Insert(L"inputCount", WDJ::JsonValue::CreateNumberValue(
                            static_cast<double>(desc.inputPins.size())));
                        effects.Append(effect);
                    }
                    return Json(200, WideToUtf8(std::wstring(effects.Stringify())));
                });
        }

        // GET /effects: every effect name by category. The Direct2D "Analysis"
        // category is left out: the ShaderLab analysis effects supersede those
        // wrappers.
        void RegisterListEffects(McpRouter& router)
        {
            router.AddRoute(L"GET", L"/effects",
                [](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    JsonWriter json;
                    json.BeginObject().Key("builtIn").BeginObject();
                    auto& registry = Effects::EffectRegistry::Instance();
                    for (const auto& category : registry.Categories())
                    {
                        if (category == L"Analysis") continue;
                        json.Key(WideToUtf8(category)).BeginArray();
                        for (const auto* effect : registry.ByCategory(category))
                            json.String(effect->name);
                        json.EndArray();
                    }
                    json.EndObject().Key("shaderLab").BeginObject();
                    auto& shaderLab = Effects::ShaderLabEffects::Instance();
                    for (const auto& category : shaderLab.Categories())
                    {
                        json.Key(WideToUtf8(category)).BeginArray();
                        for (const auto* effect : shaderLab.ByCategory(category))
                            json.String(effect->name);
                        json.EndArray();
                    }
                    json.EndObject();

                    // User effects are listed in their categories above; this adds
                    // where they were loaded from and what failed to load.
                    auto list = [&json](const char* key, const std::vector<std::wstring>& values)
                    {
                        json.Key(key).BeginArray();
                        for (const auto& value : values)
                            json.String(value);
                        json.EndArray();
                    };
                    const auto& user = shaderLab.UserEffectReport();
                    json.Key("user").BeginObject();
                    list("directories", user.directories);
                    list("loaded", user.loaded);
                    list("errors", user.errors);
                    json.EndObject().EndObject();
                    return Json(200, json.Take());
                });
        }

        // GET /context: pipeline and shader conventions for a calling agent.
        // The API itself is described by tools/list.
        void RegisterContext(McpRouter& router)
        {
            router.AddRoute(L"GET", L"/context",
                [](const std::wstring&, const std::wstring&, const std::string&) -> Response
                {
                    static constexpr const char* cContext = R"JSON({
"name": "ShaderLab",
"description": "D2D shader effect development tool with node graph, HDR/WCG pipeline.",
"api": "tools/list describes every tool this host serves, with its arguments.",
"pipeline": {
    "format": "scRGB FP16 linear light",
    "whitePoint": "1.0 = 80 nits SDR white",
    "valuesAbove1": "HDR, super-white",
    "colorSpace": "Linear sRGB primaries, Rec.709; negative components carry wide-gamut colour"
},
"shaderConventions": {
    "signature": "Pixel shaders MUST declare (SV_POSITION, SCENE_POSITION, TEXCOORD0..N), one TEXCOORD per input. Omitting SCENE_POSITION binds the scene coordinate to TEXCOORD0 and displaces sampling whenever content is smaller than the D2D intermediate",
    "texcoords": "With the signature correct TEXCOORD is NORMALIZED 0-1; pixel coordinates come from SCENE_POSITION",
    "sampling": "Sample(InputSampler, uvN.xy) using each input's own TEXCOORD; never reuse one coordinate across inputs",
    "filteredSampling": "Use SampleLevel with GetDimensions normalization for bilinear",
    "constantBuffer": "register b0, variables packed by D3DReflect offsets",
    "textures": "register t0..t7, one per input",
    "computeOutput": "RWTexture2D<float4> Output : register u0"
},
"analysisEffects": {
    "pattern": "Compute shaders can act as analysis effects that read an entire image and produce typed output fields",
    "outputTypes": "float, float2, float3, float4, floatarray, float2array, float3array, float4array",
    "outputConvention": "Write results to Output[int2(pixelOffset, 0)]. Each field occupies pixelCount() pixels sequentially; SL_FIELD_<name> is each field's offset",
    "readback": "The host reads the output row and unpacks typed fields based on analysisFields descriptors",
    "fieldDescriptors": "Define analysisFields array in custom effect definition with name, type, and length (for arrays)",
    "propertyBindings": "Analysis output fields can be bound to downstream node properties via propertyBindings"
},
"nodeTypes": ["Source", "BuiltInEffect", "PixelShader", "ComputeShader", "Output"],
"outputNote": "PNG captures clip to SDR. Read FP32 scRGB values with POST /render/pixel-region (read_pixel_region). Values above 1.0 are HDR."
})JSON";
                    return Json(200, cContext);
                });
        }
    }

    void RegisterCatalogRoutes(McpRouter& router)
    {
        RegisterRegistry(router);
        RegisterListEffects(router);
        RegisterContext(router);
    }
}
