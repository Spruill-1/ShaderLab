#include "pch.h"
#include "EffectRegistry.h"

using namespace ShaderLab::Graph;
namespace Numerics = winrt::Windows::Foundation::Numerics;

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Singleton
    // -----------------------------------------------------------------------

    EffectRegistry& EffectRegistry::Instance()
    {
        static EffectRegistry instance;
        return instance;
    }

    EffectRegistry::EffectRegistry()
    {
        RegisterBuiltInEffects();
    }

    void EffectRegistry::Register(EffectDescriptor desc)
    {
        // Every effect has at least one output pin.
        if (desc.outputPins.empty())
            desc.outputPins.push_back({ L"Output", 0 });

        m_descriptors.push_back(std::move(desc));
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    const EffectDescriptor* EffectRegistry::FindByName(std::wstring_view name) const
    {
        for (const auto& d : m_descriptors)
        {
            if (_wcsicmp(d.name.c_str(), name.data()) == 0)
                return &d;
        }
        return nullptr;
    }

    const EffectDescriptor* EffectRegistry::FindByClsid(const GUID& clsid) const
    {
        for (const auto& d : m_descriptors)
        {
            if (IsEqualGUID(d.clsid, clsid))
                return &d;
        }
        return nullptr;
    }

    std::vector<const EffectDescriptor*> EffectRegistry::ByCategory(std::wstring_view category) const
    {
        std::vector<const EffectDescriptor*> result;
        for (const auto& d : m_descriptors)
        {
            if (d.category == category)
                result.push_back(&d);
        }
        return result;
    }

    std::vector<std::wstring> EffectRegistry::Categories() const
    {
        std::set<std::wstring> unique;
        for (const auto& d : m_descriptors)
            unique.insert(d.category);
        return { unique.begin(), unique.end() };
    }

    // -----------------------------------------------------------------------
    // Node factory
    // -----------------------------------------------------------------------

    EffectNode EffectRegistry::CreateNode(const EffectDescriptor& desc)
    {
        EffectNode node;
        node.type = NodeType::BuiltInEffect;
        node.name = desc.name;
        node.effectClsid = desc.clsid;
        node.inputPins = desc.inputPins;
        node.outputPins = desc.outputPins;
        node.properties = desc.defaultProperties;
        return node;
    }

    EffectNode EffectRegistry::CreateOutputNode()
    {
        EffectNode node;
        node.type = NodeType::Output;
        node.name = L"Output";
        node.inputPins.push_back({ L"Input", 0 });
        return node;
    }

    // -----------------------------------------------------------------------
    // Built-in D2D effect catalog
    // -----------------------------------------------------------------------

    // Helper macros for common pin patterns.
    #define SINGLE_INPUT  {{ L"Input", 0 }}
    #define DUAL_INPUT    {{ L"Input", 0 }, { L"Input 2", 1 }}

    void EffectRegistry::RegisterBuiltInEffects()
    {
        // ===== Blur =====

        Register({
            .clsid = CLSID_D2D1GaussianBlur,
            .name = L"Gaussian Blur",
            .category = L"Blur",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"StandardDeviation", 3.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1DirectionalBlur,
            .name = L"Directional Blur",
            .category = L"Blur",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"StandardDeviation", 3.0f },
                { L"Angle", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Shadow,
            .name = L"Shadow",
            .category = L"Blur",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"BlurStandardDeviation", 3.0f },
                { L"Color", Numerics::float4{ 0.0f, 0.0f, 0.0f, 1.0f } },
            },
        });

        // ===== Color =====

        Register({
            .clsid = CLSID_D2D1ColorMatrix,
            .name = L"Color Matrix",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Brightness,
            .name = L"Brightness",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Contrast,
            .name = L"Contrast",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Contrast", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Exposure,
            .name = L"Exposure",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"ExposureValue", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Grayscale,
            .name = L"Grayscale",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1HueRotation,
            .name = L"Hue Rotation",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Angle", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Invert,
            .name = L"Invert",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Saturation,
            .name = L"Saturation",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Saturation", 0.5f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Sepia,
            .name = L"Sepia",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Intensity", 0.5f },
            },
        });

        Register({
            .clsid = CLSID_D2D1TemperatureTint,
            .name = L"Temperature & Tint",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Temperature", 0.0f },
                { L"Tint", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Vignette,
            .name = L"Vignette",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Color", Numerics::float4{ 0.0f, 0.0f, 0.0f, 1.0f } },
                { L"Amount", 0.5f },
                { L"Size", 0.5f },
            },
        });

        // ===== Composition =====

        Register({
            .clsid = CLSID_D2D1Blend,
            .name = L"Blend",
            .category = L"Composition",
            .inputPins = DUAL_INPUT,
            .defaultProperties = {
                { L"Mode", static_cast<uint32_t>(D2D1_BLEND_MODE_MULTIPLY) },
            },
        });

        Register({
            .clsid = CLSID_D2D1Composite,
            .name = L"Composite",
            .category = L"Composition",
            .inputPins = DUAL_INPUT,
            .defaultProperties = {
                { L"Mode", static_cast<uint32_t>(D2D1_COMPOSITE_MODE_SOURCE_OVER) },
            },
        });

        Register({
            .clsid = CLSID_D2D1AlphaMask,
            .name = L"Alpha Mask",
            .category = L"Composition",
            .inputPins = DUAL_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Opacity,
            .name = L"Opacity",
            .category = L"Composition",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Opacity", 1.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1CrossFade,
            .name = L"Cross Fade",
            .category = L"Composition",
            .inputPins = DUAL_INPUT,
            .defaultProperties = {
                { L"Weight", 0.5f },
            },
        });

        // ===== Transform =====

        Register({
            .clsid = CLSID_D2D1Scale,
            .name = L"Scale",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Scale", Numerics::float2{ 1.0f, 1.0f } },
                { L"CenterPoint", Numerics::float2{ 0.0f, 0.0f } },
            },
        });

        Register({
            .clsid = CLSID_D2D12DAffineTransform,
            .name = L"2D Affine Transform",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Border,
            .name = L"Border",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Crop,
            .name = L"Crop",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
        });

        // ===== Sharpen / Detail =====

        Register({
            .clsid = CLSID_D2D1Sharpen,
            .name = L"Sharpen",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Sharpness", 0.0f },
                { L"Threshold", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1EdgeDetection,
            .name = L"Edge Detection",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Strength", 0.5f },
                { L"BlurRadius", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Emboss,
            .name = L"Emboss",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Height", 1.0f },
                { L"Direction", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Posterize,
            .name = L"Posterize",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
        });

        // ===== Lighting =====

        Register({
            .clsid = CLSID_D2D1PointDiffuse,
            .name = L"Point Diffuse",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"LightPosition", Numerics::float3{ 0.0f, 0.0f, 100.0f } },
                { L"DiffuseConstant", 1.0f },
                { L"SurfaceScale", 1.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1SpotDiffuse,
            .name = L"Spot Diffuse",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1PointSpecular,
            .name = L"Point Specular",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"LightPosition", Numerics::float3{ 0.0f, 0.0f, 100.0f } },
                { L"SpecularExponent", 1.0f },
                { L"SpecularConstant", 1.0f },
                { L"SurfaceScale", 1.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1SpotSpecular,
            .name = L"Spot Specular",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1DistantDiffuse,
            .name = L"Distant Diffuse",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1DistantSpecular,
            .name = L"Distant Specular",
            .category = L"Lighting",
            .inputPins = SINGLE_INPUT,
        });

        // ===== Distort =====

        Register({
            .clsid = CLSID_D2D1DisplacementMap,
            .name = L"Displacement Map",
            .category = L"Distort",
            .inputPins = DUAL_INPUT,
            .defaultProperties = {
                { L"Scale", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1Morphology,
            .name = L"Morphology",
            .category = L"Distort",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Turbulence,
            .name = L"Turbulence",
            .category = L"Source",
            .inputPins = {},          // no inputs — generates noise
            .defaultProperties = {
                { L"Offset", Numerics::float2{ 0.0f, 0.0f } },
            },
        });

        // ===== HDR / Tone Map =====

        Register({
            .clsid = CLSID_D2D1HdrToneMap,
            .name = L"HDR Tone Map",
            .category = L"HDR",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"InputMaxLuminance", 4000.0f },
                { L"OutputMaxLuminance", 300.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1WhiteLevelAdjustment,
            .name = L"White Level Adjustment",
            .category = L"HDR",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"InputWhiteLevel", 80.0f },
                { L"OutputWhiteLevel", 80.0f },
            },
        });

        // ===== Other =====

        Register({
            .clsid = CLSID_D2D1Histogram,
            .name = L"Histogram",
            .category = L"Analysis",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Tile,
            .name = L"Tile",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Premultiply,
            .name = L"Premultiply",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1UnPremultiply,
            .name = L"Unpremultiply",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1TableTransfer,
            .name = L"Table Transfer",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1GammaTransfer,
            .name = L"Gamma Transfer",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1ConvolveMatrix,
            .name = L"Convolve Matrix",
            .category = L"Detail",
            .inputPins = SINGLE_INPUT,
        });

        Register({
            .clsid = CLSID_D2D1Straighten,
            .name = L"Straighten",
            .category = L"Transform",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Angle", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1HighlightsShadows,
            .name = L"Highlights & Shadows",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
            .defaultProperties = {
                { L"Highlights", 0.0f },
                { L"Shadows", 0.0f },
            },
        });

        Register({
            .clsid = CLSID_D2D1LookupTable3D,
            .name = L"3D Lookup Table",
            .category = L"Color",
            .inputPins = SINGLE_INPUT,
        });
    }

    #undef SINGLE_INPUT
    #undef DUAL_INPUT
}
